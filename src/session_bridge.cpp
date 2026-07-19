// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session_bridge.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstring>

#include "protocol.h"

// NOTE ON API VERSIONING
// ----------------------
// The chiaki-ng C API is stable in shape but a few struct field names and enum
// spellings have drifted between releases. This file targets the chiaki-ng
// pinned by the git submodule (see .gitmodules). Spots that are most likely to
// need a one-line adjustment against a different tag are marked `// API:`.

using json = nlohmann::json;

namespace fcrp {

namespace {
uint64_t NowUs() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

// Minimal base64 (credentials blobs only — no external dep needed).
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
}  // namespace

SessionBridge::SessionBridge(EmitText emit_text, EmitBinary emit_binary)
    : emit_text_(std::move(emit_text)), emit_binary_(std::move(emit_binary)) {
    // Route chiaki's logs to stderr at a modest level. The app captures stderr.
    chiaki_log_init(&log_, CHIAKI_LOG_WARNING | CHIAKI_LOG_ERROR | CHIAKI_LOG_INFO,
                    chiaki_log_cb_print, nullptr);
}

SessionBridge::~SessionBridge() { TeardownSession(); }

void SessionBridge::EmitError(const std::string& code, const std::string& message) {
    emit_text_(json{{"type", evt::kError}, {"code", code}, {"message", message}}.dump());
}

// ── Discovery ────────────────────────────────────────────────────────────────

void SessionBridge::Discover(int timeout_ms) {
    discovery_ = std::make_unique<ChiakiDiscovery>();
    if (chiaki_discovery_init(discovery_.get(), &log_, AF_INET) != CHIAKI_ERR_SUCCESS) {
        EmitError("discover_failed", "could not init discovery socket");
        discovery_.reset();
        return;
    }

    // One-shot broadcast; hosts arrive via DiscoveryCb. chiaki-ng exposes a
    // thread helper that calls our cb for each responding console.
    ChiakiDiscoveryThread thread;
    if (chiaki_discovery_thread_start(&thread, discovery_.get(), &SessionBridge::DiscoveryCb, this)
        != CHIAKI_ERR_SUCCESS) {
        EmitError("discover_failed", "could not start discovery thread");
        chiaki_discovery_fini(discovery_.get());
        discovery_.reset();
        return;
    }

    ChiakiDiscoveryPacket packet;
    std::memset(&packet, 0, sizeof(packet));
    packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;  // API: search command
    chiaki_discovery_send(discovery_.get(), &packet, nullptr);

    // Give consoles time to respond, then close out. DiscoveryCb aggregates and
    // emits the final list.
    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 3000));
    chiaki_discovery_thread_stop(&thread);
    chiaki_discovery_fini(discovery_.get());
    discovery_.reset();
}

void SessionBridge::DiscoveryCb(ChiakiDiscoveryHost* hosts, size_t count, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    json list = json::array();
    for (size_t i = 0; i < count; ++i) {
        const auto& h = hosts[i];
        const bool ready = h.state == CHIAKI_DISCOVERY_HOST_STATE_READY;
        list.push_back({
            {"host",       h.host_addr ? h.host_addr : ""},
            {"name",       h.host_name ? h.host_name : "PS5"},
            {"hostId",     h.host_id ? h.host_id : ""},
            {"state",      ready ? "ready" : "standby"},
            {"registered", h.host_type != nullptr},  // API: registration hint
        });
    }
    self->emit_text_(json{{"type", evt::kConsoles}, {"list", list}}.dump());
}

// ── Registration (pairing) ───────────────────────────────────────────────────

void SessionBridge::Register(const std::string& host, const std::string& psn_account_id,
                             const std::string& pin) {
    ChiakiRegistInfo info;
    std::memset(&info, 0, sizeof(info));
    info.target      = CHIAKI_TARGET_PS5_1;                 // API: PS5 registration target
    info.host        = host.c_str();
    info.broadcast   = false;
    info.pin         = static_cast<uint32_t>(std::strtoul(pin.c_str(), nullptr, 10));

    // PSN account id: the app passes the base64 account id; chiaki wants the
    // raw 8 bytes. Decode into the fixed-size field.
    auto acct = Base64Decode(psn_account_id);
    if (acct.size() >= CHIAKI_PSN_ACCOUNT_ID_SIZE) {
        std::memcpy(info.psn_account_id, acct.data(), CHIAKI_PSN_ACCOUNT_ID_SIZE);
    } else {
        EmitError("register_failed", "invalid PSN account id");
        return;
    }

    regist_ = std::make_unique<ChiakiRegist>();
    if (chiaki_regist_start(regist_.get(), &log_, &info, &SessionBridge::RegistCb, this)
        != CHIAKI_ERR_SUCCESS) {
        EmitError("register_failed", "could not start registration");
        regist_.reset();
    }
}

void SessionBridge::RegistCb(ChiakiRegistEvent* event, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    switch (event->type) {
        case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS: {
            const ChiakiRegisteredHost* rh = event->registered_host;
            Credentials creds;
            creds.regist_key.assign(rh->rp_regist_key,
                                    rh->rp_regist_key + sizeof(rh->rp_regist_key));
            creds.rp_key.assign(rh->rp_key, rh->rp_key + sizeof(rh->rp_key));
            creds.ps5 = true;
            self->emit_text_(json{
                {"type", evt::kRegistered},
                {"host", rh->server_nickname ? rh->server_nickname : ""},
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
        default:
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
    // The wake "user credential" is derived from the regist key (first 8 bytes,
    // parsed as hex → uint64). chiaki provides a helper for this.
    uint64_t credential = 0;
    std::memcpy(&credential, creds.regist_key.data(),
                std::min<size_t>(sizeof(credential), creds.regist_key.size()));
    chiaki_discovery_wakeup(&log_, nullptr, host.c_str(), credential, creds.ps5);
}

// ── Session ──────────────────────────────────────────────────────────────────

void SessionBridge::Connect(const std::string& host, const std::string& credentials_b64,
                            const std::string& resolution, int fps) {
    {
        std::lock_guard<std::mutex> lk(state_mtx_);
        if (session_active_) TeardownSession();
        video_config_sent_ = false;
    }

    Credentials creds;
    if (!DecodeCredentials(credentials_b64, &creds)) {
        EmitError("session_failed", "bad credentials");
        return;
    }

    ChiakiConnectInfo connect_info;
    std::memset(&connect_info, 0, sizeof(connect_info));
    connect_info.host             = host.c_str();
    connect_info.ps5              = creds.ps5;
    connect_info.enable_keyboard  = false;
    connect_info.enable_dualsense = false;
    std::memcpy(connect_info.regist_key, creds.regist_key.data(),
                std::min(sizeof(connect_info.regist_key), creds.regist_key.size()));
    std::memcpy(connect_info.morning, creds.rp_key.data(),
                std::min(sizeof(connect_info.morning), creds.rp_key.size()));

    // Resolution/fps preset. Force H.264 — Chromium's WebCodecs decodes it
    // everywhere; HEVC is platform-conditional. This is the authoritative
    // codec choice; videoConfig echoes it to the client.
    ChiakiVideoResolutionPreset res = (resolution == "720")
        ? CHIAKI_VIDEO_RESOLUTION_PRESET_720p
        : CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
    ChiakiVideoFPSPreset fps_preset =
        (fps <= 30) ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60;
    chiaki_connect_video_profile_preset(&connect_info.video_profile, res, fps_preset);
    connect_info.video_profile.codec = CHIAKI_CODEC_H264;  // API: force H.264

    session_ = std::make_unique<ChiakiSession>();
    if (chiaki_session_init(session_.get(), &connect_info, &log_) != CHIAKI_ERR_SUCCESS) {
        EmitError("session_failed", "could not init session");
        session_.reset();
        return;
    }

    chiaki_session_set_event_cb(session_.get(), &SessionBridge::SessionEventCb, this);
    chiaki_session_set_video_sample_cb(session_.get(), &SessionBridge::VideoSampleCb, this);

    // Opus → PCM: chiaki hands us Opus; we decode to s16le and forward raw PCM
    // so the renderer needs no audio decoder.
    opus_ = std::make_unique<ChiakiOpusDecoder>();
    chiaki_opus_decoder_init(opus_.get(), &log_);
    ChiakiAudioSink sink;
    chiaki_opus_decoder_get_sink(opus_.get(), &sink);
    chiaki_session_set_audio_sink(session_.get(), &sink);
    chiaki_opus_decoder_set_cb(opus_.get(), &SessionBridge::AudioHeaderCb,
                               &SessionBridge::AudioFrameCb, this);

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
            const char* reason = event->quit.reason_str ? event->quit.reason_str : "quit";
            self->emit_text_(json{
                {"type", evt::kSession}, {"state", "stopped"}, {"reason", reason}}.dump());
            std::lock_guard<std::mutex> lk(self->state_mtx_);
            self->session_active_ = false;
            break;
        }
        default:
            break;
    }
}

// Video: chiaki delivers Annex B access units. On the first frame, announce the
// codec/dimensions; then forward each unit as a media frame (keyframe = SPS/IDR).
bool SessionBridge::VideoSampleCb(uint8_t* buf, size_t buf_size, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    if (!buf || buf_size < 5) return true;

    // NAL type of the first unit after the start code → keyframe if SPS(7)/IDR(5).
    size_t off = (buf[2] == 1) ? 3 : 4;
    uint8_t nal_type = buf[off] & 0x1F;
    bool keyframe = (nal_type == 7 || nal_type == 5);

    if (!self->video_config_sent_ && keyframe) {
        // The concrete WxH is carried in the SPS; the renderer's VideoDecoder
        // reads dimensions from the bitstream, so nominal values are fine here.
        self->emit_text_(json{
            {"type", evt::kVideoConfig},
            {"codec", "avc1.640028"},
            {"width", 1920}, {"height", 1080}, {"fps", 60}}.dump());
        self->video_config_sent_ = true;
    }

    auto frame = MakeMediaFrame(kKindVideo, keyframe ? kFlagKeyframe : 0, NowUs(), buf, buf_size);
    self->emit_binary_(std::move(frame));
    return true;
}

void SessionBridge::AudioHeaderCb(ChiakiAudioHeader* header, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    self->emit_text_(json{
        {"type", evt::kAudioConfig},
        {"sampleRate", kAudioSampleRate},
        {"channels", kAudioChannels},
        {"format", "s16le"}}.dump());
}

void SessionBridge::AudioFrameCb(int16_t* buf, size_t samples_count, void* user) {
    auto* self = static_cast<SessionBridge*>(user);
    // samples_count is per channel; PCM is interleaved stereo s16le.
    const auto* bytes = reinterpret_cast<const uint8_t*>(buf);
    size_t len = samples_count * kAudioChannels * sizeof(int16_t);
    self->emit_binary_(MakeMediaFrame(kKindAudio, 0, NowUs(), bytes, len));
}

// ── Control ──────────────────────────────────────────────────────────────────

void SessionBridge::Disconnect() {
    TeardownSession();
    emit_text_(json{{"type", evt::kSession}, {"state", "stopped"}, {"reason", "disconnect"}}.dump());
}

void SessionBridge::RequestIdr() {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (session_active_ && session_) {
        chiaki_session_request_idr(session_.get());  // API: force keyframe
    }
}

void SessionBridge::LoginPin(const std::string& pin) {
    std::lock_guard<std::mutex> lk(state_mtx_);
    if (session_active_ && session_) {
        chiaki_session_set_login_pin(session_.get(),
                                     reinterpret_cast<const uint8_t*>(pin.c_str()), pin.size());
    }
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
    // Layout: [1 ps5][2 regist_len][regist][2 rp_len][rp] → base64
    std::vector<uint8_t> raw;
    raw.push_back(c.ps5 ? 1 : 0);
    auto push_len = [&](size_t n) { raw.push_back(n & 0xFF); raw.push_back((n >> 8) & 0xFF); };
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
    auto read_len = [&]() -> size_t { size_t n = raw[p] | (raw[p + 1] << 8); p += 2; return n; };
    size_t rl = read_len();
    if (p + rl > raw.size()) return false;
    out->regist_key.assign(raw.begin() + p, raw.begin() + p + rl); p += rl;
    if (p + 2 > raw.size()) return false;
    size_t kl = read_len();
    if (p + kl > raw.size()) return false;
    out->rp_key.assign(raw.begin() + p, raw.begin() + p + kl); p += kl;
    return true;
}

}  // namespace fcrp
