// Диск → база: один проход по каталогу storage. Читаются только изменившиеся файлы
// (размер или mtime не совпали с головой); оборванные слои в базу не попадают.
#pragma once
#include <cstdint>
#include <map>
#include <string>

#include "store.hpp"

namespace dpdb {

struct SyncOptions {
    // Файл из головы пропал с диска: при работе сервера это удаление (движок или мод),
    // на старте — потеря, которую восстановит repair(). Поэтому на старте false.
    bool deletions = true;
    // Не читать файлы моложе этого возраста (тики FILETIME): запись могла не закончиться.
    std::int64_t min_age = 0;
    std::int64_t max_file = fs::kMaxFile;
    std::int64_t now = 0;   // 0 — текущее время
};

struct SyncReport {
    int scanned = 0;      // файлов под наблюдением
    int stored = 0;       // новых версий
    int same = 0;         // перечитан, содержимое прежнее
    int deleted = 0;
    int deferred = 0;     // занят писателем или ещё не отстоялся — в следующий проход
    int unfinished = 0;   // оборванные и обнулённые слои (лежат на диске, в базу не взяты)
    int oversized = 0;
    int failed = 0;
    std::int64_t bytes = 0;
    bool pending() const { return deferred > 0; }
};

// Вердикты по файлам, которые в базу не берём: чтобы не перечитывать их каждый проход.
struct Skipped {
    std::int64_t size = 0, mtime = 0;
    bool unfinished = false;   // иначе — слишком большой
};
using SkipCache = std::map<std::string, Skipped>;

// Путь относится к players.db: им занимается players.cpp, а не файловый проход.
bool is_players_path(std::string_view rel);

SyncReport sync_files(Store& store, Store::Writer& w, const fs::Path& root, const SyncOptions& opt, SkipCache& skip);

}  // namespace dpdb
