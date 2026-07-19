// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fc-remoteplay protocol v1 — shared wire format between the sidecar and the
// FullClutch desktop renderer. This MUST stay byte-compatible with the JS
// client at fullclutch-desktop/src/renderer/remoteplay/RemotePlayClient.js.
//
// Transport: WebSocket on ws://127.0.0.1:<port>/session?token=<hex>
//   - JSON text frames  → control (see message `type` strings below)
//   - Binary frames      → media (layout below)
//
// Binary media frame (little-endian):
//   byte 0     : kind   (0x01 video, 0x02 audio)
//   byte 1     : flags  (bit0 = keyframe, video only)
//   bytes 2..9 : pts    (uint64, microseconds)
//   bytes 10.. : payload (Annex B H.264 NAL units, or interleaved s16le PCM)
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace fcrp {

constexpr int      kProtocolVersion = 1;
constexpr uint8_t  kKindVideo       = 0x01;
constexpr uint8_t  kKindAudio       = 0x02;
constexpr uint8_t  kFlagKeyframe    = 0x01;
constexpr size_t   kMediaHeaderLen  = 10;

// Audio is always delivered decoded as s16le / 48 kHz / stereo in v1.
constexpr uint32_t kAudioSampleRate = 48000;
constexpr uint8_t  kAudioChannels   = 2;

// Build a binary media frame: [kind][flags][pts_u64_le][payload...]
inline std::vector<uint8_t> MakeMediaFrame(uint8_t kind, uint8_t flags,
                                           uint64_t pts_us,
                                           const uint8_t* payload, size_t len) {
    std::vector<uint8_t> out(kMediaHeaderLen + len);
    out[0] = kind;
    out[1] = flags;
    for (int i = 0; i < 8; ++i) out[2 + i] = static_cast<uint8_t>((pts_us >> (8 * i)) & 0xFF);
    if (len && payload) std::copy(payload, payload + len, out.begin() + kMediaHeaderLen);
    return out;
}

// Control message `type` strings (client → sidecar)
namespace cmd {
constexpr const char* kHello       = "hello";
constexpr const char* kDiscover    = "discover";
constexpr const char* kRegister    = "register";
constexpr const char* kWake        = "wake";
constexpr const char* kConnect     = "connect";
constexpr const char* kDisconnect  = "disconnect";
constexpr const char* kRequestIdr  = "requestIdr";
constexpr const char* kLoginPin    = "loginPin";
}  // namespace cmd

// Control message `type` strings (sidecar → client)
namespace evt {
constexpr const char* kHello            = "hello";
constexpr const char* kConsoles         = "consoles";
constexpr const char* kRegistered       = "registered";
constexpr const char* kSession          = "session";
constexpr const char* kVideoConfig      = "videoConfig";
constexpr const char* kAudioConfig      = "audioConfig";
constexpr const char* kLoginPinRequired = "loginPinRequired";
constexpr const char* kError            = "error";
}  // namespace evt

}  // namespace fcrp
