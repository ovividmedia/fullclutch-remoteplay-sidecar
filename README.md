# fc-remoteplay

Headless PS5 **Remote Play** sidecar for [FullClutch](https://fullclutch.io) — lets the
FullClutch desktop app capture a PS5 over the network with **no capture card**,
by speaking Sony's Remote Play protocol via the open-source
[chiaki-ng](https://github.com/streetpea/chiaki-ng) core.

It is a small standalone program: it connects to the console, decodes nothing
itself except audio, and streams the console's H.264 video + PCM audio to the
FullClutch renderer over a **local WebSocket**. The renderer decodes with
WebCodecs and turns it into an ordinary `MediaStream`.

## Why this is a separate repository

chiaki-ng is licensed **AGPL-3.0**, so anything that links it must also be
AGPL-3.0 and have its source published. `fc-remoteplay` is therefore:

- a **separate process** from the proprietary FullClutch desktop app;
- kept in **its own public repository** (this one), not inside the FullClutch
  monorepo;
- communicating with the app **only** over the documented WebSocket protocol
  below — never by linking.

The FullClutch app is a separate program that talks to this one over a socket;
it is not a derivative work. This repo (and any modifications you make) must
stay publicly available per AGPL §13. Ship a copy of `LICENSE` and a link to
this repo in the FullClutch app's About / licenses screen.

## Protocol v1

Transport: WebSocket at `ws://127.0.0.1:<port>/session?token=<hex>` — localhost
only, one client at a time. The sidecar is launched as:

```
fc-remoteplay --port <n|0> --token <hex>
```

and prints `FCRP LISTENING <port>` to **stdout** once the server is up (port
`0` = OS-assigned ephemeral port). The app reads that line to learn the port.

### Control — JSON text frames

Client → sidecar:

| type         | payload                                                        |
|--------------|----------------------------------------------------------------|
| `hello`      | `{ protocol: 1 }`                                               |
| `discover`   | `{ timeoutMs? }`                                                |
| `register`   | `{ host, psnAccountId, pin }` — `pin` is the 8-digit PS5 code   |
| `wake`       | `{ host, credentials }`                                         |
| `connect`    | `{ host, credentials, codec:'h264', resolution:'1080'|'720', fps:60|30 }` |
| `disconnect` | `{}`                                                            |
| `requestIdr` | `{}` — ask the console for a keyframe after a decode error      |
| `loginPin`   | `{ pin }` — reply to `loginPinRequired`                         |

Sidecar → client:

| type               | payload                                                              |
|--------------------|----------------------------------------------------------------------|
| `hello`            | `{ protocol: 1, version }`                                            |
| `consoles`         | `{ list: [{ host, name, hostId, state:'ready'|'standby', registered }] }` |
| `registered`       | `{ host, credentials }` — **opaque** blob; the app persists it        |
| `session`          | `{ state:'connecting'|'streaming'|'stopped', reason? }`               |
| `videoConfig`      | `{ codec:'avc1.640028', width, height, fps }`                         |
| `audioConfig`      | `{ sampleRate:48000, channels:2, format:'s16le' }`                    |
| `loginPinRequired` | `{}`                                                                  |
| `error`            | `{ code, message }`                                                   |

`credentials` is opaque to the app — the sidecar stays **stateless** and the
app hands the blob back on `wake`/`connect`.

### Media — binary frames (little-endian)

```
byte 0      kind   : 0x01 video | 0x02 audio
byte 1      flags  : bit0 = keyframe (video only)
bytes 2..9  pts    : uint64, microseconds
bytes 10..  payload: Annex B H.264 NAL units | interleaved s16le PCM
```

Video is always **H.264** (forced on the console — HEVC decode in Chromium is
platform-conditional). Audio is decoded from Opus to PCM here so the renderer
needs no audio decoder.

> This is the exact contract the mock sidecar in the FullClutch repo implements
> (`fullclutch-desktop/tools/mock-remoteplay-sidecar`). Test the whole renderer
> pipeline against the mock before wiring in the real one.

## Building

### Prerequisites

- CMake ≥ 3.20, a C++17 compiler
- chiaki-ng's build deps: OpenSSL, Opus, protobuf (nanopb is vendored by chiaki)
- Windows: Visual Studio 2022 + [vcpkg]; macOS: Xcode CLT + Homebrew

```bash
git clone https://github.com/ovividmedia/fullclutch-remoteplay-sidecar.git
cd fullclutch-remoteplay-sidecar
git submodule update --init --recursive     # pulls chiaki-ng

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
# → build/fc-remoteplay (or build/Release/fc-remoteplay.exe on Windows)
```

CI in `.github/workflows/build.yml` produces `win-x64` and `mac-universal`
artifacts.

### API version note

The chiaki-ng C API is stable in shape but a few struct field names / enum
spellings drift between tags. Spots most likely to need a one-line tweak
against a different submodule tag are marked `// API:` in
[`src/session_bridge.cpp`](src/session_bridge.cpp) and the `chiaki-lib` target
name in [`CMakeLists.txt`](CMakeLists.txt). Pin the submodule to a known-good
chiaki-ng release and adjust those spots once against it.

## Shipping it inside FullClutch

1. Build for each platform → drop binaries into the FullClutch app's
   `resources/fc-remoteplay/` via electron-builder `extraResources`:

   ```yaml
   extraResources:
     - from: ../fullclutch-remoteplay-sidecar/dist/${os}
       to: fc-remoteplay
   ```

2. macOS: the binary **must be codesigned + notarized** or Gatekeeper kills the
   spawned process. Sign it in CI (see the commented step in the workflow).
3. Flip **REMOTE PLAY NATIVE** on in the FullClutch admin dashboard. Every
   installed app switches to native mode on next launch — no app update needed.

## License

AGPL-3.0-or-later. See [`LICENSE`](LICENSE).
