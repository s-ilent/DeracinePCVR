// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The bridge between an original UE4 RHI index-buffer object and the guest GPU allocation the
// draw consumed.
//
// The two are different namespaces. The semantic tape records the RHI invocation argument, which is
// an original UE4 object pointer. The resource package records a guest GPU address. Comparing those
// numbers directly is meaningless, which is why JoinTapeToPackage refuses to.
//
// This header defines the record that carries the recovered relationship between them, so the join
// can compare two values that are genuinely in the same namespace.

#pragma once

#include <cstdint>
#include <string_view>

namespace Core::IndexIdentity {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/// Opt-in for the original index-resource observer. Default OFF.
inline constexpr std::string_view EnableEnvironmentVariable = "SHADPS4_DERACINE_INDEX_IDENTITY";

/// Bounded number of retained identity records.
inline constexpr size_t MaxIdentityRecords = 256;

/// How strongly a field is supported. Every field carries one, so a reader can tell a disassembly
/// fact from an inference without reading the report.
enum class EvidenceClass : u8 {
    /// Not observed at all.
    None = 0,
    /// Read from the target binary: an instruction that loads or stores the field.
    StaticVerified = 1,
    /// Read from guest memory at runtime, at the offset the static analysis identified.
    RuntimeObserved = 2,
    /// Plausible by shape only. Never used to claim a match.
    Inferred = 3,
};

[[nodiscard]] constexpr std::string_view EvidenceClassName(EvidenceClass cls) noexcept {
    switch (cls) {
    case EvidenceClass::StaticVerified:
        return "static_verified";
    case EvidenceClass::RuntimeObserved:
        return "runtime_observed";
    case EvidenceClass::Inferred:
        return "inferred";
    default:
        return "none";
    }
}

/// The object layout recovered from the indexed-draw implementation at 0x0CBB9C0.
///
/// The RHI receives an interface pointer in its second argument. The resource base is that pointer
/// minus 0x68, which the implementation computes with `leaq -0x68(%rsi), %rdx`. Every field below
/// is relative to that base.
///
/// The GPU address is not at a single fixed offset. The implementation selects between +0x20 and
/// +0x40 by a thread-identity check, so both are recorded and the selected one is named. Recording
/// only one would be wrong for whichever thread took the other path.
struct ObjectLayout {
    /// The interface pointer the RHI invocation was given.
    static constexpr u32 InterfaceOffset = 0x68;
    /// The two candidate GPU-address slots. The implementation picks one per draw.
    static constexpr u32 AddressOffsetA = 0x20;
    static constexpr u32 AddressOffsetB = 0x40;
    /// The index element width indicator: zero means 16-bit, non-zero means 32-bit.
    static constexpr u32 ElementWidthOffset = 0x88;
};

/// Which address slot the implementation selected for this draw.
enum class AddressSlot : u8 {
    Unknown = 0,
    /// The +0x20 slot.
    Primary = 1,
    /// The +0x40 slot.
    Secondary = 2,
};

[[nodiscard]] constexpr std::string_view AddressSlotName(AddressSlot slot) noexcept {
    switch (slot) {
    case AddressSlot::Primary:
        return "primary_0x20";
    case AddressSlot::Secondary:
        return "secondary_0x40";
    default:
        return "unknown";
    }
}

/// One recovered identity for an indexed draw.
///
/// Presence flags are explicit because a zero address is a legitimate value and cannot double as
/// "unknown". An allocation address alone is not a unique resource identity: guest memory can be
/// reused, so the generation field is what distinguishes two resources that occupied the same
/// address at different times.
struct OriginalIndexResourceIdentity {
    /// The execution this record belongs to. Two records from different executions are never the
    /// same draw, however well their fields agree.
    u64 execution_id{};
    /// The provenance record id the draw was captured under, within that execution.
    u64 provenance_id{};

    /// The original UE4 RHI object pointer the invocation was given.
    u64 original_rhi_object{};
    bool has_original_rhi_object{};
    /// The resource base the implementation derives from it (object - 0x68).
    u64 resource_base{};
    bool has_resource_base{};

    /// The guest GPU base address the draw consumed.
    u64 guest_allocation_base{};
    bool has_guest_allocation_base{};
    /// Which slot the address came from.
    AddressSlot address_slot{AddressSlot::Unknown};

    /// The allocation size, when it could be recovered. Unknown stays unknown.
    u64 guest_allocation_size{};
    bool has_guest_allocation_size{};

    /// The byte offset into the allocation the draw starts at (StartIndex * element width).
    u64 allocation_offset{};
    bool has_allocation_offset{};

    /// The index element width: 2 or 4.
    u32 index_element_width{};
    bool has_index_element_width{};

    /// A generation counter for the allocation. Two records with the same address but different
    /// generations are different resources.
    u64 resource_generation{};
    bool has_resource_generation{};

    /// How strongly the address field is supported.
    EvidenceClass address_evidence{EvidenceClass::None};
    /// How strongly the size field is supported.
    EvidenceClass size_evidence{EvidenceClass::None};
};

/// Whether a record carries enough to compare against a package.
[[nodiscard]] constexpr bool IsComparable(const OriginalIndexResourceIdentity& identity) noexcept {
    return identity.has_guest_allocation_base && identity.guest_allocation_base != 0;
}

/// Whether two records are from the same execution.
[[nodiscard]] constexpr bool SameExecution(const OriginalIndexResourceIdentity& a,
                                           const OriginalIndexResourceIdentity& b) noexcept {
    return a.execution_id != 0 && a.execution_id == b.execution_id;
}

/// Why a bridge comparison could not be made.
enum class BridgeOutcome : u8 {
    /// The addresses agree and the ranges are compatible.
    Agreed = 0,
    /// The addresses disagree.
    Disagreed = 1,
    /// The record does not carry a comparable address.
    NoAddress = 2,
    /// The two records are from different executions.
    DifferentExecution = 3,
    /// The address agrees but the generation differs, so the memory was reused.
    GenerationMismatch = 4,
    /// The draw reads outside the resolved allocation.
    OutOfRange = 5,
};

[[nodiscard]] constexpr std::string_view BridgeOutcomeName(BridgeOutcome outcome) noexcept {
    switch (outcome) {
    case BridgeOutcome::Agreed:
        return "agreed";
    case BridgeOutcome::Disagreed:
        return "disagreed";
    case BridgeOutcome::NoAddress:
        return "no_address";
    case BridgeOutcome::DifferentExecution:
        return "different_execution";
    case BridgeOutcome::GenerationMismatch:
        return "generation_mismatch";
    case BridgeOutcome::OutOfRange:
        return "out_of_range";
    default:
        return "unknown";
    }
}

/// Compares a recovered identity against the package's guest allocation.
///
/// `package_base` and `package_size` are the package's index binding. `required_bytes` is the byte
/// range the draw reads, so a range that runs past the allocation is refused rather than accepted
/// on an address match alone.
[[nodiscard]] inline BridgeOutcome CompareToPackage(
    const OriginalIndexResourceIdentity& identity, u64 package_execution_id, u64 package_base,
    u64 package_size, u64 required_bytes) noexcept {
    if (!IsComparable(identity)) {
        return BridgeOutcome::NoAddress;
    }
    // A different execution is decisive: the same address in two processes is not the same
    // resource, and no other field can repair that.
    if (identity.execution_id != 0 && package_execution_id != 0 &&
        identity.execution_id != package_execution_id) {
        return BridgeOutcome::DifferentExecution;
    }
    if (identity.guest_allocation_base != package_base) {
        return BridgeOutcome::Disagreed;
    }
    // The address agrees. A generation mismatch means the memory was reused for a different
    // resource, so the address match is a coincidence rather than an identity.
    if (identity.has_resource_generation && identity.resource_generation != 0) {
        // The package does not carry a generation, so a non-zero generation cannot be confirmed.
        // It is reported rather than assumed equal.
        return BridgeOutcome::GenerationMismatch;
    }
    // The draw must read inside the allocation.
    if (package_size != 0 && required_bytes != 0) {
        const u64 start = identity.has_allocation_offset ? identity.allocation_offset : 0;
        if (start > package_size || required_bytes > package_size - start) {
            return BridgeOutcome::OutOfRange;
        }
    }
    return BridgeOutcome::Agreed;
}

} // namespace Core::IndexIdentity
