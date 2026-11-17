// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "common/memory_patcher.h"
#include "common/singleton.h"
#include "core/file_format/psf.h"

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        temp_path = fs::temp_directory_path() / ("shadps4_patch_test_" + std::to_string(ns) + "_" +
                                                 std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(temp_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(temp_path, ec);
    }
    const fs::path& path() const {
        return temp_path;
    }

private:
    fs::path temp_path;
};

void WriteFile(const fs::path& p, const std::string& contents) {
    std::ofstream out(p);
    out << contents;
}

constexpr const char* kValidPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Patch A" Author="author-a" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="00000001"/>
        </PatchList>
    </Metadata>
    <Metadata Title="Test Game" Name="Patch B" Author="author-b" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400004" Value="00000002"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kMaskPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Mask Patch" Author="author-m" PatchVer="1.0" AppVer="99.99" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <Line Type="mask" Address="90" Value="00" Offset="0"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kFixedAddressWrongVersion = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Wrong Version" Author="author-w" PatchVer="1.0" AppVer="99.99" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="000000FF"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kDuplicateIdsPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Dup" Author="author-d" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="00000001"/>
        </PatchList>
    </Metadata>
    <Metadata Title="Test Game" Name="Dup" Author="author-d" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <Line Type="bytes32" Address="400004" Value="00000002"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kMalformedPatch = "<Patch><Metadata Name=\"broken\">";

constexpr const char* kOOBPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="OOB Patch" Author="author-oob" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <Line Type="bytes32" Address="0x401000" Value="DEADBEEF"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kTrailingJunkAddressPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Junk Address Patch" Author="author-junk" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <Line Type="bytes32" Address="04d99138xyz" Value="DEADBEEF"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kStrictHexPrefixPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Hex Prefix Patch" Author="author-hex" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="false">
        <PatchList>
            <Line Type="bytes32" Address="0x00400000" Value="0x12345678"/>
        </PatchList>
    </Metadata>
</Patch>
)";

// The backing image is filled with 0x90, so these Expected bytes describe it.
constexpr const char* kExpectedBytesPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Expected Bytes" Author="author-e" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="00000001" Expected="90909090"/>
        </PatchList>
    </Metadata>
</Patch>
)";

// The shipped CUSA11919 line uses a signature longer than the bytes it writes; verify the
// prefix check does not require equal lengths.
constexpr const char* kExpectedLongerPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Expected Longer" Author="author-e" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="00000001" Expected="9090909090909090"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kExpectedBytesMismatchPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Expected Mismatch" Author="author-e" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="00000001" Expected="11223344"/>
        </PatchList>
    </Metadata>
</Patch>
)";

constexpr const char* kExpectedBytesInvalidPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Expected Invalid" Author="author-e" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="00000001" Expected="not-hex"/>
        </PatchList>
    </Metadata>
</Patch>
)";

// isEnabled=true with a version that is not the installed one: the write must not happen.
constexpr const char* kLegacyWrongVersionEnabled = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Wrong Version Enabled" Author="author-w" PatchVer="1.0" AppVer="99.99" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="000000FF"/>
        </PatchList>
    </Metadata>
</Patch>
)";

// A mask signature that is not in the image: the scan must fail, not write elsewhere.
constexpr const char* kMaskSignatureMissing = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Missing Signature" Author="author-m" PatchVer="1.0" AppVer="99.99" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="mask" Address="DEADBEEF" Value="00" Offset="0"/>
        </PatchList>
    </Metadata>
</Patch>
)";

class PatchTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir = std::make_unique<TempDir>();
        // APP_VER read by the managed/legacy paths.
        Common::Singleton<PSF>::Instance()->AddString("APP_VER", "01.00");

        // Safe writable buffer standing in for the loaded eboot image. Fixed-address patches use
        // Address=0x400000 so the (base + offset - 0x400000) arithmetic lands at buffer start.
        backing.resize(4096, 0x90);
        MemoryPatcher::g_eboot_address = reinterpret_cast<uintptr_t>(backing.data());
        MemoryPatcher::g_eboot_image_size = backing.size();

        MemoryPatcher::SetPatchMemoryObserver(nullptr);
    }

    void TearDown() override {
        MemoryPatcher::SetPatchMemoryObserver(nullptr);
        MemoryPatcher::ResetAppliedXmlFiles();
        MemoryPatcher::g_eboot_address = 0;
        MemoryPatcher::g_eboot_image_size = 0;
        temp_dir.reset();
    }

    fs::path WritePatch(const std::string& contents, const std::string& name = "test.xml") {
        const fs::path p = temp_dir->path() / name;
        WriteFile(p, contents);
        return p;
    }

    static std::vector<std::string>& Observed() {
        static std::vector<std::string> observed;
        return observed;
    }

    static void Observe(const std::string& modName) {
        Observed().push_back(modName);
    }

    void InstallObserver() {
        Observed().clear();
        MemoryPatcher::SetPatchMemoryObserver(&PatchTest::Observe);
    }

    std::unique_ptr<TempDir> temp_dir;
    std::vector<u8> backing;
};

MemoryPatcher::PatchIdentityMap IdentityMapFor(const fs::path& xml_path,
                                               const std::vector<std::pair<std::string, std::string>>& ids) {
    MemoryPatcher::PatchIdentityMap map;
    const auto defs = MemoryPatcher::EnumeratePatchDefinitions(xml_path);
    for (const auto& def : defs) {
        for (const auto& [name, id] : ids) {
            if (def.name == name) {
                map[def.title + "\x1f" + def.name + "\x1f" + def.author + "\x1f" + def.app_version +
                    "\x1f" + def.app_elf] = id;
            }
        }
    }
    return map;
}

} // namespace

TEST_F(PatchTest, EnumeratePatchDefinitions_ReturnsAllMetadata) {
    const auto xml = WritePatch(kValidPatch);
    const auto defs = MemoryPatcher::EnumeratePatchDefinitions(xml);
    ASSERT_EQ(defs.size(), 2u);
    EXPECT_EQ(defs[0].name, "Patch A");
    EXPECT_EQ(defs[1].name, "Patch B");
    EXPECT_TRUE(defs[0].is_enabled);
    EXPECT_TRUE(defs[1].is_enabled);
}

TEST_F(PatchTest, EnumeratePatchDefinitions_MalformedXml_ReturnsEmpty) {
    const auto xml = WritePatch(kMalformedPatch, "malformed.xml");
    EXPECT_TRUE(MemoryPatcher::EnumeratePatchDefinitions(xml).empty());
}

TEST_F(PatchTest, ManagedSelection_NoEnabledIds_AppliesNothing) {
    const auto xml = WritePatch(kValidPatch);
    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {});
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(Observed().empty());
    EXPECT_EQ(result.records.size(), 2u);
}

TEST_F(PatchTest, ManagedSelection_OneId_AppliesOnlyThatPatch) {
    const auto xml = WritePatch(kValidPatch);
    auto ids = IdentityMapFor(xml, {{"Patch A", "patch.a"}, {"Patch B", "patch.b"}});
    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"patch.a"}, ids);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(Observed().size(), 1u);
    EXPECT_EQ(Observed()[0], "Patch A");
}

TEST_F(PatchTest, ManagedSelection_UnknownId_SkipsSafely) {
    const auto xml = WritePatch(kValidPatch);
    auto ids = IdentityMapFor(xml, {{"Patch A", "patch.a"}, {"Patch B", "patch.b"}});
    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"does.not.exist"}, ids);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchTest, ManagedSelection_WrongAppVer_SkipsFixedAddress) {
    const auto xml = WritePatch(kFixedAddressWrongVersion);
    auto ids = IdentityMapFor(xml, {{"Wrong Version", "wrong.ver"}});
    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"wrong.ver"}, ids);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(Observed().empty());
    ASSERT_EQ(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].status, MemoryPatcher::PatchApplyStatus::VersionMismatch);
}

TEST_F(PatchTest, MaskSemantics_ApplyEvenOnVersionMismatch) {
    const auto xml = WritePatch(kMaskPatch);
    auto ids = IdentityMapFor(xml, {{"Mask Patch", "mask.patch"}});
    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"mask.patch"}, ids);
    EXPECT_TRUE(result.ok);
    // mask lines are version-independent, so the write is attempted even though APP_VER is 01.00.
    ASSERT_EQ(Observed().size(), 1u);
    EXPECT_EQ(Observed()[0], "Mask Patch");
}

TEST_F(PatchTest, Legacy_IsEnabledStillApplies) {
    const auto xml = WritePatch(kValidPatch);
    InstallObserver();
    MemoryPatcher::ApplyLegacyPatchesFromXML(xml);
    ASSERT_EQ(Observed().size(), 2u);
    EXPECT_EQ(Observed()[0], "Patch A");
    EXPECT_EQ(Observed()[1], "Patch B");
}

TEST_F(PatchTest, Legacy_DisabledMetadataSkipped) {
    const auto xml = WritePatch(kMaskPatch); // isEnabled=false
    InstallObserver();
    MemoryPatcher::ApplyLegacyPatchesFromXML(xml);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchTest, ManagedAndLegacy_DoNotDoubleApply) {
    const auto xml = WritePatch(kValidPatch);
    auto ids = IdentityMapFor(xml, {{"Patch A", "patch.a"}, {"Patch B", "patch.b"}});
    InstallObserver();

    MemoryPatcher::ApplyManagedPatches(xml, {"patch.a"}, ids);
    ASSERT_EQ(Observed().size(), 1u);

    // Same XML through the legacy path must be a no-op.
    MemoryPatcher::ApplyLegacyPatchesFromXML(xml);
    EXPECT_EQ(Observed().size(), 1u);
}

TEST_F(PatchTest, DuplicateStableIds_AreRejectedDeterministically) {
    const auto xml = WritePatch(kDuplicateIdsPatch);
    auto ids = IdentityMapFor(xml, {{"Dup", "dup.id"}});
    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"dup.id"}, ids);
    EXPECT_FALSE(result.ok);
    // First occurrence is applied, second is rejected as a duplicate.
    ASSERT_EQ(Observed().size(), 1u);
    EXPECT_EQ(Observed()[0], "Dup");
    EXPECT_EQ(result.records.size(), 2u);
    EXPECT_EQ(result.records[1].status, MemoryPatcher::PatchApplyStatus::InvalidDefinition);
}

TEST_F(PatchTest, AppliedFileGuard_ResetsBetweenSessions) {
    const auto xml = WritePatch(kValidPatch);
    auto ids = IdentityMapFor(xml, {{"Patch A", "patch.a"}, {"Patch B", "patch.b"}});
    InstallObserver();

    // Session 1: managed apply succeeds, then legacy is blocked.
    const auto first = MemoryPatcher::ApplyManagedPatches(xml, {"patch.a"}, ids);
    EXPECT_TRUE(first.ok);
    ASSERT_EQ(Observed().size(), 1u);
    MemoryPatcher::ApplyLegacyPatchesFromXML(xml);
    EXPECT_EQ(Observed().size(), 1u);

    // End session: the guard resets.
    MemoryPatcher::ResetAppliedXmlFiles();

    // Session 2: same XML applies again.
    const auto second = MemoryPatcher::ApplyManagedPatches(xml, {"patch.b"}, ids);
    EXPECT_TRUE(second.ok);
    ASSERT_EQ(Observed().size(), 2u);
    EXPECT_EQ(Observed()[1], "Patch B");
}

TEST_F(PatchTest, LegacyFallbackId_IsDeterministic) {
    const auto xml = WritePatch(kValidPatch);
    const auto defs = MemoryPatcher::EnumeratePatchDefinitions(xml);
    ASSERT_EQ(defs.size(), 2u);
    const std::string id1 = MemoryPatcher::PatchDefinitionIdentity(
        defs[0].title, defs[0].name, defs[0].author, defs[0].app_version, defs[0].app_elf, {});
    const std::string id2 = MemoryPatcher::PatchDefinitionIdentity(
        defs[0].title, defs[0].name, defs[0].author, defs[0].app_version, defs[0].app_elf, {});
    EXPECT_EQ(id1, id2);
    EXPECT_EQ(id1.rfind("legacy:", 0), 0u);
    EXPECT_GT(id1.size(), 7u);
}

TEST_F(PatchTest, ZeroImageSize_ReturnsWriteFailed_ObserverNotCalled) {
    MemoryPatcher::g_eboot_image_size = 0;
    const auto xml = WritePatch(kValidPatch);
    auto ids = IdentityMapFor(xml, {{"Patch A", "patch.a"}});
    InstallObserver();
    const std::vector<u8> original_backing = backing;

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"patch.a"}, ids);
    EXPECT_FALSE(result.ok);
    ASSERT_GE(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].status, MemoryPatcher::PatchApplyStatus::WriteFailed);
    EXPECT_TRUE(Observed().empty());
    EXPECT_EQ(backing, original_backing);
}

TEST_F(PatchTest, OutOfBoundsWrite_ReturnsWriteFailed_ObserverNotCalled) {
    const auto xml = WritePatch(kOOBPatch, "oob.xml");
    auto ids = IdentityMapFor(xml, {{"OOB Patch", "oob.patch"}});
    InstallObserver();

    const std::vector<u8> original_backing = backing;

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"oob.patch"}, ids);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].status, MemoryPatcher::PatchApplyStatus::WriteFailed);
    EXPECT_TRUE(Observed().empty());
    EXPECT_EQ(backing, original_backing);
}

TEST_F(PatchTest, TrailingJunkAddress_ReturnsWriteFailed_ObserverNotCalled) {
    const auto xml = WritePatch(kTrailingJunkAddressPatch, "junk.xml");
    auto ids = IdentityMapFor(xml, {{"Junk Address Patch", "junk.patch"}});
    InstallObserver();

    const std::vector<u8> original_backing = backing;

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"junk.patch"}, ids);
    EXPECT_FALSE(result.ok);
    ASSERT_EQ(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].status, MemoryPatcher::PatchApplyStatus::WriteFailed);
    EXPECT_TRUE(Observed().empty());
    EXPECT_EQ(backing, original_backing);
}

TEST_F(PatchTest, StrictHexPrefixAndValue_AppliesCleanly) {
    const auto xml = WritePatch(kStrictHexPrefixPatch, "hex.xml");
    auto ids = IdentityMapFor(xml, {{"Hex Prefix Patch", "hex.patch"}});
    InstallObserver();

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"hex.patch"}, ids);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(Observed().size(), 1u);
    EXPECT_EQ(Observed()[0], "Hex Prefix Patch");
}

// A fixed-address patch that names the bytes it expects is written only when they are there.
TEST_F(PatchTest, FixedAddress_ExpectedBytesMatch_Applies) {
    const auto xml = WritePatch(kExpectedBytesPatch, "expected-ok.xml");
    auto ids = IdentityMapFor(xml, {{"Expected Bytes", "expected.ok"}});
    InstallObserver();

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"expected.ok"}, ids);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(Observed().size(), 1u);
    ASSERT_EQ(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].status, MemoryPatcher::PatchApplyStatus::Applied);
    EXPECT_EQ(backing[0], 0x01);
    EXPECT_EQ(backing[1], 0x00);
}

TEST_F(PatchTest, FixedAddress_ExpectedSignatureLongerThanValue_Applies) {
    const auto xml = WritePatch(kExpectedLongerPatch, "expected-longer.xml");
    auto ids = IdentityMapFor(xml, {{"Expected Longer", "expected.longer"}});
    InstallObserver();

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"expected.longer"}, ids);
    EXPECT_TRUE(result.ok);
    ASSERT_EQ(Observed().size(), 1u);
    EXPECT_EQ(backing[0], 0x01);
    EXPECT_EQ(backing[3], 0x00);
}

TEST_F(PatchTest, FixedAddress_ExpectedBytesMismatch_LeavesExecutableUnchanged) {
    const auto xml = WritePatch(kExpectedBytesMismatchPatch, "expected-bad.xml");
    auto ids = IdentityMapFor(xml, {{"Expected Mismatch", "expected.bad"}});
    InstallObserver();
    const std::vector<u8> original_backing = backing;

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"expected.bad"}, ids);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(Observed().empty());
    ASSERT_EQ(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].status, MemoryPatcher::PatchApplyStatus::WriteFailed);
    EXPECT_EQ(backing, original_backing);
}

TEST_F(PatchTest, FixedAddress_InvalidExpectedBytes_LeavesExecutableUnchanged) {
    const auto xml = WritePatch(kExpectedBytesInvalidPatch, "expected-invalid.xml");
    auto ids = IdentityMapFor(xml, {{"Expected Invalid", "expected.invalid"}});
    InstallObserver();
    const std::vector<u8> original_backing = backing;

    const auto result = MemoryPatcher::ApplyManagedPatches(xml, {"expected.invalid"}, ids);
    EXPECT_FALSE(result.ok);
    EXPECT_TRUE(Observed().empty());
    ASSERT_EQ(result.records.size(), 1u);
    EXPECT_EQ(result.records[0].status, MemoryPatcher::PatchApplyStatus::WriteFailed);
    EXPECT_EQ(backing, original_backing);
}

TEST_F(PatchTest, Legacy_WrongAppVerWithEnabledMetadata_LeavesExecutableUnchanged) {
    const auto xml = WritePatch(kLegacyWrongVersionEnabled, "legacy-wrong-ver.xml");
    InstallObserver();
    const std::vector<u8> original_backing = backing;

    MemoryPatcher::ApplyLegacyPatchesFromXML(xml);
    EXPECT_TRUE(Observed().empty());
    EXPECT_EQ(backing, original_backing);
}

TEST_F(PatchTest, Legacy_MaskSignatureMissing_LeavesExecutableUnchanged) {
    const auto xml = WritePatch(kMaskSignatureMissing, "mask-missing.xml");
    InstallObserver();
    const std::vector<u8> original_backing = backing;

    MemoryPatcher::ApplyLegacyPatchesFromXML(xml);
    EXPECT_TRUE(Observed().empty());
    EXPECT_EQ(backing, original_backing);
}


