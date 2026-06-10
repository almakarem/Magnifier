#include "ipc/JsonFraming.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace magnifier;

namespace {

std::vector<std::string> Collect(JsonLineReader& r, const std::string& bytes) {
    std::vector<std::string> out;
    JsonLineReader copy(
        [&out](std::string_view s) { out.emplace_back(s); });
    copy.Feed(bytes.data(), bytes.size());
    return out;
}

} // namespace

TEST(JsonFraming, SingleLine) {
    std::vector<std::string> got;
    JsonLineReader r([&](std::string_view s) { got.emplace_back(s); });
    const std::string input = "hello\n";
    r.Feed(input.data(), input.size());
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "hello");
}

TEST(JsonFraming, MultipleLinesIncludingCrlf) {
    std::vector<std::string> got;
    JsonLineReader r([&](std::string_view s) { got.emplace_back(s); });
    const std::string input = "one\r\ntwo\nthree\r\n";
    r.Feed(input.data(), input.size());
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0], "one");
    EXPECT_EQ(got[1], "two");
    EXPECT_EQ(got[2], "three");
}

TEST(JsonFraming, SplitAcrossFeeds) {
    std::vector<std::string> got;
    JsonLineReader r([&](std::string_view s) { got.emplace_back(s); });
    const std::string part1 = "{\"cmd\":\"ent";
    const std::string part2 = "er_lens\"}\n{\"cmd\":\"toggle\"}\n";
    r.Feed(part1.data(), part1.size());
    EXPECT_EQ(got.size(), 0u);
    r.Feed(part2.data(), part2.size());
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "{\"cmd\":\"enter_lens\"}");
    EXPECT_EQ(got[1], "{\"cmd\":\"toggle\"}");
}

TEST(JsonFraming, EmptyLinesIgnored) {
    std::vector<std::string> got;
    JsonLineReader r([&](std::string_view s) { got.emplace_back(s); });
    const std::string input = "\n\nhello\n\n";
    r.Feed(input.data(), input.size());
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "hello");
}

TEST(JsonFraming, OverLongLineDroppedNoCrash) {
    std::vector<std::string> got;
    JsonLineReader r([&](std::string_view s) { got.emplace_back(s); }, 16);
    std::string big(64, 'x');
    big.push_back('\n');
    r.Feed(big.data(), big.size());
    EXPECT_EQ(got.size(), 0u);   // dropped, no crash

    const std::string ok = "ok\n";
    r.Feed(ok.data(), ok.size());
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], "ok");
}
