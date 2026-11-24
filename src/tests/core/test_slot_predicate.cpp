// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Tests for the recovered thread-dependent address-slot predicate.
//
// The defect these exist to prevent: selecting the first nonzero address slot instead of
// reproducing the game's own selection rule. A nonzero but unselected slot is not evidence of the
// buffer the draw consumed.

#include <gtest/gtest.h>

#include "core/index_identity_contract.h"

namespace {

using Core::IndexIdentity::AddressSlot;
using Core::IndexIdentity::AddressSlotName;
using Core::IndexIdentity::EvaluateSlotPredicate;
using Core::IndexIdentity::SlotPredicate;
using Core::IndexIdentity::u32;

/// A predicate whose inputs are all present and whose ids agree, so slot A is selected.
SlotPredicate MakeAgreeing() {
    SlotPredicate predicate{};
    predicate.record_present = true;
    predicate.has_flag = true;
    predicate.flag = 0;
    predicate.has_current_thread_id = true;
    predicate.current_thread_id = 36;
    predicate.has_recorded_thread_id = true;
    predicate.recorded_thread_id = 36;
    predicate.determinable = true;
    return predicate;
}

// ---------------------------------------------------------------------------------------------
// The predicate itself
// ---------------------------------------------------------------------------------------------

TEST(SlotPredicate, ANullRecordSelectsSlotA) {
    // cbbcce: cmpq $0x0, 0x44ac748 / cbbcd6: je 0xcbbcf3 (slot A).
    SlotPredicate predicate{};
    predicate.record_present = false;
    predicate.determinable = true;
    EXPECT_EQ(EvaluateSlotPredicate(predicate), AddressSlot::Primary);
}

TEST(SlotPredicate, ANonzeroFlagSelectsSlotA) {
    // cbbcd8-cbbce0: a non-zero flag jumps to slot A before the thread comparison.
    auto predicate = MakeAgreeing();
    predicate.flag = 1;
    predicate.current_thread_id = 99;
    predicate.recorded_thread_id = 36;
    EXPECT_EQ(EvaluateSlotPredicate(predicate), AddressSlot::Primary);
}

TEST(SlotPredicate, MatchingThreadIdsSelectSlotA) {
    // cbbcee: cmpl 0x44(%rcx), %eax / cbbcf1: jne slot B. Equal ids fall through to slot A.
    EXPECT_EQ(EvaluateSlotPredicate(MakeAgreeing()), AddressSlot::Primary);
}

TEST(SlotPredicate, DifferingThreadIdsSelectSlotB) {
    auto predicate = MakeAgreeing();
    predicate.current_thread_id = 99;
    EXPECT_EQ(EvaluateSlotPredicate(predicate), AddressSlot::Secondary);
}

TEST(SlotPredicate, AMissingFlagIsUnknown) {
    // The flag decides before the ids, so without it the predicate cannot be evaluated.
    auto predicate = MakeAgreeing();
    predicate.has_flag = false;
    predicate.determinable = false;
    EXPECT_EQ(EvaluateSlotPredicate(predicate), AddressSlot::Unknown);
}

TEST(SlotPredicate, AMissingCurrentThreadIdIsUnknown) {
    auto predicate = MakeAgreeing();
    predicate.has_current_thread_id = false;
    predicate.determinable = false;
    EXPECT_EQ(EvaluateSlotPredicate(predicate), AddressSlot::Unknown);
}

TEST(SlotPredicate, AMissingRecordedThreadIdIsUnknown) {
    auto predicate = MakeAgreeing();
    predicate.has_recorded_thread_id = false;
    predicate.determinable = false;
    EXPECT_EQ(EvaluateSlotPredicate(predicate), AddressSlot::Unknown);
}

TEST(SlotPredicate, AnIndeterminablePredicateIsNeverAGuess) {
    // The critical property: an unknown predicate must not fall back to a slot.
    SlotPredicate predicate{};
    predicate.determinable = false;
    EXPECT_EQ(EvaluateSlotPredicate(predicate), AddressSlot::Unknown);
}

TEST(SlotPredicate, EverySlotHasAStableName) {
    EXPECT_EQ(AddressSlotName(AddressSlot::Primary), "primary_0x20");
    EXPECT_EQ(AddressSlotName(AddressSlot::Secondary), "secondary_0x40");
    EXPECT_EQ(AddressSlotName(AddressSlot::Unknown), "unknown");
}

// ---------------------------------------------------------------------------------------------
// The recovered layout constants
// ---------------------------------------------------------------------------------------------

TEST(SlotPredicate, TheRecoveredOffsetsMatchTheDisassembly) {
    // These are the values the observer reads. A change here without a matching disassembly
    // change would silently read the wrong field.
    EXPECT_EQ(Core::IndexIdentity::ObjectLayout::InterfaceOffset, 0x68u);
    EXPECT_EQ(Core::IndexIdentity::ObjectLayout::AddressOffsetA, 0x20u);
    EXPECT_EQ(Core::IndexIdentity::ObjectLayout::AddressOffsetB, 0x40u);
    EXPECT_EQ(Core::IndexIdentity::ObjectLayout::ElementWidthOffset, 0x88u);
    EXPECT_EQ(Core::IndexIdentity::ThreadRecordGlobalRva, 0x44AC748u);
    EXPECT_EQ(Core::IndexIdentity::ThreadFlagGlobalRva, 0x44AC740u);
    EXPECT_EQ(Core::IndexIdentity::ThreadRecordIdOffset, 0x44u);
}

// ---------------------------------------------------------------------------------------------
// The identity record keeps both candidates separate
// ---------------------------------------------------------------------------------------------

TEST(SlotPredicate, BothCandidatesAreRecordedSeparately) {
    // A nonzero but unselected slot must remain observable without being promoted to the address.
    Core::IndexIdentity::OriginalIndexResourceIdentity identity{};
    identity.address_candidate_a = 0x2015e40000ull;
    identity.has_address_candidate_a = true;
    identity.address_candidate_b = 0x2015e50000ull;
    identity.has_address_candidate_b = true;
    identity.address_slot = AddressSlot::Secondary;
    identity.guest_allocation_base = identity.address_candidate_b;
    identity.has_guest_allocation_base = true;

    EXPECT_TRUE(identity.has_address_candidate_a);
    EXPECT_TRUE(identity.has_address_candidate_b);
    EXPECT_NE(identity.address_candidate_a, identity.address_candidate_b);
    // The selected address is B, not the first nonzero candidate.
    EXPECT_EQ(identity.guest_allocation_base, identity.address_candidate_b);
}

TEST(SlotPredicate, AnUnknownSlotLeavesTheAddressAbsent) {
    Core::IndexIdentity::OriginalIndexResourceIdentity identity{};
    identity.address_candidate_a = 0x2015e40000ull;
    identity.has_address_candidate_a = true;
    identity.address_slot = AddressSlot::Unknown;
    identity.guest_allocation_base = 0;
    identity.has_guest_allocation_base = false;

    EXPECT_FALSE(Core::IndexIdentity::IsComparable(identity));
}

TEST(SlotPredicate, AZeroSelectedAddressIsPresentButNotComparable) {
    // A zero address is a legitimate value, so presence is explicit. It is still not comparable,
    // because a zero address cannot identify a buffer.
    Core::IndexIdentity::OriginalIndexResourceIdentity identity{};
    identity.address_slot = AddressSlot::Primary;
    identity.guest_allocation_base = 0;
    identity.has_guest_allocation_base = true;
    EXPECT_FALSE(Core::IndexIdentity::IsComparable(identity));
}

} // namespace
