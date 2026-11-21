// Сессия: одна база + один каталог storage. Всё, что делает плагин и утилита, кроме
// перехвата и потока, — здесь, поэтому целиком проверяется тестами без игры.
//
//   boot()  — до первого чтения движком: забрать с диска то, что новее базы, закрыть дыры
//             и оборванные слои из базы, отметить обновление игры;
//   tick()  — фон: записать изменившееся с прошлого раза;
//   restore_generation()/wipe() — офлайн, при остановленном сервере.
//
// Ошибка базы никогда не мешает игре: движок работает с настоящим storage, сессия лишь
// перестаёт пополнять историю и пишет в журнал. Испорченная база заменяется новой.
#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

#include "players.hpp"
#include "restore.hpp"
#include "store.hpp"
#include "sync.hpp"

namespace dpdb {

struct Options {
    fs::Path storage_dir;
    fs::Path db_path;
    std::string build;            // версия exe игры; пусто — неизвестна
    int keep_hours = 24;
    std::int64_t max_db_bytes = 8ll << 30;
    std::int64_t settle_ms = 1000;            // файл моложе — ещё может дописываться
    std::int64_t players_period_ms = 30'000;
    // Хозяин базы (сервер, офлайн-операции утилиты): держит межпроцессный замок и вправе
    // заменить испорченную базу новой. Не хозяин только читает рядом с хозяином.
    bool exclusive = true;
    Log log = [](std::string_view) {};
};

struct Status {
    std::int64_t head_gen = 0, files = 0, chars = 0, db_bytes = 0;
    std::uint64_t ticks = 0, generations = 0, errors = 0;
    SyncReport sync;             // последний проход
    PlayersMode players = PlayersMode::None;
    RestoreReport boot_repair;
    std::int64_t last_ms = 0;    // длительность последнего прохода
    std::int64_t worst_ms = 0;
    bool db_replaced = false;    // базу пришлось отложить как испорченную
    std::string last_error;
};
std::string to_string(const Status& s);

class Session {
public:
    explicit Session(Options opt);   // занята другим процессом — std::runtime_error
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void boot();
    // Один проход. force — игроков снять сейчас, не дожидаясь периода. true — есть
    // отложенные файлы, стоит повторить скоро.
    bool tick(bool force = false);
    void retention();

    // Офлайн. Текущее состояние диска сначала сохраняется поколением — откат обратим.
    void restore_generation(std::int64_t gen);
    void wipe(WipeScope scope);

    Status status() const;   // из любого потока
    Store& store() { return *store_; }
    const Options& options() const { return opt_; }

    // База сейчас открыта хозяином (работающим сервером) в каком-то процессе.
    static bool in_use(const fs::Path& db_path);

private:
    // players: 0 — не трогать, 1 — снять, 2 — забыть (файла больше нет)
    bool pass(std::string_view kind, std::string_view note, bool deletions, std::int64_t settle_ms, int players);
    void open_store();
    void replace_broken_base(std::string_view why);
    fs::Path players_db() const { return opt_.storage_dir / "players.db"; }

    Options opt_;
    void* lock_ = nullptr;
    std::optional<Store> store_;
    SkipCache skip_;
    std::int64_t players_size_ = -1, players_mtime_ = -1, players_at_ = 0;
    mutable std::mutex mx_;
    Status status_;
};

}  // namespace dpdb
