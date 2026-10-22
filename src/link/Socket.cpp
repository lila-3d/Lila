#include "link/Socket.h"

#include "core/Profile.h"

#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SockLen = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
#endif

namespace lila::link
{
namespace
{
#ifdef _WIN32
using Native = SOCKET;
constexpr int kSendFlags = 0;

void ensureInit()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}

void closeNative(Native s) { closesocket(s); }
int pollNative(pollfd* fds, int n, int timeoutMs) { return WSAPoll(fds, static_cast<ULONG>(n), timeoutMs); }
int lastError() { return WSAGetLastError(); }
bool inProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }

void setNonBlocking(Native s, bool on)
{
    u_long mode = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &mode);
}
#else
using Native = int;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void ensureInit() {}
void closeNative(Native s) { ::close(s); }
int pollNative(pollfd* fds, int n, int timeoutMs) { return ::poll(fds, static_cast<nfds_t>(n), timeoutMs); }
int lastError() { return errno; }
bool inProgress(int e) { return e == EINPROGRESS || e == EWOULDBLOCK; }

void setNonBlocking(Native s, bool on)
{
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
#endif

Native native(TcpSocket::Handle h) { return static_cast<Native>(h); }
TcpSocket::Handle handle(Native s) { return static_cast<TcpSocket::Handle>(s); }

/** Waits for the given poll events. 1 = ready, 0 = timeout, -1 = error or hangup without data. */
int waitFor(Native s, short events, int timeoutMs)
{
    pollfd p{};
    p.fd = s;
    p.events = events;
    int r = pollNative(&p, 1, timeoutMs);
    if (r < 0)
        return -1;
    if (r == 0)
        return 0;
    if (p.revents & (events | POLLHUP))
        return 1; // a hangup still reads as 0 bytes, which the caller treats as closed
    return -1;
}

void configure(Native s)
{
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}
} // namespace

TcpSocket::~TcpSocket()
{
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : mHandle(other.mHandle)
{
    other.mHandle = kInvalid;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept
{
    if (this != &other)
    {
        close();
        mHandle = other.mHandle;
        other.mHandle = kInvalid;
    }
    return *this;
}

TcpSocket TcpSocket::connect(const std::string& host, uint16_t port, int timeoutMs, std::string* err)
{
    LILA_ZONE();

    ensureInit();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
    {
        if (err)
            *err = "cannot resolve " + host;
        return {};
    }

    Native s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (handle(s) == kInvalid)
    {
        freeaddrinfo(res);
        if (err)
            *err = "socket() failed";
        return {};
    }

    setNonBlocking(s, true);
    int rc = ::connect(s, res->ai_addr, static_cast<SockLen>(res->ai_addrlen));
    freeaddrinfo(res);
    bool ok = rc == 0;
    if (!ok && inProgress(lastError()) && waitFor(s, POLLOUT, timeoutMs) == 1)
    {
        int soErr = 0;
        SockLen len = sizeof(soErr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
        ok = soErr == 0;
    }
    if (!ok)
    {
        closeNative(s);
        if (err)
            *err = "cannot connect to " + host + ":" + portStr;
        return {};
    }
    setNonBlocking(s, false);
    configure(s);
    return TcpSocket(handle(s));
}

bool TcpSocket::sendAll(std::string_view data)
{
    LILA_ZONE();

    while (!data.empty())
    {
        if (!valid())
            return false;
        int n = ::send(native(mHandle), data.data(), static_cast<int>(data.size()), kSendFlags);
        if (n <= 0)
            return false;
        data.remove_prefix(static_cast<size_t>(n));
    }
    return true;
}

int TcpSocket::recv(char* buf, int size, int timeoutMs)
{
    if (!valid())
        return -1;
    int w = waitFor(native(mHandle), POLLIN, timeoutMs);
    if (w <= 0)
        return w;
    int n = ::recv(native(mHandle), buf, size, 0);
    return n > 0 ? n : -1;
}

void TcpSocket::shutdown()
{
    if (!valid())
        return;
#ifdef _WIN32
    ::shutdown(native(mHandle), SD_BOTH);
#else
    ::shutdown(native(mHandle), SHUT_RDWR);
#endif
}

void TcpSocket::close()
{
    if (!valid())
        return;
    shutdown();
    closeNative(native(mHandle));
    mHandle = kInvalid;
}

TcpListener::~TcpListener()
{
    close();
}

bool TcpListener::listen(uint16_t port, std::string* err)
{
    LILA_ZONE();

    ensureInit();
    close();
    Native s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (handle(s) == TcpSocket::kInvalid)
    {
        if (err)
            *err = "socket() failed";
        return false;
    }
#ifndef _WIN32
    // POSIX only: on Windows this flag lets two listeners share a port.
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0)
    {
        closeNative(s);
        if (err)
            *err = "cannot listen on port " + std::to_string(port);
        return false;
    }
    SockLen len = sizeof(addr);
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
    mPort = ntohs(addr.sin_port);
    mHandle = handle(s);
    return true;
}

TcpSocket TcpListener::accept(int timeoutMs)
{
    LILA_ZONE();

    if (mHandle == TcpSocket::kInvalid || waitFor(native(mHandle), POLLIN, timeoutMs) != 1)
        return {};
    Native c = ::accept(native(mHandle), nullptr, nullptr);
    if (handle(c) == TcpSocket::kInvalid)
        return {};
    configure(c);
    return TcpSocket(handle(c));
}

void TcpListener::close()
{
    if (mHandle == TcpSocket::kInvalid)
        return;
    closeNative(native(mHandle));
    mHandle = TcpSocket::kInvalid;
    mPort = 0;
}
} // namespace lila::link
