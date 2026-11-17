// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 The StudioRider authors
//
// StudioRider Receiver — minimal WebSocket client.
//
//   macOS : NSURLSessionWebSocketTask (ws:// and wss://, system TLS and proxies)
//   other : plain ws:// over TCP (tests and local network only, no TLS yet)
//
// Callbacks come from a background thread, one at a time, never after close()
// has returned.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace studiorider
{

class WebSocketClient
{
public:
    struct Callbacks
    {
        std::function<void()> onOpen;
        std::function<void (const std::uint8_t* data, std::size_t size)> onBinary;
        std::function<void (const std::string& text)> onText;
        // code: WebSocket close code (1000, 4000…), or 0 if the connection
        // failed or dropped without one. httpStatus: the server's answer when
        // the connection was refused (404…), 0 otherwise.
        std::function<void (int code, int httpStatus, const std::string& reason)> onClose;
    };

    virtual ~WebSocketClient() = default;

    // Starts connecting (returns at once). url: ws://… or wss://…
    virtual void open (const std::string& url, Callbacks callbacks) = 0;
    virtual void sendText (const std::string& text) = 0;
    // Closes and waits for the callbacks to stop. No onClose afterwards.
    virtual void close() = 0;

    virtual bool supportsTls() const noexcept = 0;

    // Plain HTTP(S) GET, blocking (used to read the studio's "info" page when
    // the WebSocket can't connect). Returns the HTTP status, or 0 if no answer.
    virtual int httpGet (const std::string& url, std::string& body, int timeoutMs) = 0;
};

std::unique_ptr<WebSocketClient> makeWebSocketClient();

// The plain-TCP version, available everywhere (used by the tests).
std::unique_ptr<WebSocketClient> makePlainWebSocketClient();

} // namespace studiorider
