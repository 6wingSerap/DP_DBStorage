// Сценарии целиком: то, что обещано администратору, на настоящих файлах и настоящей базе.
#include <gtest/gtest.h>

#include "session.hpp"
#include "support.hpp"

using namespace dpdb;

namespace {

struct SessionTest : ::testing::Test {
    test::TempDir dir;
    fs::Path root = dir / "mission/storage_1";
    fs::Path db = dir / "profile/dp_dbstorage/mission.storage_1.db";
    std::vector<std::string> log;

    Options options(std::string build = "1.30.164289") {
        Options o;
        o.storage_dir = root;
        o.db_path = db;
        o.build = std::move(build);
        o.settle_ms = 0;            // в тестах файлы «отстоялись» сразу
        o.players_period_ms = 0;
        o.log = [this](std::string_view s) { log.emplace_back(s); };
        return o;
    }
    static fs::Bytes layer(std::uint32_t stamp, std::string_view payload, std::string_view tag = "STORAGE_DYNAMIC") {
        return make_layer(tag, 39, 144, stamp, test::bytes(payload));
    }
    // Мир как после цикла сохранения движка с данным stamp.
    void save_world(std::uint32_t stamp) {
        test::write(root / "data/dynamic_000.bin", layer(stamp, "loot #" + std::to_string(stamp)));
        test::write(root / "data/vehicles.bin", layer(stamp, "cars #" + std::to_string(stamp), "STORAGE_VEHICLES"));
        test::write(root / "data/building_001.bin", layer(0, "", "STORAGE_BUILDING"));
        test::write(root / "spawnpoints.bin", "spawnpoints");
    }
    void save_player(std::string_view data) {
        std::filesystem::create_directories(root);
        sql::Db e(root / "players.db", sql::kReadWrite | sql::kCreate);
        e.exec("CREATE TABLE IF NOT EXISTS DB_Versions(Id INTEGER PRIMARY KEY NOT NULL, Structure INTEGER NOT NULL"
               " DEFAULT 0); CREATE TABLE IF NOT EXISTS Players(Id INTEGER PRIMARY KEY NOT NULL, Alive INTEGER NOT"
               " NULL, UID CHAR(64) UNIQUE NOT NULL, Data BLOB NULL); INSERT OR IGNORE INTO DB_Versions VALUES(1, 0)");
        const fs::Bytes b = test::bytes(data);
        e.prepare("INSERT OR REPLACE INTO Players(Id, Alive, UID, Data) VALUES(1, 1, 'uid-a', ?)")
            .bind(1, std::span<const std::byte>(b))
            .step();
    }
    std::string player() {
        sql::Db e(root / "players.db", sql::kReadOnly);
        auto q = e.prepare("SELECT Data FROM Players WHERE Id = 1");
        return q.step() ? test::str(q.blob(0)) : "<нет>";
    }
    std::string world() { return test::read(root / "data/dynamic_000.bin"); }
    static std::string str(const fs::Bytes& b) { return test::str(b); }
};

}  // namespace

TEST_F(SessionTest, FirstBootCapturesExistingStorage) {
    save_world(1);
    save_player("p1");
    Session s(options());
    s.boot();
    const Status st = s.status();
    EXPECT_EQ(st.head_gen, 1);
    EXPECT_EQ(st.files, 5);   // 4 файла + схема players.db
    EXPECT_EQ(st.chars, 1);
    EXPECT_EQ(st.players, PlayersMode::Rows);
    EXPECT_EQ(s.store().generations()[0].kind, "boot");
    EXPECT_EQ(s.store().meta("storage_dir"), fs::to_utf8(root.wstring()));
    EXPECT_FALSE(s.tick());
    EXPECT_EQ(s.status().head_gen, 1) << "без изменений поколений не прибавляется";
}

TEST_F(SessionTest, EmptyServerBootsWithNothingToDo) {
    Session s(options());
    s.boot();
    EXPECT_EQ(s.status().head_gen, 0);
    EXPECT_FALSE(std::filesystem::exists(root)) << "пустой storage сам не создаётся";
}

// Главное обещание: хранилище потеряно целиком — сервер стартует из базы.
TEST_F(SessionTest, LostStorageIsRestoredOnBoot) {
    save_world(7);
    save_player("p7");
    { Session(options()).boot(); }
    const std::string before = world();
    std::filesystem::remove_all(root);

    Session s(options());
    s.boot();
    EXPECT_EQ(world(), before);
    EXPECT_EQ(test::read(root / "data/vehicles.bin"), str(layer(7, "cars #7", "STORAGE_VEHICLES")));
    EXPECT_EQ(test::read(root / "spawnpoints.bin"), "spawnpoints");
    EXPECT_EQ(player(), "p7");
    EXPECT_EQ(s.status().boot_repair.restored, 4);
    EXPECT_TRUE(s.status().boot_repair.players);
    EXPECT_EQ(s.status().head_gen, 1) << "восстановление — не новое состояние";
}

TEST_F(SessionTest, EngineSaveCyclesBecomeGenerations) {
    save_world(1);
    save_player("p1");
    Session s(options());
    s.boot();
    save_world(2);
    save_player("p2");
    EXPECT_FALSE(s.tick());
    EXPECT_EQ(s.status().head_gen, 2);
    EXPECT_EQ(s.status().sync.stored, 2) << "building и spawnpoints не менялись";
    EXPECT_EQ(s.store().generations()[0].kind, "cycle");
    EXPECT_EQ(s.store().generations()[0].chars, 1);
}

// Сервер выключился после последнего прохода: сохранение при выходе есть только на диске.
TEST_F(SessionTest, ShutdownSaveIsPickedUpOnNextBootNotOverwritten) {
    save_world(1);
    { Session(options()).boot(); }
    save_world(2);

    Session s(options());
    s.boot();
    EXPECT_EQ(world(), str(layer(2, "loot #2")));
    EXPECT_EQ(s.status().head_gen, 2);
    EXPECT_EQ(s.status().boot_repair.restored, 0);
}

// Краш посреди сохранения: оборванный слот заменяется последним целым.
TEST_F(SessionTest, TornSaveAfterCrashIsRepairedOnBoot) {
    save_world(1);
    { Session(options()).boot(); }
    auto torn = layer(2, "loot #2, write interrupted by a crash");
    torn.resize(torn.size() - 12);
    test::write(root / "data/dynamic_000.bin", torn);

    Session s(options());
    s.boot();
    EXPECT_EQ(world(), str(layer(1, "loot #1")));
    EXPECT_EQ(s.status().boot_repair.repaired, 1);
    EXPECT_FALSE(s.tick());
    EXPECT_EQ(s.status().head_gen, 1);
}

// Обновление игры: состояние до первого запуска новой сборки закрепляется навсегда.
TEST_F(SessionTest, GameUpdatePinsLastStateOfOldBuild) {
    save_world(1);
    { Session(options("1.30.164289")).boot(); }
    save_world(2);   // сохранение при выходе старой сборки
    {
        Session s(options("1.31.170000"));
        s.boot();
        const auto gens = s.store().generations();
        ASSERT_EQ(gens.size(), 2u);
        EXPECT_TRUE(gens[0].pinned) << "закрепляется состояние, включающее последнее сохранение старой сборки";
        EXPECT_NE(gens[0].note.find("1.30.164289"), std::string::npos);
        EXPECT_NE(gens[0].note.find("1.31.170000"), std::string::npos);
        EXPECT_FALSE(gens[1].pinned);
        EXPECT_EQ(s.store().meta("build"), "1.31.170000");
    }
    Session again(options("1.31.170000"));
    again.boot();
    EXPECT_EQ(again.store().db().scalar("SELECT count(*) FROM generations WHERE pinned = 1"), 1);
}

TEST_F(SessionTest, CorruptDatabaseNeverBlocksTheServer) {
    save_world(3);
    test::write(db, std::string(100'000, 'x'));
    Session s(options());
    s.boot();
    EXPECT_TRUE(s.status().db_replaced);
    EXPECT_EQ(s.status().head_gen, 1) << "мир с диска записан в новую базу";
    EXPECT_EQ(world(), str(layer(3, "loot #3")));
}

TEST_F(SessionTest, DatabaseHasOneOwner) {
    EXPECT_FALSE(Session::in_use(db));
    {
        Session owner(options());
        EXPECT_TRUE(Session::in_use(db));
        EXPECT_THROW(Session second(options()), std::runtime_error);
        Options reader = options();
        reader.exclusive = false;
        EXPECT_NO_THROW(Session third(reader));
    }
    EXPECT_FALSE(Session::in_use(db));
}

TEST_F(SessionTest, RollbackToGenerationIsExactAndUndoable) {
    save_world(1);
    save_player("p1");
    Session s(options());
    s.boot();                                  // поколение 1
    save_world(2);
    save_player("p2");
    test::write(root / "data/new_after.bin", "added later");
    s.tick();                                  // поколение 2
    save_world(3);                             // ещё не записано в базу

    s.restore_generation(1);
    EXPECT_EQ(world(), str(layer(1, "loot #1")));
    EXPECT_EQ(player(), "p1");
    EXPECT_FALSE(std::filesystem::exists(root / "data/new_after.bin"));
    const auto gens = s.store().generations();
    EXPECT_EQ(gens[0].kind, "restore");
    EXPECT_EQ(gens[1].kind, "pre-restore") << "несохранённое состояние диска не теряется";

    s.restore_generation(gens[1].gen);         // отмена отката
    EXPECT_EQ(world(), str(layer(3, "loot #3")));
    EXPECT_EQ(player(), "p2");
    EXPECT_TRUE(std::filesystem::exists(root / "data/new_after.bin"));
}

TEST_F(SessionTest, RollbackToUnknownGenerationIsRefused) {
    save_world(1);
    Session s(options());
    s.boot();
    EXPECT_THROW(s.restore_generation(42), std::runtime_error);
    EXPECT_EQ(world(), str(layer(1, "loot #1")));
}

// Вайп — через плагин: иначе удалённые руками файлы вернулись бы из базы на старте.
TEST_F(SessionTest, WipeStaysWipedAfterReboot) {
    save_world(1);
    save_player("p1");
    {
        Session s(options());
        s.boot();
        s.wipe(WipeScope::World);
        EXPECT_FALSE(std::filesystem::exists(root / "data/dynamic_000.bin"));
        EXPECT_EQ(player(), "p1");
        EXPECT_EQ(s.store().generations()[0].kind, "wipe");
    }
    Session s(options());
    s.boot();
    EXPECT_FALSE(std::filesystem::exists(root / "data/dynamic_000.bin")) << "вайп не откатился";
    EXPECT_EQ(s.status().chars, 1);
    s.wipe(WipeScope::Players);
    EXPECT_FALSE(std::filesystem::exists(root / "players.db"));
    EXPECT_EQ(s.status().chars, 0);
}

TEST_F(SessionTest, PlayersAreSnapshottedNoMoreOftenThanPeriod) {
    save_player("p1");
    Options o = options();
    o.players_period_ms = 3'600'000;
    Session s(o);
    s.boot();
    save_player("p2");
    s.tick();
    EXPECT_EQ(s.status().head_gen, 1) << "период не истёк";
    s.tick(true);
    EXPECT_EQ(s.status().head_gen, 2);
}

TEST_F(SessionTest, RetentionRunsThroughSession) {
    save_world(1);
    Options o = options();
    o.keep_hours = 0;
    Session s(o);
    s.boot();
    for (std::uint32_t i = 2; i <= 4; ++i) {
        save_world(i);
        s.tick();
    }
    s.retention();
    EXPECT_EQ(s.store().db().scalar("SELECT count(*) FROM files WHERE path = 'data/dynamic_000.bin'"), 1);
    EXPECT_EQ(world(), str(layer(4, "loot #4")));
    EXPECT_TRUE(s.store().verify().problems.empty());
}

TEST_F(SessionTest, StatusLineIsHumanReadable) {
    save_world(1);
    Session s(options());
    s.boot();
    const std::string line = to_string(s.status());
    EXPECT_NE(line.find("поколение 1"), std::string::npos) << line;
}
