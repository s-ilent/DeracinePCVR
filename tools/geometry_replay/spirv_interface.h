// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Independent SPIR-V descriptor-interface verifier.
//
// This module reads a SPIR-V module and extracts the descriptor declarations it actually makes:
// the set and binding decorations, the descriptor class of each variable, its array count, and the
// push-constant block. It exists so the captured interface can be checked against the module rather
// than trusted because it came from the production pipeline path.
//
// It is deliberately small and handles only the constructs a shader module uses for descriptors:
// OpDecorate, OpMemberDecorate, OpVariable, OpTypePointer, OpTypeStruct, OpTypeImage,
// OpTypeSampler, OpTypeSampledImage, OpTypeArray, OpTypeRuntimeArray, OpConstant, OpEntryPoint and
// OpExecutionMode. Anything outside that set is reported rather than guessed at.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace spirv {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// What a descriptor variable is, as the module declares it.
enum class DescriptorClass : u8 {
    Unknown = 0,
    UniformBuffer = 1,
    StorageBuffer = 2,
    SampledImage = 3,
    StorageImage = 4,
    Sampler = 5,
    CombinedImageSampler = 6,
};

[[nodiscard]] std::string_view DescriptorClassName(DescriptorClass cls);

/// One descriptor declaration found in the module.
struct Descriptor {
    u32 set{};
    u32 binding{};
    DescriptorClass cls{DescriptorClass::Unknown};
    u32 count{1};
    /// The storage class the variable was declared in, so a push constant can be distinguished
    /// from a descriptor.
    u32 storage_class{};
    /// Whether the module declares this binding as a runtime-sized array, which a fixed descriptor
    /// count cannot describe.
    bool runtime_sized{};
};

/// The push-constant block the module declares.
struct PushConstant {
    bool present{};
    u32 offset{};
    u32 size{};
};

/// What the module declares.
struct Interface {
    std::vector<Descriptor> descriptors;
    PushConstant push_constant{};
    /// The entry point name, when the module declares exactly one.
    std::string entry_point;
    /// The execution model of that entry point.
    u32 execution_model{};
};

/// Why a module could not be read.
enum class Problem : u8 {
    None = 0,
    TooShort,
    BadMagic,
    BadWordCount,
    TruncatedInstruction,
    UnsupportedVersion,
    NoEntryPoint,
    MultipleEntryPoints,
};

[[nodiscard]] std::string_view ProblemName(Problem problem);

/// Reads the descriptor interface a SPIR-V module declares.
///
/// Returns false and sets `problem` when the module cannot be read. A module that reads but
/// declares nothing is not an error: a shader with no resources is legal.
[[nodiscard]] bool ReadInterface(const std::vector<u8>& module, Interface& out, Problem& problem);

/// Compares a captured interface against what the module declares.
///
/// Returns an empty string when they agree, or a description of the first disagreement. The
/// comparison is deliberately strict about the things that would make a pipeline incompatible:
/// a binding the module requires but the capture does not describe, a class mismatch, an array
/// count mismatch, and a push-constant range that does not cover what the module reads.
[[nodiscard]] std::string CompareInterface(const Interface& declared,
                                           const std::vector<Descriptor>& captured,
                                           const PushConstant& captured_push);

} // namespace spirv
