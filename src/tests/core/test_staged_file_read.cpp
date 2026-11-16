// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "common/staged_file_read.h"

TEST(StagedFileRead, ReadableBytesStopsAtEndOfFile) {
    EXPECT_EQ(Common::FS::ReadableBytes(0, 150, 64), 64u);
    EXPECT_EQ(Common::FS::ReadableBytes(100, 150, 64), 50u);
    EXPECT_EQ(Common::FS::ReadableBytes(149, 150, 64), 1u);
    EXPECT_EQ(Common::FS::ReadableBytes(150, 150, 64), 0u);
    EXPECT_EQ(Common::FS::ReadableBytes(4096, 150, 64), 0u);
    EXPECT_EQ(Common::FS::ReadableBytes(0, 150, 0), 0u);
}

#ifdef _WIN32

#include <windows.h>

namespace {

constexpr std::size_t Count = 1500000;
constexpr std::size_t BufferSize = 2 * 1024 * 1024;

unsigned char* tracked = nullptr;
std::size_t tracked_size = 0;
unsigned faults = 0;

// Stands in for the emulator's page handler: untrack the page on a write fault and resume.
LONG CALLBACK Untrack(EXCEPTION_POINTERS* exception) {
    if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        exception->ExceptionRecord->ExceptionInformation[0] != 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto address = exception->ExceptionRecord->ExceptionInformation[1];
    if (address < reinterpret_cast<ULONG_PTR>(tracked) ||
        address >= reinterpret_cast<ULONG_PTR>(tracked) + tracked_size) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD old = 0;
    if (!VirtualProtect(tracked, tracked_size, PAGE_READWRITE, &old)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    ++faults;
    return EXCEPTION_CONTINUE_EXECUTION;
}

class ProtectedGuestPage : public ::testing::Test {
protected:
    void SetUp() override {
        tracked_size = BufferSize;
        tracked = static_cast<unsigned char*>(
            VirtualAlloc(nullptr, tracked_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        ASSERT_NE(tracked, nullptr);
        std::memset(tracked, 0xcc, tracked_size);

        source.resize(Count);
        for (std::size_t i = 0; i < Count; ++i) {
            source[i] = static_cast<unsigned char>((i * 37 + i / 251) & 255);
        }

        ASSERT_EQ(tmpfile_s(&file), 0);
        ASSERT_NE(file, nullptr);
        ASSERT_EQ(std::fwrite(source.data(), 1, source.size(), file), Count);
        std::rewind(file);

        handler = AddVectoredExceptionHandler(1, Untrack);
        ASSERT_NE(handler, nullptr);
        DWORD old = 0;
        ASSERT_NE(VirtualProtect(tracked, tracked_size, PAGE_READONLY, &old), 0);
    }

    void TearDown() override {
        if (handler) {
            RemoveVectoredExceptionHandler(handler);
        }
        if (file) {
            std::fclose(file);
        }
        if (tracked) {
            VirtualFree(tracked, 0, MEM_RELEASE);
        }
        handler = nullptr;
        file = nullptr;
        tracked = nullptr;
        tracked_size = 0;
        faults = 0;
    }

    std::vector<unsigned char> source;
    FILE* file = nullptr;
    PVOID handler = nullptr;
};

} // Anonymous namespace

// The behaviour staged reads exist to avoid: an OS read straight into a protected guest page
// does not reach the page handler and returns short.
TEST_F(ProtectedGuestPage, DirectReadIntoProtectedPageFailsShort) {
    const auto read = std::fread(tracked, 1, Count, file);

    EXPECT_LT(read, Count);
    EXPECT_EQ(faults, 0u);
}

TEST_F(ProtectedGuestPage, StagedReadFillsProtectedPage) {
    const auto read =
        Common::FS::ReadStaged(tracked, tracked_size, [&](void* buffer, std::size_t size) {
            return std::fread(buffer, 1, size, file);
        });

    EXPECT_EQ(read, Count);
    EXPECT_NE(faults, 0u);
    EXPECT_EQ(std::memcmp(tracked, source.data(), Count), 0);
    for (auto i = Count; i < tracked_size; ++i) {
        ASSERT_EQ(tracked[i], 0xcc) << "byte past the read was modified at " << i;
    }
    EXPECT_EQ(std::ftell(file), static_cast<long>(Count));
}

TEST_F(ProtectedGuestPage, StagedReadAtEndOfFileReadsNothing) {
    ASSERT_EQ(std::fseek(file, static_cast<long>(Count), SEEK_SET), 0);

    const auto read =
        Common::FS::ReadStaged(tracked, 100, [&](void* buffer, std::size_t size) {
            return std::fread(buffer, 1, size, file);
        });

    EXPECT_EQ(read, 0u);
}

TEST(StagedFileRead, ZeroLengthReadDoesNotCallReader) {
    std::vector<unsigned char> buffer(16);

    bool invoked = false;
    const auto read = Common::FS::ReadStaged(buffer.data(), 0, [&](void*, std::size_t) {
        invoked = true;
        return std::size_t{0};
    });

    EXPECT_EQ(read, 0u);
    EXPECT_FALSE(invoked);
}

#endif // _WIN32
