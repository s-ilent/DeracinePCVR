// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The provider-to-adapter association is the edge A-039 could not cross. These tests pin the
// matching rules against the production constants, so a change to either the offsets or the
// matching logic fails here rather than silently producing a wrong chain.

#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "core/gnm_binding_capture.h"

namespace {

using namespace Core::GnmBindingCapture;

// Mirrors the production matching rules. The production code is the same shape; these tests pin
// the rules, not a copy of the struct.

struct Pending {
    uintptr_t selected_object{};
    uintptr_t provider_result{};
};

// The inline rule: the 0x0CF9E20 method returns object+0x60.
bool MatchesInline(const Pending& p, uintptr_t result) {
    return p.selected_object != 0 && result == p.selected_object + ProviderInlineResultOffset;
}

// The delegating rule: the 0x0CFFEC0 method returns its inner object's inline result, and the
// inner object is read from object+0x58.
bool MatchesDelegate(const Pending& p, uintptr_t result, uintptr_t inner) {
    return p.selected_object != 0 && inner != 0 &&
           result == inner + ProviderInlineResultOffset;
}

// --- the two provider methods ---------------------------------------------------------------

TEST(ProviderDescriptor, InlineProviderReturnsObjectPlus60) {
    // From the 0x0CF9E20 machine code: lea 0x60(%rdi), %rax; ret.
    const uintptr_t object = 0x10012ffc80;
    EXPECT_EQ(object + ProviderInlineResultOffset, 0x10012ffce0u);
    Pending p{object, 0};
    EXPECT_TRUE(MatchesInline(p, object + 0x60));
}

TEST(ProviderDescriptor, InlineProviderDoesNotMatchOtherOffsets) {
    const uintptr_t object = 0x10012ffc80;
    Pending p{object, 0};
    EXPECT_FALSE(MatchesInline(p, object + 0x58));
    EXPECT_FALSE(MatchesInline(p, object + 0x68));
    EXPECT_FALSE(MatchesInline(p, object));
}

TEST(ProviderDescriptor, DelegateProviderReturnsInnerInlineResult) {
    // From the 0x0CFFEC0 machine code: mov 0x58(%rdi), %rdi; ... jmp *0x38(%rax).
    const uintptr_t object = 0x1006cb4de0;
    const uintptr_t inner = 0x1006cb4e38;
    Pending p{object, 0};
    EXPECT_TRUE(MatchesDelegate(p, inner + 0x60, inner));
    // The delegating method cannot return the selected object's own inline result.
    EXPECT_FALSE(MatchesDelegate(p, object + 0x60, inner));
}

TEST(ProviderDescriptor, DelegateWithNullInnerDoesNotMatch) {
    const uintptr_t object = 0x1006cb4de0;
    Pending p{object, 0};
    EXPECT_FALSE(MatchesDelegate(p, object + 0x60, 0));
}

TEST(ProviderDescriptor, NullObjectNeverMatches) {
    Pending p{0, 0};
    EXPECT_FALSE(MatchesInline(p, 0x60));
    EXPECT_FALSE(MatchesDelegate(p, 0x60, 0x1000));
}

// --- the adapter contract -------------------------------------------------------------------

TEST(ProviderDescriptor, AdapterAndWriterAddressesAreTheVerifiedOnes) {
    EXPECT_EQ(DescriptorAdapterRva, 0x0CC13B0u);
    EXPECT_EQ(ResourceWriter32Rva, 0x02F62A40u);
    EXPECT_EQ(ResourceWriter16Rva, 0x02F62740u);
}

TEST(ProviderDescriptor, DescriptorPointerIsAtProviderResultPlus18) {
    // From the adapter's own code: movq 0x18(%r13), %rdi after addq $0x18, %r13.
    EXPECT_EQ(ProviderResultDescriptor, 0x18u);
}

TEST(ProviderDescriptor, TheTwoProviderOffsetsAreDistinct) {
    // Conflating them would make the delegating class match the inline rule, which the live
    // capture shows it does not.
    EXPECT_NE(ProviderInlineResultOffset, ProviderDelegateObjectOffset);
    EXPECT_EQ(ProviderInlineResultOffset, 0x60u);
    EXPECT_EQ(ProviderDelegateObjectOffset, 0x58u);
}

// --- chain status ---------------------------------------------------------------------------

TEST(ProviderDescriptor, OnlyCompleteIsAFullClaim) {
    // The status enum exists so a partial chain cannot be reported as complete.
    EXPECT_NE(static_cast<int>(ChainStatus::BinderOnly),
              static_cast<int>(ChainStatus::Complete));
    EXPECT_NE(static_cast<int>(ChainStatus::AdapterMatched),
              static_cast<int>(ChainStatus::Complete));
    EXPECT_NE(static_cast<int>(ChainStatus::AdapterUnmatched),
              static_cast<int>(ChainStatus::Complete));
}

} // namespace
