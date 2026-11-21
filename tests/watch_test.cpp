#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <thread>

#include "support.hpp"
#include "watch.hpp"

using namespace dpdb;

namespace {

struct WatchTest : ::testing::Test {
    test::TempDir dir;
    fs::Path root = dir / "mpmissions/dayzOffline.chernarusplus/storage_1";
    void SetUp() override {
        std::filesystem::create_directories(root / "data");
        watch::configure(root, dir.path);
    }
    std::wstring abs(std::wstring_view rel) const { return (root / rel).wstring(); }
    static HANDLE open(const std::wstring& path, DWORD access, DWORD disposition = OPEN_ALWAYS) {
        return CreateFileW(path.c_str(), access, 0, nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
};

}  // namespace

// Формы путей — из разбора движка (docs/research/dayz-persistence.md, «Что это значит»).
TEST_F(WatchTest, RecognizesEveryPathFormTheEngineUses) {
    using watch::Kind;
    // FileServerST: \\?\ + абсолютный путь
    EXPECT_EQ(watch::classify((L"\\\\?\\" + abs(L"data\\dynamic_000.bin")).c_str()), Kind::Storage);
    // SQLite win32 VFS: абсолютный путь без префикса
    EXPECT_EQ(watch::classify(abs(L"players.db-journal").c_str()), Kind::Storage);
    // регистр и разделители не важны
    std::wstring odd = abs(L"data/Dynamic_000.BIN");
    for (wchar_t& c : odd)
        c = c == L'\\' ? L'/' : static_cast<wchar_t>(towupper(c));
    EXPECT_EQ(watch::classify(odd.c_str()), Kind::Storage);
    // enf FS: относительный путь от рабочего каталога — как есть
    EXPECT_EQ(watch::classify(L"mpmissions\\dayzOffline.chernarusplus\\storage_1\\data\\x.bin"), Kind::Storage);
}

TEST_F(WatchTest, NeighboursAreNotOurStorage) {
    using watch::Kind;
    EXPECT_EQ(watch::classify((dir / "profiles/server.log").c_str()), Kind::Other);
    EXPECT_EQ(watch::classify(L"C:\\Windows\\Temp\\x.tmp"), Kind::Other);
    EXPECT_EQ(watch::classify(L""), Kind::Other);
    // storage_10 начинается с тех же букв, но это другой каталог — и это чужое хранилище
    EXPECT_EQ(watch::classify((root.wstring() + L"0\\data\\x.bin").c_str()), Kind::Foreign);
    EXPECT_EQ(watch::classify(L"\\\\?\\D:\\srv\\mpmissions\\other\\storage_-1\\data\\types.bin"), Kind::Foreign);
    // сам каталог и просто похожее имя файла — не хранилище
    EXPECT_EQ(watch::classify(L"D:\\notes\\storage_plan.txt"), Kind::Other);
}

TEST_F(WatchTest, HookSeesWritesUnderStorageOnly) {
    ASSERT_GT(watch::install(GetModuleHandleW(nullptr)), 0) << "в образе теста нет импорта CreateFileW";
    EXPECT_GT(watch::install(GetModuleHandleW(nullptr)), 0) << "повторная установка безвредна";

    const std::uint64_t e0 = watch::epoch();
    HANDLE h = open(abs(L"data\\dynamic_000.bin"), GENERIC_WRITE, CREATE_ALWAYS);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    CloseHandle(h);
    EXPECT_EQ(watch::epoch(), e0 + 1);

    h = open(abs(L"data\\dynamic_000.bin"), GENERIC_READ, OPEN_EXISTING);   // чтение — не событие
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    CloseHandle(h);
    h = open((dir / "elsewhere.bin").wstring(), GENERIC_WRITE, CREATE_ALWAYS);   // запись мимо хранилища
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    CloseHandle(h);
    EXPECT_EQ(watch::epoch(), e0 + 1);
}

// Если движок пишет в storage_*, который мы не охраняем, — пути определены неверно.
// Плагин обязан это заметить сам, а не молча охранять пустоту.
TEST_F(WatchTest, HookReportsWritesIntoStorageWeDoNotGuard) {
    ASSERT_GT(watch::install(GetModuleHandleW(nullptr)), 0);
    const fs::Path other = dir / "mpmissions/other.map/storage_7/data";
    std::filesystem::create_directories(other);
    const std::uint64_t f0 = watch::foreign();
    const HANDLE h = open((other / "types.bin").wstring(), GENERIC_WRITE, CREATE_ALWAYS);
    ASSERT_NE(h, INVALID_HANDLE_VALUE);
    CloseHandle(h);
    EXPECT_EQ(watch::foreign(), f0 + 1);
    EXPECT_NE(watch::foreign_sample().find(L"storage_7"), std::wstring::npos);
}

TEST_F(WatchTest, FailedOpenIsNotAnEventAndKeepsItsErrorCode) {
    ASSERT_GT(watch::install(GetModuleHandleW(nullptr)), 0);
    const std::uint64_t e0 = watch::epoch();
    const HANDLE h = open(abs(L"no_such_dir\\x.bin"), GENERIC_WRITE, OPEN_EXISTING);
    const DWORD err = GetLastError();
    EXPECT_EQ(h, INVALID_HANDLE_VALUE);
    EXPECT_EQ(err, static_cast<DWORD>(ERROR_PATH_NOT_FOUND));
    EXPECT_EQ(watch::epoch(), e0);
}

// Движок открывает слой монопольно. Наш фоновый читатель не имеет права стать причиной
// отказа: перехват пережидает его и повторяет открытие.
TEST_F(WatchTest, EngineWriteNeverFailsBecauseOfOurReader) {
    ASSERT_GT(watch::install(GetModuleHandleW(nullptr)), 0);
    const fs::Path file = root / "data/dynamic_000.bin";
    test::write(file, std::string(4 << 20, 'x'));

    std::atomic<bool> stop{false};
    std::thread reader([&] {
        fs::Bytes buf;
        while (!stop.load())
            fs::read_shared(file, buf);
    });
    int failures = 0;
    for (int i = 0; i < 300; ++i) {
        const HANDLE h = open(file.wstring(), GENERIC_WRITE, OPEN_EXISTING);
        if (h == INVALID_HANDLE_VALUE)
            ++failures;
        else
            CloseHandle(h);
    }
    stop = true;
    reader.join();
    EXPECT_EQ(failures, 0);
    EXPECT_GT(watch::retries(), 0u) << "гонка ни разу не случилась — тест ничего не проверил";
}
