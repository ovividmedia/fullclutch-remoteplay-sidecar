// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Minimal single-client WebSocket server built on libwebsockets.
// Enforces the ?token=<hex> gate on /session and runs the lws service loop on
// its own thread. All outbound sends are queued and flushed from the lws
// thread (libwebsockets is not thread-safe for cross-thread writes).
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct lws;
struct lws_context;

namespace fcrp {

class WsServer {
public:
    // Called on the lws thread when a JSON control frame arrives.
    using TextHandler    = std::function<void(const std::string&)>;
    // Called when the (single) client connects / disconnects.
    using ConnectHandler = std::function<void()>;

    WsServer();
    ~WsServer();

    // Bind to 127.0.0.1:<port> (0 = ephemeral). Returns the chosen port, or 0
    // on failure. `token` must match the client's ?token= query param.
    uint16_t Start(uint16_t port, std::string token);
    void Stop();

    void OnText(TextHandler h)         { on_text_ = std::move(h); }
    void OnClientConnect(ConnectHandler h)    { on_connect_ = std::move(h); }
    void OnClientDisconnect(ConnectHandler h) { on_disconnect_ = std::move(h); }

    // Thread-safe. Queue a JSON control frame or a binary media frame; both are
    // flushed on the lws thread. No-ops when no client is connected.
    void SendText(std::string json);
    void SendBinary(std::vector<uint8_t> data);

    bool HasClient() const { return client_ != nullptr; }

private:
    static int LwsCallback(lws* wsi, int reason, void* user, void* in, size_t len);
    void ServiceLoop();
    void FlushQueue(lws* wsi);

    struct OutMsg {
        std::vector<uint8_t> payload;
        bool                 binary;
    };

    lws_context*        context_ = nullptr;
    lws*                client_  = nullptr;
    std::string         token_;
    std::atomic<bool>   running_{false};
    std::thread         thread_;

    std::mutex          queue_mtx_;
    std::deque<OutMsg>  queue_;

    TextHandler         on_text_;
    ConnectHandler      on_connect_;
    ConnectHandler      on_disconnect_;
};

}  // namespace fcrp
