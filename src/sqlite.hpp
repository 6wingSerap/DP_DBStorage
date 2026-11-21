// SQLite собран из исходника прямо в плагин (версия и опции сборки — в CMakeLists):
// на любой Windows работает один и тот же движок. Обёртка — прямые вызовы C API.
// Ошибка — всегда исключение sql::Error: забыть проверку нельзя.
#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace dpdb::sql {

struct Error : std::runtime_error {
    int code;   // первичный код SQLite (младший байт)
    Error(int c, const std::string& msg) : std::runtime_error(msg), code(c) {}
};

inline constexpr int kBusy = 5, kLocked = 6, kCorrupt = 11, kFull = 13, kNotADb = 26;
inline constexpr int kReadOnly = 1, kReadWrite = 2, kCreate = 4;

std::string version();   // «3.54.1»

class Stmt {
public:
    Stmt(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}
    Stmt(Stmt&& o) noexcept : db_(o.db_), stmt_(o.stmt_) { o.stmt_ = nullptr; }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    ~Stmt();

    Stmt& bind(int i, std::int64_t v);
    Stmt& bind(int i, std::string_view text);             // копируется
    Stmt& bind(int i, std::span<const std::byte> blob);   // НЕ копируется: живёт до step/reset
    Stmt& bind(int i, std::nullptr_t);

    bool step();    // true — строка готова, false — оператор исчерпан
    void reset();

    std::int64_t i64(int col) const;
    std::string text(int col) const;
    std::span<const std::byte> blob(int col) const;   // до следующего step/reset
    bool null(int col) const;

private:
    void check(int rc) const;
    sqlite3* db_;
    sqlite3_stmt* stmt_;
};

class Db {
public:
    Db(const std::filesystem::path& path, int flags);
    Db(Db&& o) noexcept : db_(o.db_) { o.db_ = nullptr; }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    ~Db();

    void exec(std::string_view sql);             // один или несколько операторов
    Stmt prepare(std::string_view sql);
    std::int64_t scalar(std::string_view sql);   // первая колонка первой строки; пусто — 0
    std::string scalar_text(std::string_view sql);
    int changes() const;
    std::int64_t last_rowid() const;
    void busy_timeout(int ms);
    // Согласованная копия всей базы в файл (backup API).
    void backup_to(const std::filesystem::path& dest);

private:
    sqlite3* db_;
};

// BEGIN IMMEDIATE … COMMIT; без commit() — ROLLBACK в деструкторе.
class Tx {
public:
    explicit Tx(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    ~Tx();
    void commit();

private:
    Db& db_;
    bool done_ = false;
};

}  // namespace dpdb::sql
