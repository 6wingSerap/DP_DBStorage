#include <gtest/gtest.h>

#include <windows.h>

#include "store.hpp"
#include "support.hpp"
#include "sync.hpp"

using namespace dpdb;

namespace {

struct SyncTest : ::testing::Test {
    test::TempDir dir;
    fs::Path root = dir / "storage_1";
    Store store{dir / "s.db", [](std::string_view) {}};
    SkipCache skip;

    // Один проход: отчёт + номер поколения (0 — поколение не понадобилось).
    std::pair<SyncReport, std::int64_t> run(SyncOptions opt = {}) {
        auto w = store.begin("cycle");
        const SyncReport r = sync_files(store, w, root, opt, skip);
        return {r, w.commit()};
    }
    std::string head(const std::string& path) {
        const auto it = store.files().find(path);
        return it == store.files().end() ? "<нет>" : test::str(store.blob(it->second.blob));
    }
    static HANDLE lock(const fs::Path& p) {
        return CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
};

}  // namespace

TEST_F(SyncTest, CapturesWholeTreeIntoOneGeneration) {
    test::write(root / "data/dynamic_000.bin", make_layer("STORAGE_DYNAMIC", 39, 144, 5, test::bytes("loot")));
    test::write(root / "spawnpoints.bin", "sp");
    test::write(root / "communityframework/mod.bin", "cf");

    const auto [r, gen] = run();
    EXPECT_EQ(gen, 1);
    EXPECT_EQ(r.scanned, 3);
    EXPECT_EQ(r.stored, 3);
    EXPECT_EQ(head("spawnpoints.bin"), "sp");
    EXPECT_EQ(head("communityframework/mod.bin"), "cf");
    EXPECT_EQ(store.files().at("data/dynamic_000.bin").tag, "STORAGE_DYNAMIC");
    EXPECT_EQ(store.db().scalar("SELECT stamp FROM files WHERE path = 'data/dynamic_000.bin'"), 5);
}

TEST_F(SyncTest, MissingStorageDirIsNotAnError) {
    const auto [r, gen] = run();
    EXPECT_EQ(gen, 0);
    EXPECT_EQ(r.scanned, 0);
}

// Неизменённый файл узнаётся по размеру и mtime и даже не открывается: держим его
// монопольно, как держит движок, — проход всё равно чистый.
TEST_F(SyncTest, UnchangedFilesAreNotEvenOpened) {
    test::write(root / "a.bin", "v1");
    run();
    const HANDLE h = lock(root / "a.bin");
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    const auto [r, gen] = run();
    CloseHandle(h);
    EXPECT_EQ(gen, 0);
    EXPECT_EQ(r.deferred, 0);
    EXPECT_EQ(r.stored, 0);
}

TEST_F(SyncTest, OnlyChangedFileGetsNewVersion) {
    test::write(root / "a.bin", "a1");
    test::write(root / "b.bin", "b1");
    run();
    test::write(root / "a.bin", "a2-longer");
    const auto [r, gen] = run();
    EXPECT_EQ(gen, 2);
    EXPECT_EQ(r.stored, 1);
    EXPECT_EQ(head("a.bin"), "a2-longer");
    EXPECT_EQ(store.files().at("b.bin").gen, 1);
}

TEST_F(SyncTest, TouchedFileWithSameContentMakesNoGeneration) {
    test::write(root / "a.bin", "same");
    run();
    test::age(root / "a.bin", 100);
    const auto [r, gen] = run();
    EXPECT_EQ(gen, 0);
    EXPECT_EQ(r.same, 1);
    // новый mtime запомнен: в третий раз файл уже не читается
    const HANDLE h = lock(root / "a.bin");
    EXPECT_EQ(run().first.deferred, 0);
    CloseHandle(h);
}

TEST_F(SyncTest, RuntimeDeletionIsRecordedBootDeletionIsNot) {
    test::write(root / "a.bin", "a1");
    test::write(root / "b.bin", "b1");
    run();
    std::filesystem::remove(root / "a.bin");

    // на старте пропажа файла — потеря, а не удаление: голова не меняется
    const auto [boot, g0] = run({.deletions = false});
    EXPECT_EQ(g0, 0);
    EXPECT_EQ(boot.deleted, 0);
    EXPECT_EQ(head("a.bin"), "a1");

    const auto [live, g1] = run();
    EXPECT_EQ(g1, 2);
    EXPECT_EQ(live.deleted, 1);
    EXPECT_EQ(head("a.bin"), "<нет>");
    EXPECT_EQ(head("b.bin"), "b1");
}

TEST_F(SyncTest, PlayersDatabaseAndOwnTempFilesAreNotFiles) {
    test::write(root / "players.db", "sqlite");
    test::write(root / "players.db-journal", "j");
    test::write(root / "data/x.bin.dpdb-tmp", "tmp");
    test::write(root / "data/x.bin", "x");
    const auto [r, gen] = run();
    EXPECT_EQ(r.scanned, 1);
    EXPECT_EQ(store.files().size(), 1u);
}

TEST_F(SyncTest, PlayersEntriesInHeadAreNotTombstonedByFileSync) {
    {
        auto w = store.begin("cycle");
        w.put_file(kPlayersSchema, test::bytes("CREATE TABLE …"), 0, {});
        w.put_file(kPlayersFile, test::bytes("whole file"), 0, {});
        w.commit();
    }
    const auto [r, gen] = run();
    EXPECT_EQ(r.deleted, 0);
    EXPECT_EQ(store.files().size(), 2u);
}

// Оборванный слой в базу не попадает: в голове остаётся прошлая целая версия.
TEST_F(SyncTest, UnfinishedLayerIsNeverStored) {
    const auto good = make_layer("STORAGE_DYNAMIC", 39, 144, 5, test::bytes("good"));
    test::write(root / "data/dynamic_000.bin", good);
    run();
    auto torn = make_layer("STORAGE_DYNAMIC", 39, 144, 6, test::bytes("newer but torn"));
    torn.resize(torn.size() - 6);
    test::write(root / "data/dynamic_000.bin", torn);

    const auto [r, gen] = run();
    EXPECT_EQ(gen, 0);
    EXPECT_EQ(r.unfinished, 1);
    EXPECT_EQ(store.blob(store.files().at("data/dynamic_000.bin").blob), good);

    // второй проход помнит вердикт и файл не перечитывает
    const HANDLE h = lock(root / "data/dynamic_000.bin");
    const auto [again, g2] = run();
    CloseHandle(h);
    EXPECT_EQ(again.unfinished, 1);
    EXPECT_EQ(again.deferred, 0);
}

// Слой не может перестать быть слоем: пустой или обнулённый файл на его месте — след
// отключения питания, а не новое содержимое.
TEST_F(SyncTest, BlankFileInPlaceOfLayerIsNeverStored) {
    const auto good = make_layer("STORAGE_DYNAMIC", 39, 144, 5, test::bytes("good"));
    test::write(root / "data/dynamic_000.bin", good);
    test::write(root / "mod/state.lock", "");
    run();
    test::write(root / "data/dynamic_000.bin", fs::Bytes(good.size()));
    const auto [r, gen] = run();
    EXPECT_EQ(gen, 0);
    EXPECT_EQ(r.unfinished, 1);
    EXPECT_EQ(store.blob(store.files().at("data/dynamic_000.bin").blob), good);
    // а пустой файл, который слоем никогда не был, — обычный файл
    EXPECT_EQ(store.files().at("mod/state.lock").size, 0);
}

TEST_F(SyncTest, FileHeldByWriterIsDeferredThenCaptured) {
    test::write(root / "a.bin", "v1");
    const HANDLE h = lock(root / "a.bin");
    const auto [busy, g0] = run();
    EXPECT_EQ(busy.deferred, 1);
    EXPECT_TRUE(busy.pending());
    EXPECT_EQ(g0, 0);
    CloseHandle(h);
    const auto [ok, g1] = run();
    EXPECT_EQ(ok.stored, 1);
    EXPECT_EQ(g1, 1);
}

TEST_F(SyncTest, FreshFilesWaitUntilSettled) {
    test::write(root / "a.bin", "v1");
    const std::int64_t second = 1000 * fs::kTicksPerMs;
    const auto [young, g0] = run({.min_age = 3600 * second});
    EXPECT_EQ(young.deferred, 1);
    EXPECT_EQ(g0, 0);
    test::age(root / "a.bin", 7200);
    EXPECT_EQ(run({.min_age = 3600 * second}).first.stored, 1);
}

TEST_F(SyncTest, OversizedFileIsSkippedAndCounted) {
    test::write(root / "big.bin", std::string(100, 'x'));
    test::write(root / "ok.bin", "ok");
    const auto [r, gen] = run({.max_file = 10});
    EXPECT_EQ(r.oversized, 1);
    EXPECT_EQ(r.stored, 1);
    EXPECT_EQ(store.files().count("big.bin"), 0u);
}
