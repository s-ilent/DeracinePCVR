// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "tape.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

namespace tape {
namespace {

[[nodiscard]] OpKind ParseOpKind(const std::string& name) {
    if (name == "BeginFrame")
        return OpKind::BeginFrame;
    if (name == "SetViewport")
        return OpKind::SetViewport;
    if (name == "SetRenderTargets")
        return OpKind::SetRenderTargets;
    if (name == "BindVertexBuffers")
        return OpKind::BindVertexBuffers;
    if (name == "BindIndexBuffer")
        return OpKind::BindIndexBuffer;
    if (name == "SetGraphicsPipeline")
        return OpKind::SetGraphicsPipeline;
    if (name == "BindShaderResources")
        return OpKind::BindShaderResources;
    if (name == "DrawIndexed")
        return OpKind::DrawIndexed;
    if (name == "EndFrame")
        return OpKind::EndFrame;
    return OpKind::Opaque;
}

[[nodiscard]] ResourceKind ParseResourceKind(const std::string& name) {
    if (name == "index_buffer")
        return ResourceKind::IndexBuffer;
    if (name == "vertex_buffer")
        return ResourceKind::VertexBuffer;
    if (name == "render_target")
        return ResourceKind::RenderTarget;
    if (name == "depth_target")
        return ResourceKind::DepthTarget;
    if (name == "shader_resource")
        return ResourceKind::ShaderResource;
    return ResourceKind::Unknown;
}

[[nodiscard]] ResourceProvenance ParseProvenance(const std::string& name) {
    if (name == "ue4_object")
        return ResourceProvenance::Ue4Object;
    if (name == "downstream_oracle")
        return ResourceProvenance::DownstreamOracle;
    return ResourceProvenance::Unresolved;
}

} // namespace

std::string_view ProblemName(Problem problem) {
    switch (problem) {
    case Problem::None:
        return "none";
    case Problem::SchemaMismatch:
        return "schema_mismatch";
    case Problem::NoDraw:
        return "no_draw";
    case Problem::DrawNotDerivable:
        return "draw_not_derivable";
    case Problem::IndexResourceMissing:
        return "index_resource_missing";
    case Problem::IndexResourceUnresolved:
        return "index_resource_unresolved";
    case Problem::NoViewport:
        return "no_viewport";
    case Problem::NoRenderTargets:
        return "no_render_targets";
    case Problem::DuplicateResourceId:
        return "duplicate_resource_id";
    case Problem::DanglingResourceReference:
        return "dangling_resource_reference";
    case Problem::OperationOutOfOrder:
        return "operation_out_of_order";
    case Problem::DerivedInconsistent:
        return "derived_inconsistent";
    case Problem::ResourceCapacityExceeded:
        return "resource_capacity_exceeded";
    case Problem::OperationCapacityExceeded:
        return "operation_capacity_exceeded";
    case Problem::ReadFailed:
        return "read_failed";
    default:
        return "unknown";
    }
}

bool ReadTape(const std::string& directory, Tape& out, std::string& error) {
    const auto path = std::filesystem::path{directory} / "tape.json";
    std::ifstream file{path};
    if (!file) {
        error = "cannot open " + path.string();
        return false;
    }
    nlohmann::json json;
    try {
        file >> json;
    } catch (const std::exception& e) {
        error = std::string{"tape is not valid JSON: "} + e.what();
        return false;
    }

    try {
        out = {};
        out.schema_version = json.at("schema_version").get<u32>();
        out.scene = json.value("scene", "");
        out.provenance_id = json.at("provenance_id").get<u64>();
        out.image_sha256 = json.value("image_sha256", "");
        out.module_base = json.value("module_base", 0ull);
        out.submission_id = json.value("submission_id", 0ull);
        out.packet_offset = json.value("packet_offset", 0ull);
        out.packet_offset_known = json.value("packet_offset_known", false);

        for (const auto& entry : json.at("resources")) {
            Resource resource{};
            resource.id = entry.at("id").get<u32>();
            resource.kind = ParseResourceKind(entry.value("kind", "unknown"));
            resource.provenance = ParseProvenance(entry.value("provenance", "unresolved"));
            resource.package_resource_ref = entry.value("package_resource_ref", 0u);
            resource.guest_base = entry.value("guest_base", 0ull);
            resource.guest_size = entry.value("guest_size", 0ull);
            resource.element_size = entry.value("element_size", 0u);
            resource.index_domain = entry.value("index_domain", std::string{"ue4_object"});
            out.resources.push_back(resource);
        }

        for (const auto& entry : json.at("operations")) {
            Operation operation{};
            operation.kind = ParseOpKind(entry.at("kind").get<std::string>());
            operation.evidence = entry.value("evidence", "");
            operation.sequence = entry.value("sequence", 0ull);
            operation.command_capture_id = entry.value("command_capture_id", 0ull);
            operation.command_list = entry.value("command_list", 0ull);
            operation.rhi_context = entry.value("rhi_context", 0ull);
            if (entry.contains("viewport")) {
                const auto& vp = entry.at("viewport");
                operation.viewport.min_x = vp.value("min_x", 0.0f);
                operation.viewport.min_y = vp.value("min_y", 0.0f);
                operation.viewport.min_z = vp.value("min_z", 0.0f);
                operation.viewport.max_x = vp.value("max_x", 0.0f);
                operation.viewport.max_y = vp.value("max_y", 0.0f);
                operation.viewport.max_z = vp.value("max_z", 0.0f);
            }
            operation.color_target_count = entry.value("color_target_count", 0u);
            if (entry.contains("color_targets")) {
                const auto& targets = entry.at("color_targets");
                for (size_t i = 0; i < targets.size() && i < 8; ++i)
                    operation.color_targets[i] = targets[i].get<u32>();
            }
            operation.depth_target = entry.value("depth_target", 0u);
            operation.slot = entry.value("slot", 0u);
            operation.resource = entry.value("resource", 0u);
            if (entry.contains("draw")) {
                const auto& draw = entry.at("draw");
                operation.draw.index_buffer_resource =
                    draw.at("index_buffer_resource").get<u32>();
                operation.draw.primitive_type = draw.at("primitive_type").get<u32>();
                operation.draw.base_vertex_index = draw.at("base_vertex_index").get<s32>();
                operation.draw.first_instance = draw.at("first_instance").get<u32>();
                operation.draw.num_vertices = draw.at("num_vertices").get<u32>();
                operation.draw.start_index = draw.at("start_index").get<u32>();
                operation.draw.num_primitives = draw.at("num_primitives").get<u32>();
                operation.draw.num_instances = draw.at("num_instances").get<u32>();
            }
            if (entry.contains("derived")) {
                const auto& derived = entry.at("derived");
                operation.derived.valid = derived.value("valid", false);
                operation.derived.index_count = derived.value("index_count", 0u);
                operation.derived.first_index = derived.value("first_index", 0u);
                operation.derived.vertex_offset = derived.value("vertex_offset", 0);
                operation.derived.instance_count = derived.value("instance_count", 0u);
                operation.derived.index_byte_offset = derived.value("index_byte_offset", 0ull);
            }
            operation.note = entry.value("note", "");
            out.operations.push_back(operation);
        }

        if (json.contains("unresolved")) {
            for (const auto& entry : json.at("unresolved")) {
                Unresolved dependency{};
                dependency.what = entry.value("what", "");
                dependency.owner = entry.value("owner", "");
                dependency.reason = entry.value("reason", "");
                out.unresolved.push_back(dependency);
            }
        }
    } catch (const std::exception& e) {
        error = std::string{"tape is missing a required field: "} + e.what();
        return false;
    }
    error.clear();
    return true;
}

const Resource* FindResource(const Tape& tape, u32 id) {
    for (const auto& resource : tape.resources) {
        if (resource.id == id)
            return &resource;
    }
    return nullptr;
}

DerivedDraw DeriveDraw(const DrawPayload& payload, u32 index_element_size) {
    DerivedDraw out{};
    if (index_element_size != 2 && index_element_size != 4) {
        return out;
    }
    const u32 per_primitive = IndicesPerPrimitive(payload.primitive_type);
    if (per_primitive == 0) {
        return out;
    }
    if (payload.primitive_type == kPrimitiveTypeTriangleStrip) {
        if (payload.num_primitives == 0) {
            return out;
        }
        out.index_count = payload.num_primitives + 2;
    } else {
        // Guard the multiply: a corrupt payload must not wrap into a small count.
        if (payload.num_primitives > 0xFFFFFFFFu / per_primitive) {
            return out;
        }
        out.index_count = payload.num_primitives * per_primitive;
    }
    out.first_index = payload.start_index;
    out.vertex_offset = payload.base_vertex_index;
    out.instance_count = payload.num_instances;
    out.index_byte_offset = static_cast<u64>(payload.start_index) * index_element_size;
    out.valid = true;
    return out;
}

Problem ValidateTape(const Tape& tape) {
    if (tape.schema_version != kSchemaVersion)
        return Problem::SchemaMismatch;

    // The capacity contract is enforced on read as well as on write.
    if (tape.operations.size() > kMaxTapeOperations)
        return Problem::OperationCapacityExceeded;
    if (tape.resources.size() > kMaxTapeResources)
        return Problem::ResourceCapacityExceeded;
    if (tape.unresolved.size() > kMaxTapeUnresolved)
        return Problem::ResourceCapacityExceeded;

    std::set<u32> ids;
    for (const auto& resource : tape.resources) {
        if (resource.id == 0)
            return Problem::DuplicateResourceId;
        if (!ids.insert(resource.id).second)
            return Problem::DuplicateResourceId;
    }

    for (const auto& operation : tape.operations) {
        if (operation.kind == OpKind::DrawIndexed) {
            if (operation.draw.index_buffer_resource == 0)
                return Problem::IndexResourceMissing;
            if (FindResource(tape, operation.draw.index_buffer_resource) == nullptr)
                return Problem::DanglingResourceReference;
        }
        if (operation.kind == OpKind::BindIndexBuffer ||
            operation.kind == OpKind::BindVertexBuffers) {
            if (operation.resource != 0 && FindResource(tape, operation.resource) == nullptr)
                return Problem::DanglingResourceReference;
        }
        if (operation.kind == OpKind::SetRenderTargets) {
            for (u32 t = 0; t < operation.color_target_count && t < 8; ++t) {
                if (operation.color_targets[t] != 0 &&
                    FindResource(tape, operation.color_targets[t]) == nullptr)
                    return Problem::DanglingResourceReference;
            }
            if (operation.depth_target != 0 &&
                FindResource(tape, operation.depth_target) == nullptr)
                return Problem::DanglingResourceReference;
        }
    }

    bool saw_draw = false;
    for (const auto& operation : tape.operations) {
        if (operation.kind != OpKind::DrawIndexed)
            continue;
        if (saw_draw)
            return Problem::OperationOutOfOrder;
        saw_draw = true;
    }
    if (!saw_draw)
        return Problem::NoDraw;

    // The stored derived draw must agree with the semantic payload. The payload is authoritative, so
    // a tape whose derived values contradict it is inconsistent data and is refused here rather than
    // executed on a number the tape itself disproves.
    for (const auto& operation : tape.operations) {
        if (operation.kind != OpKind::DrawIndexed)
            continue;
        const Resource* index = FindResource(tape, operation.draw.index_buffer_resource);
        if (index == nullptr)
            continue; // reported as a dangling reference above
        const DerivedDraw expected = DeriveDraw(operation.draw, index->element_size);
        if (expected.valid != operation.derived.valid)
            return Problem::DerivedInconsistent;
        if (!expected.valid)
            continue;
        if (expected.index_count != operation.derived.index_count ||
            expected.first_index != operation.derived.first_index ||
            expected.vertex_offset != operation.derived.vertex_offset ||
            expected.instance_count != operation.derived.instance_count ||
            expected.index_byte_offset != operation.derived.index_byte_offset) {
            return Problem::DerivedInconsistent;
        }
    }

    return Problem::None;
}

Problem CheckExecutable(const Tape& tape) {
    const Problem structural = ValidateTape(tape);
    if (structural != Problem::None)
        return structural;

    bool saw_viewport = false;
    bool saw_render_targets = false;
    for (const auto& operation : tape.operations) {
        if (operation.kind == OpKind::DrawIndexed)
            break;
        if (operation.kind == OpKind::SetViewport)
            saw_viewport = true;
        if (operation.kind == OpKind::SetRenderTargets)
            saw_render_targets = true;
    }
    if (!saw_viewport)
        return Problem::NoViewport;
    if (!saw_render_targets)
        return Problem::NoRenderTargets;

    for (const auto& operation : tape.operations) {
        if (operation.kind != OpKind::DrawIndexed)
            continue;
        if (!operation.derived.valid)
            return Problem::DrawNotDerivable;
        const Resource* index = FindResource(tape, operation.draw.index_buffer_resource);
        if (index == nullptr)
            return Problem::IndexResourceMissing;
        if (index->provenance == ResourceProvenance::Unresolved)
            return Problem::IndexResourceUnresolved;
    }
    return Problem::None;
}

bool Execute(const Tape& tape, CommandSink& sink, ExecutionReport& report, std::string& error,
               const Substitutions& substitutions) {
    report = {};

    // A tape that is structurally sound but missing state can still be executed when the caller
    // supplies that state explicitly. The substitution is recorded, never silent.
    Problem problem = CheckExecutable(tape);
    if (problem == Problem::NoViewport && substitutions.has_viewport) {
        problem = Problem::None;
    } else if (problem == Problem::NoRenderTargets && substitutions.has_render_targets) {
        problem = Problem::None;
    }
    if (problem != Problem::None) {
        error = std::string{"tape is not executable: "} + std::string{ProblemName(problem)};
        return false;
    }

    // Structural validation still applies: a substitution covers missing state, not a broken tape.
    const Problem structural = ValidateTape(tape);
    if (structural != Problem::None) {
        error = std::string{"tape is not executable: "} + std::string{ProblemName(structural)};
        return false;
    }

    bool tape_has_viewport = false;
    bool tape_has_render_targets = false;
    for (const auto& operation : tape.operations) {
        if (operation.kind == OpKind::DrawIndexed)
            break;
        if (operation.kind == OpKind::SetViewport)
            tape_has_viewport = true;
        if (operation.kind == OpKind::SetRenderTargets)
            tape_has_render_targets = true;
    }
    if (!tape_has_viewport && substitutions.has_viewport) {
        sink.SetViewport(substitutions.viewport);
        report.viewport = substitutions.viewport;
        report.saw_viewport = true;
        report.substituted.push_back("viewport");
    }
    // The tape carries no pipeline operation, so the backend's diagnostic pipeline is what the
    // draw runs on. That is a substitution and is named as one.
    bool tape_has_pipeline = false;
    for (const auto& operation : tape.operations) {
        if (operation.kind == OpKind::SetGraphicsPipeline)
            tape_has_pipeline = true;
    }
    if (!tape_has_pipeline) {
        report.substituted.push_back("graphics_pipeline");
    }
    if (!tape_has_render_targets && substitutions.has_render_targets) {
        const u32 targets[1] = {0};
        sink.SetRenderTargets(targets, substitutions.color_target_count, substitutions.depth_target);
        report.saw_render_targets = true;
        report.substituted.push_back("render_targets");
    }

    sink.BeginFrame();
    for (const auto& operation : tape.operations) {
        switch (operation.kind) {
        case OpKind::BeginFrame:
            break;
        case OpKind::SetViewport:
            sink.SetViewport(operation.viewport);
            report.viewport = operation.viewport;
            report.saw_viewport = true;
            break;
        case OpKind::SetRenderTargets:
            sink.SetRenderTargets(operation.color_targets, operation.color_target_count,
                                  operation.depth_target);
            report.saw_render_targets = true;
            break;
        case OpKind::BindVertexBuffers: {
            const u32 resources[1] = {operation.resource};
            sink.BindVertexBuffers(resources, 1);
            break;
        }
        case OpKind::BindIndexBuffer: {
            const Resource* resource = FindResource(tape, operation.resource);
            sink.BindIndexBuffer(operation.resource,
                                 resource != nullptr ? resource->element_size : 0);
            break;
        }
        case OpKind::SetGraphicsPipeline:
            sink.SetGraphicsPipeline(operation.resource);
            break;
        case OpKind::BindShaderResources: {
            const u32 resources[1] = {operation.resource};
            sink.BindShaderResources(resources, 1);
            break;
        }
        case OpKind::DrawIndexed: {
            // The draw arguments come from the tape, which came from the UE4 deferred command.
            // Nothing here reads a captured PM4 packet.
            //
            // The semantic payload is authoritative, so the effective arguments are recomputed here
            // rather than taken from the serialized derived block. ValidateTape has already refused
            // a tape whose stored values disagree, so this recomputation is the value the sink
            // receives: a backend never sees an unverified number.
            const Resource* index = FindResource(tape, operation.draw.index_buffer_resource);
            const u32 element_size = index != nullptr ? index->element_size : 0;
            const DerivedDraw effective = DeriveDraw(operation.draw, element_size);
            sink.DrawIndexed(operation.draw, effective);
            report.draw_payload = operation.draw;
            report.derived = effective;
            ++report.draws;
            break;
        }
        case OpKind::EndFrame:
            break;
        case OpKind::Opaque:
        default:
            sink.Opaque(operation.sequence, operation.note);
            ++report.opaque_operations;
            break;
        }
        ++report.operations_executed;
    }
    sink.EndFrame();
    error.clear();
    return true;
}

} // namespace tape