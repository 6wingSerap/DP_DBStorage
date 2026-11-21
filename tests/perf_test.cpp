// Страховка от деградации скорости. Пороги нарочно с запасом в десятки раз: тест не
// измеряет (для этого `dpdb bench`), а ловит поломку — например, потерю настроек базы.
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <random>

#include "session.hpp"
#include "support.hpp"
#include "watch.hpp"

using namespace dpdb;

namespace {

constexpr int kFiles = 12;
constexpr std::size_t kFileBytes = 4 << 20;   // 12 × 4 МБ = 48 МБ на цикл сохранения

template <class F>
std::int64_t ms(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

void save_cycle(const fs::Path& root, std::uint32_t stamp) {
    std::mt19937_64 rng(stamp);
    fs::Bytes payload(kFileBytes);
    for (int i = 0; i < kFiles; ++i) {
        for (std::size_t j = 0; j + 8 <= payload.size(); j += 8) {
            const std::uint64_t v = rng();
            std::memcpy(payload.data() + j, &v, 8);
        }
        test::write(root / std::format("data/dynamic_{:03}.bin", i), make_layer("STORAGE_DYNAMIC", 39, 144, stamp, payload));
    }
}

}  // namespace

TEST(Perf, SaveCycleCaptureIdlePassAndRestoreStayFast) {
    test::TempDir dir;
    const fs::Path root = dir / "storage_1";
    Options o;
    o.storage_dir = root;
    o.db_path = dir / "s.db";
    o.settle_ms = 0;
    save_cycle(root, 1);

    std::optional<Session> s(std::in_place, o);
    const std::int64_t capture = ms([&] { s->boot(); });
    const std::int64_t idle = ms([&] { s->tick(); });
    save_cycle(root, 2);
    const std::int64_t cycle = ms([&] { s->tick(); });
    EXPECT_EQ(s->status().head_gen, 2);
    s.reset();
    std::filesystem::remove_all(root);
    s.emplace(o);
    const std::int64_t restore = ms([&] { s->boot(); });
    EXPECT_EQ(s->status().boot_repair.restored, kFiles);

    std::cout << "[perf] 48 МБ: захват " << capture << " мс, холостой проход " << idle << " мс, цикл " << cycle
              << " мс, восстановление " << restore << " мс\n";
    EXPECT_LT(capture, 10'000);
    EXPECT_LT(idle, 500);
    EXPECT_LT(cycle, 10'000);
    EXPECT_LT(restore, 10'000);
}

// Перехват стоит на пути каждого открытия файла движком: проверка пути обязана быть дешёвой.
TEST(Perf, PathCheckInHookIsCheap) {
    watch::configure(L"D:\\DayZServer\\mpmissions\\dayzOffline.chernarusplus\\storage_1", L"D:\\DayZServer");
    const wchar_t* paths[] = {
        L"\\\\?\\D:\\DayZServer\\mpmissions\\dayzOffline.chernarusplus\\storage_1\\data\\dynamic_004.001",
        L"\\\\?\\D:\\DayZServer\\profiles\\DayZServer_x64_2026-11-17_18-00-00.RPT",
    };
    int hits = 0;
    const std::int64_t took = ms([&] {
        for (int i = 0; i < 1'000'000; ++i)
            hits += watch::classify(paths[i & 1]) == watch::Kind::Storage;
    });
    EXPECT_EQ(hits, 500'000);
    std::cout << "[perf] проверка пути в перехвате: " << took << " нс на вызов (среднее по 1 млн)\n";
    EXPECT_LT(took, 1000) << "дороже 1 мкс на вызов";
}
