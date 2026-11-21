#include <gtest/gtest.h>

#include "store.hpp"
#include "support.hpp"

using namespace dpdb;

namespace {

constexpr std::int64_t kHour = 3'600'000;

struct StoreTest : ::testing::Test {
    test::TempDir dir;
    fs::Path db = dir / "s.db";
    std::vector<std::string> log;

    Store open() {
        return Store(db, [this](std::string_view s) { log.emplace_back(s); });
    }
    static std::int64_t put(Store& s, std::string_view path, std::string_view text, std::int64_t now = 1000) {
        auto w = s.begin("cycle", "1.30", "", now);
        w.put_file(path, test::bytes(text), 1, {});
        return w.commit();
    }
    static std::string content(Store& s, std::int64_t gen, const std::string& path) {
        const State st = s.state(gen);
        const auto it = st.files.find(path);
        return it == st.files.end() ? "<нет>" : test::str(s.blob(it->second));
    }
};

}  // namespace

TEST_F(StoreTest, FreshDatabaseIsEmpty) {
    Store s = open();
    EXPECT_EQ(s.head_gen(), 0);
    EXPECT_TRUE(s.files().empty());
    EXPECT_TRUE(s.chars().empty());
    EXPECT_FALSE(s.was_quarantined());
}

TEST_F(StoreTest, FileSurvivesReopen) {
    {
        Store s = open();
        auto w = s.begin("boot", "1.30.1", "первое", 1000);
        Facts f = inspect(make_layer("STORAGE_DYNAMIC", 39, 144, 7, {}));
        EXPECT_TRUE(w.put_file("data/dynamic_000.bin", test::bytes("world"), 111, f));
        EXPECT_EQ(w.commit(), 1);
        EXPECT_EQ(s.head_gen(), 1);
    }
    Store s = open();
    ASSERT_EQ(s.files().count("data/dynamic_000.bin"), 1u);
    const HeadFile& h = s.files().at("data/dynamic_000.bin");
    EXPECT_EQ(h.size, 5);
    EXPECT_EQ(h.mtime, 111);
    EXPECT_EQ(h.gen, 1);
    EXPECT_EQ(h.fmt, 39);
    EXPECT_EQ(h.tag, "STORAGE_DYNAMIC");
    EXPECT_EQ(h.hash, fs::sha256(test::bytes("world")));
    EXPECT_EQ(test::str(s.blob(h.blob)), "world");
    const auto gens = s.generations();
    ASSERT_EQ(gens.size(), 1u);
    EXPECT_EQ(gens[0].kind, "boot");
    EXPECT_EQ(gens[0].build, "1.30.1");
    EXPECT_EQ(gens[0].note, "первое");
    EXPECT_EQ(gens[0].files, 1);
}

TEST_F(StoreTest, UnchangedContentMakesNoGenerationButRemembersMtime) {
    Store s = open();
    put(s, "a.bin", "same");
    {
        auto w = s.begin("cycle", "", "", 2000);
        EXPECT_FALSE(w.put_file("a.bin", test::bytes("same"), 999, {}));
        EXPECT_EQ(w.commit(), 0);
    }
    EXPECT_EQ(s.head_gen(), 1);
    EXPECT_EQ(s.files().at("a.bin").mtime, 999);
    Store again = open();
    EXPECT_EQ(again.files().at("a.bin").mtime, 999);
    EXPECT_EQ(again.generations().size(), 1u);
}

TEST_F(StoreTest, HistoryIsReadableByGeneration) {
    Store s = open();
    EXPECT_EQ(put(s, "a.bin", "v1"), 1);
    EXPECT_EQ(put(s, "a.bin", "v2"), 2);
    {
        auto w = s.begin("cycle", "", "", 3000);
        w.del_file("a.bin");
        EXPECT_EQ(w.commit(), 3);
    }
    EXPECT_EQ(content(s, 1, "a.bin"), "v1");
    EXPECT_EQ(content(s, 2, "a.bin"), "v2");
    EXPECT_EQ(content(s, 3, "a.bin"), "<нет>");
    EXPECT_EQ(s.files().count("a.bin"), 0u);
}

TEST_F(StoreTest, IdenticalContentIsStoredOnce) {
    Store s = open();
    auto w = s.begin("cycle", "", "", 1000);
    w.put_file("building_001.bin", test::bytes("empty"), 1, {});
    w.put_file("building_002.bin", test::bytes("empty"), 1, {});
    w.commit();
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM blobs"), 1);
    EXPECT_EQ(s.files().at("building_001.bin").blob, s.files().at("building_002.bin").blob);
}

TEST_F(StoreTest, AbandonedWriterChangesNothing) {
    Store s = open();
    put(s, "a.bin", "v1");
    {
        auto w = s.begin("cycle", "", "", 2000);
        w.put_file("a.bin", test::bytes("v2"), 2, {});
        w.put_file("b.bin", test::bytes("new"), 2, {});
    }
    EXPECT_EQ(s.head_gen(), 1);
    EXPECT_EQ(s.files().count("b.bin"), 0u);
    EXPECT_EQ(content(s, 1, "a.bin"), "v1");
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM generations"), 1);
}

TEST_F(StoreTest, CharactersKeepNullAndEmptyDataApart) {
    Store s = open();
    const fs::Bytes data = test::bytes("blob"), empty;
    {
        auto w = s.begin("cycle", "", "", 1000);
        EXPECT_TRUE(w.put_char("uid-a", 1, 1, &data));
        EXPECT_TRUE(w.put_char("uid-null", 2, 1, nullptr));
        EXPECT_TRUE(w.put_char("uid-empty", 3, 0, &empty));
        EXPECT_EQ(w.commit(), 1);
    }
    Store again = open();
    ASSERT_EQ(again.chars().size(), 3u);
    EXPECT_EQ(again.chars().at("uid-null").blob, 0);
    EXPECT_NE(again.chars().at("uid-empty").blob, 0);
    EXPECT_EQ(again.chars().at("uid-a").id, 1);
    {
        auto w = again.begin("cycle", "", "", 2000);
        EXPECT_FALSE(w.put_char("uid-a", 1, 1, &data));        // то же самое
        EXPECT_FALSE(w.put_char("uid-null", 2, 1, nullptr));
        EXPECT_TRUE(w.put_char("uid-a", 1, 0, &data));         // умер
        w.del_char("uid-empty");
        EXPECT_EQ(w.commit(), 2);
    }
    EXPECT_EQ(again.chars().at("uid-a").alive, 0);
    EXPECT_EQ(again.chars().count("uid-empty"), 0u);
    EXPECT_EQ(again.state(1).chars.size(), 3u);
    EXPECT_EQ(again.state(2).chars.size(), 2u);
}

// Первая запись ключа в поколении идёт быстрым INSERT, повторная — заменой строки.
// Оба пути обязаны оставить в базе последнее значение и одно поколение.
TEST_F(StoreTest, SameKeyTwiceInOneGenerationKeepsTheLastWrite) {
    const fs::Bytes one = test::bytes("one"), two = test::bytes("two");
    {
        Store s = open();
        put(s, "data/gone.bin", "was here");
        auto w = s.begin("cycle", "", "", 2000);
        w.put_file("data/a.bin", test::bytes("first"), 1, {});
        w.put_file("data/a.bin", test::bytes("second"), 2, {});   // запись → запись
        w.put_file("data/b.bin", test::bytes("b"), 1, {});
        w.del_file("data/b.bin");                                   // запись → удаление
        w.del_file("data/gone.bin");
        w.put_file("data/gone.bin", test::bytes("back"), 3, {});   // удаление → запись
        w.put_char("uid-a", 1, 1, &one);
        w.put_char("uid-a", 1, 0, &two);
        w.put_char("uid-b", 2, 1, &one);
        w.del_char("uid-b");
        EXPECT_EQ(w.commit(), 2);
    }
    Store s = open();
    EXPECT_EQ(s.head_gen(), 2);
    EXPECT_EQ(content(s, 2, "data/a.bin"), "second");
    EXPECT_EQ(content(s, 2, "data/b.bin"), "<нет>");
    EXPECT_EQ(content(s, 2, "data/gone.bin"), "back");
    EXPECT_EQ(content(s, 1, "data/gone.bin"), "was here");
    ASSERT_EQ(s.chars().count("uid-a"), 1u);
    EXPECT_EQ(s.chars().at("uid-a").alive, 0);
    EXPECT_EQ(test::str(s.blob(s.chars().at("uid-a").blob)), "two");
    EXPECT_EQ(s.chars().count("uid-b"), 0u);
    EXPECT_TRUE(s.verify().problems.empty());
}

TEST_F(StoreTest, CorruptedBlobIsDetectedNotReturned) {
    Store s = open();
    put(s, "a.bin", "good");
    s.db().exec("UPDATE blobs SET data = x'626164'");
    EXPECT_THROW(s.blob(s.files().at("a.bin").blob), sql::Error);
    const auto v = s.verify();
    EXPECT_EQ(v.blobs, 1);
    EXPECT_EQ(v.problems.size(), 1u);
}

TEST_F(StoreTest, HealthyDatabaseVerifiesClean) {
    Store s = open();
    put(s, "a.bin", "good");
    EXPECT_TRUE(s.verify().problems.empty());
}

TEST_F(StoreTest, ForeignSqliteFileIsRefusedAndUntouched) {
    {
        sql::Db other(db, sql::kReadWrite | sql::kCreate);
        other.exec("CREATE TABLE someone_elses(x); INSERT INTO someone_elses VALUES(1)");
    }
    EXPECT_THROW(open(), sql::Error);
    sql::Db other(db, sql::kReadOnly);
    EXPECT_EQ(other.scalar("SELECT x FROM someone_elses"), 1);
}

TEST_F(StoreTest, GarbageFileIsQuarantinedAndFreshBaseCreated) {
    test::write(db, std::string(8192, 'x'));
    Store s = open();
    EXPECT_TRUE(s.was_quarantined());
    EXPECT_EQ(s.head_gen(), 0);
    EXPECT_EQ(put(s, "a.bin", "v1"), 1);
    bool kept = false;
    for (const auto& e : fs::scan(dir.path))
        kept = kept || e.rel.find("s.db.corrupt-") == 0;
    EXPECT_TRUE(kept) << "испорченный файл должен остаться для разбора";
}

TEST_F(StoreTest, NewerSchemaIsRefused) {
    { Store s = open(); }
    {
        sql::Db raw(db, sql::kReadWrite);
        raw.exec("PRAGMA user_version = 999");
    }
    EXPECT_THROW(open(), sql::Error);
}

TEST_F(StoreTest, MetaRoundTrip) {
    Store s = open();
    EXPECT_EQ(s.meta("build"), "");
    s.set_meta("build", "1.30");
    s.set_meta("build", "1.31");
    EXPECT_EQ(s.meta("build"), "1.31");
}

// ── удержание ────────────────────────────────────────────────────────────────

TEST_F(StoreTest, RetentionSquashesOldHistoryButKeepsWindowExact) {
    Store s = open();
    put(s, "a.bin", "a1", 1 * kHour);
    put(s, "a.bin", "a2", 2 * kHour);
    put(s, "b.bin", "b3", 3 * kHour);
    put(s, "a.bin", "a4", 4 * kHour);
    put(s, "a.bin", "a5", 5 * kHour);

    // сейчас = 5ч + 1мс, держим 2 часа: граница — поколение 3 (последнее старше окна)
    const auto r = s.retain(5 * kHour + 1, 2, INT64_MAX);
    EXPECT_EQ(r.cutoff, 3);
    EXPECT_EQ(r.versions, 1);   // a1 заменена a2 до границы
    EXPECT_EQ(r.blobs, 1);

    EXPECT_EQ(content(s, 3, "a.bin"), "a2");
    EXPECT_EQ(content(s, 3, "b.bin"), "b3");
    EXPECT_EQ(content(s, 4, "a.bin"), "a4");
    EXPECT_EQ(content(s, 5, "a.bin"), "a5");
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM generations WHERE gen = 1"), 0);
    EXPECT_TRUE(s.verify().problems.empty());
}

TEST_F(StoreTest, RetentionNeverTouchesPinnedGeneration) {
    Store s = open();
    put(s, "a.bin", "a1", 1 * kHour);
    put(s, "a.bin", "a2", 2 * kHour);
    put(s, "a.bin", "a3", 3 * kHour);
    s.pin(1, true, "перед обновлением игры");
    s.retain(100 * kHour, 1, 0);   // и по времени, и по размеру — схлопнуть всё
    EXPECT_EQ(content(s, 1, "a.bin"), "a1");
    EXPECT_EQ(content(s, 3, "a.bin"), "a3");
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM files"), 2);   // a2 ушла
    const auto gens = s.generations();
    EXPECT_TRUE(gens.back().pinned);
    EXPECT_EQ(gens.back().note, "перед обновлением игры");
}

TEST_F(StoreTest, OnlyPinnedOrUnsquashedGenerationsAreRestorable) {
    Store s = open();
    for (int i = 1; i <= 5; ++i)
        put(s, "a.bin", "a" + std::to_string(i), i * kHour);
    put(s, "b.bin", "b6", 6 * kHour);
    s.pin(2, true, "");
    EXPECT_TRUE(s.restorable(1));
    s.retain(6 * kHour, 2, INT64_MAX);   // граница — поколение 4
    EXPECT_FALSE(s.restorable(1)) << "поколения больше нет";
    EXPECT_TRUE(s.restorable(2)) << "закреплено";
    EXPECT_FALSE(s.restorable(3));
    EXPECT_TRUE(s.restorable(4));
    EXPECT_TRUE(s.restorable(6));
    EXPECT_FALSE(s.restorable(7));
    EXPECT_EQ(content(s, 2, "a.bin"), "a2");
}

TEST_F(StoreTest, RetentionDropsTombstonesWithNothingBehindThem) {
    Store s = open();
    put(s, "a.bin", "a1", 1 * kHour);
    {
        auto w = s.begin("cycle", "", "", 2 * kHour);
        w.del_file("a.bin");
        w.commit();
    }
    put(s, "b.bin", "b3", 3 * kHour);
    s.retain(100 * kHour, 1, INT64_MAX);
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM files WHERE path = 'a.bin'"), 0);
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM blobs"), 1);
    EXPECT_EQ(content(s, 3, "b.bin"), "b3");
}

TEST_F(StoreTest, SizeLimitSquashesEvenFreshHistoryDownToHead) {
    Store s = open();
    for (int i = 1; i <= 6; ++i)
        put(s, "a.bin", std::string(200'000, static_cast<char>('a' + i)), i * kHour);
    const std::int64_t before = s.used_bytes();
    s.retain(6 * kHour + 1, 1000, 400'000);   // окно огромное, но база больше лимита
    EXPECT_LT(s.used_bytes(), before);
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM blobs"), 1);
    EXPECT_EQ(content(s, 6, "a.bin"), std::string(200'000, 'g'));
    EXPECT_EQ(s.files().at("a.bin").size, 200'000);
}

TEST_F(StoreTest, RetentionKeepsCharactersConsistent) {
    Store s = open();
    const fs::Bytes d1 = test::bytes("d1"), d2 = test::bytes("d2");
    for (int i = 1; i <= 3; ++i) {
        auto w = s.begin("cycle", "", "", i * kHour);
        w.put_char("uid", 1, 1, i == 1 ? &d1 : &d2);
        if (i == 3)
            w.put_char("uid", 1, 0, &d2);
        w.commit();
    }
    s.retain(100 * kHour, 1, INT64_MAX);
    ASSERT_EQ(s.state(s.head_gen()).chars.size(), 1u);
    EXPECT_EQ(s.state(s.head_gen()).chars[0].alive, 0);
    EXPECT_EQ(s.db().scalar("SELECT count(*) FROM blobs"), 1);
}
