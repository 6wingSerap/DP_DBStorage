// База плагина: поколения, содержимое файлов (дедуп по SHA-256), персонажи, удержание.
//
// Модель: байты движка хранятся как есть. Каждая запись изменений — поколение; версия
// файла/персонажа принадлежит поколению, в котором появилась. Состояние на поколение G —
// последняя версия каждого пути с gen <= G. «Голова» — состояние на последнее поколение,
// держится в памяти.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "format.hpp"
#include "fs.hpp"
#include "sqlite.hpp"

namespace dpdb {

using Log = std::function<void(std::string_view)>;

// Виртуальные пути в files: у настоящих файлов двоеточия в относительном пути не бывает.
inline constexpr std::string_view kPlayersFile = "players.db";            // весь файл (запасной режим)
inline constexpr std::string_view kPlayersSchema = "players.db:schema";   // SQL-скрипт схемы движка

struct HeadFile {
    std::int64_t blob = 0, size = 0, mtime = 0, gen = 0;
    fs::Hash hash{};
    std::int64_t fmt = -1;   // версия формата слоя; -1 — не слой CE
    std::string tag;
};

struct HeadChar {
    std::int64_t id = 0, alive = 0;
    std::int64_t blob = 0;   // 0 — Data NULL
    fs::Hash hash{};
};

struct GenInfo {
    std::int64_t gen = 0, ts = 0;   // ts — unix, мс
    std::string kind, build, note;
    bool pinned = false;
    std::int64_t files = 0, chars = 0;   // версий в поколении
};

struct State {
    struct Char {
        std::string uid;
        std::int64_t id = 0, alive = 0, blob = 0;
    };
    std::map<std::string, std::int64_t> files;   // путь → blob
    std::vector<Char> chars;
};

struct RetentionReport {
    std::int64_t cutoff = 0, versions = 0, blobs = 0, generations = 0;
};

struct VerifyReport {
    std::int64_t blobs = 0;
    std::vector<std::string> problems;
};

class Store {
public:
    // Открыть или создать. Испорченный файл откладывается в сторону (*.corrupt-<время>) и
    // создаётся новая база: рабочая копия мира лежит на диске, теряется только история.
    // Так поступает только хозяин базы (may_replace); читающий рядом получает исключение.
    // Чужой SQLite-файл и база более новой схемы — исключение, файл не трогается.
    Store(const fs::Path& path, Log log, bool may_replace = true);

    // Отложить файл базы (и её -wal/-shm) в сторону. Возвращает новое имя или пусто.
    static fs::Path quarantine(const fs::Path& path);

    // Одна транзакция = не более одного поколения. Поколение заводится на первой
    // настоящей версии; без commit() всё откатывается и голова не меняется.
    class Writer {
    public:
        Writer(Store& s, std::string kind, std::string build, std::string note, std::int64_t now_ms);
        Writer(const Writer&) = delete;
        Writer& operator=(const Writer&) = delete;

        // false — содержимое совпало с головой (обновлён только mtime-ускоритель).
        bool put_file(std::string_view path, std::span<const std::byte> data, std::int64_t mtime, const Facts& facts);
        void del_file(std::string_view path);
        // data == nullptr — Data NULL. false — персонаж не изменился.
        bool put_char(std::string_view uid, std::int64_t id, std::int64_t alive, const fs::Bytes* data);
        void del_char(std::string_view uid);
        std::int64_t commit();   // номер поколения; 0 — версий не добавилось

    private:
        void open_tx();
        std::int64_t gen();
        const HeadFile* file(const std::string& path) const;
        const HeadChar* chr(const std::string& uid) const;
        Store& s_;
        std::string kind_, build_, note_;
        std::int64_t now_ms_;
        std::optional<sql::Tx> tx_;
        std::int64_t gen_ = 0;
        std::map<std::string, std::optional<HeadFile>> files_;   // nullopt — удалён
        std::map<std::string, std::optional<HeadChar>> chars_;
    };

    Writer begin(std::string kind, std::string build = {}, std::string note = {},
                 std::int64_t now_ms = fs::unix_ms()) {
        return Writer(*this, std::move(kind), std::move(build), std::move(note), now_ms);
    }

    const std::map<std::string, HeadFile>& files() const { return files_; }
    const std::map<std::string, HeadChar>& chars() const { return chars_; }
    std::int64_t head_gen() const { return head_gen_; }
    bool was_quarantined() const { return quarantined_; }

    fs::Bytes blob(std::int64_t id);   // хеш сверяется; расхождение — sql::Error(kCorrupt)
    State state(std::int64_t gen);
    std::vector<GenInfo> generations(int limit = 0);   // новые сверху
    void pin(std::int64_t gen, bool pinned, std::string_view note);
    // Состояние на это поколение ещё восстановимо точно: оно закреплено или не старше
    // границы, до которой удержание схлопнуло историю.
    bool restorable(std::int64_t gen);

    // Схлопнуть историю старше keep_hours, а если база всё ещё больше max_bytes — и более
    // свежую, вплоть до одной головы. Закреплённые поколения сохраняются всегда.
    RetentionReport retain(std::int64_t now_ms, int keep_hours, std::int64_t max_bytes);
    VerifyReport verify();
    std::int64_t used_bytes();

    std::string meta(std::string_view key);
    void set_meta(std::string_view key, std::string_view value);
    sql::Db& db() { return *db_; }

private:
    void open(const fs::Path& path);
    void load_head();
    void squash(std::int64_t cutoff, RetentionReport& r);
    std::int64_t blob_put(const fs::Hash& hash, std::span<const std::byte> data);
    sql::Stmt& stmt(const char* sql);

    Log log_;
    std::optional<sql::Db> db_;
    std::map<const char*, sql::Stmt> stmts_;   // ключ — адрес литерала
    std::map<std::string, HeadFile> files_;
    std::map<std::string, HeadChar> chars_;
    std::int64_t head_gen_ = 0;
    bool quarantined_ = false;
};

}  // namespace dpdb
