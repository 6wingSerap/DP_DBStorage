#include <gtest/gtest.h>

#include "players.hpp"
#include "restore.hpp"
#include "support.hpp"
#include "sync.hpp"

using namespace dpdb;

namespace {

struct RestoreTest : ::testing::Test {
    test::TempDir dir;
    fs::Path root = dir / "storage_1";
    fs::Path attic = dir / "quarantine";
    Store store{dir / "s.db", [](std::string_view) {}};
    SkipCache skip;
    std::vector<std::string> log;
    Log logger = [this](std::string_view s) { log.emplace_back(s); };

    std::int64_t capture(bool deletions = true) {
        auto w = store.begin("cycle");
        sync_files(store, w, root, {.deletions = deletions}, skip);
        capture_players(store, w, root / "players.db", dir.path);
        return w.commit();
    }
    RestoreReport fix() { return repair(store, root, skip, check_players(root / "players.db"), attic, logger); }
    void players(std::string_view uid, std::string_view data) {
        std::filesystem::create_directories(root);
        sql::Db e(root / "players.db", sql::kReadWrite | sql::kCreate);
        e.exec("CREATE TABLE IF NOT EXISTS DB_Versions(Id INTEGER PRIMARY KEY NOT NULL, Structure INTEGER NOT NULL"
               " DEFAULT 0); CREATE TABLE IF NOT EXISTS Players(Id INTEGER PRIMARY KEY NOT NULL, Alive INTEGER NOT"
               " NULL, UID CHAR(64) UNIQUE NOT NULL, Data BLOB NULL)");
        const fs::Bytes b = test::bytes(data);
        e.prepare("INSERT OR REPLACE INTO Players(Id, Alive, UID, Data) VALUES(1, 1, ?, ?)")
            .bind(1, uid)
            .bind(2, std::span<const std::byte>(b))
            .step();
    }
    std::string player_data() {
        sql::Db e(root / "players.db", sql::kReadOnly);
        auto q = e.prepare("SELECT Data FROM Players WHERE Id = 1");
        return q.step() ? test::str(q.blob(0)) : "<нет>";
    }
};

fs::Bytes layer(std::uint32_t fmt, std::uint32_t stamp, std::string_view payload) {
    return make_layer("STORAGE_DYNAMIC", fmt, 144, stamp, test::bytes(payload));
}

std::string str(const fs::Bytes& b) {
    return test::str(b);
}

}  // namespace

TEST_F(RestoreTest, MissingFilesComeBackExistingStayUntouched) {
    test::write(root / "data/a.bin", "a1");
    test::write(root / "data/b.bin", "b1");
    capture();
    std::filesystem::remove(root / "data/a.bin");
    test::write(root / "data/b.bin", "b2 on disk is newer");

    const auto r = fix();
    EXPECT_EQ(r.restored, 1);
    EXPECT_EQ(r.repaired, 0);
    EXPECT_EQ(test::read(root / "data/a.bin"), "a1");
    EXPECT_EQ(test::read(root / "data/b.bin"), "b2 on disk is newer");
}

TEST_F(RestoreTest, WholeStorageIsRebuiltFromNothing) {
    test::write(root / "data/dynamic_000.bin", layer(39, 5, "world"));
    test::write(root / "spawnpoints.bin", "sp");
    test::write(root / "communityframework/deep/mod.bin", "cf");
    players("uid-a", "character");
    capture();
    std::filesystem::remove_all(root);

    const auto r = fix();
    EXPECT_EQ(r.restored, 3);
    EXPECT_TRUE(r.players);
    EXPECT_EQ(test::read(root / "data/dynamic_000.bin"), str(layer(39, 5, "world")));
    EXPECT_EQ(test::read(root / "communityframework/deep/mod.bin"), "cf");
    EXPECT_EQ(player_data(), "character");
}

// Краш посреди сохранения: слот оборван. Движок с storageAutoFix=0 такой файл не отсеял бы.
TEST_F(RestoreTest, TornLayerIsReplacedByLastGoodVersion) {
    test::write(root / "data/dynamic_000.bin", layer(39, 5, "good"));
    capture();
    auto torn = layer(39, 6, "newer, but the write never finished");
    torn.resize(torn.size() - 10);
    test::write(root / "data/dynamic_000.bin", torn);
    capture(false);   // проход старта: оборванный файл в базу не берётся

    const auto r = fix();
    EXPECT_EQ(r.repaired, 1);
    EXPECT_EQ(test::read(root / "data/dynamic_000.bin"), str(layer(39, 5, "good")));
}

// Формат сменился (патч игры), и первая же запись оборвалась: наше знание раскладки на
// этой версии ещё ничем не подтверждено — файл не трогаем.
TEST_F(RestoreTest, TornLayerOfUnprovenFormatIsLeftAlone) {
    test::write(root / "data/dynamic_000.bin", layer(39, 5, "good"));
    capture();
    auto torn = layer(40, 6, "new format, torn");
    torn.resize(torn.size() - 10);
    test::write(root / "data/dynamic_000.bin", torn);
    capture(false);

    const auto r = fix();
    EXPECT_EQ(r.repaired, 0);
    EXPECT_EQ(test::read(root / "data/dynamic_000.bin"), str(torn));
}

// Отключение питания: слот остался обнулённым. Формат тут ни при чём — версия не важна.
TEST_F(RestoreTest, BlankFileInPlaceOfLayerIsReplaced) {
    test::write(root / "data/dynamic_000.bin", layer(39, 5, "good"));
    test::write(root / "mod/empty.lock", "");
    capture();
    test::write(root / "data/dynamic_000.bin", fs::Bytes(500));
    capture(false);

    const auto r = fix();
    EXPECT_EQ(r.repaired, 1);
    EXPECT_EQ(test::read(root / "data/dynamic_000.bin"), str(layer(39, 5, "good")));
    EXPECT_EQ(test::read(root / "mod/empty.lock"), "");
}

TEST_F(RestoreTest, DamagedBlobIsNeverWrittenToDisk) {
    test::write(root / "a.bin", "a1");
    test::write(root / "b.bin", "b1");
    capture();
    std::filesystem::remove_all(root);
    store.db().exec("UPDATE blobs SET data = x'00' WHERE id = (SELECT blob FROM files WHERE path = 'a.bin')");

    const auto r = fix();
    EXPECT_EQ(r.failed, 1);
    EXPECT_EQ(r.restored, 1);
    EXPECT_FALSE(std::filesystem::exists(root / "a.bin"));
    EXPECT_EQ(test::read(root / "b.bin"), "b1");
    EXPECT_FALSE(log.empty());
}

TEST_F(RestoreTest, StaleTempFilesAreSwept) {
    test::write(root / "data/a.bin", "a1");
    capture();
    test::write(root / "data/a.bin.dpdb-tmp", "half-written by a previous run");
    fix();
    EXPECT_FALSE(std::filesystem::exists(root / "data/a.bin.dpdb-tmp"));
}

TEST_F(RestoreTest, HealthyPlayersDatabaseIsNotTouched) {
    players("uid-a", "v1");
    capture();
    players("uid-a", "v2 newer on disk");
    EXPECT_FALSE(fix().players);
    EXPECT_EQ(player_data(), "v2 newer on disk");
}

TEST_F(RestoreTest, CorruptPlayersDatabaseIsSetAsideAndRebuilt) {
    players("uid-a", "v1");
    capture();
    test::write(root / "players.db", std::string(8192, 'x'));

    EXPECT_TRUE(fix().players);
    EXPECT_EQ(player_data(), "v1");
    ASSERT_EQ(fs::scan(attic).size(), 1u) << "испорченный файл сохраняется для разбора";
    EXPECT_EQ(fs::scan(attic)[0].size, 8192);
}

TEST_F(RestoreTest, CorruptPlayersDatabaseWithNothingToRestoreIsLeftAlone) {
    test::write(root / "players.db", std::string(8192, 'x'));
    EXPECT_FALSE(fix().players);
    EXPECT_EQ(test::read(root / "players.db"), std::string(8192, 'x'));
}

TEST_F(RestoreTest, MaterializeMakesDirectoryExactlyTheGeneration) {
    test::write(root / "data/a.bin", "a1");
    test::write(root / "data/gone-later.bin", "x");
    players("uid-a", "p1");
    const std::int64_t g1 = capture();
    test::write(root / "data/a.bin", "a2");
    test::write(root / "data/new.bin", "n");
    std::filesystem::remove(root / "data/gone-later.bin");
    players("uid-a", "p2");
    capture();

    const auto r = materialize(store, g1, root, logger);
    EXPECT_EQ(r.restored, 2);
    EXPECT_EQ(r.removed, 1);
    EXPECT_TRUE(r.players);
    EXPECT_EQ(test::read(root / "data/a.bin"), "a1");
    EXPECT_EQ(test::read(root / "data/gone-later.bin"), "x");
    EXPECT_FALSE(std::filesystem::exists(root / "data/new.bin"));
    EXPECT_EQ(player_data(), "p1");
}

TEST_F(RestoreTest, MaterializeTouchesNothingIfAnyBlobIsDamaged) {
    test::write(root / "a.bin", "a1");
    test::write(root / "b.bin", "b1");
    const std::int64_t g1 = capture();
    test::write(root / "a.bin", "a2");
    capture();
    store.db().exec("UPDATE blobs SET data = x'00' WHERE id = (SELECT blob FROM files WHERE path = 'b.bin')");

    EXPECT_THROW(materialize(store, g1, root, logger), sql::Error);
    EXPECT_EQ(test::read(root / "a.bin"), "a2");
}

TEST_F(RestoreTest, WipeScopes) {
    test::write(root / "data/a.bin", "a1");
    test::write(root / "spawnpoints.bin", "sp");
    players("uid-a", "p1");
    test::write(root / "players.db-journal", "j");

    EXPECT_EQ(wipe(root, WipeScope::World), 2);
    EXPECT_TRUE(std::filesystem::exists(root / "players.db"));
    EXPECT_EQ(wipe(root, WipeScope::Players), 2);
    EXPECT_TRUE(fs::scan(root).empty());

    test::write(root / "data/a.bin", "a1");
    players("uid-a", "p1");
    EXPECT_EQ(wipe(root, WipeScope::All), 2);
}
