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
//
// Verified against chiaki-ng master @ 6547d8a (see git submodule).
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

    // Protocol command handlers. Discover/Register/Connect block their calling
    // thread (main.cpp dispatches them on detached threads); results arrive
    // via the emit callbacks.
    void Discover(int timeout_ms);
    void Register(const std::string& host, const std::string& psn_account_id_b64,
                  const std::string& pin);
    void Wake(const std::string& host, const std::string& credentials_b64);
    void Connect(const std::string& host, const std::string& credentials_b64,
                 const std::string& resolution, int fps);
    void Disconnect();
    void RequestIdr();
    void LoginPin(const std::string& pin);

private:
    // chiaki C callbacks (static thunks → member methods). Signatures match
    // the chiaki-ng headers exactly.
    static void DiscoveryCb(ChiakiDiscoveryHost* host, void* user);
    static void RegistCb(ChiakiRegistEvent* event, void* user);
    static void SessionEventCb(ChiakiEvent* event, void* user);
    static bool VideoSampleCb(uint8_t* buf, size_t buf_size, int32_t frames_lost,
                              bool frame_recovered, void* user);
    static void AudioSettingsCb(uint32_t channels, uint32_t rate, void* user);
    static void AudioFrameCb(int16_t* buf, size_t samples_count, void* user);

    void EmitError(const std::string& code, const std::string& message);
    void TeardownSession();
    void FiniRegist();

    EmitText    emit_text_;
    EmitBinary  emit_binary_;

    // Credentials are an opaque blob to the app. We serialize what chiaki
    // needs to reconnect: rp_regist_key (char[16], also the hex wake
    // credential) and rp_key ("morning", uint8_t[16]).
    struct Credentials {
        std::vector<uint8_t> regist_key;  // ChiakiRegisteredHost.rp_regist_key
        std::vector<uint8_t> rp_key;      // ChiakiRegisteredHost.rp_key
        bool                 ps5 = true;
    };
    static std::string   EncodeCredentials(const Credentials&);
    static bool          DecodeCredentials(const std::string& b64, Credentials* out);

    ChiakiLog log_{};

    // Discovery results are aggregated across per-host callbacks during the
    // Discover() window, then emitted as one `consoles` message.
    struct FoundHost {
        std::string host, name, host_id;
        bool ready = false;
    };
    std::mutex             discovery_mtx_;
    std::vector<FoundHost> discovered_;

    std::unique_ptr<ChiakiRegist>      regist_;
    bool                               regist_started_ = false;
    std::unique_ptr<ChiakiSession>     session_;
    std::unique_ptr<ChiakiOpusDecoder> opus_;

    std::mutex   state_mtx_;
    bool         session_active_ = false;
    bool         video_config_sent_ = false;
    uint32_t     audio_rate_ = 48000;
    uint32_t     audio_channels_ = 2;
};

}  // namespace fcrp
