// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/quality_diag.h"
#include "video_core/renderer_vulkan/quality_probe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <vulkan/vulkan_format_traits.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan::QualityDiag {

namespace {

using Clock = std::chrono::steady_clock;

/// One summary line a second, and no more: the states a frame passes through are far more than
/// that, and the shapes are what is wanted, not a trace of every frame.
constexpr auto Window = std::chrono::seconds{1};
/// The scene targets are at least this size; together with the multisampled ones they are what
/// is summarised. A title has hundreds of other states.
constexpr u32 SceneWidth = 960;
constexpr u32 SceneHeight = 1080;
constexpr u32 MostEntries = 6;

struct Target {
    bool bound{};
    VAddr address{};
    u32 format{};
    u32 width{};
    u32 height{};
    u32 guest_samples{};
    u32 image_samples{};
    u32 image_width{};
    u32 image_height{};
    vk::Format image_format{};
    u32 image_usage{};
};

struct Entry {
    std::string name;
    Target target;
    u32 count{};
};

bool initialised{};
bool enabled{};
bool resolve_aa{true};
u32 lines{};
u64 draws{};
u64 resolves{};
u64 substituted{};
u32 collected_count{};
std::array<Target, AmdGpu::NUM_COLOR_BUFFERS> collected{};
Target collected_depth{};
u32 layers{1};
u32 pipeline_samples{};
u32 pipeline_depth_samples{};
std::vector<Entry> window;
u32 windows{};
u64 window_draws{};
u64 window_resolves{};
std::array<u64, 8> window_modes{};
u64 window_substituted{};
Clock::time_point window_at{};

u32 Samples(const VideoCore::Image& image) {
    return image.backing ? static_cast<u32>(image.backing->image.image_ci.samples) : 0;
}

bool WorthTelling(const Target& target) {
    return target.bound && (lines < 12 || target.guest_samples > 1 || target.image_samples > 1 ||
                            (target.width >= SceneWidth && target.height >= SceneHeight));
}

/// What a target is, as its descriptor and the image behind it hold it.
Target Make(VAddr address, u32 format, u32 width, u32 height, u32 guest_samples,
            const VideoCore::Image& image) {
    return {true,
            address,
            format,
            width,
            height,
            guest_samples,
            Samples(image),
            image.info.size.width,
            image.info.size.height,
            image.info.pixel_format,
            static_cast<u32>(image.usage_flags)};
}

bool Same(const Target& a, const Target& b) {
    return a.bound == b.bound && a.address == b.address && a.format == b.format &&
           a.width == b.width && a.height == b.height && a.guest_samples == b.guest_samples &&
           a.image_samples == b.image_samples && a.image_width == b.image_width &&
           a.image_height == b.image_height && a.image_format == b.image_format;
}

void Count(std::string_view name, const Target& target) {
    for (Entry& entry : window) {
        if (Same(entry.target, target)) {
            ++entry.count;
            return;
        }
    }
    window.push_back({std::string{name}, target, 1});
}

void Flush() {
    if (window.empty() && window_draws == 0 && window_resolves == 0) {
        return;
    }
    ++windows;
    std::ranges::sort(window, [](const Entry& a, const Entry& b) { return a.count > b.count; });
    std::string text;
    for (u32 i = 0; i < window.size() && i < MostEntries; ++i) {
        const Entry& entry = window[i];
        const Target& target = entry.target;
        text += fmt::format(
            "{} {}x{} s{}/{} {:#x} fmt={} x{} | ", entry.name, target.width, target.height,
            target.guest_samples, target.image_samples, target.address, target.format,
            entry.count);
    }
    if (window.size() > MostEntries) {
        text += fmt::format("+{} more | ", window.size() - MostEntries);
    }
    LOG_INFO(Render_Vulkan,
             "QUALITYDIAG window #{} draws={} resolves={} substituted={} layers={} "
             "pipeline_s={}/{} modes=disable:{} normal:{} eliminate:{} resolve:{} err:{} fmask:{} | {}",
             windows, window_draws, window_resolves, window_substituted, layers,
             pipeline_samples, pipeline_depth_samples, window_modes[0], window_modes[1],
             window_modes[2], window_modes[3], window_modes[4], window_modes[5], text);
    window.clear();
    window_draws = 0;
    window_resolves = 0;
    window_substituted = 0;
    window_modes = std::array<u64, 8>{};
}

struct Probe {
    ProbeSpec spec{};
    std::string value{"unread"};
};

std::vector<Probe> probes;
bool probes_parsed{};
bool probes_told{};
Clock::time_point probes_at{};
Clock::time_point probes_told_at{};
bool ReadGuest(VAddr address, void* out, u64 size) {
    if (address == 0 || (address % size) != 0) {
        return false;
    }
#ifdef _WIN32
    // Guest memory is committed at its guest address, so the host can say whether an address is
    // readable without going through the emulator's own mapping tables, which the guest's
    // threads change as they run and must not be walked from the GPU thread.
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        info.State != MEM_COMMIT) {
        return false;
    }
    const DWORD protect = info.Protect & 0xff;
    if (protect != PAGE_READONLY && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
        protect != PAGE_EXECUTE_READ && protect != PAGE_EXECUTE_READWRITE &&
        protect != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }
#else
    if (!Core::Memory::Instance()->IsValidMapping(address, size)) {
        return false;
    }
#endif
    std::memcpy(out, reinterpret_cast<const void*>(address), size);
    return true;
}

std::string ValueOf(const Probe& probe) {
    switch (probe.spec.kind) {
    case ProbeKind::U8: {
        u8 value{};
        return ReadGuest(probe.spec.address, &value, sizeof(value))
                   ? fmt::format("u8 {:#04x} = {}", value, value)
                   : std::string{"u8 unreadable"};
    }
    case ProbeKind::U32: {
        u32 value{};
        return ReadGuest(probe.spec.address, &value, sizeof(value))
                   ? fmt::format("u32 {:#x} = {}", value, value)
                   : std::string{"u32 unreadable"};
    }
    case ProbeKind::F32: {
        float value{};
        return ReadGuest(probe.spec.address, &value, sizeof(value))
                   ? fmt::format("f32 {:.6f}", value)
                   : std::string{"f32 unreadable"};
    }
    case ProbeKind::PointerToF32: {
        u64 pointer{};
        if (!ReadGuest(probe.spec.address, &pointer, sizeof(pointer)) || pointer == 0) {
            return "ptr_f32 unreadable";
        }
        float value{};
        if (!ReadGuest(static_cast<VAddr>(pointer), &value, sizeof(value))) {
            return fmt::format("ptr_f32 0x{:x} unreadable", pointer);
        }
        return fmt::format("ptr_f32 0x{:x} -> {:.6f}", pointer, value);
    }
    }
    return {};
}

} // namespace

bool Enabled() {
    if (!initialised) {
        initialised = true;
        const char* value = std::getenv("SHADPS4_QUALITY_DIAG");
        enabled = value != nullptr && value[0] == '1' && value[1] == '\0';
        const char* aa = std::getenv("SHADPS4_RESOLVE_AA");
        resolve_aa = aa == nullptr || aa[0] != '0';
        window_at = Clock::now();
        if (enabled) {
            const char* max_msaa = std::getenv("SHADPS4_MAX_MSAA");
            LOG_INFO(Render_Vulkan,
                     "QUALITYDIAG enabled: SHADPS4_MAX_MSAA={} SHADPS4_RESOLVE_AA={}; the host "
                     "never sets a multiview view mask, so guest multiview or instanced stereo "
                     "reaches the renderer as layers or as two passes",
                     max_msaa != nullptr ? max_msaa : "unset", aa != nullptr ? aa : "unset");
        }
    }
    return enabled;
}

void BeginRenderState() {
    if (!Enabled()) {
        return;
    }
    collected = {};
    collected_depth = {};
    collected_count = 0;
}

void NoteColorTarget(u32 index, const AmdGpu::ColorBuffer& buffer, const VideoCore::Image& image) {
    if (!Enabled() || index >= AmdGpu::NUM_COLOR_BUFFERS) {
        return;
    }
    collected[index] = Make(buffer.Address(), static_cast<u32>(buffer.info.format), buffer.Pitch(),
                            buffer.Height(), buffer.NumSamples(), image);
    collected_count = std::max(collected_count, index + 1u);
}

void NoteDepthTarget(const AmdGpu::DepthBuffer& buffer, const VideoCore::Image& image) {
    if (!Enabled()) {
        return;
    }
    collected_depth = Make(buffer.DepthAddress(), static_cast<u32>(buffer.z_info.format),
                           buffer.Pitch(), buffer.Height(), buffer.NumSamples(), image);
}

void NoteRenderState(u32 num_layers, u32 samples, u32 depth_samples) {
    if (!Enabled()) {
        return;
    }
    layers = num_layers;
    pipeline_samples = samples;
    pipeline_depth_samples = depth_samples;
    for (u32 cb = 0; cb < collected_count; ++cb) {
        if (WorthTelling(collected[cb])) {
            ++lines;
            Count(fmt::format("cb{}", cb), collected[cb]);
        }
    }
    if (WorthTelling(collected_depth)) {
        ++lines;
        Count("db", collected_depth);
    }
}

void NoteDraw() {
    if (!Enabled()) {
        return;
    }
    ++draws;
    ++window_draws;
    PollProbes();
    const auto now = Clock::now();
    if (now - window_at >= Window) {
        window_at = now;
        Flush();
    }
}

void NoteOperation(u32 mode) {
    if (!Enabled()) {
        return;
    }
    ++window_modes[mode & 7u];
}

void NoteResolve(const VideoCore::Image& src, const VideoCore::Image& dst) {
    if (!Enabled()) {
        return;
    }
    ++resolves;
    ++window_resolves;
    const u32 src_image = Samples(src);
    const char* path = "copy";
    if (src.info.num_samples > 1 && src_image == 1 && resolve_aa) {
        path = "smooth-substitute";
        ++substituted;
        ++window_substituted;
    } else if (src_image > 1) {
        path = "resolve";
    }

}

void NoteBacking(VAddr address, u32 from_samples, u32 to_samples, bool copy_backing) {
    if (!Enabled() || from_samples == to_samples) {
        return;
    }
    LOG_INFO(Render_Vulkan, "QUALITYDIAG backing {:#x}: {}x -> {}x{}", address, from_samples,
             to_samples, copy_backing ? " (copied)" : "");
}

void PollProbes() {
    if (!Enabled()) {
        return;
    }
    const auto now = Clock::now();
    if (probes_told && now - probes_at < std::chrono::seconds{1}) {
        return;
    }
    if (!probes_parsed) {
        probes_parsed = true;
        const char* list = std::getenv("SHADPS4_QUALITY_PROBE");
        if (list != nullptr) {
            for (const ProbeSpec& spec : ParseProbeList(list)) {
                probes.push_back({spec, "unread"});
            }
        }
        if (!probes.empty()) {
            LOG_INFO(Render_Vulkan, "QUALITYDIAG probe: watching {} guest value(s)", probes.size());
        }
    }
    if (probes.empty()) {
        return;
    }
    probes_at = now;
    for (Probe& probe : probes) {
        std::string value = ValueOf(probe);
        const bool changed = value != probe.value;
        const bool due = !probes_told || now - probes_told_at >= std::chrono::seconds{10};
        if (changed || due) {
            probe.value = std::move(value);
            LOG_INFO(Render_Vulkan, "QUALITYDIAG probe {:#x}: {}", probe.spec.address, probe.value);
        }
    }
    if (probes_told) {
        probes_told_at = now;
    }
    probes_told = true;
}

} // namespace Vulkan::QualityDiag



