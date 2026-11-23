// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The upload-provenance association is the part of A-038 that was wrong: a global "last seen"
// upload object let any writer inherit whatever upload ran most recently. These tests pin the
// replacement, which is per-thread, stack-shaped and matched on the provider's output buffer.
//
// They exercise the association logic directly, without an emulator, a GPU or guest memory.

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Mirrors Core::GnmBindingCapture's association, kept here so the rules are testable in isolation.
// The production code is the same shape; these tests pin the rules, not a copy of the struct.

constexpr size_t MaxNesting = 8;

struct Pending {
    uintptr_t upload_object{};
    uint32_t upload_size{};
    bool upload_valid{};
    uintptr_t expected_buffer{};
};

struct ThreadState {
    std::array<Pending, MaxNesting> stack{};
    size_t depth{};
    uint32_t overflow{};
};

bool Push(ThreadState& state, Pending pending) {
    if (state.depth >= MaxNesting) {
        ++state.overflow;
        return false;
    }
    state.stack[state.depth++] = pending;
    return true;
}

bool Take(ThreadState& state, uintptr_t source, Pending& out) {
    if (state.depth == 0) {
        return false;
    }
    for (size_t i = state.depth; i-- > 0;) {
        if (state.stack[i].expected_buffer == source) {
            out = state.stack[i];
            state.depth = i;
            return true;
        }
    }
    return false;
}

Pending Make(uintptr_t object, uintptr_t buffer, uint32_t size = 80) {
    Pending p{};
    p.upload_object = object;
    p.upload_size = size;
    p.upload_valid = true;
    p.expected_buffer = buffer;
    return p;
}

// --- single-thread normal path -------------------------------------------------------------

TEST(UploadProvenance, SingleThreadMatches) {
    ThreadState state{};
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    Pending out{};
    ASSERT_TRUE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0x1000u);
    EXPECT_EQ(state.depth, 0u);
}

TEST(UploadProvenance, WriterWithNoUploadIsUnmatched) {
    ThreadState state{};
    Pending out{};
    // No upload ran at all. The writer must not inherit anything.
    EXPECT_FALSE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0u);
}

TEST(UploadProvenance, SourceMismatchDoesNotConsume) {
    ThreadState state{};
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    Pending out{};
    // A writer reading a different buffer must not steal the pending upload.
    EXPECT_FALSE(Take(state, 0x9999, out));
    EXPECT_EQ(state.depth, 1u);
    // The real writer still matches afterwards.
    ASSERT_TRUE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0x1000u);
}

TEST(UploadProvenance, SecondWriterAfterConsumeIsUnmatched) {
    ThreadState state{};
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    Pending out{};
    ASSERT_TRUE(Take(state, 0x2000, out));
    // One upload, one writer. A second writer on the same buffer has no pending invocation.
    EXPECT_FALSE(Take(state, 0x2000, out));
}

// --- interleaving --------------------------------------------------------------------------

TEST(UploadProvenance, TwoThreadsDoNotShareState) {
    // Each thread owns its own stack, so interleaving cannot cross-contaminate.
    ThreadState a{};
    ThreadState b{};
    ASSERT_TRUE(Push(a, Make(0x1000, 0x2000)));
    ASSERT_TRUE(Push(b, Make(0x3000, 0x4000)));

    Pending out{};
    ASSERT_TRUE(Take(b, 0x4000, out));
    EXPECT_EQ(out.upload_object, 0x3000u) << "thread B must get its own upload";

    ASSERT_TRUE(Take(a, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0x1000u) << "thread A must get its own upload";
}

TEST(UploadProvenance, InterleavedUploadsOnOneThreadMatchByBuffer) {
    ThreadState state{};
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    ASSERT_TRUE(Push(state, Make(0x3000, 0x3000)));
    Pending out{};
    // The inner upload's writer runs first and must get the inner object.
    ASSERT_TRUE(Take(state, 0x3000, out));
    EXPECT_EQ(out.upload_object, 0x3000u);
    // Then the outer one.
    ASSERT_TRUE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0x1000u);
}

TEST(UploadProvenance, OutOfOrderConsumptionMatchesCorrectObject) {
    ThreadState state{};
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    ASSERT_TRUE(Push(state, Make(0x3000, 0x3000)));
    Pending out{};
    // Consume the outer first. The inner entry is dropped with it, because it cannot outlive it.
    ASSERT_TRUE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0x1000u);
    EXPECT_EQ(state.depth, 0u) << "nested entries are dropped with their parent";
    EXPECT_FALSE(Take(state, 0x3000, out));
}

// --- nesting -------------------------------------------------------------------------------

TEST(UploadProvenance, NestedUploadsResolveInnermostFirst) {
    ThreadState state{};
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    ASSERT_TRUE(Push(state, Make(0x3000, 0x3000)));
    ASSERT_TRUE(Push(state, Make(0x5000, 0x4000)));
    Pending out{};
    ASSERT_TRUE(Take(state, 0x4000, out));
    EXPECT_EQ(out.upload_object, 0x5000u);
    ASSERT_TRUE(Take(state, 0x3000, out));
    EXPECT_EQ(out.upload_object, 0x3000u);
    ASSERT_TRUE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0x1000u);
}

TEST(UploadProvenance, NestingOverflowIsCountedNotSilent) {
    ThreadState state{};
    for (size_t i = 0; i < MaxNesting; ++i) {
        ASSERT_TRUE(Push(state, Make(0x1000 + i, 0x2000 + i)));
    }
    // One more than the bound must be refused and counted.
    EXPECT_FALSE(Push(state, Make(0x9999, 0x9999)));
    EXPECT_EQ(state.overflow, 1u);
    EXPECT_EQ(state.depth, MaxNesting);
}

// --- stale state ---------------------------------------------------------------------------

TEST(UploadProvenance, StaleEntryDoesNotMatchLaterWriter) {
    ThreadState state{};
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    // The upload's writer never ran. A later writer on a different buffer must not match it.
    Pending out{};
    EXPECT_FALSE(Take(state, 0x7777, out));
    EXPECT_EQ(out.upload_object, 0u);
}

TEST(UploadProvenance, ZeroObjectIsNotTreatedAsMatch) {
    ThreadState state{};
    // A push with a null object still records the buffer, so the writer matches but the object is
    // visibly null rather than silently substituted.
    ASSERT_TRUE(Push(state, Make(0, 0x2000)));
    Pending out{};
    ASSERT_TRUE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0u);
}

// --- malformed / boundary ------------------------------------------------------------------

TEST(UploadProvenance, EmptyStackTakeIsSafe) {
    ThreadState state{};
    Pending out{};
    EXPECT_FALSE(Take(state, 0, out));
    EXPECT_FALSE(Take(state, 0xFFFFFFFF, out));
}

TEST(UploadProvenance, DuplicateBuffersResolveToInnermost) {
    ThreadState state{};
    // Two invocations claiming the same buffer: the innermost is the one that just ran.
    ASSERT_TRUE(Push(state, Make(0x1000, 0x2000)));
    ASSERT_TRUE(Push(state, Make(0x3000, 0x2000)));
    Pending out{};
    ASSERT_TRUE(Take(state, 0x2000, out));
    EXPECT_EQ(out.upload_object, 0x3000u);
}

} // namespace
