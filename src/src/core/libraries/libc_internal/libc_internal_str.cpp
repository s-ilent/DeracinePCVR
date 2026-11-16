// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cctype>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "libc_internal_str.h"

namespace Libraries::LibcInternal {

s32 PS4_SYSV_ABI internal_strcpy_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcpy_s(dest, dest_size, src);
#else
    std::strcpy(dest, src);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_strcat_s(char* dest, size_t dest_size, const char* src) {
#ifdef _WIN64
    return strcat_s(dest, dest_size, src);
#else
    std::strcat(dest, src);
    return 0; // ALL OK
#endif
}

s32 PS4_SYSV_ABI internal_strcmp(const char* str1, const char* str2) {
    return std::strcmp(str1, str2);
}

s32 PS4_SYSV_ABI internal_strncmp(const char* str1, const char* str2, size_t num) {
    return std::strncmp(str1, str2, num);
}

char* PS4_SYSV_ABI internal_strcpy(char* dest, const char* src) {
    return std::strcpy(dest, src);
}

size_t PS4_SYSV_ABI internal_strlen(const char* str) {
    return std::strlen(str);
}

size_t PS4_SYSV_ABI internal_wcslen(const u16* str) {
    const u16* end = str;
    while (*end != 0) {
        ++end;
    }
    return static_cast<size_t>(end - str);
}

char* PS4_SYSV_ABI internal_strncpy(char* dest, const char* src, std::size_t count) {
    return std::strncpy(dest, src, count);
}

s32 PS4_SYSV_ABI internal_strncpy_s(char* dest, size_t destsz, const char* src, size_t count) {
#ifdef _WIN64
    return strncpy_s(dest, destsz, src, count);
#else
    std::strcpy(dest, src);
    return 0;
#endif
}

char* PS4_SYSV_ABI internal_strcat(char* dest, const char* src) {
    return std::strcat(dest, src);
}

const char* PS4_SYSV_ABI internal_strchr(const char* str, int c) {
    return std::strchr(str, c);
}

// Character classification table behind _Getpctype(). Flags are the FreeBSD runetype bits the
// PS4 libc uses; they all fit in the low 16 bits, so a guest that indexes the table as u16 sees
// the same answers. The returned pointer is offset by one entry so index -1 (EOF) is valid, and
// a guest that indexes from 0 instead only shifts within an ASCII class.
namespace {

constexpr u32 CtypeAlpha = 0x00000100;
constexpr u32 CtypeControl = 0x00000200;
constexpr u32 CtypeDigit = 0x00000400;
constexpr u32 CtypeGraph = 0x00000800;
constexpr u32 CtypeLower = 0x00001000;
constexpr u32 CtypePunct = 0x00002000;
constexpr u32 CtypeSpace = 0x00004000;
constexpr u32 CtypeUpper = 0x00008000;
constexpr u32 CtypeXdigit = 0x00010000;
constexpr u32 CtypeBlank = 0x00020000;
constexpr u32 CtypePrint = 0x00040000;

std::array<u32, 386> ctype_table;

const u32* GetCtypeTable() {
    static const bool filled = [] {
        using namespace std;
        for (int c = 0; c < 128; ++c) {
            u32 flags = 0;
            if (c >= '0' && c <= '9') {
                flags |= CtypeDigit | CtypeGraph | CtypePrint;
            } else if (c >= 'A' && c <= 'Z') {
                flags |= CtypeUpper | CtypeAlpha | CtypeGraph | CtypePrint;
            } else if (c >= 'a' && c <= 'z') {
                flags |= CtypeLower | CtypeAlpha | CtypeGraph | CtypePrint;
            } else if (c == ' ') {
                flags |= CtypeBlank | CtypeSpace | CtypePrint;
            } else if (c == '\t') {
                flags |= CtypeBlank | CtypeSpace | CtypeControl;
            } else if (c == '\n' || c == '\v' || c == '\f' || c == '\r') {
                flags |= CtypeSpace | CtypeControl;
            } else if (c < 0x20 || c == 0x7F) {
                flags |= CtypeControl;
            } else {
                flags |= CtypePunct | CtypeGraph | CtypePrint;
            }
            if (isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) {
                flags |= CtypeXdigit;
            }
            ctype_table[c + 1] = flags;
        }
        return true;
    }();
    (void)filled;
    return ctype_table.data() + 1;
}

} // Anonymous namespace

const u32* PS4_SYSV_ABI internal_getpctype() {
    return GetCtypeTable();
}

void RegisterlibSceLibcInternalStr(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("5Xa2ACNECdo", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcpy_s);
    LIB_FUNCTION("K+gcnFFJKVc", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat_s);
    LIB_FUNCTION("Ovb2dSJOAuE", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcmp);
    LIB_FUNCTION("aesyjrHVWy4", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncmp);
    LIB_FUNCTION("j4ViWNHEgww", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strlen);
    LIB_FUNCTION("6sJWiWSRuqk", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy);
    LIB_FUNCTION("YNzNkJzYqEg", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strncpy_s);
    LIB_FUNCTION("Ls4tzzhimqQ", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strcat);
    LIB_FUNCTION("ob5xAW4ln-0", "libSceLibcInternal", 1, "libSceLibcInternal", internal_strchr);
    LIB_FUNCTION("sUP1hBaouOw", "libSceLibcInternal", 1, "libSceLibcInternal", internal_getpctype);
}

#ifdef SHADPS4_ENABLE_FEX_GUEST_CPU
void RegisterFexLibcStrAliases(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("kiZSXIWd9vg", "libc", 1, "libc", internal_strcpy);
    LIB_FUNCTION("Ls4tzzhimqQ", "libc", 1, "libc", internal_strcat);
    LIB_FUNCTION("j4ViWNHEgww", "libc", 1, "libc", internal_strlen);
    LIB_FUNCTION("WkkeywLJcgU", "libc", 1, "libc", internal_wcslen);
    LIB_FUNCTION("Ovb2dSJOAuE", "libc", 1, "libc", internal_strcmp);
}
#endif

} // namespace Libraries::LibcInternal
