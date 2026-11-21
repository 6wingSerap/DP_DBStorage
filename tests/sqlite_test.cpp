#include <gtest/gtest.h>

#include <set>

#include "sqlite.hpp"
#include "support.hpp"

using namespace dpdb;

// Движок собран внутрь плагина из закреплённого исходника с опциями из CMakeLists, а не
// взят «какой оказался в системе». Обновили ссылку в CMakeLists — обновите версию здесь.
TEST(Sqlite, EngineIsTheBundledOptimizedBuild) {
    EXPECT_EQ(sql::version(), "3.54.1");
    test::TempDir dir;
    sql::Db db(dir / "t.db", sql::kReadWrite | sql::kCreate);
    std::set<std::string> opts;
    auto q = db.prepare("PRAGMA compile_options");
    while (q.step())
        opts.insert(q.text(0));
    for (const char* o : {"THREADSAFE=2", "DEFAULT_MEMSTATUS=0", "OMIT_LOAD_EXTENSION", "OMIT_SHARED_CACHE"})
        EXPECT_TRUE(opts.contains(o)) << o;
}

TEST(Sqlite, RoundTripsEveryValueKind) {
    test::TempDir dir;
    sql::Db db(dir / "t.db", sql::kReadWrite | sql::kCreate);
    db.exec("CREATE TABLE t(i INTEGER, s TEXT, b BLOB, n BLOB); CREATE TABLE u(x)");

    const auto blob = test::bytes(std::string("a\0b", 3));
    auto ins = db.prepare("INSERT INTO t VALUES(?,?,?,?)");
    ins.bind(1, std::int64_t{-7}).bind(2, "привет").bind(3, blob).bind(4, nullptr);
    EXPECT_FALSE(ins.step());
    EXPECT_EQ(db.changes(), 1);

    auto sel = db.prepare("SELECT i, s, b, n FROM t");
    ASSERT_TRUE(sel.step());
    EXPECT_EQ(sel.i64(0), -7);
    EXPECT_EQ(sel.text(1), "привет");
    EXPECT_EQ(test::str(sel.blob(2)), std::string("a\0b", 3));
    EXPECT_FALSE(sel.null(2));
    EXPECT_TRUE(sel.null(3));
    EXPECT_FALSE(sel.step());
}

TEST(Sqlite, EmptyBlobIsNotNull) {
    test::TempDir dir;
    sql::Db db(dir / "t.db", sql::kReadWrite | sql::kCreate);
    db.exec("CREATE TABLE t(b BLOB NOT NULL)");
    db.prepare("INSERT INTO t VALUES(?)").bind(1, std::span<const std::byte>{}).step();
    auto sel = db.prepare("SELECT b FROM t");
    ASSERT_TRUE(sel.step());
    EXPECT_FALSE(sel.null(0));
    EXPECT_EQ(sel.blob(0).size(), 0u);
}

TEST(Sqlite, StatementIsReusableAfterReset) {
    test::TempDir dir;
    sql::Db db(dir / "t.db", sql::kReadWrite | sql::kCreate);
    db.exec("CREATE TABLE t(i INTEGER)");
    auto ins = db.prepare("INSERT INTO t VALUES(?)");
    for (std::int64_t i = 0; i < 3; ++i) {
        ins.bind(1, i).step();
        ins.reset();
    }
    EXPECT_EQ(db.scalar("SELECT sum(i) FROM t"), 3);
}

TEST(Sqlite, ErrorsCarryCodeAndText) {
    test::TempDir dir;
    sql::Db db(dir / "t.db", sql::kReadWrite | sql::kCreate);
    try {
        db.exec("SELECT * FROM nope");
        FAIL() << "должно бросить";
    } catch (const sql::Error& e) {
        EXPECT_EQ(e.code, 1);
        EXPECT_NE(std::string(e.what()).find("nope"), std::string::npos);
    }
}

TEST(Sqlite, TransactionRollsBackUnlessCommitted) {
    test::TempDir dir;
    sql::Db db(dir / "t.db", sql::kReadWrite | sql::kCreate);
    db.exec("CREATE TABLE t(i INTEGER)");
    {
        sql::Tx tx(db);
        db.exec("INSERT INTO t VALUES(1)");
    }
    EXPECT_EQ(db.scalar("SELECT count(*) FROM t"), 0);
    {
        sql::Tx tx(db);
        db.exec("INSERT INTO t VALUES(1)");
        tx.commit();
    }
    EXPECT_EQ(db.scalar("SELECT count(*) FROM t"), 1);
}

TEST(Sqlite, BackupCopiesWholeDatabase) {
    test::TempDir dir;
    {
        sql::Db db(dir / "src.db", sql::kReadWrite | sql::kCreate);
        db.exec("CREATE TABLE t(i INTEGER); INSERT INTO t VALUES(42)");
        db.backup_to(dir / "copy.db");
    }
    sql::Db copy(dir / "copy.db", sql::kReadOnly);
    EXPECT_EQ(copy.scalar("SELECT i FROM t"), 42);
}
