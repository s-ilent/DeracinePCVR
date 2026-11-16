// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The libc_internal test target exercises the C++ runtime registration only; the remaining
// library groups bring their own dependencies and are not under test here.

#include "core/loader/symbols_resolver.h"

namespace Libraries::LibcInternal {

void RegisterlibSceLibcInternalMath(Core::Loader::SymbolsResolver*) {}
void RegisterlibSceLibcInternalStr(Core::Loader::SymbolsResolver*) {}
void RegisterlibSceLibcInternalMemory(Core::Loader::SymbolsResolver*) {}
void RegisterlibSceLibcInternalIo(Core::Loader::SymbolsResolver*) {}
void ForceRegisterlibSceLibcInternalIo(Core::Loader::SymbolsResolver*) {}
void RegisterlibSceLibcInternalThreads(Core::Loader::SymbolsResolver*) {}

} // namespace Libraries::LibcInternal
