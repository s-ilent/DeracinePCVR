// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/capture_session.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "common/logging/log.h"
#include "common/sha256.h"

namespace Core::CaptureSession {
namespace {

struct SessionState {
    std::atomic<bool> enabled{};
    std::atomic<u64> session_id{};
    std::string directory;
};

SessionState g_state{};

/// Builds a session identity from a process-specific nonce and the clock.
///
/// A wall-clock timestamp alone is not unique: two runs can start within the same tick, and a
/// clock can move backwards. The nonce is drawn from the platform entropy source, so two processes
/// cannot produce the same identity even if they start in the same nanosecond.
[[nodiscard]] u64 MakeSessionId() noexcept {
    u64 nonce = 0;
    try {
        std::random_device device;
        nonce = (static_cast<u64>(device()) << 32) ^ static_cast<u64>(device());
    } catch (...) {
        // A platform without an entropy source still gets a clock-derived value; the nonce is a
        // collision reducer, not a security primitive.
        nonce = 0;
    }

    const u64 clock = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    // Mix so neither input alone determines the result.
    u64 mixed = clock ^ (nonce + 0x9e3779b97f4a7c15ull + (clock << 6) + (clock >> 2));
    // A zero identity is reserved for "no session", so it must never be produced.
    if (mixed == 0) {
        mixed = 1;
    }
    return mixed;
}

} // namespace

bool IsEnabled() noexcept {
    return g_state.enabled.load(std::memory_order_relaxed);
}

u64 SessionId() noexcept {
    return g_state.session_id.load(std::memory_order_relaxed);
}

void TryInstall() noexcept {
    const char* value = std::getenv(EnableEnvironmentVariable.data());
    if (value == nullptr || std::string_view{value} != "1") {
        return;
    }

    const char* directory = std::getenv(DirectoryEnvironmentVariable.data());
    if (directory == nullptr || *directory == '\0') {
        LOG_WARNING(Core_Linker, "Capture session refused: {} is not set",
                    DirectoryEnvironmentVariable);
        return;
    }

    g_state.directory = directory;
    g_state.session_id.store(MakeSessionId(), std::memory_order_relaxed);
    g_state.enabled.store(true, std::memory_order_relaxed);

    LOG_INFO(Core_Linker, "DERACINE_CAPTURE_SESSION_INSTALL session_id={} directory={}",
             SessionId(), g_state.directory);
}

void ExportManifest() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return;
        }
        const auto dir = std::filesystem::path{g_state.directory};
        if (!std::filesystem::exists(dir)) {
            LOG_WARNING(Core_Linker, "Capture session manifest refused: directory missing {}",
                        dir.string());
            return;
        }

        const auto path = dir / "session.json";
        std::ofstream out{path, std::ios::trunc};
        if (!out) {
            LOG_WARNING(Core_Linker, "Capture session manifest refused: cannot open {}",
                        path.string());
            return;
        }
        out << "{\n";
        out << "  \"schema_version\": 1,\n";
        out << "  \"session_id\": " << SessionId() << ",\n";
        out << "  \"note\": \"the session id proves the artifacts claim the same execution; it "
               "does not prove resource identity\"\n";
        out << "}\n";
        out.close();

        LOG_INFO(Core_Linker, "DERACINE_CAPTURE_SESSION_MANIFEST session_id={} path={}",
                 SessionId(), path.string());
    } catch (...) {
        // Export must never throw during shutdown.
    }
}

} // namespace Core::CaptureSession
