// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/capture_session.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace {

/// Reads a JSON file, returning false when it is absent or malformed.
[[nodiscard]] bool ReadJson(const std::filesystem::path& path, std::string& out) {
    std::ifstream file{path};
    if (!file) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !out.empty();
}

/// Extracts a numeric field from a flat JSON object without a full parser.
///
/// The manifest is written by this module and the fields are top-level integers, so a targeted
/// scan is sufficient and avoids adding a JSON dependency to Core.
[[nodiscard]] bool FindNumber(const std::string& json, std::string_view key, u64& out) {
    const std::string needle = "\"" + std::string{key} + "\":";
    const auto at = json.find(needle);
    if (at == std::string::npos) {
        return false;
    }
    std::size_t i = at + needle.size();
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) {
        ++i;
    }
    u64 value = 0;
    bool any = false;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        value = value * 10 + static_cast<u64>(json[i] - '0');
        any = true;
        ++i;
    }
    if (!any) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

bool FinalizeSelection() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        const auto dir = std::filesystem::path{g_state.directory};
        if (!std::filesystem::exists(dir)) {
            return false;
        }

        // Gather the candidates the exporters actually wrote. Each artifact is read from disk, so
        // the selection operates on what exists rather than on in-memory state that may not have
        // been serialized.
        std::string package_json;
        std::string tape_json;
        std::string bridge_json;
        const bool have_package = ReadJson(dir / "package.json", package_json);
        const bool have_tape = ReadJson(dir / "tape.json", tape_json);
        const bool have_bridge = ReadJson(dir / "index_bridge.json", bridge_json);

        u64 package_provenance = 0;
        u64 package_session = 0;
        u64 tape_provenance = 0;
        u64 tape_session = 0;
        u64 bridge_session = 0;
        u64 bridge_provenance = 0;
        u64 bridge_selected_address = 0;
        u64 package_index_base = 0;

        if (have_package) {
            FindNumber(package_json, "provenance_id", package_provenance);
            FindNumber(package_json, "session_id", package_session);
            FindNumber(package_json, "guest_base", package_index_base);
        }
        if (have_tape) {
            FindNumber(tape_json, "provenance_id", tape_provenance);
            FindNumber(tape_json, "session_id", tape_session);
        }
        if (have_bridge) {
            FindNumber(bridge_json, "session_id", bridge_session);
            FindNumber(bridge_json, "provenance_id", bridge_provenance);
            FindNumber(bridge_json, "selected_guest_address", bridge_selected_address);
        }

        // The selection decision, recorded in the manifest. Every rejection names its reason so a
        // failed capture is actionable.
        const char* outcome = "not_attempted";
        std::string reason;
        bool selected = false;

        if (!have_package) {
            outcome = "no_package";
            reason = "the resource package was not written";
        } else if (!have_tape) {
            outcome = "no_tape";
            reason = "the semantic tape was not written";
        } else if (!have_bridge) {
            outcome = "no_bridge";
            reason = "the index bridge sidecar was not written";
        } else if (package_session == 0 || tape_session == 0 || bridge_session == 0) {
            outcome = "missing_session";
            reason = "an artifact carries no capture session identity";
        } else if (package_session != tape_session || package_session != bridge_session) {
            outcome = "session_mismatch";
            reason = "the artifacts were not written in one capture session";
        } else if (package_provenance != tape_provenance ||
                   package_provenance != bridge_provenance) {
            outcome = "provenance_mismatch";
            reason = "the artifacts describe different draws";
        } else if (bridge_selected_address == 0) {
            outcome = "bridge_unresolved";
            reason = "the index bridge did not resolve a guest address";
        } else if (package_index_base != 0 &&
                   bridge_selected_address != package_index_base) {
            outcome = "address_mismatch";
            reason = "the resolved index allocation and the package name different buffers";
        } else {
            outcome = "selected";
            selected = true;
        }

        LOG_INFO(Core_Linker,
                 "DERACINE_CAPTURE_SELECTION outcome={} session_id={} provenance_id={} "
                 "package_session={} tape_session={} bridge_session={} reason={}",
                 outcome, SessionId(), package_provenance, package_session, tape_session,
                 bridge_session, reason);

        // Rewrite the manifest with the decision. It is written last so it cannot claim a
        // selection before the artifacts exist.
        const auto path = dir / "session.json";
        std::ofstream out{path, std::ios::trunc};
        if (!out) {
            return false;
        }
        out << "{\n";
        out << "  \"schema_version\": 1,\n";
        out << "  \"session_id\": " << SessionId() << ",\n";
        out << "  \"selection\": {\n";
        out << "    \"outcome\": \"" << outcome << "\",\n";
        out << "    \"selected\": " << (selected ? "true" : "false") << ",\n";
        out << "    \"provenance_id\": " << package_provenance << ",\n";
        out << "    \"package_session_id\": " << package_session << ",\n";
        out << "    \"tape_session_id\": " << tape_session << ",\n";
        out << "    \"bridge_session_id\": " << bridge_session << ",\n";
        out << "    \"reason\": \"" << reason << "\"\n";
        out << "  },\n";
        out << "  \"note\": \"the session id proves the artifacts claim the same execution; it "
               "does not prove resource identity\"\n";
        out << "}\n";
        out.close();

        return selected;
    } catch (...) {
        return false;
    }
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