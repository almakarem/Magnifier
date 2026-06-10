#include "ipc/CliParser.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <vector>

using namespace magnifier;

namespace {

CliResult Parse(std::vector<std::wstring> args) {
    args.insert(args.begin(), L"Magnifier.exe");
    std::vector<wchar_t*> ptrs;
    for (auto& s : args) ptrs.push_back(s.data());
    return ParseCli(static_cast<int>(ptrs.size()), ptrs.data());
}

} // namespace

TEST(CliParser, NoArgsIsNormal) {
    auto r = Parse({});
    EXPECT_EQ(r.mode, CliResult::Mode::Normal);
    EXPECT_FALSE(r.startup_command.has_value());
}

TEST(CliParser, Help) {
    auto r = Parse({L"--help"});
    EXPECT_EQ(r.mode, CliResult::Mode::PrintHelp);
}

TEST(CliParser, Version) {
    EXPECT_EQ(Parse({L"-v"}).mode, CliResult::Mode::PrintVersion);
    EXPECT_EQ(Parse({L"--version"}).mode, CliResult::Mode::PrintVersion);
}

TEST(CliParser, LensIsStartupCommandNormalMode) {
    auto r = Parse({L"--lens"});
    EXPECT_EQ(r.mode, CliResult::Mode::Normal);
    ASSERT_TRUE(r.startup_command);
    EXPECT_EQ(r.startup_command->kind, CmdKind::EnterLens);
}

TEST(CliParser, OffIsForwardOnly) {
    auto r = Parse({L"--off"});
    EXPECT_EQ(r.mode, CliResult::Mode::ForwardOnly);
    ASSERT_TRUE(r.startup_command);
    EXPECT_EQ(r.startup_command->kind, CmdKind::TurnOff);
}

TEST(CliParser, ToggleIsForwardOnly) {
    auto r = Parse({L"--toggle"});
    EXPECT_EQ(r.mode, CliResult::Mode::ForwardOnly);
    EXPECT_EQ(r.startup_command->kind, CmdKind::Toggle);
}

TEST(CliParser, ZoomTakesValueSpaceOrEquals) {
    auto r1 = Parse({L"--zoom", L"3.5"});
    ASSERT_TRUE(r1.startup_command);
    EXPECT_EQ(r1.startup_command->kind, CmdKind::SetZoom);
    EXPECT_FLOAT_EQ(*r1.startup_command->f_value, 3.5f);

    auto r2 = Parse({L"--zoom=2.0"});
    ASSERT_TRUE(r2.startup_command);
    EXPECT_FLOAT_EQ(*r2.startup_command->f_value, 2.0f);
}

TEST(CliParser, ZoomInOutSignedDelta) {
    auto in  = Parse({L"--zoom-in", L"0.5"});
    auto out = Parse({L"--zoom-out", L"0.25"});
    ASSERT_TRUE(in.startup_command);
    ASSERT_TRUE(out.startup_command);
    EXPECT_EQ(in.startup_command->kind,  CmdKind::ZoomDelta);
    EXPECT_EQ(out.startup_command->kind, CmdKind::ZoomDelta);
    EXPECT_FLOAT_EQ(*in.startup_command->f_value,  +0.5f);
    EXPECT_FLOAT_EQ(*out.startup_command->f_value, -0.25f);
}

TEST(CliParser, LensSizeWxH) {
    auto r = Parse({L"--lens-size", L"800x450"});
    ASSERT_TRUE(r.startup_command);
    EXPECT_EQ(r.startup_command->kind, CmdKind::SetLensSize);
    EXPECT_EQ(*r.startup_command->i_value,  800);
    EXPECT_EQ(*r.startup_command->i_value2, 450);
}

TEST(CliParser, BadFlagIsParseError) {
    auto r = Parse({L"--no-such-flag"});
    EXPECT_EQ(r.mode, CliResult::Mode::ParseError);
    EXPECT_FALSE(r.error_message.empty());
}

TEST(CliParser, BadZoomValue) {
    auto r = Parse({L"--zoom", L"abc"});
    EXPECT_EQ(r.mode, CliResult::Mode::ParseError);
}

TEST(CliParser, StartMinimizedStaysNormal) {
    auto r = Parse({L"--start-minimized"});
    EXPECT_EQ(r.mode, CliResult::Mode::Normal);
    EXPECT_TRUE(r.start_minimized);
    EXPECT_FALSE(r.startup_command.has_value());
}
