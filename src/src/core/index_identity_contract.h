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

/// The two globals the slot predicate reads, as RVAs in the approved executable.
///
/// Both are verified from the disassembly at 0xCBBCCE-0xCBBCF1:
///   0x44AC748  the global thread record pointer
///   0x44AC740  the global flag
inline constexpr u32 ThreadRecordGlobalRva = 0x44AC748;
inline constexpr u32 ThreadFlagGlobalRva = 0x44AC740;

/// The offset within the thread record the predicate compares against.
///
/// Verified at 0xCBBCCEE: `cmpl 0x44(%rcx), %eax`.
inline constexpr u32 ThreadRecordIdOffset = 0x44;

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

/// The thread-identity predicate the indexed-draw implementation uses to choose an address slot.
///
/// Recovered from 0xCBBCCE-0xCBBCF1:
///
///   cbbcce:  cmpq  $0x0, 0x44ac748        ; a global thread record
///   cbbcd6:  je    0xcbbcf3               ; null -> slot A
///   cbbcd8:  movl  0x44ac740, %eax        ; a global flag
///   cbbcde:  testl %eax, %eax
///   cbbce0:  jne   0xcbbcf3               ; flag set -> slot A
///   cbbce2:  callq 0x33c6770              ; scePthreadGetthreadid
///   cbbce7:  movq  0x44ac748, %rcx        ; the global thread record
///   cbbcee:  cmpl  0x44(%rcx), %eax       ; compare against its recorded id
///   cbbcf1:  jne   0xcbbcf9               ; differ -> slot B
///   cbbcf3:  addq  $0x20, %rbx            ; slot A
///   cbbcf9:  addq  $0x40, %rbx            ; slot B
///
/// So slot A is selected when the global record is null, when the global flag is non-zero, or when
/// the current guest thread id equals the id recorded at record+0x44. Slot B is selected only when
/// the record is non-null, the flag is zero, and the ids differ.
///
/// `scePthreadGetthreadid` returns `g_curthread->tid`, which is exactly what CurrentGuestThreadId()
/// reads, so the comparison is reproducible without a host thread identity.
struct SlotPredicate {
    /// The guest thread id the comparison observed.
    u32 current_thread_id{};
    bool has_current_thread_id{};
    /// The id recorded in the global record at +0x44.
    u32 recorded_thread_id{};
    bool has_recorded_thread_id{};
    /// Whether the global record pointer was non-null.
    bool record_present{};
    /// The global flag at 0x44ac740.
    u32 flag{};
    bool has_flag{};
    /// Whether every input needed to evaluate the predicate was readable.
    bool determinable{};
};

/// Evaluates the recovered predicate.
///
/// Returns Unknown when an input is missing, so a caller cannot fall back to a guess. The predicate
/// is the game's own selection rule; substituting "first nonzero slot" would record an address the
/// draw may not have consumed.
[[nodiscard]] inline AddressSlot EvaluateSlotPredicate(const SlotPredicate& predicate) noexcept {
    if (!predicate.determinable) {
        return AddressSlot::Unknown;
    }
    // A null record selects slot A.
    if (!predicate.record_present) {
        return AddressSlot::Primary;
    }
    // A non-zero flag selects slot A.
    if (!predicate.has_flag) {
        return AddressSlot::Unknown;
    }
    if (predicate.flag != 0) {
        return AddressSlot::Primary;
    }
    // Otherwise the thread ids decide.
    if (!predicate.has_current_thread_id || !predicate.has_recorded_thread_id) {
        return AddressSlot::Unknown;
    }
    return predicate.current_thread_id == predicate.recorded_thread_id ? AddressSlot::Primary
                                                                      : AddressSlot::Secondary;
}

/// One recovered identity for an indexed draw.
///
/// Presence flags are explicit because a zero address is a legitimate value and cannot double as
/// "unknown". An allocation address alone is not a unique resource identity: guest memory can be
/// reused, so the generation field is what distinguishes two resources that occupied the same
/// address at different times.
struct OriginalIndexResourceIdentity {
    /// The capture session this record belongs to. Generated once per emulator execution and
    /// shared by every exporter, so artifacts can be proven to claim the same execution.
    u64 session_id{};
    bool has_session_id{};
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

    /// Both address candidates, recorded separately so they are never conflated.
    u64 address_candidate_a{};
    bool has_address_candidate_a{};
    u64 address_candidate_b{};
    bool has_address_candidate_b{};

    /// The predicate inputs and its verdict.
    SlotPredicate predicate{};

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