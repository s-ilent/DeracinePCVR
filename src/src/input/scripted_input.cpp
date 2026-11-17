// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <numbers>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/vr/vr_runtime.h"
#include "input/controller.h"
#include "input/guest_frame.h"
#include "input/pad_gestures.h"
#include "input/scripted_input.h"
#include "input/scripted_timeline.h"
#include "input/stick_finger.h"

namespace Input {

namespace {

using Buttons = Libraries::Pad::OrbisPadButtonDataOffset;

std::atomic<float> microphone_level{0.0f};
std::atomic<bool> blowing{false};

struct Step {
    Scripted::StepTiming timing;
    Buttons buttons{Buttons::None};
    // Left x, left y, right x, right y; negative means "leave centred".
    std::array<int, 4> sticks{-1, -1, -1, -1};
    std::optional<Core::Vr::Pose> head;
    std::optional<Core::Vr::Pose> pad;
    std::optional<Core::Vr::Quat> pad_rotation;
    // Where a host sees the hands hold the controller: x, y, z and heading in degrees.
    std::optional<std::array<float, 4>> hands;
    bool hands_lost{};
    // A Move controller's buttons and trigger, as the title's libSceMove sees them: for a
    // headless run where the pad mapping is not what the scene needs. 0 is the right hand,
    // 1 the left (see SHADPS4_VR_MOVE_FROM_PAD).
    struct MoveInput {
        u16 buttons{};
        u8 trigger{};
    };
    std::array<std::optional<MoveInput>, 2> move;
    // A finger on the touchpad, 0..1 across and down.
    std::optional<std::array<float, 2>> touch;
    // The right stick as it stands in for that finger (Input::StickFinger), -1..1 to the right
    // and towards the player.
    std::optional<std::array<float, 2>> finger;
    // The buttons that do the touchpad's gestures on a controller without one
    // (Input::PadGestures), held while the line lasts.
    PadGestures::Controls gestures;
    // The place a controller that nothing locates is held to be at: moved by so many metres
    // (right, up, towards the player), or switched between the standard one and the player's.
    std::optional<Core::Vr::Vec3> place_move;
    bool place_switch{};
    // What the microphone hears: noise of this loudness (root mean square, 1 is full scale).
    float microphone{};
    bool recenter{};
    // The headset taken off the head (false) or put back on (true), from this line on.
    std::optional<bool> worn;
};

std::vector<float> ParseNumbers(const std::string& text) {
    std::vector<float> numbers;
    std::istringstream stream{text};
    std::string item;
    while (std::getline(stream, item, ',')) {
        numbers.push_back(std::stof(item));
    }
    return numbers;
}

Core::Vr::Quat ParseRotation(const std::vector<float>& numbers, size_t first) {
    static constexpr float Radians = std::numbers::pi_v<float> / 180.0f;
    const auto angle = [&](size_t index) {
        return first + index < numbers.size() ? numbers[first + index] * Radians : 0.0f;
    };
    return Core::Vr::FromYawPitchRoll(angle(0), angle(1), angle(2));
}

std::optional<Core::Vr::Pose> ParsePose(const std::string& text) {
    const auto numbers = ParseNumbers(text);
    if (numbers.size() < 3) {
        return std::nullopt;
    }
    Core::Vr::Pose pose;
    pose.position = {numbers[0], numbers[1], numbers[2]};
    pose.orientation = ParseRotation(numbers, 3);
    return pose;
}

const std::unordered_map<std::string, Buttons> ButtonNames = {
    {"cross", Buttons::Cross},     {"circle", Buttons::Circle},
    {"square", Buttons::Square},   {"triangle", Buttons::Triangle},
    {"up", Buttons::Up},           {"down", Buttons::Down},
    {"left", Buttons::Left},       {"right", Buttons::Right},
    {"l1", Buttons::L1},           {"r1", Buttons::R1},
    {"l2", Buttons::L2},           {"r2", Buttons::R2},
    {"l3", Buttons::L3},           {"r3", Buttons::R3},
    {"options", Buttons::Options}, {"touchpad", Buttons::TouchPad},
};

std::optional<double> ParseNumber(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) {
        return std::nullopt;
    }
    return value;
}

std::vector<Step> ParseScript(const std::filesystem::path& script) {
    std::vector<Step> steps;
    std::ifstream file{script};
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream stream{line};
        std::vector<std::string> tokens;
        for (std::string word; stream >> word;) {
            tokens.push_back(std::move(word));
        }
        if (tokens.empty()) {
            continue;
        }
        Step step;
        size_t first = 0;
        // The two times come first, unless the line begins with a frame number or an input: a
        // frame-numbered line is timed by the title's own frames instead of the clock.
        if (const auto at = ParseNumber(tokens[0]); at && tokens.size() >= 2) {
            if (const auto hold = ParseNumber(tokens[1]); hold) {
                step.timing.start = *at;
                step.timing.end = *at + *hold;
                first = 2;
            }
        }
        for (size_t index = first; index < tokens.size(); ++index) {
            const std::string& token = tokens[index];
            if (Scripted::ParseFrameToken(token, step.timing.frame)) {
                continue;
            }
            if (Scripted::ParseFramesToken(token, step.timing.frames)) {
                continue;
            }
            if (const auto it = ButtonNames.find(token); it != ButtonNames.end()) {
                step.buttons |= it->second;
                continue;
            }
            if (token.starts_with("head=")) {
                step.head = ParsePose(token.substr(5));
                continue;
            }
            if (token.starts_with("pad=")) {
                step.pad = ParsePose(token.substr(4));
                continue;
            }
            if (token.starts_with("padrot=")) {
                step.pad_rotation = ParseRotation(ParseNumbers(token.substr(7)), 0);
                continue;
            }
            if (token == "hands=off") {
                step.hands_lost = true;
                continue;
            }
            if (token.starts_with("hands=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 3) {
                    step.hands = std::array<float, 4>{numbers[0], numbers[1], numbers[2],
                                                      numbers.size() > 3 ? numbers[3] : 0.0f};
                }
                continue;
            }
            if (token.starts_with("mic=")) {
                step.microphone = std::stof(token.substr(4));
                continue;
            }
            if (token == "recenter") {
                step.recenter = true;
                continue;
            }
            if (token == "worn=0" || token == "worn=1") {
                step.worn = token.back() == '1';
                continue;
            }
            if (token.starts_with("touch=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 2) {
                    step.touch = std::array<float, 2>{numbers[0], numbers[1]};
                }
                continue;
            }
            if (token.starts_with("finger=")) {
                const auto numbers = ParseNumbers(token.substr(7));
                if (numbers.size() >= 2) {
                    step.finger = std::array<float, 2>{numbers[0], numbers[1]};
                }
                continue;
            }
            if (token == "gesture=press" || token == "gesture=swipe" || token == "gesture=pull") {
                step.gestures.press = step.gestures.press || token == "gesture=press";
                step.gestures.swipe = step.gestures.swipe || token == "gesture=swipe";
                step.gestures.pull = step.gestures.pull || token == "gesture=pull";
                continue;
            }
            if (token == "place=switch") {
                step.place_switch = true;
                continue;
            }
            if (token.starts_with("place=")) {
                const auto numbers = ParseNumbers(token.substr(6));
                if (numbers.size() >= 3) {
                    step.place_move = Core::Vr::Vec3{numbers[0], numbers[1], numbers[2]};
                }
                continue;
            }
            // "move0=<buttons>,<trigger>" / "move1=...": the title's own view of the hand's
            // buttons, for a scripted run that needs exact bits (decimal or 0x hex).
            static constexpr std::array<std::string_view, 2> MoveNames = {"move0=", "move1="};
            bool move_parsed = false;
            for (size_t hand = 0; hand < MoveNames.size() && !move_parsed; ++hand) {
                if (!token.starts_with(MoveNames[hand])) {
                    continue;
                }
                const std::string value = token.substr(MoveNames[hand].size());
                const char* at = value.c_str();
                char* end = nullptr;
                const unsigned long buttons = std::strtoul(at, &end, 0);
                if (end == at || *end != ',') {
                    continue;
                }
                at = end + 1;
                const unsigned long trigger = std::strtoul(at, &end, 0);
                if (end == at) {
                    continue;
                }
                step.move[hand] =
                    Step::MoveInput{static_cast<u16>(std::min<unsigned long>(buttons, 0xffff)),
                                    static_cast<u8>(std::min<unsigned long>(trigger, 0xff))};
                move_parsed = true;
            }
            if (move_parsed) {
                continue;
            }
            static constexpr std::array<std::string_view, 4> AxisNames = {
                "lx=", "ly=", "rx=", "ry="};
            for (size_t axis = 0; axis < AxisNames.size(); ++axis) {
                if (token.starts_with(AxisNames[axis])) {
                    step.sticks[axis] = std::stoi(token.substr(AxisNames[axis].size()));
                }
            }
        }
        steps.push_back(step);
    }
    return steps;
}

void Replay(std::vector<Step> steps, std::filesystem::path live_script) {
    Common::SetCurrentThreadName("shadPS4:ScriptedInput");
    const auto begin = std::chrono::steady_clock::now();
    double last_end = 0.0;
    for (const Step& step : steps) {
        last_end = std::max(last_end, step.timing.end);
    }

    // The script's poses are counted from where the player sits.
    Core::Vr::Runtime::Instance().FixSeat();
    std::vector<bool> started(steps.size());

    Buttons previous_buttons{Buttons::None};
    std::array<int, 6> previous_axes{128, 128, 128, 128, 0, 0};
    std::optional<std::array<float, 2>> previous_touch;
    StickFinger stick_finger;
    PadGestures gestures;
    std::filesystem::file_time_type live_stamp{};
    while (true) {
        if (!live_script.empty()) {
            std::error_code error;
            const auto stamp = std::filesystem::last_write_time(live_script, error);
            if (!error && stamp != live_stamp) {
                live_stamp = stamp;
                steps = ParseScript(live_script);
                started.resize(steps.size());
                LOG_INFO(Input, "Live script now has {} steps", steps.size());
            }
        }
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const std::uint64_t frame = Input::GuestFrame();

        Buttons buttons{Buttons::None};
        std::array<int, 6> axes{128, 128, 128, 128, 0, 0};
        const Step* head_step = nullptr;
        const Step* pad_step = nullptr;
        const Step* hands_step = nullptr;
        std::array<std::optional<Step::MoveInput>, 2> move_override;
        std::optional<std::array<float, 2>> touch;
        std::array<float, 2> finger{};
        PadGestures::Controls controls;
        float microphone = 0.0f;
        for (const Step& step : steps) {
            if (Scripted::Begun(step.timing, now, frame)) {
                // Poses persist, the most recently started line wins.
                if (step.head &&
                    (head_step == nullptr || Scripted::Later(step.timing, head_step->timing))) {
                    head_step = &step;
                }
                if ((step.pad || step.pad_rotation) &&
                    (pad_step == nullptr || Scripted::Later(step.timing, pad_step->timing))) {
                    pad_step = &step;
                }
                if ((step.hands || step.hands_lost) &&
                    (hands_step == nullptr || Scripted::Later(step.timing, hands_step->timing))) {
                    hands_step = &step;
                }
            }
            if (!Scripted::Active(step.timing, now, frame)) {
                continue;
            }
            if (step.touch) {
                touch = step.touch;
            }
            if (step.finger) {
                finger = *step.finger;
            }
            for (size_t hand = 0; hand < step.move.size(); ++hand) {
                if (step.move[hand]) {
                    move_override[hand] = step.move[hand];
                }
            }
            controls.press = controls.press || step.gestures.press;
            controls.swipe = controls.swipe || step.gestures.swipe;
            controls.pull = controls.pull || step.gestures.pull;
            microphone = std::max(microphone, step.microphone);
            buttons |= step.buttons;
            for (size_t axis = 0; axis < step.sticks.size(); ++axis) {
                if (step.sticks[axis] >= 0) {
                    axes[axis] = step.sticks[axis];
                }
            }
        }
        microphone_level.store(microphone, std::memory_order_relaxed);
        // What buttons do on the touchpad comes before what the stick does there.
        if (const auto made = gestures.Update(now, controls); made.down && !touch) {
            touch = std::array<float, 2>{made.x, made.y};
            if (made.pressed) {
                buttons |= Buttons::TouchPad;
            }
            stick_finger.Reset();
        } else if (const auto stick = stick_finger.Update(now, finger[0], finger[1]);
                   stick.down && !touch) {
            // (A stick at rest between its lines: that is what lifts its finger.)
            touch = std::array<float, 2>{stick.x, stick.y};
        }
        auto& vr = Core::Vr::Runtime::Instance();
        if (head_step != nullptr) {
            vr.UpdateHead({.pose = *head_step->head, .tracked = true});
        }
        for (size_t i = 0; i < steps.size(); ++i) {
            if (started[i] || !Scripted::Begun(steps[i].timing, now, frame)) {
                continue;
            }
            // Things that happen once, when their line starts.
            started[i] = true;
            if (steps[i].recenter) {
                // The player asks for the view to be reset, wherever the head is right now.
                vr.RequestRecenter();
            }
            if (steps[i].place_move) {
                vr.MoveOwnPadPlace(*steps[i].place_move);
            }
            if (steps[i].place_switch) {
                vr.SwitchPadPlace();
            }
            if (steps[i].worn) {
                LOG_INFO(Input, "Scripted: the headset is {}",
                         *steps[i].worn ? "put on" : "taken off");
                vr.SetHeadsetWorn(*steps[i].worn);
            }
        }
        if (pad_step != nullptr) {
            if (pad_step->pad) {
                vr.UpdatePad({.pose = *pad_step->pad, .tracked = true});
            } else {
                vr.UpdatePadOrientation(*pad_step->pad_rotation, {});
            }
        }

        if (hands_step != nullptr) {
            if (hands_step->hands) {
                const auto& hands = *hands_step->hands;
                vr.UpdatePadPosition({hands[0], hands[1], hands[2]}, {});
                vr.UpdatePadYawReference(hands[3] * std::numbers::pi_v<float> / 180.0f);
            } else {
                vr.ClearPadPosition();
            }
        }

        // A scripted Move controller beats the mapping from the gamepad while its line lasts;
        // without one the mapping (or the host) is what the title sees.
        for (size_t hand = 0; hand < move_override.size(); ++hand) {
            if (move_override[hand]) {
                vr.SetScriptedHandInput(static_cast<int>(hand), move_override[hand]->buttons,
                                        move_override[hand]->trigger);
            } else {
                vr.ClearScriptedHandInput(static_cast<int>(hand));
            }
        }

        // The analog triggers follow their digital buttons.
        axes[4] = True(buttons & Buttons::L2) ? 255 : 0;
        axes[5] = True(buttons & Buttons::R2) ? 255 : 0;

        if (buttons != previous_buttons || axes != previous_axes || touch != previous_touch) {
            auto* controllers = Common::Singleton<GameControllers>::Instance();
            (*controllers)[0]->ApplyRemoteState(buttons, axes, touch.has_value(),
                                                touch ? (*touch)[0] : 0.0f,
                                                touch ? (*touch)[1] : 0.0f);
            previous_buttons = buttons;
            previous_axes = axes;
            previous_touch = touch;
        }
        if (live_script.empty() && now > last_end + 1.0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    microphone_level.store(0.0f, std::memory_order_relaxed);
    LOG_INFO(Input, "Scripted input finished");
}

} // namespace

float ScriptedMicrophoneLevel() {
    // (0.36: a little above what ASTRO BOT takes for blowing as hard as can be.)
    return std::max(microphone_level.load(std::memory_order_relaxed),
                    blowing.load(std::memory_order_relaxed) ? 0.36f : 0.0f);
}

void SetBlowing(bool blowing_now) {
    if (blowing.exchange(blowing_now, std::memory_order_relaxed) != blowing_now && blowing_now) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Input,
                     "Blowing with buttons: the game hears it as blowing into the microphone");
        }
    }
}

void StartScriptedInput(const std::filesystem::path& script) {
    auto steps = ParseScript(script);
    if (steps.empty()) {
        LOG_ERROR(Input, "No usable steps in input script {}", script.string());
        return;
    }
    LOG_INFO(Input, "Replaying {} input steps from {}", steps.size(), script.string());
    const char* live = std::getenv("SHADPS4_LIVE_INPUT");
    std::thread{Replay, std::move(steps), std::filesystem::path{live ? live : ""}}.detach();
}

} // namespace Input
