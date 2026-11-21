// Файловые примитивы ядра: обход каталога, чтение рядом с пишущим движком, атомарная
// запись, SHA-256, перекодировка путей.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dpdb::fs {

using Bytes = std::vector<std::byte>;
using Hash = std::array<std::byte, 32>;
using Path = std::filesystem::path;

// Суффикс наших временных файлов: в базу они не попадают, на старте подчищаются.
inline constexpr std::string_view kTmpSuffix = ".dpdb-tmp";
// Потолок размера одного файла: блоб SQLite ограничен ~1 ГБ, слои движка сегментированы.
inline constexpr std::int64_t kMaxFile = 512ll * 1024 * 1024;

Hash sha256(std::span<const std::byte> data);
std::string hex(std::span<const std::byte> data);

std::string to_utf8(std::wstring_view w);
std::wstring to_wide(std::string_view s);

// Текущее время и mtime файлов — в одних единицах (FILETIME: 100 нс от 1601 года).
std::int64_t now_ticks();
inline constexpr std::int64_t kTicksPerMs = 10'000;
std::int64_t unix_ms();

struct Entry {
    std::string rel;   // путь от корня: UTF-8, '/', ASCII в нижнем регистре
    std::int64_t size = 0;
    std::int64_t mtime = 0;
};
// Все файлы под root рекурсивно, отсортированы по rel. Нет каталога — пусто.
std::vector<Entry> scan(const Path& root);

enum class Read { Ok, Missing, Busy, TooBig, Failed };
// Чтение целиком с полным share. Busy — файл держит писатель (движок открывает слои с
// share=0): не ошибка, повторить позже.
Read read_shared(const Path& path, Bytes& out, std::int64_t max_size = kMaxFile);

// Движок важнее нас. Он открывает слои монопольно, и если в этот миг файл читаем мы,
// получает отказ. Перехват CreateFileW (watch.cpp) в таком случае повторяет открытие, а
// чтобы ожидание было коротким и конечным, объявляет «писатель ждёт»: пока флаг поднят,
// read_shared новых чтений не начинает (отвечает Busy), а идущее заканчивается за одно
// чтение файла.
int active_readers();           // чтений read_shared прямо сейчас
void writer_waiting(int delta); // +1 — движок ждёт файл, −1 — дождался

// Запись через временный файл и переименование: наполовину записанный файл под
// настоящим именем не появляется никогда. Каталоги создаются. durable — дождаться сброса
// на диск (переживает отключение питания); без него в разы быстрее.
void write_atomic(const Path& path, std::span<const std::byte> data, bool durable = true);

}  // namespace dpdb::fs
