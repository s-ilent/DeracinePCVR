// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>

namespace Input {

/// Replays controller input from a text file onto the first controller, for unattended test
/// runs. Each line is "<start seconds> <hold seconds> <input>..." where an input is a button
/// name (cross, circle, square, triangle, up, down, left, right, l1, r1, l2, r2, l3, r3,
/// options, touchpad) or an axis assignment (lx=, ly=, rx=, ry= in 0..255, 128 is centred).
/// For VR titles a line may also carry "head=x,y,z[,yaw,pitch,roll]" and
/// "pad=x,y,z[,yaw,pitch,roll]" (metres and degrees, relative to the resting head position) or
/// "padrot=yaw,pitch,roll"; those poses stay in effect until a later line replaces them.
/// "hands=x,y,z[,heading]" stands for a host that sees where the controller is held but not how
/// it is turned ("hands=off" for losing sight of it), "touch=x,y" (0..1) for a finger on the
/// touchpad while the line lasts, "finger=x,y" (-1..1) for the right stick as it moves that
/// finger, "gesture=press", "gesture=swipe" or "gesture=pull" for the buttons that do the
/// touchpad's gestures on a controller without one, held while the line lasts,
/// "mic=level" for noise of that loudness in the microphone
/// (root mean square, 1 is full scale: 0.35 is what a title takes for blowing hard),
/// "recenter" for the player asking for the view to be reset where the head is at that moment,
/// "worn=0" / "worn=1" for the headset being taken off and put back on.
/// Lines starting with '#' are ignored. Times count from when the script is started.
/// A line may also start on a guest frame ("frame=<n>", optionally with "frames=<m>" for how
/// many frames it lasts), which follows the title's own frame count instead of the clock; the
/// two kinds mix freely. The script ends once every clock-numbered line has run out and every
/// frame-numbered line has reached its frame (plus its frames= count, if any).
///
/// SHADPS4_INPUT_SCRIPT names the script (unset: none is loaded and nothing runs).
/// SHADPS4_SCRIPT_FRAME_STALL_SECONDS bounds how long the guest frame counter may stand still
/// while frame-numbered lines are still pending: once it has not moved for that many seconds the
/// script is given up on, its inputs released and a warning logged. Default 30; 0 (or less)
/// disables the watchdog; a value that is not a number falls back to the default. Only read when
/// a script is started.
void StartScriptedInput(const std::filesystem::path& script);

/// Stops the replay thread, if one is running, and joins it: shutdown calls this before the VR
/// runtime and the controllers the script reads are taken down. The wait is bounded (2 s); a
/// thread that does not stop in time is left detached and a warning is logged, rather than
/// deadlocking the exit. Safe to call when no script ever ran, and more than once.
void StopScriptedInput();

/// The loudness a running script wants the microphone to hear right now, 0 for none. Also
/// what a title takes for blowing hard while the player holds the buttons that stand for it
/// (SetBlowing).
float ScriptedMicrophoneLevel();

/// The player blows without a microphone: with buttons (the PS button and square on a gamepad,
/// X and Y together on a headset's controllers), for where the microphone is not to be had,
/// gives too little, or blowing is not what the player wants to do.
void SetBlowing(bool blowing);

} // namespace Input
