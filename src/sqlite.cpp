#include "sqlite.hpp"

#include <sqlite3.h>

#include "fs.hpp"

namespace dpdb::sql {

static_assert(kBusy == SQLITE_BUSY && kLocked == SQLITE_LOCKED && kCorrupt == SQLITE_CORRUPT &&
              kFull == SQLITE_FULL && kNotADb == SQLITE_NOTADB);
static_assert(kReadOnly == SQLITE_OPEN_READONLY && kReadWrite == SQLITE_OPEN_READWRITE &&
              kCreate == SQLITE_OPEN_CREATE);

std::string version() {
    return sqlite3_libversion();
}

namespace {

[[noreturn]] void fail(sqlite3* db, int rc, std::string_view what) {
    const std::string msg = db ? sqlite3_errmsg(db) : "нет соединения";
    throw Error(rc & 0xff, std::string(what) + ": " + msg + " (код " + std::to_string(rc) + ")");
}

}  // namespace

// ── Stmt ─────────────────────────────────────────────────────────────────────

Stmt::~Stmt() {
    sqlite3_finalize(stmt_);   // nullptr допустим
}

void Stmt::check(int rc) const {
    if (rc != SQLITE_OK)
        fail(db_, rc, "sqlite");
}

Stmt& Stmt::bind(int i, std::int64_t v) {
    check(sqlite3_bind_int64(stmt_, i, v));
    return *this;
}

Stmt& Stmt::bind(int i, std::string_view text) {
    check(sqlite3_bind_text64(stmt_, i, text.data(), text.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    return *this;
}

Stmt& Stmt::bind(int i, std::span<const std::byte> blob) {
    // Нулевой указатель SQLite понимает как NULL — пустой blob привязываем к заглушке.
    static const std::byte empty{};
    check(sqlite3_bind_blob64(stmt_, i, blob.empty() ? &empty : blob.data(), blob.size(), SQLITE_STATIC));
    return *this;
}

Stmt& Stmt::bind(int i, std::nullptr_t) {
    check(sqlite3_bind_null(stmt_, i));
    return *this;
}

bool Stmt::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)
        return true;
    if (rc == SQLITE_DONE)
        return false;
    fail(db_, rc, "sqlite step");
}

void Stmt::reset() {
    sqlite3_reset(stmt_);   // код повторяет ошибку step — уже брошена
}

std::int64_t Stmt::i64(int col) const {
    return sqlite3_column_int64(stmt_, col);
}

std::string Stmt::text(int col) const {
    const auto* p = sqlite3_column_text(stmt_, col);
    return p ? std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(sqlite3_column_bytes(stmt_, col)))
             : std::string();
}

std::span<const std::byte> Stmt::blob(int col) const {
    const void* p = sqlite3_column_blob(stmt_, col);
    return {static_cast<const std::byte*>(p), p ? static_cast<std::size_t>(sqlite3_column_bytes(stmt_, col)) : 0};
}

bool Stmt::null(int col) const {
    return sqlite3_column_type(stmt_, col) == SQLITE_NULL;
}

// ── Db ───────────────────────────────────────────────────────────────────────

Db::Db(const std::filesystem::path& path, int flags) : db_(nullptr) {
    sqlite3_initialize();   // сборка с SQLITE_OMIT_AUTOINIT; повторный вызов ничего не стоит
    const int rc = sqlite3_open_v2(fs::to_utf8(path.wstring()).c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "нет памяти";
        sqlite3_close(db_);
        db_ = nullptr;
        throw Error(rc & 0xff, "не открыть базу «" + fs::to_utf8(path.wstring()) + "»: " + msg);
    }
}

Db::~Db() {
    sqlite3_close(db_);   // nullptr допустим
}

void Db::exec(std::string_view sql) {
    const std::string z(sql);
    const int rc = sqlite3_exec(db_, z.c_str(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK)
        fail(db_, rc, z.substr(0, 120));
}

Stmt Db::prepare(std::string_view sql) {
    sqlite3_stmt* s = nullptr;
    const int rc = sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &s, nullptr);
    if (rc != SQLITE_OK)
        fail(db_, rc, sql.substr(0, 120));
    return Stmt(db_, s);
}

std::int64_t Db::scalar(std::string_view sql) {
    Stmt s = prepare(sql);
    return s.step() ? s.i64(0) : 0;
}

std::string Db::scalar_text(std::string_view sql) {
    Stmt s = prepare(sql);
    return s.step() ? s.text(0) : std::string();
}

int Db::changes() const {
    return sqlite3_changes(db_);
}

std::int64_t Db::last_rowid() const {
    return sqlite3_last_insert_rowid(db_);
}

void Db::busy_timeout(int ms) {
    sqlite3_busy_timeout(db_, ms);
}

void Db::backup_to(const std::filesystem::path& dest) {
    Db out(dest, kReadWrite | kCreate);
    sqlite3_backup* b = sqlite3_backup_init(out.db_, "main", db_, "main");
    if (!b)
        fail(out.db_, SQLITE_ERROR, "backup");
    const int rc = sqlite3_backup_step(b, -1);
    sqlite3_backup_finish(b);
    if (rc != SQLITE_DONE)
        fail(out.db_, rc, "backup");
}

// ── Tx ───────────────────────────────────────────────────────────────────────

Tx::~Tx() {
    if (done_)
        return;
    try {
        db_.exec("ROLLBACK");
    } catch (const Error&) {
        // транзакции уже нет (SQLite сам откатил после ошибки) — откатывать нечего
    }
}

void Tx::commit() {
    db_.exec("COMMIT");
    done_ = true;
}

}  // namespace dpdb::sql
