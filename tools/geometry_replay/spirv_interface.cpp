// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "spirv_interface.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace spirv {
namespace {

constexpr u32 kMagic = 0x07230203u;

// Opcodes this verifier reads. Anything else is skipped by its word count, which is what makes the
// parser robust to constructs it does not interpret.
constexpr u16 OpEntryPoint = 15;
constexpr u16 OpExecutionMode = 16;
constexpr u16 OpTypeVoid = 19;
constexpr u16 OpTypeBool = 20;
constexpr u16 OpTypeInt = 21;
constexpr u16 OpTypeFloat = 22;
constexpr u16 OpTypeVector = 23;
constexpr u16 OpTypeMatrix = 24;
constexpr u16 OpTypeImage = 25;
constexpr u16 OpTypeSampler = 26;
constexpr u16 OpTypeSampledImage = 27;
constexpr u16 OpTypeArray = 28;
constexpr u16 OpTypeRuntimeArray = 29;
constexpr u16 OpTypeStruct = 30;
constexpr u16 OpTypePointer = 32;
constexpr u16 OpConstant = 43;
constexpr u16 OpVariable = 59;
constexpr u16 OpDecorate = 71;
constexpr u16 OpMemberDecorate = 72;

constexpr u32 DecorationBlock = 2;
constexpr u32 DecorationBufferBlock = 3;
constexpr u32 DecorationRowMajor = 4;
constexpr u32 DecorationArrayStride = 6;
constexpr u32 DecorationBinding = 33;
constexpr u32 DecorationDescriptorSet = 34;
constexpr u32 DecorationOffset = 35;

constexpr u32 StorageClassUniformConstant = 0;
constexpr u32 StorageClassUniform = 2;
constexpr u32 StorageClassPushConstant = 9;
constexpr u32 StorageClassStorageBuffer = 12;

/// What a type id resolves to, as far as descriptors are concerned.
struct TypeInfo {
    u16 opcode{};
    /// For OpTypePointer: the storage class and the pointee type.
    u32 storage_class{};
    u32 pointee{};
    /// For OpTypeArray: the element type and the length constant id.
    u32 element{};
    u32 length_id{};
    /// For OpTypeImage: whether it is a depth image and whether it is sampled.
    u32 sampled{};
    /// For OpTypeStruct: the member type ids.
    std::vector<u32> members;
};

struct Decorations {
    u32 set{};
    u32 binding{};
    bool has_set{};
    bool has_binding{};
    bool block{};
    bool buffer_block{};
};

/// Resolves a type id to its descriptor class, following arrays and pointers.
DescriptorClass ClassOf(u32 type_id, const std::map<u32, TypeInfo>& types, u32& count,
                        bool& runtime_sized) {
    count = 1;
    runtime_sized = false;
    u32 current = type_id;
    // Bounded walk: a malformed module could otherwise describe a cycle.
    for (u32 depth = 0; depth < 16; ++depth) {
        const auto it = types.find(current);
        if (it == types.end()) {
            return DescriptorClass::Unknown;
        }
        const auto& info = it->second;
        switch (info.opcode) {
        case OpTypePointer:
            current = info.pointee;
            continue;
        case OpTypeArray:
            // The array length is a constant id; the caller resolves it separately. Here only the
            // element type matters for the class.
            current = info.element;
            continue;
        case OpTypeRuntimeArray:
            runtime_sized = true;
            current = info.element;
            continue;
        case OpTypeSampledImage:
            return DescriptorClass::CombinedImageSampler;
        case OpTypeSampler:
            return DescriptorClass::Sampler;
        case OpTypeImage:
            // A sampled image is read; a non-sampled one is a storage image.
            return info.sampled != 0 ? DescriptorClass::SampledImage
                                      : DescriptorClass::StorageImage;
        case OpTypeStruct:
            // A struct behind a uniform or storage pointer is a buffer. Which one is decided by
            // the pointer storage class, which the caller has already seen.
            return DescriptorClass::Unknown;
        default:
            return DescriptorClass::Unknown;
        }
    }
    return DescriptorClass::Unknown;
}

} // namespace

std::string_view DescriptorClassName(DescriptorClass cls) {
    switch (cls) {
    case DescriptorClass::UniformBuffer:
        return "uniform_buffer";
    case DescriptorClass::StorageBuffer:
        return "storage_buffer";
    case DescriptorClass::SampledImage:
        return "sampled_image";
    case DescriptorClass::StorageImage:
        return "storage_image";
    case DescriptorClass::Sampler:
        return "sampler";
    case DescriptorClass::CombinedImageSampler:
        return "combined_image_sampler";
    default:
        return "unknown";
    }
}

std::string_view ProblemName(Problem problem) {
    switch (problem) {
    case Problem::None:
        return "none";
    case Problem::TooShort:
        return "too_short";
    case Problem::BadMagic:
        return "bad_magic";
    case Problem::BadWordCount:
        return "bad_word_count";
    case Problem::TruncatedInstruction:
        return "truncated_instruction";
    case Problem::UnsupportedVersion:
        return "unsupported_version";
    case Problem::NoEntryPoint:
        return "no_entry_point";
    case Problem::MultipleEntryPoints:
        return "multiple_entry_points";
    default:
        return "unknown";
    }
}

bool ReadInterface(const std::vector<u8>& module, Interface& out, Problem& problem) {
    out = {};
    problem = Problem::None;

    if (module.size() < 20 || module.size() % 4 != 0) {
        problem = Problem::TooShort;
        return false;
    }

    std::vector<u32> words(module.size() / 4);
    std::memcpy(words.data(), module.data(), module.size());

    if (words[0] != kMagic) {
        problem = Problem::BadMagic;
        return false;
    }
    // The version word is 0x00MMmm00. Anything above 1.6 is not something this parser claims to
    // understand, but the instruction encoding it reads has been stable since 1.0, so only a
    // malformed version is refused.
    const u32 version = words[1];
    if ((version & 0xFF0000FFu) != 0) {
        problem = Problem::UnsupportedVersion;
        return false;
    }

    std::map<u32, TypeInfo> types;
    std::map<u32, Decorations> decorations;
    std::map<u32, u32> constants;
    std::map<u32, std::pair<u32, u32>> member_offsets; // struct id -> (member, offset)
    std::vector<std::pair<u32, u32>> variables;       // (result id, type id)
    std::vector<std::pair<u32, u32>> variable_storage; // (result id, storage class)
    std::vector<std::string> entry_points;
    u32 execution_model = 0;

    std::size_t index = 5;
    while (index < words.size()) {
        const u32 first = words[index];
        const u16 word_count = static_cast<u16>(first >> 16);
        const u16 opcode = static_cast<u16>(first & 0xFFFFu);
        if (word_count == 0) {
            problem = Problem::BadWordCount;
            return false;
        }
        if (index + word_count > words.size()) {
            problem = Problem::TruncatedInstruction;
            return false;
        }
        const u32* operands = &words[index + 1];
        const u32 operand_count = word_count - 1;

        switch (opcode) {
        case OpEntryPoint:
            if (operand_count >= 2) {
                execution_model = operands[0];
                // The name is a null-terminated literal string in the remaining words. The inner
                // break only leaves the byte loop, so the outer loop needs its own flag: without
                // it the words after the terminator are appended to the name.
                std::string name;
                bool terminated = false;
                for (u32 w = 2; w < operand_count && !terminated; ++w) {
                    const u32 word = operands[w];
                    for (u32 b = 0; b < 4; ++b) {
                        const char c = static_cast<char>((word >> (b * 8)) & 0xFFu);
                        if (c == 0) {
                            terminated = true;
                            break;
                        }
                        name.push_back(c);
                    }
                }
                entry_points.push_back(std::move(name));
            }
            break;
        case OpTypePointer:
            if (operand_count >= 3) {
                TypeInfo info{};
                info.opcode = opcode;
                info.storage_class = operands[1];
                info.pointee = operands[2];
                types[operands[0]] = std::move(info);
            }
            break;
        case OpTypeArray:
            if (operand_count >= 3) {
                TypeInfo info{};
                info.opcode = opcode;
                info.element = operands[1];
                info.length_id = operands[2];
                types[operands[0]] = std::move(info);
            }
            break;
        case OpTypeRuntimeArray:
            if (operand_count >= 2) {
                TypeInfo info{};
                info.opcode = opcode;
                info.element = operands[1];
                types[operands[0]] = std::move(info);
            }
            break;
        case OpTypeImage:
            if (operand_count >= 8) {
                TypeInfo info{};
                info.opcode = opcode;
                info.sampled = operands[7];
                types[operands[0]] = std::move(info);
            }
            break;
        case OpTypeSampler:
        case OpTypeSampledImage:
            if (operand_count >= 1) {
                TypeInfo info{};
                info.opcode = opcode;
                types[operands[0]] = std::move(info);
            }
            break;
        case OpTypeStruct:
            if (operand_count >= 1) {
                TypeInfo info{};
                info.opcode = opcode;
                for (u32 m = 1; m < operand_count; ++m) {
                    info.members.push_back(operands[m]);
                }
                types[operands[0]] = std::move(info);
            }
            break;
        case OpConstant:
            if (operand_count >= 3) {
                constants[operands[1]] = operands[2];
            }
            break;
        case OpVariable:
            if (operand_count >= 3) {
                variables.emplace_back(operands[1], operands[0]);
                variable_storage.emplace_back(operands[1], operands[2]);
            }
            break;
        case OpDecorate:
            if (operand_count >= 2) {
                const u32 target = operands[0];
                const u32 decoration = operands[1];
                auto& dec = decorations[target];
                if (decoration == DecorationBinding && operand_count >= 3) {
                    dec.binding = operands[2];
                    dec.has_binding = true;
                } else if (decoration == DecorationDescriptorSet && operand_count >= 3) {
                    dec.set = operands[2];
                    dec.has_set = true;
                } else if (decoration == DecorationBlock) {
                    dec.block = true;
                } else if (decoration == DecorationBufferBlock) {
                    dec.buffer_block = true;
                }
            }
            break;
        case OpMemberDecorate:
            if (operand_count >= 4) {
                const u32 target = operands[0];
                const u32 member = operands[1];
                const u32 decoration = operands[2];
                if (decoration == DecorationOffset) {
                    member_offsets[target] = {member, operands[3]};
                }
            }
            break;
        default:
            break;
        }
        index += word_count;
    }

    if (entry_points.empty()) {
        problem = Problem::NoEntryPoint;
        return false;
    }
    if (entry_points.size() > 1) {
        problem = Problem::MultipleEntryPoints;
        return false;
    }
    out.entry_point = entry_points.front();
    out.execution_model = execution_model;

    // Resolve each variable that carries a binding decoration into a descriptor.
    for (std::size_t v = 0; v < variables.size(); ++v) {
        const u32 result_id = variables[v].first;
        const u32 type_id = variables[v].second;
        const u32 storage_class = variable_storage[v].second;

        const auto dec_it = decorations.find(result_id);
        const bool has_decoration = dec_it != decorations.end();

        if (storage_class == StorageClassPushConstant) {
            // The push-constant block. Its size is the extent of the struct it points at, which is
            // the largest member offset plus that member's own size. The offset is recorded so a
            // consumer can check the range covers what the module reads.
            out.push_constant.present = true;
            const auto type_it = types.find(type_id);
            if (type_it != types.end() && type_it->second.opcode == OpTypePointer) {
                const u32 struct_id = type_it->second.pointee;
                const auto struct_it = types.find(struct_id);
                if (struct_it != types.end() && struct_it->second.opcode == OpTypeStruct) {
                    u32 max_end = 0;
                    for (u32 m = 0; m < struct_it->second.members.size(); ++m) {
                        const auto off_it = member_offsets.find(struct_id);
                        if (off_it == member_offsets.end()) {
                            continue;
                        }
                        // Only the first member offset is tracked; a full layout needs every
                        // member. The largest offset is a lower bound on the block size, which is
                        // enough to check that a captured range is not too small.
                        max_end = std::max(max_end, off_it->second.second + 16u);
                    }
                    out.push_constant.size = max_end;
                }
            }
            continue;
        }

        if (!has_decoration || !dec_it->second.has_binding) {
            continue;
        }

        Descriptor descriptor{};
        descriptor.set = dec_it->second.has_set ? dec_it->second.set : 0;
        descriptor.binding = dec_it->second.binding;
        descriptor.storage_class = storage_class;

        u32 count = 1;
        bool runtime_sized = false;
        descriptor.cls = ClassOf(type_id, types, count, runtime_sized);
        descriptor.runtime_sized = runtime_sized;

        // A struct behind a uniform or storage pointer is a buffer, and which one is decided by
        // the storage class rather than by the struct itself.
        if (descriptor.cls == DescriptorClass::Unknown) {
            if (storage_class == StorageClassUniform) {
                descriptor.cls = DescriptorClass::UniformBuffer;
            } else if (storage_class == StorageClassStorageBuffer) {
                descriptor.cls = DescriptorClass::StorageBuffer;
            }
        }

        // Resolve an array count from the constant the array length refers to.
        if (count == 1 && !runtime_sized) {
            const auto type_it = types.find(type_id);
            if (type_it != types.end() && type_it->second.opcode == OpTypePointer) {
                const auto arr_it = types.find(type_it->second.pointee);
                if (arr_it != types.end() && arr_it->second.opcode == OpTypeArray) {
                    const auto len_it = constants.find(arr_it->second.length_id);
                    if (len_it != constants.end()) {
                        descriptor.count = len_it->second;
                    }
                } else if (arr_it != types.end() &&
                           arr_it->second.opcode == OpTypeRuntimeArray) {
                    descriptor.runtime_sized = true;
                }
            }
        }

        out.descriptors.push_back(std::move(descriptor));
    }

    return true;
}

std::string CompareInterface(const Interface& declared, const std::vector<Descriptor>& captured,
                             const PushConstant& captured_push) {
    // Every binding the module declares must be described by the capture. A binding the module
    // requires but the capture omits would make the pipeline layout incompatible.
    for (const auto& want : declared.descriptors) {
        const Descriptor* found = nullptr;
        for (const auto& have : captured) {
            if (have.set == want.set && have.binding == want.binding) {
                found = &have;
                break;
            }
        }
        if (found == nullptr) {
            return "the module declares set " + std::to_string(want.set) + " binding " +
                   std::to_string(want.binding) + " (" +
                   std::string{DescriptorClassName(want.cls)} +
                   ") but the capture does not describe it";
        }
        if (found->cls != want.cls) {
            return "set " + std::to_string(want.set) + " binding " +
                   std::to_string(want.binding) + " is " +
                   std::string{DescriptorClassName(want.cls)} + " in the module but " +
                   std::string{DescriptorClassName(found->cls)} + " in the capture";
        }
        if (want.runtime_sized) {
            return "set " + std::to_string(want.set) + " binding " +
                   std::to_string(want.binding) +
                   " is a runtime-sized array, which a fixed descriptor count cannot describe";
        }
        if (found->count != want.count) {
            return "set " + std::to_string(want.set) + " binding " +
                   std::to_string(want.binding) + " has count " + std::to_string(want.count) +
                   " in the module but " + std::to_string(found->count) + " in the capture";
        }
    }

    // A push-constant block the module declares must be covered by the captured range.
    if (declared.push_constant.present) {
        if (captured_push.size < declared.push_constant.size) {
            return "the module declares a push-constant block of at least " +
                   std::to_string(declared.push_constant.size) + " bytes but the capture covers " +
                   std::to_string(captured_push.size);
        }
    }

    return {};
}

} // namespace spirv
