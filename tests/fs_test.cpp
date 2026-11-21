#include <gtest/gtest.h>

#include <windows.h>

#include "fs.hpp"
#include "support.hpp"

using namespace dpdb;

TEST(Fs, Sha256MatchesKnownVectors) {
    EXPECT_EQ(fs::hex(fs::sha256({})), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(fs::hex(fs::sha256(test::bytes("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Fs, Utf8RoundTrip) {
    EXPECT_EQ(fs::to_utf8(fs::to_wide("Миссия/storage_1")), "Миссия/storage_1");
    EXPECT_EQ(fs::to_utf8(L""), "");
}

TEST(Fs, ScanListsFilesRecursivelyWithNormalizedNames) {
    test::TempDir dir;
    test::write(dir / "data/Dynamic_000.BIN", "12345");
    test::write(dir / "spawnpoints.bin", "x");
    test::write(dir / "mod/Deep/file.dat", "");

    const auto list = fs::scan(dir.path);
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].rel, "data/dynamic_000.bin");   // сортировка, нижний регистр, '/'
    EXPECT_EQ(list[0].size, 5);
    EXPECT_GT(list[0].mtime, 0);
    EXPECT_EQ(list[1].rel, "mod/deep/file.dat");
    EXPECT_EQ(list[2].rel, "spawnpoints.bin");
}

TEST(Fs, ScanOfMissingDirIsEmpty) {
    test::TempDir dir;
    EXPECT_TRUE(fs::scan(dir / "nope").empty());
}

TEST(Fs, ReadSharedReadsWholeFile) {
    test::TempDir dir;
    test::write(dir / "a.bin", "hello");
    fs::Bytes out;
    EXPECT_EQ(fs::read_shared(dir / "a.bin", out), fs::Read::Ok);
    EXPECT_EQ(test::str(out), "hello");
    EXPECT_EQ(fs::read_shared(dir / "none.bin", out), fs::Read::Missing);
}

// Движок пишет слой с share=0: пока он не закрыл файл, читать нельзя — это «занят», не ошибка.
TEST(Fs, ReadSharedReportsBusyWhileWriterHoldsFile) {
    test::TempDir dir;
    const auto p = dir / "a.bin";
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    fs::Bytes out;
    EXPECT_EQ(fs::read_shared(p, out), fs::Read::Busy);
    CloseHandle(h);
    EXPECT_EQ(fs::read_shared(p, out), fs::Read::Ok);
}

TEST(Fs, ReadSharedRefusesOversizedFile) {
    test::TempDir dir;
    test::write(dir / "a.bin", "123456");
    fs::Bytes out;
    EXPECT_EQ(fs::read_shared(dir / "a.bin", out, 5), fs::Read::TooBig);
}

TEST(Fs, WriteAtomicCreatesDirsReplacesAndLeavesNoTemp) {
    test::TempDir dir;
    const auto p = dir / "data/sub/a.bin";
    fs::write_atomic(p, test::bytes("one"));
    fs::write_atomic(p, test::bytes("two!"));
    EXPECT_EQ(test::read(p), "two!");
    EXPECT_EQ(fs::scan(dir.path).size(), 1u);
}

TEST(Fs, WriteAtomicWithoutFlushWritesTheSameBytes) {
    test::TempDir dir;
    fs::write_atomic(dir / "a.bin", test::bytes("fast path"), /*durable=*/false);
    EXPECT_EQ(test::read(dir / "a.bin"), "fast path");
}

TEST(Fs, ReaderCounterIsZeroWhenIdle) {
    EXPECT_EQ(fs::active_readers(), 0);
}
