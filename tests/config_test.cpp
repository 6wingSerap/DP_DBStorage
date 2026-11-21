#include <gtest/gtest.h>

#include "config.hpp"
#include "support.hpp"

using namespace dpdb;

TEST(Config, DefaultsWorkWithoutAnyFile) {
    const auto r = parse_config("");
    ASSERT_TRUE(r.error.empty());
    EXPECT_TRUE(r.cfg.enabled);
    EXPECT_EQ(r.cfg.keep_hours, 24);
    EXPECT_EQ(r.cfg.max_db_mb, 8192);
    EXPECT_EQ(r.cfg.players_period_sec, 30);
    EXPECT_EQ(r.cfg.rescan_sec, 30);
}

TEST(Config, ParsesKeysCommentsAndBom) {
    const auto r = parse_config("\xEF\xBB\xBF# комментарий\r\n"
                                "keep_hours=6\n"
                                "  storage_dir = D:\\Миссии\\storage_1  \n"
                                "enabled = 0\n");
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_EQ(r.cfg.keep_hours, 6);
    EXPECT_EQ(r.cfg.storage_dir, "D:\\Миссии\\storage_1");
    EXPECT_FALSE(r.cfg.enabled);
}

// Опечатка в ключе не должна молча превращаться в значение по умолчанию.
TEST(Config, UnknownKeyAndBadValuesAreErrors) {
    EXPECT_NE(parse_config("keep_hour = 6").error.find("keep_hour"), std::string::npos);
    EXPECT_FALSE(parse_config("keep_hours = many").error.empty());
    EXPECT_FALSE(parse_config("keep_hours = -1").error.empty());
    EXPECT_FALSE(parse_config("just a line").error.empty());
    // конфиг прежней версии плагина: понятная ошибка, а не тихая работа «как-нибудь»
    EXPECT_NE(parse_config("host = 127.0.0.1").error.find("host"), std::string::npos);
}

TEST(CommandLine, ReadsArgumentsTheWayTheEngineDoes) {
    const std::wstring cmd = L"\"C:\\Dayz Server\\DayZServer_x64.exe\" -config=serverDZ.cfg -port=2302 "
                             L"\"-profiles=C:\\My Profiles\" -PROFILES=last -mission .\\mpmissions\\x -doLogs";
    EXPECT_EQ(arg(cmd, L"config"), L"serverDZ.cfg");
    EXPECT_EQ(arg(cmd, L"profiles"), L"last");                  // регистр не важен, побеждает последний
    EXPECT_EQ(arg(cmd, L"mission"), L".\\mpmissions\\x");       // форма «-ключ значение»
    EXPECT_EQ(arg(cmd, L"doLogs"), L"");                        // флаг без значения
    EXPECT_EQ(arg(cmd, L"nope"), L"");
    EXPECT_EQ(arg(L"x.exe \"-profiles=C:\\My Profiles\"", L"profiles"), L"C:\\My Profiles");
    EXPECT_EQ(arg(L"x.exe -profiles=\"C:\\My Profiles\" -a", L"profiles"), L"C:\\My Profiles");
}

TEST(ServerCfg, FindsTemplateAndInstanceIdIgnoringComments) {
    const auto c = parse_server_cfg(R"(hostname = "my server";   // instanceId = 99;
/* template="commented.out"; */
instanceId = 7;			// идентификатор
class Missions
{
    class DayZ
    {
        template="dayzOffline.chernarusplus"; // миссия
    };
};)");
    EXPECT_EQ(c.mission_template, "dayzOffline.chernarusplus");
    ASSERT_TRUE(c.instance_id.has_value());
    EXPECT_EQ(*c.instance_id, 7);
}

TEST(ServerCfg, NegativeInstanceAndMissingKeys) {
    EXPECT_EQ(*parse_server_cfg("instanceId=-1;").instance_id, -1);
    const auto none = parse_server_cfg("hostname = \"x\"; myinstanceId = 5; templateX = \"y\";");
    EXPECT_FALSE(none.instance_id.has_value());
    EXPECT_TRUE(none.mission_template.empty());
}

namespace {

struct PathsTest : ::testing::Test {
    test::TempDir dir;
    fs::Path exe = dir / "server";
    fs::Path appdata = dir / "appdata";
    void SetUp() override {
        test::write(exe / "serverDZ.cfg",
                    "instanceId = 3;\nclass Missions { class DayZ { template=\"dayzOffline.enoch\"; }; };");
    }
};

}  // namespace

TEST_F(PathsTest, ResolvesFromConfigTemplate) {
    const auto p = resolve_paths(exe, L"x.exe -config=serverDZ.cfg -profiles=prof", appdata);
    ASSERT_TRUE(p.resolved) << p.why;
    EXPECT_EQ(p.profile, exe / "prof");
    EXPECT_EQ(p.mission, exe / "mpmissions" / "dayzOffline.enoch");
    EXPECT_EQ(p.storage, exe / "mpmissions" / "dayzOffline.enoch" / "storage_3");
    EXPECT_EQ(p.instance_id, 3);
    EXPECT_EQ(db_file_name(p.storage), L"dayzOffline.enoch.storage_3.db");
    EXPECT_EQ(db_file_name(L"D:/srv/mpmissions/my.map/storage_-1/"), L"my.map.storage_-1.db");
}

TEST_F(PathsTest, MissionArgumentWinsAndProfileDefaultsLikeTheGame) {
    const auto p = resolve_paths(exe, L"x.exe -mission=DEBUG/mpmissions/dayzOffline.chernarusplus", appdata);
    ASSERT_TRUE(p.resolved) << p.why;
    EXPECT_EQ(p.mission, (exe / "DEBUG/mpmissions/dayzOffline.chernarusplus").lexically_normal());
    EXPECT_EQ(p.storage.filename(), "storage_3");
    EXPECT_EQ(p.profile, appdata / "DayZ");
}

TEST_F(PathsTest, AbsoluteArgumentsAreKeptAsIs) {
    const fs::Path prof = dir / "elsewhere";
    const auto p = resolve_paths(exe, L"x.exe \"-profiles=" + prof.wstring() + L"\"", appdata);
    EXPECT_EQ(p.profile, prof);
}

TEST_F(PathsTest, UnresolvableSetupSaysWhy) {
    const auto no_cfg = resolve_paths(exe, L"x.exe -config=missing.cfg", appdata);
    EXPECT_FALSE(no_cfg.resolved);
    EXPECT_FALSE(no_cfg.why.empty());

    test::write(exe / "noid.cfg", "class Missions { class DayZ { template=\"m\"; }; };");
    EXPECT_FALSE(resolve_paths(exe, L"x.exe -config=noid.cfg", appdata).resolved);

    // свой корень хранилища: его раскладку не угадываем
    const auto custom = resolve_paths(exe, L"x.exe -config=serverDZ.cfg -storage=D:\\st", appdata);
    EXPECT_FALSE(custom.resolved);
    EXPECT_NE(custom.why.find("storage_dir"), std::string::npos);
}

TEST(Version, ReadsFileVersionOfSystemBinary) {
    const std::string v = file_version(L"C:\\Windows\\System32\\kernel32.dll");
    EXPECT_FALSE(v.empty());
    EXPECT_NE(v.find('.'), std::string::npos);
    EXPECT_EQ(file_version(L"C:\\no\\such.exe"), "");
}
