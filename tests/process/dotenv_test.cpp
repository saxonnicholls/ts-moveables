//
//  dotenv_test.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - process/dotenv.hpp tests, ported from an earlier application framework's dotenv tests.
//

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>

#include "mini_gtest.hpp"

#include "process/dotenv.hpp"

namespace {

using snicholls::config_error;
using snicholls::parse_dotenv;

std::map<std::string, std::string> as_map(std::string_view text, const snicholls::dotenv_lookup& lookup = {})
{
    std::map<std::string, std::string> out;
    for (const auto& e : parse_dotenv(text, "<test>", lookup))
        out[e.key] = e.value;
    return out;
}

TEST(AppDotenv, BasicAssignmentsCommentsAndBlankLines)
{
    const auto m = as_map("# comment\n\nA=1\n  B = two words  \nexport C=3\nEMPTY=\n");
    EXPECT_EQ(m.at("A"), "1");
    EXPECT_EQ(m.at("B"), "two words");
    EXPECT_EQ(m.at("C"), "3");
    EXPECT_EQ(m.at("EMPTY"), "");
    EXPECT_EQ(m.size(), 4u);
}

TEST(AppDotenv, InlineCommentsOnlyAfterWhitespace)
{
    const auto m = as_map("A=value # note\nB=a#b\nC='x' # note\nD=\"y\"#n\n");
    EXPECT_EQ(m.at("A"), "value");
    EXPECT_EQ(m.at("B"), "a#b");
    EXPECT_EQ(m.at("C"), "x");
    EXPECT_EQ(m.at("D"), "y");
}

TEST(AppDotenv, SingleQuotesAreLiteral)
{
    const auto m = as_map("A='$HOME ${X} \\n # not a comment'\n");
    EXPECT_EQ(m.at("A"), "$HOME ${X} \\n # not a comment");
}

TEST(AppDotenv, DoubleQuotesHaveEscapesAndSpanLines)
{
    const auto m = as_map("A=\"l1\\nl2\\t\\\"q\\\" \\\\ \\$X \\z\"\nPEM=\"-----BEGIN-----\nabc\n-----END-----\"\nB=after\n");
    EXPECT_EQ(m.at("A"), "l1\nl2\t\"q\" \\ $X \\z");
    EXPECT_EQ(m.at("PEM"), "-----BEGIN-----\nabc\n-----END-----");
    EXPECT_EQ(m.at("B"), "after");
}

TEST(AppDotenv, LineNumbersCountMultiLineValues)
{
    const auto entries = parse_dotenv("A=\"1\n2\"\nB='x\ny'\nC=3\n");
    ASSERT_EQ(entries.size(), 3u);
    EXPECT_EQ(entries[0].line, 1u);
    EXPECT_EQ(entries[1].line, 3u);
    EXPECT_EQ(entries[2].line, 5u);
}

TEST(AppDotenv, InterpolationFromEarlierKeysThenLookupThenDefault)
{
    const snicholls::dotenv_lookup env = [](std::string_view name) -> std::optional<std::string> {
        if (name == "HOME")
            return "/home/me";
        if (name == "EMPTY")
            return "";
        return std::nullopt;
    };
    const auto m = as_map("BASE=/srv\nA=${BASE}/data\nB=\"${HOME}/x\"\nC=${MISSING:-fallback}\nD=${MISSING}\n"
                          "E=${EMPTY:-used}\nF='${BASE}'\n",
                          env);
    EXPECT_EQ(m.at("A"), "/srv/data");
    EXPECT_EQ(m.at("B"), "/home/me/x");
    EXPECT_EQ(m.at("C"), "fallback");
    EXPECT_EQ(m.at("D"), "");
    EXPECT_EQ(m.at("E"), "used");
    EXPECT_EQ(m.at("F"), "${BASE}");
}

TEST(AppDotenv, CrlfAndBomAccepted)
{
    const auto m = as_map("\xEF\xBB\xBF" "A=1\r\nB=\"x\r\ny\"\r\nC='p\r\nq'\r\n");
    EXPECT_EQ(m.at("A"), "1");
    EXPECT_EQ(m.at("B"), "x\ny");
    EXPECT_EQ(m.at("C"), "p\nq");
}

TEST(AppDotenv, LaterDuplicateWins)
{
    EXPECT_EQ(as_map("A=1\nA=2\n").at("A"), "2");
}

TEST(AppDotenv, KeysMayContainDotsAndDashes)
{
    EXPECT_EQ(as_map("app.log-level=debug\n").at("app.log-level"), "debug");
}

TEST(AppDotenv, ErrorsNameSourceAndLineButNeverTheValue)
{
    const auto expect_error = [](std::string_view text, std::string_view fragment) {
        try
        {
            static_cast<void>(parse_dotenv(text, "conf.env"));
            ADD_FAILURE() << "no error for: " << text;
        }
        catch (const config_error& e)
        {
            const std::string what = e.what();
            EXPECT_NE(what.find("conf.env:"), std::string::npos) << what;
            EXPECT_NE(what.find(fragment), std::string::npos) << what;
            EXPECT_EQ(what.find("s3cr3t"), std::string::npos) << what;
        }
    };
    expect_error("A=1\nPASSWORD='s3cr3t\n", "conf.env:2: unterminated single");
    expect_error("PASSWORD=\"s3cr3t\n", "unterminated double");
    expect_error("PASSWORD='s3cr3t' trailing\n", "after closing quote");
    expect_error("1BAD=x\n", "expected a key");
    expect_error("NOEQUALS s3cr3t\n", "expected KEY=VALUE");
    expect_error("A=${UNCLOSED s3cr3t\n", "unterminated ${");
    expect_error("A=${1X}\n", "invalid name");
    expect_error("=s3cr3t\n", "expected a key");
}

TEST(AppDotenv, ExportPrefixNeedsWhitespace)
{
    const auto m = as_map("exported=1\nexport\tX=2\n");
    EXPECT_EQ(m.at("exported"), "1");
    EXPECT_EQ(m.at("X"), "2");
}

TEST(AppDotenv, ReadsFilesAndReportsMissingAsNullopt)
{
    const auto dir = std::filesystem::temp_directory_path() / "tsm_process_dotenv_test";
    std::filesystem::create_directories(dir);
    const auto file = dir / "test.env";
    std::ofstream(file) << "PORT=8080\n";
    const auto entries = snicholls::read_dotenv_file(file);
    ASSERT_TRUE(entries.has_value());
    EXPECT_EQ(entries->at(0).value, "8080");
    EXPECT_FALSE(snicholls::read_dotenv_file(dir / "missing.env").has_value());
    std::filesystem::remove_all(dir);
}

} // namespace
