// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/semantic_tape.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/logging/log.h"
#include "common/sha256.h"
#include "core/draw_package.h"
#include "core/module.h"
#include "core/rhi_command_capture.h"
#include "core/rhi_command_capture_contract.h"
#include "core/rhi_entry_probe.h"
#include "core/capture_session.h"
#include "core/semantic_tape_contract.h"

namespace Core::SemanticTape {
namespace {

struct TapeState {
    bool enabled{};
    std::string output_directory;
    /// Explicit draw selection, or 0 to select automatically.
    u64 selected_provenance_id{};
    std::string image_sha256;
    u64 module_base{};
};

TapeState g_state{};

[[nodiscard]] const char* OpKindName(OpKind kind) noexcept {
    switch (kind) {
    case OpKind::BeginFrame:
        return "BeginFrame";
    case OpKind::SetViewport:
        return "SetViewport";
    case OpKind::SetRenderTargets:
        return "SetRenderTargets";
    case OpKind::BindVertexBuffers:
        return "BindVertexBuffers";
    case OpKind::BindIndexBuffer:
        return "BindIndexBuffer";
    case OpKind::SetGraphicsPipeline:
        return "SetGraphicsPipeline";
    case OpKind::BindShaderResources:
        return "BindShaderResources";
    case OpKind::DrawIndexed:
        return "DrawIndexed";
    case OpKind::EndFrame:
        return "EndFrame";
    case OpKind::Opaque:
        return "Opaque";
    default:
        return "Unknown";
    }
}

[[nodiscard]] const char* EvidenceName(Evidence evidence) noexcept {
    switch (evidence) {
    case Evidence::VerifiedTargetBinary:
        return "verified_target_binary";
    case Evidence::PublicSourceCorrespondence:
        return "public_source_correspondence";
    case Evidence::RuntimeConfirmed:
        return "runtime_confirmed";
    case Evidence::Tentative:
        return "tentative";
    default:
        return "unknown";
    }
}

[[nodiscard]] const char* ResourceKindName(ResourceKind kind) noexcept {
    switch (kind) {
    case ResourceKind::IndexBuffer:
        return "index_buffer";
    case ResourceKind::VertexBuffer:
        return "vertex_buffer";
    case ResourceKind::RenderTarget:
        return "render_target";
    case ResourceKind::DepthTarget:
        return "depth_target";
    case ResourceKind::ShaderResource:
        return "shader_resource";
    default:
        return "unknown";
    }
}

[[nodiscard]] const char* ResourceProvenanceName(ResourceProvenance provenance) noexcept {
    switch (provenance) {
    case ResourceProvenance::Ue4Object:
        return "ue4_object";
    case ResourceProvenance::DownstreamOracle:
        return "downstream_oracle";
    default:
        return "unresolved";
    }
}

/// The UE4 EPrimitiveType the command payload carries. A4 established the payload offset; the value
/// is recorded verbatim and only interpreted where the conversion is established.
[[nodiscard]] u32 PayloadPrimitiveType(const RhiCommandCapture::CapturedCommandView& command) noexcept {
    return static_cast<u32>(command.payload[1] & 0xFFFFFFFFull);
}

[[nodiscard]] s32 PayloadBaseVertex(const RhiCommandCapture::CapturedCommandView& command) noexcept {
    return static_cast<s32>(command.payload[2] & 0xFFFFFFFFull);
}

[[nodiscard]] u32 PayloadFirstInstance(const RhiCommandCapture::CapturedCommandView& command) noexcept {
    return static_cast<u32>(command.payload[3] & 0xFFFFFFFFull);
}

[[nodiscard]] u32 PayloadNumVertices(const RhiCommandCapture::CapturedCommandView& command) noexcept {
    return static_cast<u32>(command.payload[4] & 0xFFFFFFFFull);
}

[[nodiscard]] u32 PayloadStartIndex(const RhiCommandCapture::CapturedCommandView& command) noexcept {
    return static_cast<u32>(command.payload[5] & 0xFFFFFFFFull);
}

[[nodiscard]] u32 PayloadNumPrimitives(const RhiCommandCapture::CapturedCommandView& command) noexcept {
    return static_cast<u32>(command.payload[6] & 0xFFFFFFFFull);
}

[[nodiscard]] u32 PayloadNumInstances(const RhiCommandCapture::CapturedCommandView& command) noexcept {
    return static_cast<u32>(command.payload[7] & 0xFFFFFFFFull);
}

/// Reads the viewport payload. A4 established the six fields and their order from the Execute
/// wrapper: MinX, MinY, MinZ (float), MaxX, MaxY, MaxZ (float).
[[nodiscard]] float PayloadFloat(const RhiCommandCapture::CapturedCommandView& command,
                                 u32 index) noexcept {
    const u32 bits = static_cast<u32>(command.payload[index] & 0xFFFFFFFFull);
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] float PayloadIntAsFloat(const RhiCommandCapture::CapturedCommandView& command,
                                      u32 index) noexcept {
    const s32 value = static_cast<s32>(command.payload[index] & 0xFFFFFFFFull);
    return static_cast<float>(value);
}

/// The index element width, taken from the draw package when the package describes the same draw.
/// A tape that cannot establish the width leaves the derived byte offset invalid rather than
/// guessing.
[[nodiscard]] u32 IndexElementSizeFromPackage(const DrawPackage::Package& package,
                                              bool have_package) noexcept {
    if (!have_package || !package.has_index_binding)
        return 0;
    return package.index_binding.index_size;
}

/// Builds the tape for one provenance record.
///
/// The construction itself lives in the contract header as BuildTapeFromCapture, so the live exporter
/// and the tests run the same code. This function only gathers the capture records and the
/// provenance facts into the plain values that builder takes.
[[nodiscard]] bool BuildTape(Tape& tape, const RhiEntryProbe::ProvenanceSnapshot& snapshot,
                             const DrawPackage::Package& package, bool have_package,
                             std::string& error) {
    const u32 captured = RhiCommandCapture::CapturedCommandCount();
    if (captured == 0) {
        error = "the command capture is not installed, so no command sequence is available";
        return false;
    }

    std::vector<CapturedCommandRecord> records;
    records.reserve(captured);
    for (u32 i = 0; i < captured; ++i) {
        RhiCommandCapture::CapturedCommandView command{};
        if (!RhiCommandCapture::GetCapturedCommand(i, command))
            break;
        CapturedCommandRecord record{};
        record.sequence = command.sequence;
        record.command_id = command.command_id;
        record.command_list = command.list_pointer;
        record.rhi_context = command.context_pointer;
        for (size_t w = 0; w < 8; ++w) {
            record.payload[w] = command.payload[w];
            record.render_targets[w] = command.render_targets[w];
        }
        record.depth_target = command.depth_target;
        record.render_target_count = command.render_target_count;
        records.push_back(record);
    }

    // The draw package, when it describes this same draw, is the bridge from command provenance to
    // resource provenance. The association is recorded, not assumed.
    const bool package_matches = have_package && package.provenance_id == snapshot.id;

    TapeSource source{};
    source.provenance_id = snapshot.id;
    source.command_list = snapshot.command_list;
    source.index_resource = snapshot.rhi_index_resource;
    source.ue4_primitive_type = snapshot.ue4_primitive_type;
    source.rhi_context = snapshot.rhi_context;
    source.submission_id = snapshot.submission_id;
    source.packet_offset = snapshot.packet_offset;
    source.packet_offset_known = snapshot.packet_offset_known;
    source.viewport_valid = snapshot.viewport.valid;
    source.viewport_min_x = snapshot.viewport.min_x;
    source.viewport_min_y = snapshot.viewport.min_y;
    source.viewport_min_z = snapshot.viewport.min_z;
    source.viewport_max_x = snapshot.viewport.max_x;
    source.viewport_max_y = snapshot.viewport.max_y;
    source.viewport_max_z = snapshot.viewport.max_z;
    source.index_element_size = IndexElementSizeFromPackage(package, package_matches);
    if (package_matches && package.has_index_binding) {
        source.package_index_resource_ref = package.index_binding.resource_ref;
    }

    const BuildOutcome outcome = BuildTapeFromCapture(records, source, tape);
    if (outcome != BuildOutcome::Ok) {
        error = std::string{"the tape could not be built: "} + std::string{BuildOutcomeName(outcome)};
        return false;
    }

    tape.scene = "classroom-title";
    // The one capture session identity, so the tape can be shown to come from the same execution as
    // the package and the index bridge.
    tape.session_id = CaptureSession::SessionId();
    tape.image_sha256 = g_state.image_sha256;
    tape.module_base = g_state.module_base;
    error.clear();
    return true;
}

[[nodiscard]] std::string EscapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }
    return out;
}

[[nodiscard]] bool WriteTape(const Tape& tape, const std::string& directory,
                             std::string& error) {
    const auto dir = std::filesystem::path{directory};
    if (!std::filesystem::exists(dir)) {
        error = "output directory does not exist";
        return false;
    }
    const auto path = dir / "tape.json";
    std::ofstream out{path, std::ios::trunc};
    if (!out) {
        error = "cannot open the tape for writing";
        return false;
    }

    out << "{\n";
    out << "  \"schema_version\": " << tape.schema_version << ",\n";
    out << "  \"scene\": \"" << EscapeJson(tape.scene) << "\",\n";
    out << "  \"provenance_id\": " << tape.provenance_id << ",\n";
    out << "  \"session_id\": " << tape.session_id << ",\n";
    out << "  \"image_sha256\": \"" << EscapeJson(tape.image_sha256) << "\",\n";
    out << "  \"module_base\": " << tape.module_base << ",\n";
    out << "  \"submission_id\": " << tape.submission_id << ",\n";
    out << "  \"packet_offset\": " << tape.packet_offset << ",\n";
    out << "  \"packet_offset_known\": " << (tape.packet_offset_known ? "true" : "false")
        << ",\n";

    out << "  \"resources\": [\n";
    for (u32 i = 0; i < tape.resource_count; ++i) {
        const auto& resource = tape.resources[i];
        out << "    {\n";
        out << "      \"id\": " << resource.id << ",\n";
        out << "      \"kind\": \"" << ResourceKindName(resource.kind) << "\",\n";
        out << "      \"provenance\": \"" << ResourceProvenanceName(resource.provenance)
            << "\",\n";
        out << "      \"package_resource_ref\": " << resource.package_resource_ref << ",\n";
        out << "      \"guest_base\": " << resource.guest_base << ",\n";
        out << "      \"guest_size\": " << resource.guest_size << ",\n";
        out << "      \"element_size\": " << resource.element_size << "\n";
        out << "    }" << (i + 1 < tape.resource_count ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"operations\": [\n";
    for (u32 i = 0; i < tape.operation_count; ++i) {
        const auto& operation = tape.operations[i];
        out << "    {\n";
        out << "      \"kind\": \"" << OpKindName(operation.kind) << "\",\n";
        out << "      \"evidence\": \"" << EvidenceName(operation.evidence) << "\",\n";
        out << "      \"sequence\": " << operation.sequence << ",\n";
        out << "      \"command_capture_id\": " << operation.command_capture_id << ",\n";
        out << "      \"command_list\": " << operation.command_list << ",\n";
        out << "      \"rhi_context\": " << operation.rhi_context;
        if (operation.kind == OpKind::SetViewport) {
            out << ",\n      \"viewport\": {\n";
            out << "        \"min_x\": " << operation.viewport_min_x << ",\n";
            out << "        \"min_y\": " << operation.viewport_min_y << ",\n";
            out << "        \"min_z\": " << operation.viewport_min_z << ",\n";
            out << "        \"max_x\": " << operation.viewport_max_x << ",\n";
            out << "        \"max_y\": " << operation.viewport_max_y << ",\n";
            out << "        \"max_z\": " << operation.viewport_max_z << "\n";
            out << "      }";
        }
        if (operation.kind == OpKind::SetRenderTargets) {
            out << ",\n      \"color_target_count\": " << operation.color_target_count << ",\n";
            out << "      \"color_targets\": [";
            for (u32 t = 0; t < operation.color_target_count && t < 8; ++t) {
                out << operation.color_targets[t] << (t + 1 < operation.color_target_count ? ", " : "");
            }
            out << "],\n";
            out << "      \"depth_target\": " << operation.depth_target;
        }
        if (operation.kind == OpKind::BindVertexBuffers ||
            operation.kind == OpKind::BindIndexBuffer) {
            out << ",\n      \"slot\": " << operation.slot << ",\n";
            out << "      \"resource\": " << operation.resource;
        }
        if (operation.kind == OpKind::DrawIndexed) {
            out << ",\n      \"draw\": {\n";
            out << "        \"index_buffer_resource\": " << operation.draw.index_buffer_resource
                << ",\n";
            out << "        \"primitive_type\": " << operation.draw.primitive_type << ",\n";
            out << "        \"base_vertex_index\": " << operation.draw.base_vertex_index << ",\n";
            out << "        \"first_instance\": " << operation.draw.first_instance << ",\n";
            out << "        \"num_vertices\": " << operation.draw.num_vertices << ",\n";
            out << "        \"start_index\": " << operation.draw.start_index << ",\n";
            out << "        \"num_primitives\": " << operation.draw.num_primitives << ",\n";
            out << "        \"num_instances\": " << operation.draw.num_instances << "\n";
            out << "      },\n";
            out << "      \"derived\": {\n";
            out << "        \"valid\": " << (operation.derived.valid ? "true" : "false") << ",\n";
            out << "        \"index_count\": " << operation.derived.index_count << ",\n";
            out << "        \"first_index\": " << operation.derived.first_index << ",\n";
            out << "        \"vertex_offset\": " << operation.derived.vertex_offset << ",\n";
            out << "        \"instance_count\": " << operation.derived.instance_count << ",\n";
            out << "        \"index_byte_offset\": " << operation.derived.index_byte_offset
                << "\n";
            out << "      }";
        }
        if (!operation.note.empty()) {
            out << ",\n      \"note\": \"" << EscapeJson(operation.note) << "\"";
        }
        out << "\n    }" << (i + 1 < tape.operation_count ? "," : "") << "\n";
    }
    out << "  ],\n";

    out << "  \"unresolved\": [\n";
    for (u32 i = 0; i < tape.unresolved_count; ++i) {
        const auto& dependency = tape.unresolved[i];
        out << "    {\n";
        out << "      \"what\": \"" << EscapeJson(dependency.what) << "\",\n";
        out << "      \"owner\": \"" << EscapeJson(dependency.owner) << "\",\n";
        out << "      \"reason\": \"" << EscapeJson(dependency.reason) << "\"\n";
        out << "    }" << (i + 1 < tape.unresolved_count ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    out.close();
    if (!out) {
        error = "the tape could not be written completely";
        return false;
    }
    return true;
}

} // namespace

void TryInstallBeforeGuestThread(Module& module, std::string_view title_id,
                                 std::string_view app_version) {
    if (!IsOptedIn(std::getenv(EnableEnvironmentVariable.data())))
        return;

    if (module.memory == nullptr || module.name != RhiCommandCapture::ModuleName ||
        module.file.filename().string() != RhiCommandCapture::ModuleName ||
        title_id != RhiCommandCapture::TitleId) {
        LOG_WARNING(Core_Linker,
                    "Semantic tape refused: expected native CUSA11919 eboot.bin, got "
                    "title='{}' module='{}' path='{}'",
                    title_id, module.name, module.file.string());
        return;
    }

    const char* directory = std::getenv(OutputDirectoryEnvironmentVariable.data());
    if (directory == nullptr || *directory == '\0') {
        LOG_WARNING(Core_Linker, "Semantic tape refused: {} is not set",
                    OutputDirectoryEnvironmentVariable);
        return;
    }
    g_state.output_directory = directory;
    g_state.module_base = module.base_virtual_addr;

    if (const char* selection = std::getenv(DrawSelectionEnvironmentVariable.data());
        selection != nullptr && *selection != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(selection, &end, 10);
        if (end == selection || *end != '\0' || parsed == 0) {
            LOG_WARNING(Core_Linker,
                        "Semantic tape refused: {} must be a positive provenance record id",
                        DrawSelectionEnvironmentVariable);
            return;
        }
        g_state.selected_provenance_id = static_cast<u64>(parsed);
    }

    g_state.enabled = true;
    LOG_INFO(Core_Linker,
             "DERACINE_SEMANTIC_TAPE_INSTALL title_id={} app_version={} module_base={:#x} "
             "output={} selected_draw={}",
             title_id, app_version, module.base_virtual_addr, g_state.output_directory,
             g_state.selected_provenance_id);
}

void ExportTape() noexcept {
    try {
        if (!g_state.enabled)
            return;

        // Select the draw.
        //
        // In combined-capture mode the resource package is authoritative: it selects its own draw by
        // completeness, and the tape must describe that same original command. Defaulting to the
        // last recorded draw would export a tape for a different draw than the package, which no
        // later validation can repair.
        //
        // An explicit manual selection is still honoured for standalone diagnostic work, but it is
        // refused when it conflicts with the package's choice, because the two artifacts would then
        // describe different draws.
        u64 record_id = 0;
        bool package_selected = false;
        {
            DrawPackage::Package probe{};
            std::string probe_error;
            if (DrawPackage::ReadPackage(g_state.output_directory, probe, probe_error) &&
                probe.provenance_id != 0) {
                package_selected = true;
                record_id = probe.provenance_id;
                if (g_state.selected_provenance_id != 0 &&
                    g_state.selected_provenance_id != record_id) {
                    LOG_WARNING(Core_Linker,
                                "DERACINE_SEMANTIC_TAPE_MANUAL_CONFLICT manual={} package={} "
                                "the package selection is authoritative",
                                g_state.selected_provenance_id, record_id);
                }
            }
        }
        if (!package_selected) {
            // No package: a standalone diagnostic run. The explicit selection wins, otherwise the
            // last recorded draw is used so a run without a selection still produces a tape.
            record_id = g_state.selected_provenance_id;
            if (record_id == 0)
                record_id = RhiEntryProbe::LastRecordedProvenanceId();
        }
        if (record_id == 0) {
            LOG_INFO(Core_Linker, "DERACINE_SEMANTIC_TAPE_NONE no recorded draw to describe");
            return;
        }
        LOG_INFO(Core_Linker, "DERACINE_SEMANTIC_TAPE_SELECTED provenance_id={} source={}",
                 record_id, package_selected ? "package" : "manual_or_last");

        RhiEntryProbe::ProvenanceSnapshot snapshot{};
        if (!RhiEntryProbe::GetProvenanceSnapshot(record_id, snapshot)) {
            LOG_INFO(Core_Linker, "DERACINE_SEMANTIC_TAPE_NONE provenance record {} not found",
                     record_id);
            return;
        }

        // The draw package in the same directory, when present, is the resource bridge. It is read
        // rather than assumed, and a package describing a different draw is not used.
        DrawPackage::Package package{};
        bool have_package = false;
        {
            std::string read_error;
            have_package = DrawPackage::ReadPackage(g_state.output_directory, package, read_error);
            if (have_package && package.provenance_id != record_id) {
                LOG_INFO(Core_Linker,
                         "DERACINE_SEMANTIC_TAPE_PACKAGE_MISMATCH package_provenance={} "
                         "tape_provenance={}",
                         package.provenance_id, record_id);
                have_package = false;
            }
        }

        Tape tape{};
        std::string error;
        if (!BuildTape(tape, snapshot, package, have_package, error)) {
            LOG_INFO(Core_Linker, "DERACINE_SEMANTIC_TAPE_NONE {}", error);
            return;
        }

        const TapeProblem problem = ValidateTape(tape);
        if (problem != TapeProblem::None) {
            LOG_WARNING(Core_Linker, "Semantic tape refused: {}", TapeProblemName(problem));
            return;
        }
        const TapeProblem executable = CheckExecutable(tape);
        if (!WriteTape(tape, g_state.output_directory, error)) {
            LOG_WARNING(Core_Linker, "Semantic tape write failed: {}", error);
            return;
        }

        u32 typed = 0;
        u32 opaque = 0;
        for (u32 i = 0; i < tape.operation_count; ++i) {
            if (tape.operations[i].kind == OpKind::Opaque)
                ++opaque;
            else
                ++typed;
        }
        LOG_INFO(Core_Linker,
                 "DERACINE_SEMANTIC_TAPE provenance_id={} operations={} typed={} opaque={} "
                 "resources={} unresolved={} executable={} problem={} submission={} "
                 "packet_offset={:#x}",
                 tape.provenance_id, tape.operation_count, typed, opaque, tape.resource_count,
                 tape.unresolved_count, executable == TapeProblem::None ? 1 : 0,
                 TapeProblemName(executable), tape.submission_id, tape.packet_offset);

        for (u32 i = 0; i < tape.operation_count; ++i) {
            const auto& operation = tape.operations[i];
            LOG_INFO(Core_Linker,
                     "DERACINE_SEMANTIC_TAPE_OP index={} kind={} evidence={} sequence={} "
                     "list={:#x} resource={}",
                     i, OpKindName(operation.kind), EvidenceName(operation.evidence),
                     operation.sequence, operation.command_list, operation.resource);
        }
        for (u32 i = 0; i < tape.unresolved_count; ++i) {
            LOG_INFO(Core_Linker, "DERACINE_SEMANTIC_TAPE_UNRESOLVED what={} owner={} reason={}",
                     tape.unresolved[i].what, tape.unresolved[i].owner,
                     tape.unresolved[i].reason);
        }
    } catch (const std::exception& e) {
        LOG_WARNING(Core_Linker, "Semantic tape export threw: {}", e.what());
    } catch (...) {
        LOG_WARNING(Core_Linker, "Semantic tape export threw an unknown exception");
    }
}

} // namespace Core::SemanticTape