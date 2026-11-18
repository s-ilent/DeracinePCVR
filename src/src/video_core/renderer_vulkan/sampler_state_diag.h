// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>

#include <magic_enum/magic_enum.hpp>

#include "common/logging/log.h"
#include "common/types.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"

namespace Vulkan {

/// Reports the guest sampler state a shader is bound with, together with the texture descriptor
/// the sampler is paired with, once per unique binding.
///
/// CUSA11919's 32-cubed colour-grading LUT is sampled as a 3D image, where the guest's clamp_z
/// becomes addressModeW and so decides what an out-of-range Z lookup reads: the tonemap
/// coordinate (31/32 * c + 0.5/32) leaves [0,1] exactly when the colour it grades does. What the
/// hardware did there is only recoverable from the S# the title actually uploads, which this
/// prints.
///
/// Bounded and off by default, so it stays safe to leave in the tree. Two forms, by the value:
///   SHADPS4_DBG_SAMPLER=1             every unique binding, capped at MaxLines
///   SHADPS4_DBG_SAMPLER=0x2008f80000  only bindings whose paired texture is at that address
class SamplerStateDiag {
public:
    static bool Enabled() noexcept {
        static const bool enabled = [] {
            const char* value = std::getenv("SHADPS4_DBG_SAMPLER");
            if (value == nullptr || value[0] == '\0') {
                return false;
            }
            return !(value[0] == '0' && value[1] == '\0');
        }();
        return enabled;
    }

    /// `ssharp` must be the words handed to TextureCache::GetSampler, so that what is logged is
    /// what the VkSampler was built from. `tsharp` is the paired texture descriptor, or nullptr
    /// when the sampler has none.
    static void LogBinding(const Shader::Info& stage, u32 binding,
                           const Shader::SamplerResource& sampler, const AmdGpu::Sampler& ssharp,
                           const AmdGpu::Image* tsharp) {
        if (const std::optional<VAddr> address = FilterAddress();
            address && (tsharp == nullptr || tsharp->Address() != *address)) {
            return;
        }
        static std::mutex seen_mutex;
        static std::unordered_set<u64> seen;
        {
            std::scoped_lock lock{seen_mutex};
            if (seen.size() >= MaxLines ||
                !seen.emplace(Key(stage, binding, ssharp, tsharp)).second) {
                return;
            }
        }

        const auto clamp_x = ssharp.clamp_x.Value();
        const auto clamp_y = ssharp.clamp_y.Value();
        const auto clamp_z = ssharp.clamp_z.Value();
        const u32 aniso_off = sampler.disable_aniso;
        if (tsharp == nullptr) {
            LOG_INFO(Render_Vulkan,
                     "SAMPLER_STATE shader={:#x} stage={} binding={} origin={} aniso_off={} "
                     "raw0={:#x} raw1={:#x} clamp_x={} clamp_y={} clamp_z={} names={}/{}/{} "
                     "vulkan={}/{}/{} tex=none",
                     stage.pgm_hash, magic_enum::enum_name(stage.stage), binding, Origin(sampler),
                     aniso_off, ssharp.raw0, ssharp.raw1, static_cast<u32>(clamp_x),
                     static_cast<u32>(clamp_y), static_cast<u32>(clamp_z),
                     magic_enum::enum_name(clamp_x), magic_enum::enum_name(clamp_y),
                     magic_enum::enum_name(clamp_z),
                     vk::to_string(LiverpoolToVK::ClampMode(clamp_x)),
                     vk::to_string(LiverpoolToVK::ClampMode(clamp_y)),
                     vk::to_string(LiverpoolToVK::ClampMode(clamp_z)));
            return;
        }

        const auto type = tsharp->GetType();
        LOG_INFO(Render_Vulkan,
                 "SAMPLER_STATE shader={:#x} stage={} binding={} origin={} aniso_off={} "
                 "raw0={:#x} raw1={:#x} clamp_x={} clamp_y={} clamp_z={} names={}/{}/{} "
                 "vulkan={}/{}/{} tex=addr={:#x} type={} {}x{}x{} layers={} levels={} volume={}",
                 stage.pgm_hash, magic_enum::enum_name(stage.stage), binding, Origin(sampler),
                 aniso_off, ssharp.raw0, ssharp.raw1, static_cast<u32>(clamp_x),
                 static_cast<u32>(clamp_y), static_cast<u32>(clamp_z),
                 magic_enum::enum_name(clamp_x), magic_enum::enum_name(clamp_y),
                 magic_enum::enum_name(clamp_z),
                 vk::to_string(LiverpoolToVK::ClampMode(clamp_x)),
                 vk::to_string(LiverpoolToVK::ClampMode(clamp_y)),
                 vk::to_string(LiverpoolToVK::ClampMode(clamp_z)), tsharp->Address(),
                 magic_enum::enum_name(type), tsharp->width + 1, tsharp->height + 1,
                 tsharp->depth + 1, tsharp->NumLayers(), tsharp->NumLevels(),
                 type == AmdGpu::ImageType::Color3D ? 1 : 0);
    }

private:
    static constexpr std::size_t MaxLines = 400;

    /// Nothing for "every binding" (and for "off", which Enabled() has already rejected).
    static std::optional<VAddr> FilterAddress() noexcept {
        static const std::optional<VAddr> filter = []() -> std::optional<VAddr> {
            const char* value = std::getenv("SHADPS4_DBG_SAMPLER");
            if (value == nullptr || value[0] == '\0' || (value[0] == '0' && value[1] == '\0') ||
                (value[0] == '1' && value[1] == '\0')) {
                return std::nullopt;
            }
            return VAddr(std::strtoull(value, nullptr, 16));
        }();
        return filter;
    }

    static std::string Origin(const Shader::SamplerResource& sampler) {
        return sampler.is_inline_sampler ? std::string{"inline"}
                                         : fmt::format("sgpr:{}", sampler.sharp_idx);
    }

    /// Distinct stage, program, binding, S# words and paired T#, so that a title's ordinary
    /// per-frame churn does not repeat a line while a changed sampler state still does.
    static u64 Key(const Shader::Info& stage, u32 binding, const AmdGpu::Sampler& ssharp,
                   const AmdGpu::Image* tsharp) {
        u64 hash = 0x9E3779B97F4A7C15ULL;
        const auto mix = [&hash](u64 value) { hash = (hash ^ value) * 0x100000001B3ULL; };
        mix(static_cast<u32>(stage.stage));
        mix(stage.pgm_hash);
        mix(binding);
        mix(ssharp.raw0);
        mix(ssharp.raw1);
        if (tsharp == nullptr) {
            mix(0xFFFFFFFFFFFFFFFFULL);
            return hash;
        }
        mix(tsharp->Address());
        mix(static_cast<u64>(tsharp->GetType()));
        mix((u64(tsharp->width) << 32) | tsharp->height);
        mix((u64(tsharp->depth) << 16) | (u64(tsharp->NumLayers()) << 8) | tsharp->NumLevels());
        return hash;
    }
};

} // namespace Vulkan
