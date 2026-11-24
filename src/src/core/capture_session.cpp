// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/capture_session.h"

#include <nlohmann/json.hpp>

#include "core/capture_artifacts.h"

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

bool FinalizeSelection() noexcept {
    try {
        if (!g_state.enabled.load(std::memory_order_relaxed)) {
            return false;
        }
        const auto dir = std::filesystem::path{g_state.directory};
        if (!std::filesystem::exists(dir)) {
            return false;
        }

        // Read the artifacts structurally. The package index address is read from its own object,
        // never by searching the file for the first "guest_base", because the package writes
        // vertex_bindings before index_binding and both carry that field name.
        CaptureArtifacts::PackageMetadata package{};
        CaptureArtifacts::FieldProblem package_problem{};
        std::string package_error;
        const bool have_package = CaptureArtifacts::ReadPackageMetadata(
            (dir / "package.json").string(), package, package_problem, package_error);

        std::vector<CaptureArtifacts::BridgeRecord> bridge_records;
        CaptureArtifacts::FieldProblem bridge_problem{};
        std::string bridge_error;
        const bool have_bridge = CaptureArtifacts::ReadBridgeRecords(
            (dir / "index_bridge.json").string(), bridge_records, bridge_problem, bridge_error);

        // The tape is read for its provenance and session, so the manifest can report what was
        // actually exported rather than what was intended.
        u64 tape_provenance = 0;
        u64 tape_session = 0;
        {
            std::ifstream tape_file{dir / "tape.json"};
            if (tape_file) {
                try {
                    nlohmann::json tape_json;
                    tape_file >> tape_json;
                    tape_provenance = tape_json.value("provenance_id", 0ull);
                    tape_session = tape_json.value("session_id", 0ull);
                } catch (const std::exception&) {
                    // A malformed tape is reported through the zero values below.
                }
            }
        }

        const u64 session_id = SessionId();
        const char* outcome = "insufficient_evidence";
        const char* verification = "not_attempted";
        std::string reason;
        bool candidate_selected = false;
        bool identity_verified = false;

        u64 bridge_provenance = 0;
        u64 bridge_object = 0;
        u64 bridge_address = 0;
        u64 package_index_address = 0;

        if (!have_package) {
            outcome = "rejected";
            reason = "package: " + std::string{CaptureArtifacts::FieldProblemName(package_problem)} +
                     " (" + package_error + ")";
        } else if (!package.index.present) {
            outcome = "insufficient_evidence";
            reason = "the package carries no index binding";
        } else if (!package.index.has_guest_base) {
            outcome = "insufficient_evidence";
            reason = "the package index binding carries no guest address";
        } else if (!have_bridge) {
            outcome = "insufficient_evidence";
            reason = "bridge: " + std::string{CaptureArtifacts::FieldProblemName(bridge_problem)} +
                     " (" + bridge_error + ")";
        } else if (!package.has_session_id || package.session_id == 0) {
            outcome = "insufficient_evidence";
            reason = "the package carries no capture session identity";
        } else if (package.session_id != session_id) {
            outcome = "rejected";
            reason = "the package was written in a different capture session";
        } else if (tape_session == 0) {
            outcome = "insufficient_evidence";
            reason = "the tape carries no capture session identity";
        } else if (tape_session != session_id) {
            outcome = "rejected";
            reason = "the tape was written in a different capture session";
        } else if (tape_provenance != package.provenance_id) {
            outcome = "rejected";
            reason = "the tape describes a different draw than the package";
        } else {
            // The candidate is the package-selected provenance. The bridge record is selected by
            // session and provenance, not by position in the array.
            CaptureArtifacts::BridgeRecord bridge{};
            bool ambiguous = false;
            std::string select_error;
            if (!CaptureArtifacts::SelectBridgeRecord(bridge_records, session_id,
                                                      package.provenance_id, bridge, ambiguous,
                                                      select_error)) {
                outcome = ambiguous ? "ambiguous" : "insufficient_evidence";
                reason = select_error;
            } else {
                candidate_selected = true;
                bridge_provenance = bridge.provenance_id;
                bridge_object = bridge.original_rhi_object;
                bridge_address = bridge.selected_guest_address;
                package_index_address = package.index.guest_base;

                if (!bridge.has_selected_guest_address || bridge.selected_guest_address == 0) {
                    outcome = "insufficient_evidence";
                    verification = "insufficient_evidence";
                    reason = "the bridge did not resolve a guest address";
                } else if (!bridge.predicate_determinable) {
                    outcome = "insufficient_evidence";
                    verification = "insufficient_evidence";
                    reason = "the address-slot predicate was not determinable";
                } else if (package.index.guest_base == 0) {
                    // A zero index address is not comparable. Accepting it would let a missing or
                    // unparsed address pass as a match.
                    outcome = "insufficient_evidence";
                    verification = "insufficient_evidence";
                    reason = "the package index address is zero, so it cannot be compared";
                } else if (bridge.selected_guest_address != package.index.guest_base) {
                    outcome = "rejected";
                    verification = "rejected";
                    reason = "the resolved index allocation and the package name different "
                             "buffers";
                } else {
                    outcome = "candidate_selected";
                    verification = "identity_verified";
                    identity_verified = true;
                }
            }
        }

        LOG_INFO(Core_Linker,
                 "DERACINE_CAPTURE_SELECTION outcome={} verification={} session_id={} "
                 "package_provenance={} tape_provenance={} bridge_provenance={} "
                 "package_index_address={:#x} bridge_address={:#x} bridge_object={:#x} "
                 "reason={}",
                 outcome, verification, session_id, package.provenance_id, tape_provenance,
                 bridge_provenance, package_index_address, bridge_address, bridge_object, reason);

        // The manifest records selection and verification separately. It never reports success
        // merely because the artifacts share a provenance ordinal.
        const auto path = dir / "session.json";
        std::ofstream out{path, std::ios::trunc};
        if (!out) {
            return false;
        }
        out << "{\n";
        out << "  \"schema_version\": 2,\n";
        out << "  \"session_id\": " << session_id << ",\n";
        out << "  \"selection\": {\n";
        out << "    \"outcome\": \"" << outcome << "\",\n";
        out << "    \"candidate_selected\": " << (candidate_selected ? "true" : "false")
            << ",\n";
        out << "    \"identity_verified\": " << (identity_verified ? "true" : "false")
            << ",\n";
        out << "    \"verification\": \"" << verification << "\",\n";
        out << "    \"package_provenance_id\": " << package.provenance_id << ",\n";
        out << "    \"tape_provenance_id\": " << tape_provenance << ",\n";
        out << "    \"bridge_provenance_id\": " << bridge_provenance << ",\n";
        out << "    \"original_rhi_object\": " << bridge_object << ",\n";
        out << "    \"package_index_address\": " << package_index_address << ",\n";
        out << "    \"bridge_selected_address\": " << bridge_address << ",\n";
        out << "    \"package_schema_version\": " << package.schema_version << ",\n";
        out << "    \"reason\": \"" << reason << "\"\n";
        out << "  },\n";
        out << "  \"note\": \"the session id proves the artifacts claim the same execution; it "
               "does not prove resource identity\"\n";
        out << "}\n";
        out.close();

        return identity_verified;
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