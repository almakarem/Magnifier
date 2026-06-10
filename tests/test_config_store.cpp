#include "config/ConfigStore.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <filesystem>
#include <fstream>

using namespace magnifier;
namespace fs = std::filesystem;

namespace {

fs::path TempConfigPath(const std::string& suffix) {
    wchar_t buf[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, buf);
    fs::path p = fs::path(buf) / ("magnifier_test_" + suffix + ".toml");
    std::error_code ec;
    fs::remove(p, ec);
    return p;
}

} // namespace

TEST(ConfigStore, LoadMissingFileMaterialisesDefaults) {
    const auto p = TempConfigPath("missing");
    auto r = LoadConfig(p);
    EXPECT_FALSE(r.loaded_from_file);
    EXPECT_EQ(r.config.schema_version, 1);
    EXPECT_TRUE(fs::exists(p));
    EXPECT_EQ(r.config.zoom.initial, 2.0f);
    fs::remove(p);
}

TEST(ConfigStore, RoundTrip) {
    const auto p = TempConfigPath("round");
    auto r = LoadConfig(p);
    r.config.lens.width   = 1024;
    r.config.zoom.initial = 3.0f;
    r.config.ipc.http_port = 53901;
    ASSERT_TRUE(SaveConfig(p, r.config));

    auto r2 = LoadConfig(p);
    EXPECT_TRUE(r2.loaded_from_file);
    EXPECT_EQ(r2.config.lens.width, 1024);
    EXPECT_FLOAT_EQ(r2.config.zoom.initial, 3.0f);
    EXPECT_EQ(r2.config.ipc.http_port, 53901);
    fs::remove(p);
}

TEST(ConfigStore, BadTomlFallsBackToDefaultsWithWarning) {
    const auto p = TempConfigPath("bad");
    {
        std::ofstream out(p, std::ios::binary);
        out << "this is = not [ valid ] toml :\n";
    }
    auto r = LoadConfig(p);
    EXPECT_TRUE(r.loaded_from_file);
    EXPECT_FALSE(r.warnings.empty());
    EXPECT_EQ(r.config.schema_version, 1);   // default
    fs::remove(p);
}

TEST(HotkeyParse, Basics) {
    auto a = ParseHotkey("ctrl+alt+z");
    ASSERT_TRUE(a);
    EXPECT_NE(a->modifiers & MOD_CONTROL, 0u);
    EXPECT_NE(a->modifiers & MOD_ALT,     0u);
    EXPECT_EQ(a->vk, static_cast<unsigned>('Z'));

    auto b = ParseHotkey("f5");
    ASSERT_TRUE(b);
    EXPECT_EQ(b->modifiers, 0u);
    EXPECT_EQ(b->vk, static_cast<unsigned>(VK_F5));
}

TEST(HotkeyParse, RejectsUnknownTokens) {
    EXPECT_FALSE(ParseHotkey("crtl+z"));            // typo'd modifier
    EXPECT_FALSE(ParseHotkey("ctrl+alt+nope"));     // unknown key name
    EXPECT_FALSE(ParseHotkey(""));
}
