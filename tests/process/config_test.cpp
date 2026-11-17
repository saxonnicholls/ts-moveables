//
//  config_test.cpp
//  TSMoveables
//
//  Copyright 2010-2026 Saxon Herschel Nicholls
//
//  Thread Safe Moveables - process/config.hpp tests, ported from an earlier application framework's config tests.
//

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "mini_gtest.hpp"

#include "process/config.hpp"

namespace {

using namespace std::chrono_literals;
using snicholls::config;
using snicholls::config_error;
using snicholls::config_source;

// A temporary directory with .env fixtures (tests never read real credentials).
struct temp_dir
{
    std::filesystem::path path;
    temp_dir()
    {
        path = std::filesystem::temp_directory_path() /
               ("tsm_process_config_" + std::to_string(::getpid()) + "_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path);
    }
    ~temp_dir() { std::filesystem::remove_all(path); }
    std::filesystem::path write(const std::string& name, const std::string& text) const
    {
        std::ofstream(path / name) << text;
        return path / name;
    }
};

TEST(AppConfigParse, StrictNumbers)
{
    EXPECT_EQ(snicholls::parse_value<int>("42"), 42);
    EXPECT_EQ(snicholls::parse_value<int>(" +7 "), 7);
    EXPECT_EQ(snicholls::parse_value<double>("2.5e3"), 2500.0);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<int>("12abc")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<int>("")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<std::uint16_t>("70000")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<unsigned>("-1")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<double>("1.5x")), snicholls::bad_config_value);
}

TEST(AppConfigParse, Booleans)
{
    for (const char* t : {"true", "TRUE", "1", "yes", "On"})
        EXPECT_TRUE(snicholls::parse_value<bool>(t)) << t;
    for (const char* f : {"false", "0", "no", "OFF"})
        EXPECT_FALSE(snicholls::parse_value<bool>(f)) << f;
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<bool>("maybe")), snicholls::bad_config_value);
}

TEST(AppConfigParse, DurationsNeedUnitsAndConvertExactly)
{
    EXPECT_EQ(snicholls::parse_value<std::chrono::milliseconds>("250ms"), 250ms);
    EXPECT_EQ(snicholls::parse_value<std::chrono::milliseconds>("30s"), 30s);
    EXPECT_EQ(snicholls::parse_value<std::chrono::seconds>("5m"), 300s);
    EXPECT_EQ(snicholls::parse_value<std::chrono::seconds>("2 h"), 7200s);
    EXPECT_EQ(snicholls::parse_value<std::chrono::nanoseconds>("3us"), std::chrono::nanoseconds(3000));
    EXPECT_EQ(snicholls::parse_value<std::chrono::microseconds>("7us"), std::chrono::microseconds(7));
    EXPECT_EQ(snicholls::parse_value<std::chrono::minutes>("1min"), std::chrono::minutes(1));
    EXPECT_EQ(snicholls::parse_value<std::chrono::milliseconds>("-5ms"), -5ms);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<std::chrono::seconds>("1500ms")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<std::chrono::seconds>("10")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<std::chrono::seconds>("ms")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<std::chrono::seconds>("-")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<std::chrono::duration<std::int8_t>>("200s")), snicholls::bad_config_value);
}

TEST(AppConfigParse, OptionalsVectorsPathsSecrets)
{
    EXPECT_EQ(snicholls::parse_value<std::optional<int>>(""), std::nullopt);
    EXPECT_EQ(snicholls::parse_value<std::optional<int>>("3"), 3);
    EXPECT_EQ(snicholls::parse_value<std::vector<int>>("[1, 2,3]"), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(snicholls::parse_value<std::vector<std::string>>("a,b"), (std::vector<std::string>{"a", "b"}));
    EXPECT_TRUE(snicholls::parse_value<std::vector<int>>("[]").empty());
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<std::vector<int>>("1,,2")), snicholls::bad_config_value);
    EXPECT_EQ(snicholls::parse_value<std::filesystem::path>("/tmp/x"), std::filesystem::path("/tmp/x"));
    EXPECT_EQ(snicholls::parse_value<snicholls::secret<std::string>>("pw").reveal(), "pw");
    static_assert(snicholls::config_value<int>);
    static_assert(!snicholls::config_value<char>);
}

TEST(AppConfig, TypedAccess)
{
    config cfg;
    cfg.set("PORT", "8080");
    cfg.set("BAD", "eighty");
    EXPECT_EQ(cfg.get<int>("PORT"), 8080);
    EXPECT_EQ(cfg.get_or<int>("MISSING", 7), 7);
    EXPECT_EQ(cfg.find<int>("MISSING"), std::nullopt);
    EXPECT_EQ(cfg.find<int>("PORT"), 8080);
    EXPECT_THROW(static_cast<void>(cfg.get<int>("MISSING")), config_error);
    EXPECT_THROW(static_cast<void>(cfg.get_or<int>("BAD", 1)), config_error);   // invalid never becomes the default
    EXPECT_TRUE(cfg.contains("PORT"));
    EXPECT_EQ(cfg.size(), 2u);
    EXPECT_EQ(cfg.keys(), (std::vector<std::string>{"BAD", "PORT"}));
}

TEST(AppConfig, ErrorsMaskSecretValues)
{
    config cfg;
    cfg.set("DB_PASSWORD", "hunter2");
    cfg.set("RETRIES", "lots");
    try
    {
        static_cast<void>(cfg.get<int>("DB_PASSWORD"));
        FAIL();
    }
    catch (const config_error& e)
    {
        EXPECT_EQ(std::string(e.what()).find("hunter2"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("DB_PASSWORD"), std::string::npos);
    }
    try
    {
        static_cast<void>(cfg.get<int>("RETRIES"));
        FAIL();
    }
    catch (const config_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("\"lots\""), std::string::npos);   // non-secrets are shown
    }
}

TEST(AppConfig, DisplayAndDescribeMaskSecrets)
{
    config cfg;
    cfg.set("API_TOKEN", "t0k3n", config_source::env_file, ".env:3");
    cfg.set("DATABASE_URL", "postgres://u:pw@h/db", config_source::command_line, "--database-url");
    cfg.set("CUSTOM", "mine");
    cfg.mark_secret("CUSTOM");
    cfg.set("PATH", "/bin", config_source::environment, "$PATH");
    EXPECT_EQ(cfg.display("API_TOKEN"), "********");
    EXPECT_EQ(cfg.display("DATABASE_URL"), "postgres://u:********@h/db");
    EXPECT_EQ(cfg.display("CUSTOM"), "********");
    EXPECT_EQ(cfg.display("NOPE"), "<unset>");
    const std::string d = cfg.describe();
    EXPECT_EQ(d.find("t0k3n"), std::string::npos);
    EXPECT_EQ(d.find("mine"), std::string::npos);
    EXPECT_EQ(d.find(":pw@"), std::string::npos);
    EXPECT_NE(d.find("# env-file .env:3"), std::string::npos);
    EXPECT_EQ(d.find("PATH="), std::string::npos);   // environment-only keys hidden by default
    EXPECT_NE(cfg.describe(true).find("PATH=/bin"), std::string::npos);
}

TEST(AppConfig, ProfileStripsPrefixAndKeepsSecretMarks)
{
    config cfg;
    cfg.set("DB_HOST", "h");
    cfg.set("DB_PASS", "x");
    cfg.mark_secret("DB_PASS");
    cfg.set("DBX", "no");
    cfg.set("OTHER", "o");
    const config db = cfg.profile("DB");
    EXPECT_EQ(db.size(), 2u);
    EXPECT_EQ(db.get<std::string>("HOST"), "h");
    EXPECT_EQ(db.display("PASS"), "********");
    EXPECT_EQ(cfg.profile("DB_").size(), 2u);
}

TEST(AppConfig, PrecedenceDefaultsFilesEnvironmentCommandLine)
{
    temp_dir dir;
    const auto base = dir.write("base.env", "A=file1\nB=file1\nC=file1\nD=file1\nREF=${E_ONLY}/x\n");
    const auto local = dir.write("local.env", "B=file2\nC=file2\nD=file2\nCHAIN=${A}-${B}\n");
    snicholls::config_sources s;
    s.defaults = {{"A", "default"}, {"Z", "default"}};
    s.env_files = {base, local, dir.path / "missing.env"};
    s.environment = std::map<std::string, std::string>{{"C", "env"}, {"D", "env"}, {"E_ONLY", "/e"}};
    s.overrides = {{"D", "cli"}};
    const config cfg = snicholls::load_config(s);
    EXPECT_EQ(cfg.get<std::string>("Z"), "default");
    EXPECT_EQ(cfg.get<std::string>("A"), "file1");
    EXPECT_EQ(cfg.get<std::string>("B"), "file2");
    EXPECT_EQ(cfg.get<std::string>("C"), "env");
    EXPECT_EQ(cfg.get<std::string>("D"), "cli");
    EXPECT_EQ(cfg.get<std::string>("REF"), "/e/x");
    EXPECT_EQ(cfg.get<std::string>("CHAIN"), "file1-file2");
    EXPECT_EQ(cfg.entry("D")->source, config_source::command_line);
    EXPECT_EQ(cfg.entry("B")->origin, local.string() + ":1");
    EXPECT_EQ(cfg.entry("C")->origin, "$C");
}

TEST(AppConfig, RequiredMissingFileIsAnError)
{
    snicholls::config_sources s;
    s.env_files = {"/nonexistent/snicholls/app.env"};
    s.read_environment = false;
    EXPECT_NO_THROW(static_cast<void>(snicholls::load_config(s)));
    s.env_files_required = true;
    EXPECT_THROW(static_cast<void>(snicholls::load_config(s)), config_error);
}

TEST(AppConfig, ProcessEnvironmentIsReadWhenNotReplaced)
{
    ::setenv("TSM_PROCESS_CONFIG_TEST_VAR", "from-env", 1);
    snicholls::config_sources s;
    s.env_files.clear();
    const config cfg = snicholls::load_config(s);
    EXPECT_EQ(cfg.get<std::string>("TSM_PROCESS_CONFIG_TEST_VAR"), "from-env");
    ::unsetenv("TSM_PROCESS_CONFIG_TEST_VAR");
    EXPECT_FALSE(snicholls::process_environment().contains("TSM_PROCESS_CONFIG_TEST_VAR"));
}

TEST(AppConfig, SecretKeysFromSources)
{
    snicholls::config_sources s;
    s.env_files.clear();
    s.read_environment = false;
    s.defaults = {{"ODD", "v"}};
    s.secret_keys = {"ODD"};
    EXPECT_EQ(snicholls::load_config(s).display("ODD"), "********");
}

TEST(AppConfig, ExportToEnvironmentOnlyFileKeysWithoutOverwriting)
{
    temp_dir dir;
    const auto file = dir.write("x.env", "TSM_PROCESS_EXPORT_A=file\nTSM_PROCESS_EXPORT_B=file\n");
    ::setenv("TSM_PROCESS_EXPORT_B", "existing", 1);
    snicholls::config_sources s;
    s.env_files = {file};
    s.read_environment = false;
    s.defaults = {{"TSM_PROCESS_EXPORT_DEFAULT", "d"}};
    const config cfg = snicholls::load_config(s);
    EXPECT_EQ(snicholls::export_to_environment(cfg), 2u);
    EXPECT_STREQ(std::getenv("TSM_PROCESS_EXPORT_A"), "file");
    EXPECT_STREQ(std::getenv("TSM_PROCESS_EXPORT_B"), "existing");
    EXPECT_EQ(std::getenv("TSM_PROCESS_EXPORT_DEFAULT"), nullptr);
    snicholls::export_to_environment(cfg, true);
    EXPECT_STREQ(std::getenv("TSM_PROCESS_EXPORT_B"), "file");
    ::unsetenv("TSM_PROCESS_EXPORT_A");
    ::unsetenv("TSM_PROCESS_EXPORT_B");
}

// New here: require names every missing key at once; missing lists them; try_get returns a result
TEST(AppConfig, RequireNamesEveryMissingKey)
{
    config cfg;
    cfg.set("A", "1");
    EXPECT_NO_THROW(cfg.require({"A"}));
    EXPECT_EQ(cfg.missing({"A", "B", "C"}), (std::vector<std::string>{"B", "C"}));
    try
    {
        cfg.require({"A", "B", "C"});
        ADD_FAILURE() << "no error";
    }
    catch (const config_error& e)
    {
        EXPECT_NE(std::string(e.what()).find("keys B, C"), std::string::npos) << e.what();
    }
}

TEST(AppConfig, TryGetReturnsAResult)
{
    config cfg;
    cfg.set("PORT", "80x");
    cfg.set("API_TOKEN", "s3cr3t-and-not-a-number");
    EXPECT_TRUE(cfg.try_get<int>("MISSING").is_err());
    const auto bad = cfg.try_get<int>("PORT");
    ASSERT_TRUE(bad.is_err());
    EXPECT_NE(std::string(bad.unwrap_err_unchecked().what()).find("PORT"), std::string::npos);
    const auto secret = cfg.try_get<int>("API_TOKEN");
    ASSERT_TRUE(secret.is_err());
    EXPECT_EQ(std::string(secret.unwrap_err_unchecked().what()).find("s3cr3t"), std::string::npos);
    cfg.set("PORT", "8080");
    EXPECT_EQ(*cfg.try_get<int>("PORT"), 8080);
}

TEST(AppConfigParse, Maps)
{
    using m = std::map<std::string, int>;
    EXPECT_EQ(snicholls::parse_value<m>("a=1, b = 2"), (m{{"a", 1}, {"b", 2}}));
    EXPECT_EQ(snicholls::parse_value<m>("{x=3}"), (m{{"x", 3}}));
    EXPECT_EQ(snicholls::parse_value<m>(""), m{});
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<m>("a=1, a=2")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<m>("a, b")), snicholls::bad_config_value);
    EXPECT_THROW(static_cast<void>(snicholls::parse_value<m>("a=one")), snicholls::bad_config_value);
}

TEST(AppConfig, KeyFor)
{
    EXPECT_EQ(config::key_for("svc", "db.host"), "SVC_DB_HOST");
    EXPECT_EQ(config::key_for("", "port"), "PORT");
    EXPECT_EQ(config::key_for("A_", "x-y"), "A_X_Y");
}

} // namespace
