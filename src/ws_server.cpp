// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ws_server.h"

#include <libwebsockets.h>

#include <cstring>

namespace fcrp {

namespace {
// libwebsockets needs LWS_PRE bytes of headroom before the payload for framing.
std::vector<uint8_t> WithLwsPre(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf(LWS_PRE + payload.size());
    std::memcpy(buf.data() + LWS_PRE, payload.data(), payload.size());
    return buf;
}
}  // namespace

WsServer::WsServer() = default;

WsServer::~WsServer() { Stop(); }

// Per-connection scratch owned by libwebsockets (pss).
struct PerSession {
    bool authed = false;
};

int WsServer::LwsCallback(lws* wsi, int reason, void* user, void* in, size_t len) {
    lws_context* ctx = lws_get_context(wsi);
    auto* self = static_cast<WsServer*>(lws_context_user(ctx));
    auto* pss  = static_cast<PerSession*>(user);

    switch (reason) {
        case LWS_CALLBACK_ESTABLISHED: {
            // Reject a second client — one Remote Play session at a time.
            if (self->client_) return -1;

            // Validate ?token=<hex> against the expected value.
            char buf[256] = {0};
            int n = lws_get_urlarg_by_name_safe(wsi, "token", buf, sizeof(buf) - 1);
            if (n <= 0 || self->token_ != buf) {
                lwsl_warn("[ws] rejected connection: bad/missing token\n");
                return -1;
            }
            pss->authed   = true;
            self->client_ = wsi;
            if (self->on_connect_) self->on_connect_();
            break;
        }

        case LWS_CALLBACK_RECEIVE: {
            if (!pss || !pss->authed) return -1;
            if (lws_frame_is_binary(wsi)) break;  // client never sends binary in v1
            if (self->on_text_ && in && len) {
                self->on_text_(std::string(static_cast<const char*>(in), len));
            }
            break;
        }

        case LWS_CALLBACK_SERVER_WRITEABLE: {
            self->FlushQueue(wsi);
            break;
        }

        case LWS_CALLBACK_CLOSED: {
            if (self->client_ == wsi) {
                self->client_ = nullptr;
                if (self->on_disconnect_) self->on_disconnect_();
            }
            break;
        }

        default:
            break;
    }
    return 0;
}

void WsServer::FlushQueue(lws* wsi) {
    std::deque<OutMsg> pending;
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        pending.swap(queue_);
    }
    for (auto& msg : pending) {
        auto framed = WithLwsPre(msg.payload);
        lws_write(wsi, framed.data() + LWS_PRE, msg.payload.size(),
                  msg.binary ? LWS_WRITE_BINARY : LWS_WRITE_TEXT);
    }
}

uint16_t WsServer::Start(uint16_t port, std::string token) {
    token_ = std::move(token);

    static const lws_protocols protocols[] = {
        {"fcrp", &WsServer::LwsCallback, sizeof(PerSession), 4096, 0, nullptr, 0},
        LWS_PROTOCOL_LIST_TERM,
    };

    lws_context_creation_info info;
    std::memset(&info, 0, sizeof(info));
    info.port      = port;
    info.iface     = "127.0.0.1";     // localhost only — never expose on the LAN
    info.protocols = protocols;
    info.user      = this;
    info.options   = LWS_SERVER_OPTION_VALIDATE_UTF8;

    context_ = lws_create_context(&info);
    if (!context_) {
        lwsl_err("[ws] lws_create_context failed\n");
        return 0;
    }

    // Resolve the actual bound port when 0 (ephemeral) was requested.
    uint16_t bound = port;
    if (port == 0) {
        int vhost_port = lws_get_vhost_port(lws_get_vhost_by_name(context_, "default"));
        if (vhost_port > 0) bound = static_cast<uint16_t>(vhost_port);
    }

    running_ = true;
    thread_  = std::thread(&WsServer::ServiceLoop, this);
    return bound;
}

void WsServer::ServiceLoop() {
    while (running_) {
        lws_service(context_, 50);
    }
}

void WsServer::Stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (context_) {
        lws_context_destroy(context_);
        context_ = nullptr;
    }
    client_ = nullptr;
}

void WsServer::SendText(std::string json) {
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        queue_.push_back({std::vector<uint8_t>(json.begin(), json.end()), false});
    }
    if (context_) lws_cancel_service(context_);  // wake the loop → WRITEABLE
}

void WsServer::SendBinary(std::vector<uint8_t> data) {
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        queue_.push_back({std::move(data), true});
    }
    if (context_) lws_cancel_service(context_);
}

}  // namespace fcrp
