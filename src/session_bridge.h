// SPDX-License-Identifier: AGPL-3.0-or-later
//
// SessionBridge — wraps the chiaki-ng core (libchiaki) and translates between
// the fc-remoteplay protocol and chiaki's discovery / registration / session
// APIs. It owns no socket of its own; it emits protocol messages through the
// callbacks the caller (main.cpp) wires to the WsServer.
//
// Threading: chiaki invokes its callbacks on its own worker threads. The
// emit callbacks below therefore fire off-thread; WsServer::Send* are
// thread-safe, so main.cpp can forward them directly.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>

// chiaki-ng public headers (from third_party/chiaki-ng/lib/include)
#include <chiaki/discovery.h>
#include <chiaki/regist.h>
#include <chiaki/session.h>
#include <chiaki/opusdecoder.h>
#include <chiaki/log.h>

namespace fcrp {

class SessionBridge {
public:
    using EmitText   = std::function<void(std::string)>;                 // JSON control
    using EmitBinary = std::function<void(std::vector<uint8_t>)>;        // media frame

    SessionBridge(EmitText emit_text, EmitBinary emit_binary);
    ~SessionBridge();

    // Protocol command handlers (called from the WS thread). Each is
    // non-blocking; results arrive later via the emit callbacks.
    void Discover(int timeout_ms);
    void Register(const std::string& host, const std::string& psn_account_id,
                  const std::string& pin);
    void Wake(const std::string& host, const std::string& credentials_b64);
    void Connect(const std::string& host, const std::string& credentials_b64,
                 const std::string& resolution, int fps);
    void Disconnect();
    void RequestIdr();
    void LoginPin(const std::string& pin);

private:
    // chiaki C callbacks (static thunks → member methods)
    static void DiscoveryCb(ChiakiDiscoveryHost* hosts, size_t count, void* user);
    static void RegistCb(ChiakiRegistEvent* event, void* user);
    static void SessionEventCb(ChiakiEvent* event, void* user);
    static bool VideoSampleCb(uint8_t* buf, size_t buf_size, void* user);
    static void AudioHeaderCb(ChiakiAudioHeader* header, void* user);
    static void AudioFrameCb(int16_t* buf, size_t samples_count, void* user);

    void EmitError(const std::string& code, const std::string& message);
    void TeardownSession();

    EmitText    emit_text_;
    EmitBinary  emit_binary_;

    // Credentials are an opaque blob to the app: we serialize the fields chiaki
    // needs (regist key, RP key, MAC) to base64 and hand them back on connect.
    struct Credentials {
        std::vector<uint8_t> regist_key;  // ChiakiRegisteredHost.rp_regist_key
        std::vector<uint8_t> rp_key;      // ChiakiRegisteredHost.rp_key
        uint64_t             account_id = 0;
        bool                 ps5 = true;
    };
    static std::string   EncodeCredentials(const Credentials&);
    static bool          DecodeCredentials(const std::string& b64, Credentials* out);

    ChiakiLog                    log_{};
    std::unique_ptr<ChiakiDiscovery>       discovery_;
    std::unique_ptr<ChiakiRegist>          regist_;
    std::unique_ptr<ChiakiSession>         session_;
    std::unique_ptr<ChiakiOpusDecoder>     opus_;

    std::mutex   state_mtx_;
    bool         session_active_ = false;
    bool         video_config_sent_ = false;
};

}  // namespace fcrp
