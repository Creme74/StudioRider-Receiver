// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// Plain ws:// client over TCP (POSIX sockets). Used by the tests, and as the
// fallback on systems without a native WebSocket API. No TLS.

#include "WebSocketClient.h"

#if ! defined (_WIN32)

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace studiorider
{

namespace
{
    struct Url
    {
        std::string host, port, target;
    };

    bool parseWsUrl (const std::string& url, Url& out)
    {
        std::string scheme = "ws://";
        if (url.compare (0, 7, "http://") == 0)
            scheme = "http://";
        if (url.compare (0, scheme.size(), scheme) != 0)
            return false;
        auto rest = url.substr (scheme.size());
        const auto slash = rest.find ('/');
        auto authority = rest.substr (0, slash);
        out.target = slash == std::string::npos ? "/" : rest.substr (slash);
        out.port = "80";
        if (! authority.empty() && authority[0] == '[') // [::1]:port
        {
            const auto close = authority.find (']');
            if (close == std::string::npos)
                return false;
            out.host = authority.substr (1, close - 1);
            if (close + 1 < authority.size() && authority[close + 1] == ':')
                out.port = authority.substr (close + 2);
        }
        else
        {
            const auto colon = authority.rfind (':');
            out.host = authority.substr (0, colon);
            if (colon != std::string::npos)
                out.port = authority.substr (colon + 1);
        }
        return ! out.host.empty();
    }

    std::string base64 (const std::uint8_t* d, std::size_t n)
    {
        static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string s;
        for (std::size_t i = 0; i < n; i += 3)
        {
            const std::uint32_t v = ((std::uint32_t) d[i] << 16) | (i + 1 < n ? (std::uint32_t) d[i + 1] << 8 : 0u)
                                  | (i + 2 < n ? (std::uint32_t) d[i + 2] : 0u);
            s += t[(v >> 18) & 63];
            s += t[(v >> 12) & 63];
            s += i + 1 < n ? t[(v >> 6) & 63] : '=';
            s += i + 2 < n ? t[v & 63] : '=';
        }
        return s;
    }
}

class PlainWebSocketClient final : public WebSocketClient
{
public:
    ~PlainWebSocketClient() override { close(); }

    void open (const std::string& url, Callbacks cb) override
    {
        close();
        callbacks = std::move (cb);
        quit = false;
        worker = std::thread ([this, url] { run (url); });
    }

    void sendText (const std::string& text) override
    {
        sendFrame (0x1, reinterpret_cast<const std::uint8_t*> (text.data()), text.size());
    }

    void close() override
    {
        quit = true;
        {
            std::lock_guard<std::mutex> lock (sendMutex);
            if (fd >= 0)
                ::shutdown (fd, SHUT_RDWR);
        }
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id())
            worker.join();
        std::lock_guard<std::mutex> lock (sendMutex);
        if (fd >= 0)
            ::close (fd);
        fd = -1;
    }

    bool supportsTls() const noexcept override { return false; }

    int httpGet (const std::string& url, std::string& body, int timeoutMs) override
    {
        Url u;
        if (! parseWsUrl (url, u))
            return 0;
        const int s = connectTo (u, timeoutMs);
        if (s < 0)
            return 0;
        const auto req = "GET " + u.target + " HTTP/1.0\r\nHost: " + u.host + (u.port == "80" ? "" : ":" + u.port)
                       + "\r\nUser-Agent: StudioRider-Receiver\r\nConnection: close\r\n\r\n";
        std::string all;
        bool ok = ::send (s, req.data(), req.size(), 0) == (ssize_t) req.size();
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);
        char buf[4096];
        while (ok && all.size() < (1u << 20) && std::chrono::steady_clock::now() < end)
        {
            pollfd p { s, POLLIN, 0 };
            if (::poll (&p, 1, 100) <= 0) continue;
            const auto k = ::recv (s, buf, sizeof (buf), 0);
            if (k <= 0) break;
            all.append (buf, (std::size_t) k);
        }
        ::close (s);
        int status = 0;
        if (std::sscanf (all.c_str(), "HTTP/1.%*d %d", &status) != 1)
            return 0;
        const auto h = all.find ("\r\n\r\n");
        body = h == std::string::npos ? std::string() : all.substr (h + 4);
        return status;
    }

private:
    static int connectTo (const Url& u, int timeoutMs)
    {
        addrinfo hints {};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo (u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || res == nullptr)
            return -1;
        int s = -1;
        for (auto* ai = res; ai != nullptr && s < 0; ai = ai->ai_next)
        {
            s = ::socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (s < 0)
                continue;
            timeval tv { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv));
           #ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
           #endif
            if (::connect (s, ai->ai_addr, ai->ai_addrlen) != 0)
            {
                ::close (s);
                s = -1;
            }
        }
        freeaddrinfo (res);
        return s;
    }

    void finish (int code, int httpStatus, const std::string& reason)
    {
        if (! quit.exchange (true) && callbacks.onClose)
            callbacks.onClose (code, httpStatus, reason);
    }

    void run (const std::string& url)
    {
        Url u;
        if (! parseWsUrl (url, u))
            return finish (0, 0, "Unsupported address");

        const int s = connectTo (u, 10000);
        if (s < 0)
            return finish (0, 0, "Can't connect");
        int one = 1;
        setsockopt (s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof (one));
        {
            std::lock_guard<std::mutex> lock (sendMutex);
            if (quit) { ::close (s); return; }
            fd = s;
        }

        std::uint8_t keyBytes[16];
        std::random_device rd;
        for (auto& b : keyBytes) b = (std::uint8_t) rd();
        const auto request = "GET " + u.target + " HTTP/1.1\r\nHost: " + u.host + (u.port == "80" ? "" : ":" + u.port)
                           + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
                           + base64 (keyBytes, sizeof (keyBytes))
                           + "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: StudioRider-Receiver\r\n\r\n";
        if (! sendAll (reinterpret_cast<const std::uint8_t*> (request.data()), request.size()))
            return finish (0, 0, "Connection lost");

        // Response headers
        std::string head;
        std::uint8_t c;
        while (head.size() < 16384 && head.find ("\r\n\r\n") == std::string::npos)
        {
            if (! readExact (&c, 1, 10000))
                return finish (0, 0, "No answer");
            head += (char) c;
        }
        int status = 0;
        if (std::sscanf (head.c_str(), "HTTP/1.%*d %d", &status) != 1)
            return finish (0, 0, "Bad answer");
        if (status != 101)
            return finish (0, status, "Refused");

        if (callbacks.onOpen)
            callbacks.onOpen();

        std::vector<std::uint8_t> message;
        int messageOpcode = 0;
        std::vector<std::uint8_t> payload;
        while (! quit)
        {
            std::uint8_t h[2];
            if (! readExact (h, 2, -1))
                return finish (0, 0, "Connection lost");
            const bool fin = (h[0] & 0x80) != 0;
            const int opcode = h[0] & 0x0f;
            const bool masked = (h[1] & 0x80) != 0;
            std::uint64_t len = h[1] & 0x7f;
            if (len == 126)
            {
                std::uint8_t e[2];
                if (! readExact (e, 2, 10000)) return finish (0, 0, "Connection lost");
                len = ((std::uint64_t) e[0] << 8) | e[1];
            }
            else if (len == 127)
            {
                std::uint8_t e[8];
                if (! readExact (e, 8, 10000)) return finish (0, 0, "Connection lost");
                len = 0;
                for (auto b : e) len = (len << 8) | b;
            }
            if (len > (64u << 20))
                return finish (0, 0, "Message too large");
            std::uint8_t mask[4] = {};
            if (masked && ! readExact (mask, 4, 10000))
                return finish (0, 0, "Connection lost");
            payload.resize ((std::size_t) len);
            if (len > 0 && ! readExact (payload.data(), payload.size(), 10000))
                return finish (0, 0, "Connection lost");
            if (masked)
                for (std::size_t i = 0; i < payload.size(); ++i)
                    payload[i] ^= mask[i & 3];

            if (opcode == 0x8) // close
            {
                int code = 1005;
                std::string reason;
                if (payload.size() >= 2)
                {
                    code = (payload[0] << 8) | payload[1];
                    reason.assign (payload.begin() + 2, payload.end());
                }
                const std::uint8_t echo[2] = { (std::uint8_t) (code >> 8), (std::uint8_t) code };
                sendFrame (0x8, echo, 2);
                return finish (code, 0, reason);
            }
            if (opcode == 0x9) { sendFrame (0xA, payload.data(), payload.size()); continue; }
            if (opcode == 0xA) continue;

            if (opcode != 0)
            {
                messageOpcode = opcode;
                message.clear();
            }
            message.insert (message.end(), payload.begin(), payload.end());
            if (! fin)
                continue;
            if (messageOpcode == 0x2 && callbacks.onBinary)
                callbacks.onBinary (message.data(), message.size());
            else if (messageOpcode == 0x1 && callbacks.onText)
                callbacks.onText (std::string (message.begin(), message.end()));
        }
    }

    // timeoutMs < 0: wait until data arrives or close() is called.
    bool readExact (std::uint8_t* dst, std::size_t n, int timeoutMs)
    {
        std::size_t got = 0;
        int waited = 0;
        while (got < n)
        {
            if (quit)
                return false;
            pollfd p { fd, POLLIN, 0 };
            const int r = ::poll (&p, 1, 200);
            if (r < 0)
                return false;
            if (r == 0)
            {
                waited += 200;
                if (timeoutMs >= 0 && waited >= timeoutMs)
                    return false;
                continue;
            }
            const auto k = ::recv (fd, dst + got, n - got, 0);
            if (k <= 0)
                return false;
            got += (std::size_t) k;
            waited = 0;
        }
        return true;
    }

    bool sendAll (const std::uint8_t* d, std::size_t n)
    {
        while (n > 0)
        {
           #ifdef MSG_NOSIGNAL
            const auto k = ::send (fd, d, n, MSG_NOSIGNAL);
           #else
            const auto k = ::send (fd, d, n, 0);
           #endif
            if (k <= 0)
                return false;
            d += k;
            n -= (std::size_t) k;
        }
        return true;
    }

    void sendFrame (int opcode, const std::uint8_t* d, std::size_t n)
    {
        std::vector<std::uint8_t> f;
        f.push_back ((std::uint8_t) (0x80 | opcode));
        if (n < 126)
            f.push_back ((std::uint8_t) (0x80 | n));
        else if (n < 65536)
        {
            f.push_back (0x80 | 126);
            f.push_back ((std::uint8_t) (n >> 8));
            f.push_back ((std::uint8_t) n);
        }
        else
        {
            f.push_back (0x80 | 127);
            for (int i = 7; i >= 0; --i)
                f.push_back ((std::uint8_t) ((std::uint64_t) n >> (8 * i)));
        }
        std::uint8_t mask[4];
        for (auto& m : mask) m = (std::uint8_t) rng();
        f.insert (f.end(), mask, mask + 4);
        for (std::size_t i = 0; i < n; ++i)
            f.push_back (d[i] ^ mask[i & 3]);

        std::lock_guard<std::mutex> lock (sendMutex);
        if (fd >= 0)
            sendAll (f.data(), f.size());
    }

    Callbacks callbacks;
    std::thread worker;
    std::atomic<bool> quit { false };
    std::mutex sendMutex;
    int fd = -1;
    std::minstd_rand rng { std::random_device{}() };
};

std::unique_ptr<WebSocketClient> makePlainWebSocketClient()
{
    return std::make_unique<PlainWebSocketClient>();
}

#if ! defined (__APPLE__)
std::unique_ptr<WebSocketClient> makeWebSocketClient()
{
    return makePlainWebSocketClient();
}
#endif

} // namespace studiorider

#endif // ! _WIN32
