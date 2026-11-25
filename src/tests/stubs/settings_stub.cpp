// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The two symbols the logging stack needs that a test binary has no reason to provide.
//
// `shader_interface_capture.cpp` logs at install and export, which pulls in the logging stack, which
// pulls in the emulator settings and the filesystem paths. The resolution path under test logs
// nothing, so these stubs are enough to link it without dragging the emulator in.

#include <filesystem>
#include <memory>

#include "common/path_util.h"
#include "core/emulator_settings.h"

namespace Common::FS {

const std::filesystem::path& GetUserPath(PathType type) {
    static const std::filesystem::path empty{};
    return empty;
}

} // namespace Common::FS

std::shared_ptr<EmulatorSettingsImpl> EmulatorSettingsImpl::GetInstance() {
    return nullptr;
}
