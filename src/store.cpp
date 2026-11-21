#include "store.hpp"

#include <algorithm>
#include <format>

namespace dpdb {

namespace {

constexpr std::int64_t kAppId = 0x44504442;   // 'DPDB': по нему отличаем свою базу от чужой

// Схема меняется только добавлением миграции в конец; существующие не правятся.
// user_version базы = число применённых миграций.
constexpr std::string_view kMigrations[] = {
    R"sql(
CREATE TABLE meta(k TEXT PRIMARY KEY, v TEXT NOT NULL);
CREATE TABLE generations(
    gen    INTEGER PRIMARY KEY,
    ts     INTEGER NOT NULL,              -- unix, мс
    kind   TEXT    NOT NULL,              -- boot | cycle | restore | wipe | …
    build  TEXT    NOT NULL DEFAULT '',   -- версия exe игры
    pinned INTEGER NOT NULL DEFAULT 0,    -- удержание не трогает
    note   TEXT    NOT NULL DEFAULT '');
CREATE TABLE blobs(
    id   INTEGER PRIMARY KEY,
    hash BLOB    NOT NULL UNIQUE,         -- SHA-256 содержимого
    size INTEGER NOT NULL,
    data BLOB    NOT NULL);
CREATE TABLE files(
    path  TEXT    NOT NULL,               -- от корня storage: '/', нижний регистр
    gen   INTEGER NOT NULL REFERENCES generations(gen),
    blob  INTEGER REFERENCES blobs(id),   -- NULL — файл удалён
    mtime INTEGER NOT NULL DEFAULT 0,     -- mtime на диске при захвате: ускоритель, не истина
    fmt   INTEGER,                        -- заголовок слоя CE, NULL — не слой
    save  INTEGER,
    stamp INTEGER,
    tag   TEXT,
    PRIMARY KEY(path, gen));
CREATE INDEX files_gen  ON files(gen);
CREATE INDEX files_blob ON files(blob);
CREATE TABLE characters(
    uid   TEXT    NOT NULL,               -- Players.UID
    gen   INTEGER NOT NULL REFERENCES generations(gen),
    id    INTEGER,                        -- Players.Id
    alive INTEGER,
    blob  INTEGER REFERENCES blobs(id),   -- Players.Data, NULL — Data NULL
    gone  INTEGER NOT NULL DEFAULT 0,     -- 1 — строки в Players больше нет
    PRIMARY KEY(uid, gen));
CREATE INDEX characters_gen  ON characters(gen);
CREATE INDEX characters_blob ON characters(blob);
)sql",
};
constexpr int kSchemaVersion = static_cast<int>(std::size(kMigrations));

bool is_corruption(const sql::Error& e) {
    return e.code == sql::kCorrupt || e.code == sql::kNotADb;
}

}  // namespace

// ── открытие ─────────────────────────────────────────────────────────────────

Store::Store(const fs::Path& path, Log log, bool may_replace) : log_(std::move(log)) {
    try {
        open(path);
    } catch (const sql::Error& e) {
        if (!is_corruption(e) || !may_replace)
            throw;
        stmts_.clear();
        db_.reset();
        const fs::Path moved = quarantine(path);
        log_(std::format("база испорчена ({}), отложена как «{}», создаётся новая", e.what(),
                         fs::to_utf8(moved.filename().wstring())));
        quarantined_ = true;
        open(path);
    }
}

fs::Path Store::quarantine(const fs::Path& path) {
    const std::wstring suffix = L".corrupt-" + std::to_wstring(fs::unix_ms());
    fs::Path moved;
    for (const wchar_t* tail : {L"", L"-wal", L"-shm"}) {
        fs::Path from = path;
        from += tail;
        fs::Path to = path;
        to += suffix;
        to += tail;
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        if (!ec && !*tail)
            moved = to;
    }
    return moved;
}

void Store::open(const fs::Path& path) {
    std::filesystem::create_directories(path.parent_path());
    db_.emplace(path, sql::kReadWrite | sql::kCreate);
    sql::Db& db = *db_;
    db.busy_timeout(5000);
    // Настройки из замеров (README, «Производительность»): страница 64 КБ и кэш 64 МБ
    // дают кратный выигрыш на больших блобах. page_size действует только на новую базу.
    db.exec("PRAGMA page_size = 65536");
    db.exec("PRAGMA journal_mode = WAL");
    db.exec("PRAGMA synchronous = FULL");
    db.exec("PRAGMA foreign_keys = ON");
    db.exec("PRAGMA cache_size = -65536");

    const std::int64_t app = db.scalar("PRAGMA application_id");
    int version = static_cast<int>(db.scalar("PRAGMA user_version"));
    if (app == 0 && db.scalar("SELECT count(*) FROM sqlite_master") == 0) {
        db.exec(std::format("PRAGMA application_id = {}", kAppId));
        version = 0;
    } else if (app != kAppId) {
        throw sql::Error(1, "файл «" + fs::to_utf8(path.wstring()) + "» — не база DP_DBStorage, не трогаю");
    }
    if (version > kSchemaVersion)
        throw sql::Error(1, std::format("база схемы v{} новее плагина (v{}): обновите плагин", version,
                                        kSchemaVersion));
    for (; version < kSchemaVersion; ++version) {
        sql::Tx tx(db);
        db.exec(kMigrations[version]);
        db.exec(std::format("PRAGMA user_version = {}", version + 1));
        tx.commit();
    }
    load_head();
}

void Store::load_head() {
    files_.clear();
    chars_.clear();
    head_gen_ = db_->scalar("SELECT coalesce(max(gen), 0) FROM generations");
    auto f = db_->prepare(
        "SELECT f.path, f.gen, f.blob, f.mtime, f.fmt, f.tag, b.hash, b.size"
        " FROM files f JOIN blobs b ON b.id = f.blob"
        " WHERE f.gen = (SELECT max(gen) FROM files WHERE path = f.path)");
    while (f.step()) {
        HeadFile h;
        h.gen = f.i64(1);
        h.blob = f.i64(2);
        h.mtime = f.i64(3);
        h.fmt = f.null(4) ? -1 : f.i64(4);
        h.tag = f.text(5);
        const auto hash = f.blob(6);
        if (hash.size() != h.hash.size())
            throw sql::Error(sql::kCorrupt, "blobs.hash неверной длины");
        std::ranges::copy(hash, h.hash.begin());
        h.size = f.i64(7);
        files_.emplace(f.text(0), std::move(h));
    }
    auto c = db_->prepare(
        "SELECT c.uid, c.id, c.alive, c.blob, b.hash"
        " FROM characters c LEFT JOIN blobs b ON b.id = c.blob"
        " WHERE c.gone = 0 AND c.gen = (SELECT max(gen) FROM characters WHERE uid = c.uid)");
    while (c.step()) {
        HeadChar h;
        h.id = c.i64(1);
        h.alive = c.i64(2);
        if (!c.null(3)) {
            h.blob = c.i64(3);
            const auto hash = c.blob(4);
            if (hash.size() != h.hash.size())
                throw sql::Error(sql::kCorrupt, "blobs.hash неверной длины");
            std::ranges::copy(hash, h.hash.begin());
        }
        chars_.emplace(c.text(0), h);
    }
}

sql::Stmt& Store::stmt(const char* sql) {
    auto it = stmts_.find(sql);
    if (it == stmts_.end())
        it = stmts_.emplace(sql, db_->prepare(sql)).first;
    it->second.reset();
    return it->second;
}

// ── запись ───────────────────────────────────────────────────────────────────

// Строка (ключ, поколение) пишется обычным INSERT: при включённых внешних ключах
// INSERT OR REPLACE заводит журнал оператора и копирует в него страницы по 64 КБ — это
// 20 мкс на строку против 1. Замена нужна только повторной записи ключа в том же
// поколении, а такая запись всегда есть в pending.
template <class Pending>
const char* row_sql(const Pending& pending, const std::string& key, const char* insert, const char* replace) {
    return pending.contains(key) ? replace : insert;
}

std::int64_t Store::blob_put(const fs::Hash& hash, std::span<const std::byte> data) {
    auto& find = stmt("SELECT id FROM blobs WHERE hash = ?");
    find.bind(1, std::span<const std::byte>(hash));
    const bool found = find.step();
    const std::int64_t id = found ? find.i64(0) : 0;
    find.reset();
    if (found)
        return id;
    auto& ins = stmt("INSERT INTO blobs(hash, size, data) VALUES(?, ?, ?)");
    ins.bind(1, std::span<const std::byte>(hash)).bind(2, static_cast<std::int64_t>(data.size())).bind(3, data);
    ins.step();
    ins.reset();
    return db_->last_rowid();
}

Store::Writer::Writer(Store& s, std::string kind, std::string build, std::string note, std::int64_t now_ms)
    : s_(s), kind_(std::move(kind)), build_(std::move(build)), note_(std::move(note)), now_ms_(now_ms) {}

void Store::Writer::open_tx() {
    if (!tx_)
        tx_.emplace(*s_.db_);
}

std::int64_t Store::Writer::gen() {
    if (gen_ == 0) {
        open_tx();
        auto& q = s_.stmt("INSERT INTO generations(ts, kind, build, note) VALUES(?, ?, ?, ?)");
        q.bind(1, now_ms_).bind(2, kind_).bind(3, build_).bind(4, note_);
        q.step();
        gen_ = s_.db_->last_rowid();
    }
    return gen_;
}

const HeadFile* Store::Writer::file(const std::string& path) const {
    if (const auto it = files_.find(path); it != files_.end())
        return it->second ? &*it->second : nullptr;
    const auto it = s_.files_.find(path);
    return it == s_.files_.end() ? nullptr : &it->second;
}

const HeadChar* Store::Writer::chr(const std::string& uid) const {
    if (const auto it = chars_.find(uid); it != chars_.end())
        return it->second ? &*it->second : nullptr;
    const auto it = s_.chars_.find(uid);
    return it == s_.chars_.end() ? nullptr : &it->second;
}

bool Store::Writer::put_file(std::string_view path, std::span<const std::byte> data, std::int64_t mtime,
                             const Facts& facts) {
    const std::string key(path);
    const fs::Hash hash = fs::sha256(data);
    if (const HeadFile* cur = file(key); cur && cur->hash == hash) {
        if (cur->mtime != mtime) {
            open_tx();
            auto& q = s_.stmt("UPDATE files SET mtime = ? WHERE path = ? AND gen = ?");
            q.bind(1, mtime).bind(2, key).bind(3, cur->gen);
            q.step();
            HeadFile touched = *cur;
            touched.mtime = mtime;
            files_[key] = std::move(touched);
        }
        return false;
    }
    HeadFile h;
    h.gen = gen();
    h.blob = s_.blob_put(hash, data);
    h.hash = hash;
    h.size = static_cast<std::int64_t>(data.size());
    h.mtime = mtime;
    const bool layer = facts.shape == Shape::Complete;
    h.fmt = layer ? facts.fmt : -1;
    h.tag = layer ? facts.tag : std::string();
    auto& q = s_.stmt(row_sql(files_, key,
                              "INSERT INTO files(path, gen, blob, mtime, fmt, save, stamp, tag)"
                              " VALUES(?, ?, ?, ?, ?, ?, ?, ?)",
                              "INSERT OR REPLACE INTO files(path, gen, blob, mtime, fmt, save, stamp, tag)"
                              " VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    q.bind(1, key).bind(2, h.gen).bind(3, h.blob).bind(4, mtime);
    const auto opt = [&](int i, std::int64_t v) { layer && v >= 0 ? q.bind(i, v) : q.bind(i, nullptr); };
    opt(5, facts.fmt);
    opt(6, facts.save);
    opt(7, facts.stamp);
    layer ? q.bind(8, facts.tag) : q.bind(8, nullptr);
    q.step();
    files_[key] = std::move(h);
    return true;
}

void Store::Writer::del_file(std::string_view path) {
    const std::string key(path);
    if (!file(key))
        return;
    auto& q = s_.stmt(row_sql(files_, key, "INSERT INTO files(path, gen, blob) VALUES(?, ?, NULL)",
                              "INSERT OR REPLACE INTO files(path, gen, blob) VALUES(?, ?, NULL)"));
    q.bind(1, key).bind(2, gen());
    q.step();
    files_[key] = std::nullopt;
}

bool Store::Writer::put_char(std::string_view uid, std::int64_t id, std::int64_t alive, const fs::Bytes* data) {
    const std::string key(uid);
    const fs::Hash hash = data ? fs::sha256(*data) : fs::Hash{};
    if (const HeadChar* cur = chr(key);
        cur && cur->id == id && cur->alive == alive && (data ? cur->blob != 0 && cur->hash == hash : cur->blob == 0))
        return false;
    HeadChar h;
    h.id = id;
    h.alive = alive;
    const std::int64_t g = gen();
    if (data) {
        h.blob = s_.blob_put(hash, *data);
        h.hash = hash;
    }
    auto& q = s_.stmt(row_sql(chars_, key,
                              "INSERT INTO characters(uid, gen, id, alive, blob, gone) VALUES(?, ?, ?, ?, ?, 0)",
                              "INSERT OR REPLACE INTO characters(uid, gen, id, alive, blob, gone) VALUES(?, ?, ?, ?, ?, 0)"));
    q.bind(1, key).bind(2, g).bind(3, id).bind(4, alive);
    data ? q.bind(5, h.blob) : q.bind(5, nullptr);
    q.step();
    chars_[key] = h;
    return true;
}

void Store::Writer::del_char(std::string_view uid) {
    const std::string key(uid);
    if (!chr(key))
        return;
    auto& q = s_.stmt(row_sql(chars_, key, "INSERT INTO characters(uid, gen, gone) VALUES(?, ?, 1)",
                              "INSERT OR REPLACE INTO characters(uid, gen, gone) VALUES(?, ?, 1)"));
    q.bind(1, key).bind(2, gen());
    q.step();
    chars_[key] = std::nullopt;
}

std::int64_t Store::Writer::commit() {
    if (!tx_)
        return 0;
    tx_->commit();
    tx_.reset();
    for (auto& [path, h] : files_)
        h ? void(s_.files_[path] = std::move(*h)) : void(s_.files_.erase(path));
    for (auto& [uid, h] : chars_)
        h ? void(s_.chars_[uid] = *h) : void(s_.chars_.erase(uid));
    files_.clear();
    chars_.clear();
    if (gen_)
        s_.head_gen_ = gen_;
    return std::exchange(gen_, 0);
}

// ── чтение ───────────────────────────────────────────────────────────────────

fs::Bytes Store::blob(std::int64_t id) {
    auto& q = stmt("SELECT hash, data FROM blobs WHERE id = ?");
    q.bind(1, id);
    if (!q.step())
        throw sql::Error(sql::kCorrupt, std::format("блоб {} отсутствует", id));
    const auto hash = q.blob(0);
    const auto data = q.blob(1);
    fs::Bytes out(data.begin(), data.end());
    const bool ok = std::ranges::equal(hash, fs::sha256(out));
    q.reset();
    if (!ok)
        throw sql::Error(sql::kCorrupt, std::format("блоб {} повреждён: хеш не сходится", id));
    return out;
}

State Store::state(std::int64_t gen) {
    State st;
    auto f = db_->prepare("SELECT path, blob FROM files f WHERE blob IS NOT NULL"
                          " AND gen = (SELECT max(gen) FROM files WHERE path = f.path AND gen <= ?1)");
    f.bind(1, gen);
    while (f.step())
        st.files.emplace(f.text(0), f.i64(1));
    auto c = db_->prepare("SELECT uid, id, alive, coalesce(blob, 0) FROM characters c WHERE gone = 0"
                          " AND gen = (SELECT max(gen) FROM characters WHERE uid = c.uid AND gen <= ?1)"
                          " ORDER BY id");
    c.bind(1, gen);
    while (c.step())
        st.chars.push_back({c.text(0), c.i64(1), c.i64(2), c.i64(3)});
    return st;
}

std::vector<GenInfo> Store::generations(int limit) {
    std::vector<GenInfo> out;
    auto q = db_->prepare("SELECT g.gen, g.ts, g.kind, g.build, g.note, g.pinned,"
                          " (SELECT count(*) FROM files f WHERE f.gen = g.gen),"
                          " (SELECT count(*) FROM characters c WHERE c.gen = g.gen)"
                          " FROM generations g ORDER BY g.gen DESC LIMIT ?");
    q.bind(1, std::int64_t{limit > 0 ? limit : -1});
    while (q.step())
        out.push_back({q.i64(0), q.i64(1), q.text(2), q.text(3), q.text(4), q.i64(5) != 0, q.i64(6), q.i64(7)});
    return out;
}

void Store::pin(std::int64_t gen, bool pinned, std::string_view note) {
    auto q = db_->prepare("UPDATE generations SET pinned = ?, note = CASE WHEN ?2 = '' THEN note ELSE ?2 END"
                          " WHERE gen = ?3");
    q.bind(1, std::int64_t{pinned}).bind(2, note).bind(3, gen);
    q.step();
    if (db_->changes() == 0)
        throw sql::Error(1, std::format("поколения {} нет", gen));
}

bool Store::restorable(std::int64_t gen) {
    auto q = db_->prepare("SELECT pinned FROM generations WHERE gen = ?");
    q.bind(1, gen);
    if (!q.step())
        return false;
    const std::string floor = meta("floor");
    return q.i64(0) != 0 || gen >= (floor.empty() ? 0 : std::stoll(floor));
}

std::string Store::meta(std::string_view key) {
    auto q = db_->prepare("SELECT v FROM meta WHERE k = ?");
    q.bind(1, key);
    return q.step() ? q.text(0) : std::string();
}

void Store::set_meta(std::string_view key, std::string_view value) {
    auto q = db_->prepare("INSERT OR REPLACE INTO meta(k, v) VALUES(?, ?)");
    q.bind(1, key).bind(2, value);
    q.step();
}

// ── удержание ────────────────────────────────────────────────────────────────

std::int64_t Store::used_bytes() {
    return (db_->scalar("PRAGMA page_count") - db_->scalar("PRAGMA freelist_count")) * db_->scalar("PRAGMA page_size");
}

// Версия нужна, пока она «последняя на момент» хотя бы одной точки, которую мы обязаны
// уметь восстановить: любого поколения >= cutoff и любого закреплённого. Значит, удалить
// её можно, когда следующая версия того же пути появилась не позже cutoff и между ними
// нет закреплённого поколения.
void Store::squash(std::int64_t cutoff, RetentionReport& r) {
    sql::Tx tx(*db_);
    for (const auto& [table, key, dead] : {std::tuple{"files", "path", "blob IS NULL"},
                                           std::tuple{"characters", "uid", "gone = 1"}}) {
        auto superseded = db_->prepare(std::format(
            "DELETE FROM {0} WHERE gen < ?1 AND EXISTS (SELECT 1 FROM {0} n WHERE n.{1} = {0}.{1}"
            " AND n.gen > {0}.gen AND n.gen <= ?1 AND NOT EXISTS (SELECT 1 FROM generations p"
            " WHERE p.pinned = 1 AND p.gen >= {0}.gen AND p.gen < n.gen))",
            table, key));
        superseded.bind(1, cutoff);
        superseded.step();
        r.versions += db_->changes();
        // Отметка об удалении, за которой ничего не осталось, уже ничего не сообщает.
        auto tombstones = db_->prepare(std::format(
            "DELETE FROM {0} WHERE {2} AND gen <= ?1"
            " AND NOT EXISTS (SELECT 1 FROM {0} o WHERE o.{1} = {0}.{1} AND o.gen < {0}.gen)",
            table, key, dead));
        tombstones.bind(1, cutoff);
        tombstones.step();
        r.versions += db_->changes();
    }
    db_->exec("DELETE FROM blobs WHERE NOT EXISTS (SELECT 1 FROM files WHERE blob = blobs.id)"
              " AND NOT EXISTS (SELECT 1 FROM characters WHERE blob = blobs.id)");
    r.blobs += db_->changes();
    auto gens = db_->prepare("DELETE FROM generations WHERE gen < ?1 AND pinned = 0"
                             " AND NOT EXISTS (SELECT 1 FROM files WHERE gen = generations.gen)"
                             " AND NOT EXISTS (SELECT 1 FROM characters WHERE gen = generations.gen)");
    gens.bind(1, cutoff);
    gens.step();
    r.generations += db_->changes();
    // Ниже границы история неполна: откат туда возможен только на закреплённые поколения.
    auto floor = db_->prepare("INSERT OR REPLACE INTO meta(k, v) VALUES('floor', ?)");
    floor.bind(1, std::to_string(cutoff));
    floor.step();
    tx.commit();
    r.cutoff = cutoff;
}

RetentionReport Store::retain(std::int64_t now_ms, int keep_hours, std::int64_t max_bytes) {
    RetentionReport r;
    stmts_.clear();   // открытые курсоры кэша не должны мешать контрольной точке
    auto q = db_->prepare("SELECT coalesce(max(gen), 0) FROM generations WHERE ts <= ?");
    q.bind(1, now_ms - std::int64_t{keep_hours} * 3'600'000);
    q.step();
    std::int64_t cutoff = q.i64(0);
    q.reset();
    if (cutoff > 0)
        squash(cutoff, r);
    // Диск не резиновый: если окно по времени не уложилось в лимит — режем окно пополам,
    // пока не уложится или не останется одна голова.
    while (used_bytes() > max_bytes && cutoff < head_gen_) {
        cutoff += std::max<std::int64_t>(1, (head_gen_ - cutoff + 1) / 2);
        squash(cutoff, r);
    }
    if (r.versions || r.blobs) {
        try {
            db_->exec("PRAGMA wal_checkpoint(TRUNCATE)");
        } catch (const sql::Error& e) {
            log_(std::format("контрольная точка WAL не прошла: {}", e.what()));
        }
    }
    return r;
}

VerifyReport Store::verify() {
    VerifyReport v;
    for (const char* check : {"PRAGMA integrity_check", "PRAGMA foreign_key_check"}) {
        auto q = db_->prepare(check);
        while (q.step())
            if (const std::string row = q.text(0); row != "ok")
                v.problems.push_back(std::string(check) + ": " + row);
    }
    auto q = db_->prepare("SELECT id, hash, size, data FROM blobs");
    while (q.step()) {
        ++v.blobs;
        const auto data = q.blob(3);
        if (static_cast<std::int64_t>(data.size()) != q.i64(2) || !std::ranges::equal(q.blob(1), fs::sha256(data)))
            v.problems.push_back(std::format("блоб {}: содержимое не соответствует хешу", q.i64(0)));
    }
    return v;
}

}  // namespace dpdb
