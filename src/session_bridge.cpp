// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Verified against chiaki-ng master @ 6547d8a — all struct fields, callback
// signatures, and function names below match lib/include/chiaki/*.h of the
// pinned submodule.
#include "session_bridge.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "protocol.h"

using json = nlohmann::json;

namespace fcrp {

namespace {
uint64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Minimal base64 (credentials blobs + PSN account id — no external dep).
const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<uint8_t>& in) {
    std::string out;
    int val = 0, bits = -6;
    for (uint8_t c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) { out.push_back(kB64[(val >> bits) & 0x3F]); bits -= 6; }
    }
    if (bits > -6) out.push_back(kB64[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

std::vector<uint8_t> Base64Decode(const std::string& in) {
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; ++i) T[(uint8_t)kB64[i]] = i;
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (uint8_t c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out.push_back((val >> bits) & 0xFF); bits -= 8; }
    }
    return out;
}
// chiaki_log_cb_print writes to stdout. That is wrong for us twice over:
// stdout carries the FCRP handshake line, and when stdout is a pipe (which it
// always is under the desktop app) the CRT fully buffers it — main() flushes
// its own handshake, but chiaki's log calls never flush, so every diagnostic
// died in the buffer and the log file showed only libwebsockets stderr output.
// Write to stderr and flush each line so failures are actually visible.
void LogToStderr(ChiakiLogLevel level, const char* msg, void* /*user*/) {
    std::fprintf(stderr, "[chiaki:%c] %s\n", chiaki_log_level_char(level), msg);
    std::fflush(stderr);
}
}  // namespace

SessionBridge::SessionBridge(EmitText emit_text, EmitBinary emit_binary)
    : emit_text_(std::move(emit_text)), emit_binary_(std::move(emit_binary)) {
    // INFO included: chiaki reports the session-request/ctrl handshake at INFO,
    // which is exactly what is needed to tell a stale registration from an
    // unreachable console. VERBOSE/DEBUG stay off — they log per-packet.
    chiaki_log_init(&log_, CHIAKI_LOG_INFO | CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR,
                    LogToStderr, nullptr);
}

SessionBridge::~SessionBridge() {
    TeardownSession();
    FiniRegist();
}

void SessionBridge::EmitError(const std::string& code, const std::string& message) {
    emit_text_(json{{"type", evt::kError}, {"code", code}, {"message", message}}.dump());
}

// ── Discovery ────────────────────────────────────────────────────────────────
// chiaki's discovery cb fires once PER RESPONDING HOST; we aggregate for the
// timeout window, then emit a single `consoles` list.

void SessionBridge::DiscoveryCb(ChiakiDiscoveryHost* host, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    if (!host) return;
    FoundHost fh;
    fh.host    = host->host_addr ? host->host_addr : "";
    fh.name    = host->host_name ? host->host_name : "PS5";
    fh.host_id = host->host_id ? host->host_id : "";
    fh.ready   = host->state == CHIAKI_DISCOVERY_HOST_STATE_READY;

    std::lock_guard<std::mutex> lk(self->discovery_mtx_);
    for (auto& existing : self->discovered_)
        if (existing.host == fh.host) { existing = fh; return; }  // dedupe re-announces
    self->discovered_.push_back(std::move(fh));
}

void SessionBridge::Discover(int timeout_ms) {
    {
        std::lock_guard<std::mutex> lk(discovery_mtx_);
        discovered_.clear();
    }

    ChiakiDiscovery discovery;
    if (chiaki_discovery_init(&discovery, &log_, AF_INET) != CHIAKI_ERR_SUCCESS) {
        EmitError("discover_failed", "could not init discovery socket");
        return;
    }

    ChiakiDiscoveryThread thread;
    if (chiaki_discovery_thread_start(&thread, &discovery, &SessionBridge::DiscoveryCb, this)
        != CHIAKI_ERR_SUCCESS) {
        EmitError("discover_failed", "could not start discovery thread");
        chiaki_discovery_fini(&discovery);
        return;
    }

    // Broadcast a SRCH to PS5 (port 9302) and PS4 (port 987) with the right
    // protocol version each — same as chiaki's own CLI discover.
    auto send_srch = [&](uint16_t port, const char* proto_version) {
        ChiakiDiscoveryPacket packet;
        std::memset(&packet, 0, sizeof(packet));
        packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
        packet.protocol_version = const_cast<char*>(proto_version);

        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_BROADCAST;
        chiaki_discovery_send(&discovery, &packet,
                              reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    };
    send_srch(CHIAKI_DISCOVERY_PORT_PS5, CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5);
    send_srch(CHIAKI_DISCOVERY_PORT_PS4, CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4);

    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 3000));

    chiaki_discovery_thread_stop(&thread);
    chiaki_discovery_fini(&discovery);

    json list = json::array();
    {
        std::lock_guard<std::mutex> lk(discovery_mtx_);
        for (const auto& h : discovered_) {
            list.push_back({
                {"host", h.host},
                {"name", h.name},
                {"hostId", h.host_id},
                {"state", h.ready ? "ready" : "standby"},
                {"registered", false},  // registration state lives app-side
            });
        }
    }
    emit_text_(json{{"type", evt::kConsoles}, {"list", list}}.dump());
}

// ── Registration (pairing) ───────────────────────────────────────────────────

void SessionBridge::FiniRegist() {
    if (regist_ && regist_started_) {
        chiaki_regist_stop(regist_.get());
        chiaki_regist_fini(regist_.get());
    }
    regist_.reset();
    regist_started_ = false;
}

void SessionBridge::Register(const std::string& host, const std::string& psn_account_id_b64,
                             const std::string& pin) {
    FiniRegist();  // clean up any previous attempt

    ChiakiRegistInfo info;
    std::memset(&info, 0, sizeof(info));
    info.target        = CHIAKI_TARGET_PS5_1;
    info.host          = host.c_str();
    info.broadcast     = false;
    info.psn_online_id = nullptr;  // use psn_account_id (required for PS5)
    info.pin           = static_cast<uint32_t>(std::strtoul(pin.c_str(), nullptr, 10));
    info.holepunch_info = nullptr; // local regist, not PSN remote
    info.rudp           = nullptr;

    // The app passes the base64 PSN account id; chiaki wants the raw 8 bytes.
    auto acct = Base64Decode(psn_account_id_b64);
    if (acct.size() < CHIAKI_PSN_ACCOUNT_ID_SIZE) {
        EmitError("register_failed",
                  "invalid PSN account id (expected base64 of 8 bytes)");
        return;
    }
    std::memcpy(info.psn_account_id, acct.data(), CHIAKI_PSN_ACCOUNT_ID_SIZE);

    regist_ = std::make_unique<ChiakiRegist>();
    if (chiaki_regist_start(regist_.get(), &log_, &info, &SessionBridge::RegistCb, this)
        != CHIAKI_ERR_SUCCESS) {
        EmitError("register_failed", "could not start registration");
        regist_.reset();
        return;
    }
    regist_started_ = true;
    // NOTE: host string must outlive the regist thread; chiaki copies the info
    // in chiaki_regist_start, so the locals here are safe to release.
}

void SessionBridge::RegistCb(ChiakiRegistEvent* event, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    switch (event->type) {
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS: {
            const ChiakiRegisteredHost* rh = event->registered_host;
            Credentials creds;
            const auto* rk = reinterpret_cast<const uint8_t*>(rh->rp_regist_key);
            creds.regist_key.assign(rk, rk + sizeof(rh->rp_regist_key));
            creds.rp_key.assign(rh->rp_key, rh->rp_key + sizeof(rh->rp_key));
            creds.ps5 = chiaki_target_is_ps5(rh->target);
            self->emit_text_(json{
                {"type", evt::kRegistered},
                {"host", rh->server_nickname},
                {"credentials", EncodeCredentials(creds)},
            }.dump());
            break;
        }
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_FAILED:
            self->EmitError("register_failed", "registration failed — check the PIN");
            break;
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_CANCELED:
            self->EmitError("register_failed", "registration canceled");
            break;
    }
}

// ── Wake ─────────────────────────────────────────────────────────────────────

void SessionBridge::Wake(const std::string& host, const std::string& credentials_b64) {
    Credentials creds;
    if (!DecodeCredentials(credentials_b64, &creds)) {
        EmitError("wake_failed", "bad credentials");
        return;
    }
    // Per discovery.h: "for wakeup, this is just the regist key interpreted as
    // hex" — rp_regist_key is a hex STRING; parse it, don't memcpy it.
    char key_str[CHIAKI_SESSION_AUTH_SIZE + 1] = {0};
    std::memcpy(key_str, creds.regist_key.data(),
                std::min<size_t>(CHIAKI_SESSION_AUTH_SIZE, creds.regist_key.size()));
    uint64_t credential = std::strtoull(key_str, nullptr, 16);
    chiaki_discovery_wakeup(&log_, nullptr, host.c_str(), credential, creds.ps5);
}

// ── Session ──────────────────────────────────────────────────────────────────

void SessionBridge::Connect(const std::string& host, const std::string& credentials_b64,
                            const std::string& resolution, int fps) {
    TeardownSession();
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        video_config_sent_ = false;
    }

    Credentials creds;
    if (!DecodeCredentials(credentials_b64, &creds)) {
        EmitError("session_failed", "bad credentials");
        return;
    }

    ChiakiConnectInfo connect_info;
    std::memset(&connect_info, 0, sizeof(connect_info));
    connect_info.ps5              = creds.ps5;
    connect_info.host             = host.c_str();
    connect_info.enable_keyboard  = false;
    connect_info.enable_dualsense = false;
    connect_info.holepunch_session = nullptr;  // local network session
    connect_info.rudp_sock         = nullptr;
    connect_info.video_profile_auto_downgrade = true;
    connect_info.enable_idr_on_fec_failure    = true;
    connect_info.packet_loss_max  = 0.05;
    std::memcpy(connect_info.regist_key, creds.regist_key.data(),
                std::min(sizeof(connect_info.regist_key), creds.regist_key.size()));
    std::memcpy(connect_info.morning, creds.rp_key.data(),
                std::min(sizeof(connect_info.morning), creds.rp_key.size()));

    // Resolution/fps preset, then force H.264: Chromium's WebCodecs decodes it
    // everywhere; HEVC is platform-conditional. videoConfig echoes this.
    ChiakiVideoResolutionPreset res = (resolution == "720")
        ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p
        : CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
    ChiakiVideoFPSPreset fps_preset =
        (fps <= 30) ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
    chiaki_connect_video_profile_preset(&connect_info.video_profile, res, fps_preset);
    connect_info.video_profile.codec = CHIAKI_CODEC_H264;

    session_ = std::make_unique<ChiakiSession>();
    if (chiaki_session_init(session_.get(), &connect_info, &log_) != CHIAKI_ERR_SUCCESS) {
        EmitError("session_failed", "could not init session");
        session_.reset();
        return;
    }

    chiaki_session_set_event_cb(session_.get(), &SessionBridge::SessionEventCb, this);
    chiaki_session_set_video_sample_cb(session_.get(), &SessionBridge::VideoSampleCb, this);

    // Opus → PCM: chiaki hands the sink Opus packets; the decoder invokes our
    // frame cb with interleaved s16 samples, so the renderer needs no decoder.
    opus_ = std::make_unique<ChiakiOpusDecoder>();
    chiaki_opus_decoder_init(opus_.get(), &log_);
    chiaki_opus_decoder_set_cb(opus_.get(), &SessionBridge::AudioSettingsCb,
                               &SessionBridge::AudioFrameCb, this);
    ChiakiAudioSink sink;
    chiaki_opus_decoder_get_sink(opus_.get(), &sink);
    chiaki_session_set_audio_sink(session_.get(), &sink);

    emit_text_(json{{"type", evt::kSession}, {"state", "connecting"}}.dump());

    if (chiaki_session_start(session_.get()) != CHIAKI_ERR_SUCCESS) {
        EmitError("session_failed", "could not start session");
        TeardownSession();
        return;
    }
    std::lock_guard<std::mutex> lk(state_mtx_);
    session_active_ = true;
}

void SessionBridge::SessionEventCb(ChiakiEvent* event, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    switch (event->type) {
        case CHIAKI_EVENT_CONNECTED:
            self->emit_text_(json{{"type", evt::kSession}, {"state", "streaming"}}.dump());
            break;
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
            self->emit_text_(json{{"type", evt::kLoginPinRequired}}.dump());
            break;
        case CHIAKI_EVENT_QUIT: {
            const char* reason = event->quit.reason_str
                ? event->quit.reason_str
                : chiaki_quit_reason_string(event->quit.reason);
            self->emit_text_(json{
                {"type", evt::kSession}, {"state", "stopped"},
                {"reason", reason ? reason : "quit"}}.dump());
            std::lock_guard<std::mutex> lk(self->state_mtx_);
            self->session_active_ = false;
            break;
        }
        default:
            break;  // rumble/haptics/keyboard etc. — not used for capture
    }
}

// Video: chiaki delivers Annex B access units. Announce codec once (on the
// first keyframe), then forward every unit; flag keyframes via NAL type.
bool SessionBridge::VideoSampleCb(uint8_t* buf, size_t buf_size, int32_t frames_lost,
                                  bool frame_recovered, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    if (!buf || buf_size < 5) return true;
    (void)frames_lost; (void)frame_recovered;  // chiaki handles IDR-on-FEC-failure

    size_t off = (buf[2] == 1) ? 3 : 4;  // 3- or 4-byte start code
    uint8_t nal_type = buf[off] & 0x1F;
    bool keyframe = (nal_type == 7 || nal_type == 5);  // SPS or IDR

    if (!self->video_config_sent_ && keyframe) {
        // Real WxH is in the SPS; the renderer's VideoDecoder reads dimensions
        // from the bitstream, so nominal values are fine here.
        self->emit_text_(json{
            {"type", evt::kVideoConfig},
            {"codec", "avc1.640028"},
            {"width", 1920}, {"height", 1080}, {"fps", 60}}.dump());
        self->video_config_sent_ = true;
    }

    self->emit_binary_(MakeMediaFrame(kKindVideo, keyframe ? kFlagKeyframe : 0,
                                      NowUs(), buf, buf_size));
    return true;
}

void SessionBridge::AudioSettingsCb(uint32_t channels, uint32_t rate, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    self->audio_channels_ = channels;
    self->audio_rate_     = rate;
    self->emit_text_(json{
        {"type", evt::kAudioConfig},
        {"sampleRate", rate},
        {"channels", channels},
        {"format", "s16le"}}.dump());
}

void SessionBridge::AudioFrameCb(int16_t* buf, size_t samples_count, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    // samples_count counts individual s16 samples across channels (interleaved).
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf);
    self->emit_binary_(MakeMediaFrame(kKindAudio, 0, NowUs(), bytes,
                                      samples_count * sizeof(int16_t)));
}

// ── Control ──────────────────────────────────────────────────────────────────

void SessionBridge::Disconnect() {
    TeardownSession();
    emit_text_(json{{"type", evt::kSession}, {"state", "stopped"}, {"reason", "disconnect"}}.dump());
}

void SessionBridge::RequestIdr() {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (session_active_ && session_) {
        chiaki_session_request_idr(session_.get());
    }
}

void SessionBridge::LoginPin(const std::string& pin) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (session_ ) {
        chiaki_session_set_login_pin(session_.get(),
                                     reinterpret_cast<const uint8_t*>(pin.c_str()), pin.size());
    }
}

void SessionBridge::SetControllerState(const InputState& in) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (!session_active_ || !session_) return;
    ChiakiControllerState state;
    chiaki_controller_state_set_idle(&state);  // zeroes buttons + centres sticks
    state.buttons  = in.buttons;
    state.l2_state = in.l2;
    state.r2_state = in.r2;
    state.left_x   = in.left_x;
    state.left_y   = in.left_y;
    state.right_x  = in.right_x;
    state.right_y  = in.right_y;
    // chiaki's feedback sender picks this up and forwards it to the console.
    chiaki_session_set_controller_state(session_.get(), &state);
}

void SessionBridge::TeardownSession() {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (session_) {
        chiaki_session_stop(session_.get());
        chiaki_session_join(session_.get());
        chiaki_session_fini(session_.get());
        session_.reset();
    }
    if (opus_) {
        chiaki_opus_decoder_fini(opus_.get());
        opus_.reset();
    }
    session_active_ = false;
    video_config_sent_ = false;
}

// ── Credentials (opaque blob) ────────────────────────────────────────────────

std::string SessionBridge::EncodeCredentials(const Credentials& c) {
    // Layout: [1 ps5][2 regist_len LE][regist][2 rp_len LE][rp] → base64
    std::vector<uint8_t> raw;
    raw.push_back(c.ps5 ? 1 : 0);
    auto push_len = [&](size_t n) {
        raw.push_back(n & 0xFF);
        raw.push_back((n >> 8) & 0xFF);
    };
    push_len(c.regist_key.size());
    raw.insert(raw.end(), c.regist_key.begin(), c.regist_key.end());
    push_len(c.rp_key.size());
    raw.insert(raw.end(), c.rp_key.begin(), c.rp_key.end());
    return Base64Encode(raw);
}

bool SessionBridge::DecodeCredentials(const std::string& b64, Credentials* out) {
    auto raw = Base64Decode(b64);
    if (raw.size() < 5) return false;
    size_t p = 0;
    out->ps5 = raw[p++] != 0;
    auto read_len = [&]() -> size_t {
        size_t n = raw[p] | (static_cast<size_t>(raw[p + 1]) << 8);
        p += 2;
        return n;
    };
    size_t rl = read_len();
    if (p + rl > raw.size()) return false;
    out->regist_key.assign(raw.begin() + p, raw.begin() + p + rl);
    p += rl;
    if (p + 2 > raw.size()) return false;
    size_t kl = read_len();
    if (p + kl > raw.size()) return false;
    out->rp_key.assign(raw.begin() + p, raw.begin() + p + kl);
    return true;
}

}  // namespace fcrp
