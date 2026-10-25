#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "link/Socket.h"
#include "stream/ChangeStream.h"
#include "stream/Internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace lila::stream
{
namespace
{
using detail::MessagePtr;

/** A client this far behind loses its backlog and is told to resync. */
constexpr size_t kMaxBacklogBytes = size_t(256) << 20;
constexpr size_t kMaxRequestBytes = size_t(1) << 20;
constexpr int kProtocolVersion = 1;
/** A client that must authenticate and hasn't by then is dropped. */
constexpr auto kAuthTimeout = std::chrono::seconds(10);
/** Stopping waits at most this long for what is queued to reach the clients. */
constexpr auto kDrainTimeout = std::chrono::seconds(1);

/** Compares in time independent of where the first difference is. */
bool sameToken(std::string_view a, std::string_view b)
{
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ (i < b.size() ? b[i] : 0));
    return diff == 0;
}

struct Client
{
    uint64_t id = 0;
    link::TcpSocket sock;
    std::thread thread;
    std::mutex m; // outbox, backlog, filter
    std::condition_variable cv;
    std::deque<std::string> outbox;
    size_t backlog = 0;
    Filter filter;
    std::atomic<bool> closed{false};
    std::atomic<bool> authed{false};
    std::atomic<bool> sending{false}; // taken from the outbox and not yet written
    bool closeAfterSend = false; // client thread only: hang up once the outbox is sent

    void push(std::string text)
    {
        {
            std::scoped_lock lock(m);
            if (closed)
                return;
            if (backlog + text.size() > kMaxBacklogBytes)
            {
                outbox.clear();
                backlog = 0;
                text = detail::line({{"type", "resync"}, {"seq", sequence()}, {"reason", "overflow"}});
            }
            backlog += text.size();
            outbox.push_back(std::move(text));
        }
        cv.notify_one();
    }

    void push(const json& j) { push(detail::line(j)); }
};

class Server
{
public:
    ~Server() { stopThreads(); }

    /** An empty token lets every client in. */
    bool start(int port, bool localOnly, std::string token, std::string* err)
    {
        LILA_ZONE();

        if (!mListener.listen(uint16_t(port), err, localOnly))
            return false;
        mLocalOnly = localOnly;
        mToken = std::move(token);
        mRunning = true;
        mDispatch = std::thread([this] { dispatchLoop(); });
        mAccept = std::thread([this] { acceptLoop(); });
        log::info("change stream: listening on {}:{}", localOnly ? "127.0.0.1" : "0.0.0.0", mListener.port());
        return true;
    }

    void stopThreads()
    {
        if (!mRunning)
            return;
        drain();
        if (!mRunning.exchange(false))
            return;
        mQueueCv.notify_all();
        if (mAccept.joinable())
            mAccept.join();
        if (mDispatch.joinable())
            mDispatch.join();
        std::vector<std::shared_ptr<Client>> clients;
        {
            std::scoped_lock lock(mClientsMutex);
            clients.swap(mClients);
        }
        for (const auto& c : clients)
            close(*c);
        mListener.close();
        std::scoped_lock lock(mQueueMutex);
        mQueue.clear();
        mQueued = 0;
    }

    bool running() const { return mRunning; }
    int port() const { return mRunning ? mListener.port() : 0; }
    bool localOnly() const { return mLocalOnly; }
    bool requiresToken() const { return !mToken.empty(); }

    size_t clientCount()
    {
        std::scoped_lock lock(mClientsMutex);
        size_t n = 0;
        for (const auto& c : mClients)
            n += c->closed ? 0 : 1;
        return n;
    }

    void send(MessagePtr m, uint64_t target)
    {
        if (!mRunning)
            return;
        {
            std::scoped_lock lock(mQueueMutex);
            mQueue.emplace_back(std::move(m), target);
            ++mQueued;
        }
        mQueueCv.notify_one();
    }

private:
    /** Waits, up to kDrainTimeout, until every queued message has been written to its clients. */
    void drain()
    {
        const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            bool idle = mQueued.load() == 0;
            if (idle)
            {
                std::scoped_lock lock(mClientsMutex);
                for (const auto& c : mClients)
                {
                    std::scoped_lock clientLock(c->m);
                    idle &= c->closed || (c->outbox.empty() && !c->sending);
                }
            }
            if (idle)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    static void close(Client& c)
    {
        c.closed = true;
        c.cv.notify_all();
        c.sock.shutdown();
        if (c.thread.joinable())
            c.thread.join();
    }

    void acceptLoop()
    {
        LILA_THREAD_NAME("Change stream");
        while (mRunning)
        {
            link::TcpSocket sock = mListener.accept(100);
            prune();
            if (!sock.valid())
                continue;
            auto c = std::make_shared<Client>();
            c->id = mNextClient++;
            c->sock = std::move(sock);
            c->authed = mToken.empty();
            c->push({{"type", "hello"}, {"protocol", kProtocolVersion}, {"app", "lila"}, {"seq", sequence()}, {"auth", !c->authed}});
            c->thread = std::thread([this, c] { clientLoop(*c); });
            std::scoped_lock lock(mClientsMutex);
            mClients.push_back(std::move(c));
        }
    }

    /** Joins the threads of clients that went away. */
    void prune()
    {
        std::vector<std::shared_ptr<Client>> gone;
        {
            std::scoped_lock lock(mClientsMutex);
            std::erase_if(mClients, [&gone](const std::shared_ptr<Client>& c) {
                if (!c->closed)
                    return false;
                gone.push_back(c);
                return true;
            });
        }
        for (const auto& c : gone)
            close(*c);
        if (!gone.empty())
            updateWants();
    }

    /** Tells the recorder what the clients' filters ask for. */
    void updateWants()
    {
        std::vector<Filter> filters;
        {
            std::scoped_lock lock(mClientsMutex);
            for (const auto& c : mClients)
            {
                if (c->closed)
                    continue;
                std::scoped_lock clientLock(c->m);
                filters.push_back(c->filter);
            }
        }
        detail::setServerFilters(std::move(filters));
        mainq::post(detail::syncServerFilters);
    }

    void dispatchLoop()
    {
        LILA_THREAD_NAME("Change stream send");
        while (true)
        {
            std::deque<std::pair<MessagePtr, uint64_t>> batch;
            {
                std::unique_lock lock(mQueueMutex);
                mQueueCv.wait(lock, [this] { return !mQueue.empty() || !mRunning; });
                if (!mRunning)
                    return;
                batch.swap(mQueue);
            }
            std::vector<std::shared_ptr<Client>> clients;
            {
                std::scoped_lock lock(mClientsMutex);
                clients = mClients;
            }
            for (const auto& [msg, target] : batch)
                deliver(*msg, target, clients);
            mQueued -= batch.size();
        }
    }

    /** Serializes once per distinct filter among the clients it goes to. */
    void deliver(const detail::Message& msg, uint64_t target, const std::vector<std::shared_ptr<Client>>& clients)
    {
        LILA_ZONE();

        std::map<std::string, std::string> byFilter;
        for (const auto& c : clients)
        {
            if (c->closed || !c->authed || (target && c->id != target))
                continue;
            Filter f;
            {
                std::scoped_lock lock(c->m);
                f = c->filter;
            }
            std::string key = f.toJson().dump();
            auto it = byFilter.find(key);
            if (it == byFilter.end())
            {
                json j = detail::toJson(msg, f);
                it = byFilter.emplace(std::move(key), j.is_null() ? std::string() : detail::line(j)).first;
            }
            if (!it->second.empty())
                c->push(it->second);
        }
    }

    void clientLoop(Client& c)
    {
        LILA_THREAD_NAME("Change stream client");
        std::string pending;
        char buf[4096];
        const auto connected = std::chrono::steady_clock::now();
        while (mRunning && !c.closed)
        {
            if (!c.authed && !c.closeAfterSend && std::chrono::steady_clock::now() - connected > kAuthTimeout)
                refuse(c, "auth", "no token sent");
            std::deque<std::string> out;
            {
                std::unique_lock lock(c.m);
                c.cv.wait_for(lock, std::chrono::milliseconds(20), [&c] { return !c.outbox.empty() || c.closed.load(); });
                out.swap(c.outbox);
                c.backlog = 0;
                c.sending = !out.empty();
            }
            for (const std::string& text : out)
                if (!c.sock.sendAll(text))
                {
                    c.closed = true;
                    break;
                }
            c.sending = false;
            if (c.closed || c.closeAfterSend)
                break;
            const int n = c.sock.recv(buf, int(sizeof(buf)), 0);
            if (n < 0)
                break;
            pending.append(buf, size_t(n));
            for (size_t eol; !c.closeAfterSend && (eol = pending.find('\n')) != std::string::npos;)
            {
                std::string request = pending.substr(0, eol);
                pending.erase(0, eol + 1);
                handle(c, request);
            }
            if (pending.size() > kMaxRequestBytes)
            {
                c.push({{"type", "error"}, {"message", "request too long"}});
                pending.clear();
            }
        }
        c.closed = true;
        c.sock.shutdown();
    }

    void handle(Client& c, std::string_view text)
    {
        if (!text.empty() && text.back() == '\r')
            text.remove_suffix(1);
        if (text.empty())
            return;
        json req = json::parse(text, nullptr, false);
        if (req.is_discarded() || !req.is_object())
        {
            c.push({{"type", "error"}, {"message", "each request is one JSON object per line"}});
            return;
        }
        const std::string op = req.value("op", "");
        if (!c.authed)
        {
            if (op != "auth" && op != "subscribe")
                return refuse(c, op, "this server needs a token: send {\"op\": \"auth\", \"token\": ...} first");
            const json token = req.value("token", json());
            if (!token.is_string() || !sameToken(token.get<std::string>(), mToken))
                return refuse(c, op, "wrong token");
            c.authed = true;
            if (op == "auth")
            {
                c.push({{"type", "authed"}, {"seq", sequence()}});
                return;
            }
        }
        if (op == "auth")
            c.push({{"type", "authed"}, {"seq", sequence()}});
        else if (op == "subscribe")
        {
            try
            {
                Filter f = Filter::fromJson(req);
                json echo = f.toJson();
                {
                    std::scoped_lock lock(c.m);
                    c.filter = std::move(f);
                }
                updateWants();
                c.push({{"type", "subscribed"}, {"seq", sequence()}, {"filter", echo}});
            }
            catch (const std::exception& e)
            {
                c.push({{"type", "error"}, {"op", op}, {"message", e.what()}});
                return;
            }
            if (req.value("snapshot", false))
                requestSnapshot(c);
        }
        else if (op == "snapshot")
            requestSnapshot(c);
        else if (op == "ping")
            c.push({{"type", "pong"}, {"seq", sequence()}});
        else
            c.push({{"type", "error"}, {"op", op}, {"message", "unknown op '" + op + "'; use auth, subscribe, snapshot or ping"}});
    }

    /** Sends the error, then hangs up. */
    static void refuse(Client& c, const std::string& op, const std::string& message)
    {
        c.push({{"type", "error"}, {"op", op}, {"message", message}});
        c.closeAfterSend = true;
    }

    /** Built on the main thread, so the client can apply every later message with a greater seq on top of it. */
    void requestSnapshot(Client& c)
    {
        bool world = false, evaluated = false;
        {
            std::scoped_lock lock(c.m);
            world = c.filter.world;
            evaluated = c.filter.evaluated;
        }
        mainq::post([this, client = c.id, world, evaluated] {
            if (mRunning)
                send(detail::buildSnapshot(world, evaluated), client);
        });
    }

    link::TcpListener mListener;
    std::atomic<bool> mRunning{false};
    bool mLocalOnly = true;
    std::string mToken; // set before the threads start, read-only while they run
    std::thread mAccept;
    std::thread mDispatch;
    std::mutex mQueueMutex;
    std::condition_variable mQueueCv;
    std::deque<std::pair<MessagePtr, uint64_t>> mQueue;
    std::atomic<size_t> mQueued{0}; // sent and not yet delivered
    std::mutex mClientsMutex;
    std::vector<std::shared_ptr<Client>> mClients;
    std::atomic<uint64_t> mNextClient{1};
};

Server& server()
{
    static Server s;
    return s;
}
} // namespace

bool startServer(int port, bool localOnly, std::string* err, bool requireToken)
{
    Server& s = server();
    const bool token = requireToken || !localOnly;
    if (s.running())
    {
        if ((port == 0 || port == s.port()) && localOnly == s.localOnly() && token == s.requiresToken())
            return true;
        stopServer();
    }
    if (!s.start(port, localOnly, token ? tokenFromPrefs() : std::string(), err))
        return false;
    detail::setServerActive(true);
    return true;
}

void stopServer()
{
    Server& s = server();
    if (!s.running())
        return;
    detail::setServerActive(false);
    detail::setServerFilters({});
    s.stopThreads();
    log::info("change stream: stopped");
}

bool serverRunning() { return server().running(); }
int serverPort() { return server().port(); }
bool serverLocalOnly() { return server().localOnly(); }
bool serverRequiresToken() { return server().running() && server().requiresToken(); }
size_t clientCount() { return server().clientCount(); }

namespace detail
{
void serverSend(MessagePtr m, uint64_t target)
{
    server().send(std::move(m), target);
}
} // namespace detail
} // namespace lila::stream
