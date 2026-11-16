// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include <memory>
#include <utility>

#include "common/unique_function.h"

namespace {

struct Callable {
    int value;

    int operator()() const {
        return value;
    }
};

} // Anonymous namespace

TEST(UniqueFunction, OwnsLvalueCallable) {
    Callable original{17};
    const Callable& const_original = original;

    Common::UniqueFunction<int> owned(const_original);
    original.value = 91;

    EXPECT_EQ(owned(), 17);
}

TEST(UniqueFunction, OwnsCallableAfterCreatingScopeExits) {
    auto delayed = [] {
        const auto local = [] { return 42; };
        return Common::UniqueFunction<int>(local);
    }();

    EXPECT_EQ(delayed(), 42);
}

TEST(UniqueFunction, KeepsMoveOnlyCaptures) {
    Common::UniqueFunction<int> movable([value = std::make_unique<int>(29)] { return *value; });
    auto moved = std::move(movable);

    EXPECT_FALSE(movable);
    EXPECT_EQ(moved(), 29);

    Common::UniqueFunction<int> assigned;
    assigned = std::move(moved);

    EXPECT_FALSE(moved);
    EXPECT_EQ(assigned(), 29);
}

TEST(UniqueFunction, ForwardsArguments) {
    Common::UniqueFunction<void, int&> increment([](int& value) { ++value; });

    int value = 0;
    increment(value);

    EXPECT_EQ(value, 1);
}
