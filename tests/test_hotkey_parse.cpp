// Round-trip + edge-case coverage for the IPC command JSON.
#include "ipc/Commands.h"

#include <gtest/gtest.h>

using namespace magnifier;

TEST(IpcCommandJson, RoundTripSetZoom) {
    Command c{};
    c.kind    = CmdKind::SetZoom;
    c.f_value = 2.75f;
    const auto out = SerializeCommand(c);
    auto back = DeserializeCommand(out);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->kind, CmdKind::SetZoom);
    ASSERT_TRUE(back->f_value);
    EXPECT_FLOAT_EQ(*back->f_value, 2.75f);
}

TEST(IpcCommandJson, RoundTripLensSize) {
    Command c{};
    c.kind     = CmdKind::SetLensSize;
    c.i_value  = 640;
    c.i_value2 = 360;
    auto back = DeserializeCommand(SerializeCommand(c));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->kind, CmdKind::SetLensSize);
    EXPECT_EQ(*back->i_value,  640);
    EXPECT_EQ(*back->i_value2, 360);
}

TEST(IpcCommandJson, BadJsonRejected) {
    EXPECT_FALSE(DeserializeCommand("not json"));
    EXPECT_FALSE(DeserializeCommand("{}"));
    EXPECT_FALSE(DeserializeCommand("{\"cmd\":42}"));
}

TEST(IpcCommandJson, UnknownCmdMapsToNoop) {
    auto back = DeserializeCommand(R"({"cmd":"levitate"})");
    ASSERT_TRUE(back);
    EXPECT_EQ(back->kind, CmdKind::Noop);
}
