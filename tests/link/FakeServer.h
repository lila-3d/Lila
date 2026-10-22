#pragma once

#include "link/Socket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/**
 * In-process stand-in for the LilaLink editor plugin. Each request line goes to `handler`; a returned value is sent
 * back (an array sends several replies at once), nullopt sends nothing (to provoke timeouts). `rawWrite` replaces
 * the plain send so tests can split replies across writes.
 */
class FakeServer
{
public:
    using json = nlohmann::json;
    using Handler = std::function<std::optional<json>(const json& request)>;

    explicit FakeServer(Handler h) : mHandler(std::move(h))
    {
        mListener.listen(0);
        mThread = std::thread([this] { run(); });
    }

    ~FakeServer()
    {
        mStop = true;
        mThread.join();
    }

    uint16_t port() const { return mListener.port(); }

    std::vector<json> requests()
    {
        std::scoped_lock lock(mMutex);
        return mRequests;
    }

    /** Called with each finished reply line (newline included); write it however the test likes. */
    std::function<void(lila::link::TcpSocket&, const std::string&)> rawWrite;

    static json reply(const json& req, json result) { return {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", std::move(result)}}; }
    static json progress(const json& req, const std::string& stage, double elapsed)
    {
        return {{"jsonrpc", "2.0"}, {"method", "progress"}, {"params", {{"id", req["id"]}, {"stage", stage}, {"elapsed", elapsed}}}};
    }

    /** Writes one message now, from inside a handler (which runs on the server thread). */
    void send(const json& msg)
    {
        if (mClient)
            mClient->sendAll(msg.dump() + "\n");
    }

private:
    void run()
    {
        lila::link::TcpSocket client;
        while (!mStop && !client.valid())
            client = mListener.accept(20);
        mClient = &client;
        std::string buf;
        char chunk[4096];
        while (!mStop)
        {
            int n = client.recv(chunk, sizeof(chunk), 20);
            if (n < 0)
                break;
            buf.append(chunk, static_cast<size_t>(n));
            for (size_t nl; (nl = buf.find('\n')) != std::string::npos;)
            {
                json req = json::parse(buf.substr(0, nl));
                buf.erase(0, nl + 1);
                {
                    std::scoped_lock lock(mMutex);
                    mRequests.push_back(req);
                }
                std::optional<json> out = mHandler(req);
                if (!out)
                    continue;
                std::string line;
                for (const json& r : out->is_array() ? *out : json::array({*out}))
                    line += r.dump() + "\n";
                if (rawWrite)
                    rawWrite(client, line);
                else
                    client.sendAll(line);
            }
        }
        mClient = nullptr;
    }

    Handler mHandler;
    lila::link::TcpListener mListener;
    std::thread mThread;
    std::atomic<bool> mStop{false};
    lila::link::TcpSocket* mClient = nullptr;
    std::mutex mMutex;
    std::vector<json> mRequests;
};
