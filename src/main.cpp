// SPDX-License-Identifier: AGPL-3.0-or-later
//
// fc-remoteplay — headless PS5 Remote Play sidecar for FullClutch.
//
//   fc-remoteplay --port <n|0> --token <hex>
//
// On success prints "FCRP LISTENING <port>" to stdout, then serves the
// fc-remoteplay WebSocket protocol v1 (see protocol.h). One client at a time.
//
// This program links the AGPL-licensed chiaki-ng core and is itself AGPL. It
// communicates with the proprietary FullClutch desktop app ONLY over the
// documented local WebSocket protocol — the two are separate programs.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "protocol.h"
#include "session_bridge.h"
#include "ws_server.h"

using json = nlohmann::json;
using namespace fcrp;

namespace {
std::atomic<bool> g_running{true};
void OnSignal(int) { g_running = false; }

std::string ArgVal(int argc, char** argv, const char* name, const char* def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return def ? def : "";
}
}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    uint16_t port  = static_cast<uint16_t>(std::strtoul(ArgVal(argc, argv, "--port", "0").c_str(), nullptr, 10));
    std::string token = ArgVal(argc, argv, "--token", "");
    if (token.empty()) {
        std::fprintf(stderr, "fc-remoteplay: --token is required\n");
        return 2;
    }

    WsServer ws;

    // The bridge emits protocol messages; forward them straight to the socket.
    SessionBridge bridge(
        [&ws](std::string text)          { ws.SendText(std::move(text)); },
        [&ws](std::vector<uint8_t> bin)  { ws.SendBinary(std::move(bin)); });

    // Dispatch inbound JSON control frames to the bridge.
    ws.OnText([&bridge, &ws](const std::string& raw) {
        json msg;
        try { msg = json::parse(raw); } catch (...) { return; }
        const std::string type = msg.value("type", "");

        if (type == cmd::kHello) {
            ws.SendText(json{{"type", evt::kHello},
                             {"protocol", kProtocolVersion},
                             {"version", "fc-remoteplay 1.0.0"}}.dump());
        } else if (type == cmd::kDiscover) {
            std::thread([&bridge, ms = msg.value("timeoutMs", 3000)] {
                bridge.Discover(ms);
            }).detach();
        } else if (type == cmd::kRegister) {
            std::thread([&bridge,
                         host = msg.value("host", ""),
                         acct = msg.value("psnAccountId", ""),
                         pin  = msg.value("pin", "")] {
                bridge.Register(host, acct, pin);
            }).detach();
        } else if (type == cmd::kWake) {
            bridge.Wake(msg.value("host", ""), msg.value("credentials", ""));
        } else if (type == cmd::kConnect) {
            std::thread([&bridge,
                         host  = msg.value("host", ""),
                         creds = msg.value("credentials", ""),
                         res   = msg.value("resolution", "1080"),
                         fps   = msg.value("fps", 60)] {
                bridge.Connect(host, creds, res, fps);
            }).detach();
        } else if (type == cmd::kDisconnect) {
            bridge.Disconnect();
        } else if (type == cmd::kRequestIdr) {
            bridge.RequestIdr();
        } else if (type == cmd::kLoginPin) {
            bridge.LoginPin(msg.value("pin", ""));
        } else if (type == cmd::kInput) {
            // Controller state forwarded from the client's gamepad. The renderer
            // has already mapped to chiaki's button bitmask + analog ranges.
            SessionBridge::InputState in;
            in.buttons = msg.value("buttons", 0u);
            in.l2 = static_cast<uint8_t>(msg.value("l2", 0));
            in.r2 = static_cast<uint8_t>(msg.value("r2", 0));
            in.left_x  = static_cast<int16_t>(msg.value("lx", 0));
            in.left_y  = static_cast<int16_t>(msg.value("ly", 0));
            in.right_x = static_cast<int16_t>(msg.value("rx", 0));
            in.right_y = static_cast<int16_t>(msg.value("ry", 0));
            bridge.SetControllerState(in);
        }
    });

    // If the client vanishes, tear the session down so the console frees the slot.
    ws.OnClientDisconnect([&bridge] { bridge.Disconnect(); });

    uint16_t bound = ws.Start(port, token);
    if (bound == 0) {
        std::fprintf(stderr, "fc-remoteplay: failed to bind WebSocket server\n");
        return 1;
    }

    // Handshake line the app's sidecar manager waits for.
    std::printf("FCRP LISTENING %u\n", bound);
    std::fflush(stdout);

    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ws.Stop();
    return 0;
}
