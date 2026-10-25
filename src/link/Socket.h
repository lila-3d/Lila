#pragma once

#include "core/Export.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lila::link
{
/** Blocking TCP stream over WinSock or BSD sockets. Move-only; closes on destruction. */
class LILA_SCENE_API TcpSocket
{
public:
    TcpSocket() = default;
    ~TcpSocket();
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /** Connects to host:port, giving up after timeoutMs. */
    static TcpSocket connect(const std::string& host, uint16_t port, int timeoutMs, std::string* err = nullptr);

    bool valid() const { return mHandle != kInvalid; }
    bool sendAll(std::string_view data);

    /** Bytes read (> 0), 0 on timeout, -1 when the peer closed or the socket failed. */
    int recv(char* buf, int size, int timeoutMs);

    /** Unblocks a recv in another thread and releases the socket. */
    void close();
    /** Stops further sends and receives without releasing the handle; safe while another thread is in recv. */
    void shutdown();

    using Handle = std::intptr_t;
    static constexpr Handle kInvalid = -1;
    explicit TcpSocket(Handle h) : mHandle(h) {}

private:
    Handle mHandle = kInvalid;
};

/** Listening socket, on 127.0.0.1 unless told otherwise. */
class LILA_SCENE_API TcpListener
{
public:
    TcpListener() = default;
    ~TcpListener();
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    /** Port 0 picks a free one; see port(). localOnly = false binds every interface. */
    bool listen(uint16_t port, std::string* err = nullptr, bool localOnly = true);
    uint16_t port() const { return mPort; }

    /** Waits up to timeoutMs for a connection; invalid socket on timeout. */
    TcpSocket accept(int timeoutMs);
    void close();

private:
    TcpSocket::Handle mHandle = TcpSocket::kInvalid;
    uint16_t mPort = 0;
};
} // namespace lila::link
