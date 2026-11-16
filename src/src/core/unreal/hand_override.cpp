// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/unreal/hand_override.h"

#include <algorithm>

namespace Core::Unreal {

void HandOverrideSession::SetEnabled(bool value) {
    if (enabled != value) {
        // Disabling keeps the saved pose so the next frame can restore it.
        last_refusal = value ? "" : last_refusal;
    }
    enabled = value;
}

bool HandOverrideSession::CanWrite(const OverrideFrame& frame) const {
    switch (frame.phase) {
    case AnimationPhase::AfterEvaluation:
        return true;
    case AnimationPhase::BeforeEvaluation:
    case AnimationPhase::EvaluationRunning:
    case AnimationPhase::SkinningHandoff:
    case AnimationPhase::Unknown:
    default:
        return false;
    }
}

u32 HandOverrideSession::Apply(std::span<const Quat> animated, std::span<const Quat> retargeted,
                               float weight, const OverrideFrame& frame, std::span<Quat> out) {
    last_written = 0;
    last_frame = frame.frame_index;
    last_refusal.clear();

    if (!enabled) {
        // A disabled session still owes the game its own pose back if it ever wrote one.
        if (has_saved) {
            last_written = Restore(out);
        }
        last_refusal = "disabled";
        return 0;
    }
    if (!CanWrite(frame)) {
        last_refusal = frame.phase == AnimationPhase::SkinningHandoff
                           ? "too late: skinning handoff"
                           : "not after animation evaluation";
        return 0;
    }
    const auto clamped = std::clamp(weight, 0.0f, 1.0f);
    if (clamped <= minimum_weight) {
        if (has_saved) {
            last_written = Restore(out);
        }
        last_refusal = "weight below threshold";
        return 0;
    }

    const auto count = std::min({animated.size(), retargeted.size(), out.size()});
    if (count == 0) {
        last_refusal = "empty pose";
        return 0;
    }
    if (!has_saved || saved_frame != frame.frame_index) {
        saved_pose.assign(animated.begin(), animated.begin() + count);
        has_saved = true;
        saved_frame = frame.frame_index;
    }
    for (auto i = 0u; i < count; i++) {
        // Blend against the animation value of this frame, not against a previous override.
        const auto& base = saved_frame == frame.frame_index && saved_pose.size() >= count
                               ? saved_pose[i]
                               : animated[i];
        out[i] = QuatSlerp(base, retargeted[i], clamped);
    }
    last_written = count;
    return count;
}

u32 HandOverrideSession::Restore(std::span<Quat> out) {
    if (!has_saved || saved_pose.empty()) {
        has_saved = false;
        return 0;
    }
    const auto count = std::min(saved_pose.size(), out.size());
    for (auto i = 0u; i < count; i++) {
        out[i] = saved_pose[i];
    }
    has_saved = false;
    saved_pose.clear();
    last_written = 0;
    return count;
}

} // namespace Core::Unreal
