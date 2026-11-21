#include "players.hpp"

#include <algorithm>
#include <set>

namespace dpdb {

namespace {

constexpr int kMaxAuxRows = 10'000;   // служебные таблицы малы; больше — раскладка незнакома

std::string lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; });
    return s;
}

std::string ident(std::string_view name) {
    std::string q = "\"";
    for (const char c : name)
        q += c == '"' ? std::string("\"\"") : std::string(1, c);
    return q + "\"";
}

std::vector<std::string> columns(sql::Db& db, const std::string& table) {
    std::vector<std::string> cols;
    auto q = db.prepare("PRAGMA table_info(" + ident(table) + ")");
    while (q.step())
        cols.push_back(q.text(1));
    return cols;
}

std::vector<std::string> user_tables(sql::Db& db) {
    std::vector<std::string> out;
    auto q = db.prepare("SELECT name FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY rowid");
    while (q.step())
        out.push_back(q.text(0));
    return out;
}

bool is_players(const std::string& table) {
    return lower(table) == "players";
}

bool known_players_layout(sql::Db& db, const std::vector<std::string>& tables) {
    const auto it = std::ranges::find_if(tables, is_players);
    if (it == tables.end())
        return false;
    std::vector<std::string> cols = columns(db, *it);
    std::ranges::transform(cols, cols.begin(), lower);
    std::ranges::sort(cols);
    return cols == std::vector<std::string>{"alive", "data", "id", "uid"};
}

// Схема движка дословно + содержимое всех таблиц, кроме Players. Пусто — служебные
// таблицы неожиданно велики.
std::string schema_script(sql::Db& db, const std::vector<std::string>& tables) {
    std::string script;
    auto ddl = db.prepare("SELECT sql FROM sqlite_master WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%'"
                          " ORDER BY rowid");
    while (ddl.step())
        script += ddl.text(0) + ";\n";
    int rows = 0;
    for (const std::string& t : tables) {
        if (is_players(t))
            continue;
        std::string values;
        for (const std::string& c : columns(db, t))
            values += (values.empty() ? "quote(" : " || ',' || quote(") + ident(c) + ")";
        auto q = db.prepare("SELECT " + values + " FROM " + ident(t) + " ORDER BY rowid");
        while (q.step()) {
            if (++rows > kMaxAuxRows)
                return {};
            script += "INSERT INTO " + ident(t) + " VALUES(" + q.text(0) + ");\n";
        }
    }
    return script;
}

std::span<const std::byte> as_bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

bool busy(const sql::Error& e) {
    return e.code == sql::kBusy || e.code == sql::kLocked;
}

void remove_sidecars(const fs::Path& db) {
    for (const wchar_t* tail : {L"-journal", L"-wal", L"-shm"}) {
        fs::Path p = db;
        p += tail;
        std::error_code ec;
        std::filesystem::remove(p, ec);
    }
}

void capture_rows(sql::Db& engine, Store& store, Store::Writer& w, const std::string& script, PlayersReport& r) {
    w.put_file(kPlayersSchema, as_bytes(script), 0, {});
    w.del_file(kPlayersFile);
    std::set<std::string> seen;
    fs::Bytes data;
    auto q = engine.prepare("SELECT Id, Alive, UID, Data FROM Players");
    while (q.step()) {
        std::string uid = q.text(2);
        const bool null = q.null(3);
        if (!null) {
            const auto b = q.blob(3);
            data.assign(b.begin(), b.end());
        }
        r.stored += w.put_char(uid, q.i64(0), q.i64(1), null ? nullptr : &data);
        seen.insert(std::move(uid));
    }
    for (const auto& [uid, head] : store.chars())
        if (!seen.contains(uid)) {
            w.del_char(uid);
            ++r.gone;
        }
}

void capture_file(sql::Db& engine, Store& store, Store::Writer& w, const fs::Path& tmp_dir) {
    const fs::Path tmp = tmp_dir / (L"players.snapshot" + fs::to_wide(fs::kTmpSuffix));
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    engine.backup_to(tmp);
    fs::Bytes bytes;
    const fs::Read rd = fs::read_shared(tmp, bytes);
    std::filesystem::remove(tmp, ec);
    if (rd != fs::Read::Ok)
        throw std::runtime_error("копия players.db не прочиталась");
    w.put_file(kPlayersFile, bytes, 0, {});
    // Построчные записи больше не описывают базу — снимаем их, чтобы состояние было одно.
    w.del_file(kPlayersSchema);
    for (const auto& [uid, head] : store.chars())
        w.del_char(uid);
}

}  // namespace

PlayersHealth check_players(const fs::Path& players_db) {
    std::error_code ec;
    if (!std::filesystem::exists(players_db, ec))
        return PlayersHealth::Missing;
    try {
        sql::Db db(players_db, sql::kReadWrite);
        db.busy_timeout(1000);
        if (db.scalar_text("PRAGMA quick_check(1)") != "ok")
            return PlayersHealth::Corrupt;
        return std::ranges::any_of(user_tables(db), is_players) ? PlayersHealth::Ok : PlayersHealth::Empty;
    } catch (const sql::Error& e) {
        return e.code == sql::kCorrupt || e.code == sql::kNotADb ? PlayersHealth::Corrupt : PlayersHealth::Busy;
    }
}

PlayersReport capture_players(Store& store, Store::Writer& w, const fs::Path& players_db, const fs::Path& tmp_dir,
                              int busy_ms) {
    PlayersReport r;
    std::error_code ec;
    if (!std::filesystem::exists(players_db, ec))
        return r;
    try {
        sql::Db engine(players_db, sql::kReadOnly);
        engine.busy_timeout(busy_ms);
        engine.exec("BEGIN");   // один снимок на всё чтение
        const std::vector<std::string> tables = user_tables(engine);
        if (!std::ranges::any_of(tables, is_players))
            return r;   // движок ещё не создал таблицы
        const std::string script = known_players_layout(engine, tables) ? schema_script(engine, tables) : std::string();
        if (!script.empty()) {
            r.mode = PlayersMode::Rows;
            capture_rows(engine, store, w, script, r);
        } else {
            r.mode = PlayersMode::File;
            capture_file(engine, store, w, tmp_dir);
        }
    } catch (const sql::Error& e) {
        if (!busy(e))
            throw;
        r.busy = true;   // движок пишет дольше нашего ожидания — в следующий раз
    }
    return r;
}

bool rebuild_players(Store& store, const State& state, const fs::Path& players_db) {
    if (const auto whole = state.files.find(std::string(kPlayersFile)); whole != state.files.end()) {
        remove_sidecars(players_db);
        fs::write_atomic(players_db, store.blob(whole->second));
        return true;
    }
    const auto schema = state.files.find(std::string(kPlayersSchema));
    if (schema == state.files.end())
        return false;
    const fs::Bytes script = store.blob(schema->second);

    fs::Path tmp = players_db;
    tmp += fs::to_wide(fs::kTmpSuffix);
    std::filesystem::create_directories(players_db.parent_path());
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    remove_sidecars(tmp);
    {
        sql::Db out(tmp, sql::kReadWrite | sql::kCreate);
        // Как у движка: страница 4 КБ, журнал delete, синхронная запись.
        out.exec("PRAGMA page_size = 4096; PRAGMA journal_mode = DELETE; PRAGMA synchronous = FULL");
        sql::Tx tx(out);
        out.exec(std::string_view(reinterpret_cast<const char*>(script.data()), script.size()));
        auto ins = out.prepare("INSERT INTO Players(Id, Alive, UID, Data) VALUES(?, ?, ?, ?)");
        fs::Bytes data;
        for (const State::Char& c : state.chars) {
            ins.bind(1, c.id).bind(2, c.alive).bind(3, c.uid);
            if (c.blob) {
                data = store.blob(c.blob);
                ins.bind(4, std::span<const std::byte>(data));
            } else {
                ins.bind(4, nullptr);
            }
            ins.step();
            ins.reset();
        }
        tx.commit();
    }
    remove_sidecars(players_db);
    std::filesystem::rename(tmp, players_db);   // на Windows заменяет существующий файл
    return true;
}

}  // namespace dpdb
