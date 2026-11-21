#include <gtest/gtest.h>

#include "players.hpp"
#include "support.hpp"

using namespace dpdb;

namespace {

// DDL дословно из players.db живого сервера (сборка 2026-11-04, SQLite движка 3.21.0).
constexpr const char* kEngineDdl =
    "CREATE TABLE DB_Versions (                  Id         INTEGER  PRIMARY KEY NOT NULL,"
    "               Structure  INTEGER              NOT NULL DEFAULT 0 );"
    "CREATE TABLE \"Players\" (            Id    INTEGER  PRIMARY KEY NOT NULL,   Alive INTEGER"
    "              NOT NULL,   UID   CHAR(64) UNIQUE      NOT NULL,   Data  BLOB                 NULL    );"
    "INSERT INTO DB_Versions VALUES(1, 0);";

struct PlayersTest : ::testing::Test {
    test::TempDir dir;
    fs::Path pdb = dir / "storage_1/players.db";
    Store store{dir / "s.db", [](std::string_view) {}};

    void SetUp() override { std::filesystem::create_directories(pdb.parent_path()); }

    sql::Db engine() { return sql::Db(pdb, sql::kReadWrite | sql::kCreate); }
    void create(std::string_view extra = {}) {
        auto e = engine();
        e.exec(kEngineDdl);
        e.exec(extra);
    }
    void player(std::int64_t id, std::int64_t alive, std::string_view uid, const char* data) {
        auto e = engine();
        auto q = e.prepare("INSERT OR REPLACE INTO Players(Id, Alive, UID, Data) VALUES(?, ?, ?, ?)");
        q.bind(1, id).bind(2, alive).bind(3, uid);
        const fs::Bytes b = data ? test::bytes(data) : fs::Bytes{};
        data ? q.bind(4, std::span<const std::byte>(b)) : q.bind(4, nullptr);
        q.step();
    }
    std::pair<PlayersReport, std::int64_t> capture(int busy_ms = 2000) {
        auto w = store.begin("cycle");
        const PlayersReport r = capture_players(store, w, pdb, dir.path, busy_ms);
        return {r, w.commit()};
    }
    // Все строки Players одной строкой: «id|alive|uid|data;…» (NULL как <null>).
    static std::string dump(const fs::Path& db) {
        sql::Db d(db, sql::kReadOnly);
        auto q = d.prepare("SELECT Id, Alive, UID, Data FROM Players ORDER BY Id");
        std::string out;
        while (q.step())
            out += std::to_string(q.i64(0)) + "|" + std::to_string(q.i64(1)) + "|" + q.text(2) + "|" +
                   (q.null(3) ? "<null>" : test::str(q.blob(3))) + ";";
        return out;
    }
};

}  // namespace

TEST_F(PlayersTest, HealthOfMissingGoodAndGarbageFile) {
    EXPECT_EQ(check_players(pdb), PlayersHealth::Missing);
    create();
    EXPECT_EQ(check_players(pdb), PlayersHealth::Ok);
    test::write(pdb, std::string(8192, 'x'));
    EXPECT_EQ(check_players(pdb), PlayersHealth::Corrupt);
}

// Пустой файл без таблиц — не база движка: игроков в нём нет, брать из него нечего.
TEST_F(PlayersTest, FileWithoutEngineTablesIsEmptyNotOk) {
    test::write(pdb, "");
    EXPECT_EQ(check_players(pdb), PlayersHealth::Empty);
    const auto [r, gen] = capture();
    EXPECT_EQ(r.mode, PlayersMode::None);
    EXPECT_EQ(gen, 0);
}

TEST_F(PlayersTest, CapturesRowsAndEngineSchema) {
    create();
    player(1, 1, "uid-a", "blob-a");
    player(2, 0, "uid-b", nullptr);
    const auto [r, gen] = capture();
    EXPECT_EQ(r.mode, PlayersMode::Rows);
    EXPECT_EQ(r.stored, 2);
    EXPECT_EQ(gen, 1);
    ASSERT_EQ(store.chars().size(), 2u);
    EXPECT_EQ(store.chars().at("uid-a").id, 1);
    EXPECT_EQ(test::str(store.blob(store.chars().at("uid-a").blob)), "blob-a");
    EXPECT_EQ(store.chars().at("uid-b").blob, 0);
    EXPECT_EQ(store.files().count(std::string(kPlayersSchema)), 1u);
}

TEST_F(PlayersTest, UnchangedPlayersMakeNoGeneration) {
    create();
    player(1, 1, "uid-a", "blob-a");
    capture();
    const auto [r, gen] = capture();
    EXPECT_EQ(gen, 0);
    EXPECT_EQ(r.stored, 0);
}

TEST_F(PlayersTest, OnlyChangedRowsAreStoredAndRemovedRowsTombstoned) {
    create();
    player(1, 1, "uid-a", "a1");
    player(2, 1, "uid-b", "b1");
    player(3, 1, "uid-c", "c1");
    capture();
    player(1, 1, "uid-a", "a2");
    player(4, 1, "uid-d", "d1");
    engine().exec("DELETE FROM Players WHERE Id = 3");
    const auto [r, gen] = capture();
    EXPECT_EQ(gen, 2);
    EXPECT_EQ(r.stored, 2);
    EXPECT_EQ(r.gone, 1);
    EXPECT_EQ(store.chars().count("uid-c"), 0u);
    EXPECT_EQ(store.chars().at("uid-b").id, 2);
    EXPECT_EQ(store.db().scalar("SELECT count(*) FROM characters WHERE gen = 2"), 3);
}

TEST_F(PlayersTest, RebuiltDatabaseEqualsOriginal) {
    create();
    player(1, 1, "uid-a", "a1");
    player(7, 0, "uid-b", nullptr);
    player(9, 1, "uid-c", "");
    capture();
    const std::string before = dump(pdb);
    std::string ddl_before;
    {
        sql::Db d(pdb, sql::kReadOnly);
        auto q = d.prepare("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY rowid");
        while (q.step())
            ddl_before += q.text(0) + "\n";
    }
    std::filesystem::remove(pdb);
    test::write(dir / "storage_1/players.db-journal", "stale journal of the lost database");

    EXPECT_TRUE(rebuild_players(store, store.state(store.head_gen()), pdb));
    EXPECT_EQ(dump(pdb), before);
    sql::Db d(pdb, sql::kReadOnly);
    std::string ddl_after;
    auto q = d.prepare("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL ORDER BY rowid");
    while (q.step())
        ddl_after += q.text(0) + "\n";
    EXPECT_EQ(ddl_after, ddl_before);
    EXPECT_EQ(d.scalar("SELECT Structure FROM DB_Versions WHERE Id = 1"), 0);
    EXPECT_EQ(d.scalar_text("PRAGMA journal_mode"), "delete");
    // чужой журнал рядом с новой базой SQLite принял бы за незавершённую транзакцию
    EXPECT_FALSE(std::filesystem::exists(dir / "storage_1/players.db-journal"));
    EXPECT_EQ(check_players(pdb), PlayersHealth::Ok);
}

TEST_F(PlayersTest, RebuildFromOlderGenerationGivesOlderCharacters) {
    create();
    player(1, 1, "uid-a", "a1");
    capture();
    player(1, 0, "uid-a", "a2");
    capture();
    EXPECT_TRUE(rebuild_players(store, store.state(1), dir / "old.db"));
    EXPECT_EQ(dump(dir / "old.db"), "1|1|uid-a|a1;");
}

TEST_F(PlayersTest, NothingToRebuildFromEmptyState) {
    EXPECT_FALSE(rebuild_players(store, store.state(0), dir / "none.db"));
    EXPECT_FALSE(std::filesystem::exists(dir / "none.db"));
}

// Патч добавил служебную таблицу — она переносится целиком, плагин править не нужно.
TEST_F(PlayersTest, UnknownAuxiliaryTablesAreCarriedOver) {
    create("CREATE TABLE Extra(k TEXT PRIMARY KEY, v BLOB); INSERT INTO Extra VALUES('it''s', x'00ff');"
           "CREATE INDEX extra_v ON Extra(v);");
    player(1, 1, "uid-a", "a1");
    EXPECT_EQ(capture().first.mode, PlayersMode::Rows);
    EXPECT_TRUE(rebuild_players(store, store.state(store.head_gen()), dir / "re.db"));
    sql::Db d(dir / "re.db", sql::kReadOnly);
    EXPECT_EQ(d.scalar_text("SELECT hex(v) FROM Extra WHERE k = 'it''s'"), "00FF");
    EXPECT_EQ(d.scalar("SELECT count(*) FROM sqlite_master WHERE name = 'extra_v'"), 1);
    EXPECT_EQ(dump(dir / "re.db"), "1|1|uid-a|a1;");
}

// Патч изменил саму таблицу Players: построчный разбор больше не уверен в себе и
// плагин переходит на копию файла целиком — игроки защищены и без обновления плагина.
TEST_F(PlayersTest, UnknownPlayersLayoutFallsBackToWholeFile) {
    create();
    player(1, 1, "uid-a", "a1");
    EXPECT_EQ(capture().first.mode, PlayersMode::Rows);
    engine().exec("ALTER TABLE Players ADD COLUMN Extra INTEGER DEFAULT 5");

    const auto [r, gen] = capture();
    EXPECT_EQ(r.mode, PlayersMode::File);
    EXPECT_EQ(gen, 2);
    EXPECT_EQ(store.files().count(std::string(kPlayersFile)), 1u);
    EXPECT_EQ(store.files().count(std::string(kPlayersSchema)), 0u);
    EXPECT_TRUE(store.chars().empty());

    EXPECT_TRUE(rebuild_players(store, store.state(2), dir / "re.db"));
    sql::Db d(dir / "re.db", sql::kReadOnly);
    EXPECT_EQ(d.scalar("SELECT Extra FROM Players WHERE UID = 'uid-a'"), 5);
    // прошлое поколение по-прежнему собирается построчно
    EXPECT_TRUE(rebuild_players(store, store.state(1), dir / "old.db"));
    EXPECT_EQ(dump(dir / "old.db"), "1|1|uid-a|a1;");
    // а неизменившийся файл второй раз не сохраняется
    EXPECT_EQ(capture().second, 0);
}

TEST_F(PlayersTest, BusyEngineDatabaseIsSkippedNotBroken) {
    create();
    player(1, 1, "uid-a", "a1");
    capture();
    player(1, 1, "uid-a", "a2");
    auto writer = engine();
    writer.exec("BEGIN EXCLUSIVE");
    const auto [r, gen] = capture(50);
    EXPECT_TRUE(r.busy);
    EXPECT_EQ(gen, 0);
    writer.exec("COMMIT");
    EXPECT_EQ(capture(50).first.stored, 1);
}
