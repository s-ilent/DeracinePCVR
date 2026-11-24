// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "effective_shader.h"

#include <algorithm>
#include <map>

#include "spirv_interface.h"

namespace effective {

namespace {

/// The producer's logical stage values.
constexpr u32 kLogicalVertex = 0;
constexpr u32 kLogicalFragment = 4;

/// Maps the verifier's descriptor class to a Vulkan descriptor type.
///
/// The class comes from the module itself, so the layout is derived from what the shader declares
/// rather than from a table that could disagree with it.
u32 ToDescriptorType(spirv::DescriptorClass cls) {
    switch (cls) {
    case spirv::DescriptorClass::UniformBuffer:
        return 6; // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    case spirv::DescriptorClass::StorageBuffer:
        return 7; // VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    case spirv::DescriptorClass::SampledImage:
        return 2; // VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
    case spirv::DescriptorClass::StorageImage:
        return 3; // VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
    case spirv::DescriptorClass::Sampler:
        return 0; // VK_DESCRIPTOR_TYPE_SAMPLER
    case spirv::DescriptorClass::CombinedImageSampler:
        return 1; // VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
    default:
        return 0xFFFFFFFFu;
    }
}

} // namespace

const char* ProblemName(Problem problem) noexcept {
    switch (problem) {
    case Problem::None:
        return "none";
    case Problem::NotEffectiveSchema:
        return "not_effective_schema";
    case Problem::MissingModule:
        return "missing_module";
    case Problem::InvalidModule:
        return "invalid_module";
    case Problem::PermutationHashMismatch:
        return "permutation_hash_mismatch";
    case Problem::InterfaceMismatch:
        return "interface_mismatch";
    case Problem::MissingBinding:
        return "missing_binding";
    case Problem::MissingResource:
        return "missing_resource";
    case Problem::MissingPipelineState:
        return "missing_pipeline_state";
    case Problem::UnsupportedDescriptor:
        return "unsupported_descriptor";
    case Problem::UnsupportedStageSet:
        return "unsupported_stage_set";
    }
    return "unknown";
}

u64 HashCombine(u64 seed, u64 value) noexcept {
    return seed ^ (value + 0x9e3779b9ull + (seed << 12) + (seed >> 4));
}

Resolution Resolve(const package::Package& package) {
    Resolution out{};

    if (package.schema_version != package::SchemaEffectiveShader) {
        out.problem = Problem::NotEffectiveSchema;
        out.reason = "the package is schema " + std::to_string(package.schema_version) +
                     ", which carries no effective shader state";
        return out;
    }

    // --- Stages --------------------------------------------------------------------------------
    // Exactly one vertex and one fragment stage. A package with a different set is refused rather
    // than executed with whatever it happens to carry.
    const package::ShaderStage* vertex = nullptr;
    const package::ShaderStage* fragment = nullptr;
    for (const auto& stage : package.shader_stages) {
        if (stage.logical_stage == kLogicalVertex) {
            if (vertex != nullptr) {
                out.problem = Problem::UnsupportedStageSet;
                out.reason = "the package declares more than one vertex stage";
                return out;
            }
            vertex = &stage;
        } else if (stage.logical_stage == kLogicalFragment) {
            if (fragment != nullptr) {
                out.problem = Problem::UnsupportedStageSet;
                out.reason = "the package declares more than one fragment stage";
                return out;
            }
            fragment = &stage;
        }
    }
    if (vertex == nullptr || fragment == nullptr) {
        out.problem = Problem::UnsupportedStageSet;
        out.reason = std::string{"the package declares "} +
                     (vertex == nullptr ? "no vertex stage" : "a vertex stage") + " and " +
                     (fragment == nullptr ? "no fragment stage" : "a fragment stage") +
                     "; effective mode needs exactly one of each";
        return out;
    }

    for (const package::ShaderStage* stage : {vertex, fragment}) {
        if (stage->effective_bytes.empty()) {
            out.problem = Problem::MissingModule;
            out.reason = "the stage for logical " + std::to_string(stage->logical_stage) +
                         " carries no effective module bytes";
            return out;
        }
        // The permutation hash must be the combination of its parts. A package that records a hash
        // it did not compute is refused, because the hash is what distinguishes one compiled
        // permutation from another.
        const u64 expected = HashCombine(stage->program_hash, stage->permutation_index);
        if (stage->permutation_hash != expected) {
            out.problem = Problem::PermutationHashMismatch;
            out.reason = "the stage for logical " + std::to_string(stage->logical_stage) +
                         " records permutation hash " + std::to_string(stage->permutation_hash) +
                         " but its program hash and index combine to " + std::to_string(expected);
            return out;
        }

        ResolvedStage resolved{};
        resolved.logical_stage = stage->logical_stage;
        resolved.bytes = stage->effective_bytes;
        resolved.program_hash = stage->program_hash;
        resolved.permutation_index = stage->permutation_index;
        resolved.permutation_hash = stage->permutation_hash;
        resolved.patched = stage->patched;

        // The module is the authority for the interface. A captured interface that disagrees with
        // it is refused, because the layout would then not match the shader that runs.
        spirv::Interface declared{};
        spirv::Problem problem{};
        if (!spirv::ReadInterface(resolved.bytes, declared, problem)) {
            out.problem = Problem::InvalidModule;
            out.reason = "the stage for logical " + std::to_string(stage->logical_stage) +
                         " is not readable SPIR-V: " + std::string{spirv::ProblemName(problem)};
            return out;
        }
        for (const auto& descriptor : declared.descriptors) {
            package::ShaderDescriptor copy{};
            copy.set = descriptor.set;
            copy.binding = descriptor.binding;
            copy.count = descriptor.count;
            copy.descriptor_class = static_cast<u32>(descriptor.cls);
            resolved.declared.push_back(copy);
        }
        resolved.push_constant_offset = declared.push_constant.offset;
        resolved.push_constant_size = declared.push_constant.size;

        out.stages.push_back(std::move(resolved));
    }

    // --- Descriptor bindings -------------------------------------------------------------------
    // Each descriptor the modules declare must have a captured binding, and that binding must name
    // a resource the package carries. A missing one is a precise failure, not a reason to bind a
    // dummy.
    for (const auto& stage : out.stages) {
        const u32 stage_flag = stage.logical_stage == kLogicalVertex ? 1u : 16u;
        for (const auto& descriptor : stage.declared) {
            const u32 type = ToDescriptorType(static_cast<spirv::DescriptorClass>(
                descriptor.descriptor_class));
            if (type == 0xFFFFFFFFu) {
                out.problem = Problem::UnsupportedDescriptor;
                out.reason = "the stage for logical " + std::to_string(stage.logical_stage) +
                             " declares a descriptor class this renderer cannot bind";
                return out;
            }

            // The captured binding is matched on the unified Vulkan binding number, which is what
            // the production descriptor write used. The guest resource index is a different
            // namespace and is not compared against it.
            const package::ShaderBinding* captured = nullptr;
            for (const auto& binding : package.shader_bindings) {
                if (binding.vk_binding == descriptor.binding) {
                    captured = &binding;
                    break;
                }
            }

            // A descriptor that needs a buffer must have a captured binding naming a resource.
            const bool needs_buffer = type == 6 || type == 7;
            if (needs_buffer) {
                if (captured == nullptr) {
                    out.problem = Problem::MissingBinding;
                    out.reason = "binding " + std::to_string(descriptor.binding) +
                                 " is declared by the stage for logical " +
                                 std::to_string(stage.logical_stage) +
                                 " but the package records no captured binding for it";
                    return out;
                }
                if (package::FindResource(package, captured->resource_ref) == nullptr) {
                    out.problem = Problem::MissingResource;
                    out.reason = "binding " + std::to_string(descriptor.binding) +
                                 " names resource " + std::to_string(captured->resource_ref) +
                                 ", which the package does not carry";
                    return out;
                }
            }

            // A binding already added by the other stage gains that stage's visibility rather than
            // becoming a second descriptor.
            bool merged = false;
            for (auto& existing : out.bindings) {
                if (existing.set == descriptor.set && existing.binding == descriptor.binding) {
                    existing.stage_flags |= stage_flag;
                    merged = true;
                    break;
                }
            }
            if (merged) {
                continue;
            }

            ResolvedBinding resolved{};
            resolved.set = descriptor.set;
            resolved.binding = descriptor.binding;
            resolved.descriptor_type = type;
            resolved.count = descriptor.count;
            resolved.stage_flags = stage_flag;
            if (captured != nullptr) {
                resolved.resource_ref = captured->resource_ref;
                resolved.offset = captured->vk_offset;
                resolved.range = captured->vk_range;
            }
            out.bindings.push_back(resolved);
        }
    }

    // --- Push constants ------------------------------------------------------------------------
    for (const auto& stage : out.stages) {
        if (stage.push_constant_size == 0) {
            continue;
        }
        out.push_constant_size = std::max(out.push_constant_size, stage.push_constant_size);
        out.push_constant_offset = stage.push_constant_offset;
        out.push_constant_stages |= stage.logical_stage == kLogicalVertex ? 1u : 16u;
    }
    out.push_constant_bytes = package.pipeline_state.push_constant_bytes;

    // --- Pipeline state ------------------------------------------------------------------------
    // The modules need a topology and a colour format to build a pipeline. A package that did not
    // capture them cannot be executed as original state, and substituting a default would present
    // a diagnostic pipeline as a captured one.
    if (!package.pipeline_state.has_topology) {
        out.problem = Problem::MissingPipelineState;
        out.reason = "the package did not capture the primitive topology";
        return out;
    }
    if (package.pipeline_state.color_formats.empty()) {
        out.problem = Problem::MissingPipelineState;
        out.reason = "the package did not capture a colour attachment format";
        return out;
    }

    out.problem = Problem::None;
    out.reason.clear();
    return out;
}

} // namespace effective
