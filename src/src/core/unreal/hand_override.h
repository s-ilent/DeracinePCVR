// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/unreal/hand_retarget.h"

namespace Core::Unreal {

// Where a caller is in the frame's animation pipeline. Only one of these is a safe place to replace
// finger rotations, and it has to be established from the runtime, not assumed.
enum class AnimationPhase : u32 {
    Unknown = 0,
    BeforeEvaluation = 1,   // the animation update will overwrite anything written here
    EvaluationRunning = 2,  // parallel evaluation may still read/write the pose buffers
    AfterEvaluation = 3,    // pose arrays hold this frame's animation, skinning has not consumed it
    SkinningHandoff = 4,    // matrices derived/consumed: too late for this frame
};

struct OverrideFrame {
    AnimationPhase phase = AnimationPhase::Unknown;
    u64 frame_index = 0;
};

// Applies retargeted finger rotations only at a verified boundary and keeps the animation values it
// replaced so they can be restored. It holds no guest pointers and performs no writes: the caller
// owns the actual store, which keeps the experiment reversible.
class HandOverrideSession {
public:
    void SetEnabled(bool value);
    bool Enabled() const {
        return enabled;
    }

    // Blends `retargeted` over `animated` into `out` when enabled and inside AfterEvaluation.
    // Returns the number of joints written (0 when refused). The animation pose is saved the first
    // time a frame is written, so repeated calls in one frame cannot save the session's own output.
    u32 Apply(std::span<const Quat> animated, std::span<const Quat> retargeted, float weight,
              const OverrideFrame& frame, std::span<Quat> out);

    // Writes the saved animation rotations back into `out`; returns how many joints were restored.
    u32 Restore(std::span<Quat> out);

    bool HasSavedPose() const {
        return has_saved;
    }
    u32 LastWritten() const {
        return last_written;
    }
    u64 LastFrame() const {
        return last_frame;
    }
    const std::string& LastRefusal() const {
        return last_refusal;
    }

private:
    bool CanWrite(const OverrideFrame& frame) const;

    bool enabled = false;
    bool has_saved = false;
    u64 saved_frame = 0;
    u64 last_frame = 0;
    u32 last_written = 0;
    float minimum_weight = 0.02f;
    std::vector<Quat> saved_pose;
    std::string last_refusal;
};

} // namespace Core::Unreal
