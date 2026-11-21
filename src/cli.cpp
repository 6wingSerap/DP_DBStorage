// dpdb.exe — утилита администратора и бенчмарк. Работает той же сессией, что и плагин.
#include <windows.h>

#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <random>

#include "session.hpp"
#include "watch.hpp"

using namespace dpdb;

namespace {

const char* kUsage = R"(dpdb — база DP_DBStorage: состояние, проверка, откат, вайп, замеры

  dpdb status  <база>                    сводка
  dpdb gens    <база> [N]                последние N поколений (по умолчанию 20)
  dpdb verify  <база>                    целостность базы и каждого сохранённого файла
  dpdb export  <база> <поколение> <каталог>   выгрузить состояние поколения в каталог
  dpdb pin     <база> <поколение> [заметка]   закрепить: удержание не тронет
  dpdb unpin   <база> <поколение>

  Только при остановленном сервере (storage берётся из базы, если не задан):
  dpdb restore <база> <поколение> [storage]   откатить хранилище к поколению
  dpdb wipe    <база> world|players|all [storage]

  dpdb bench [storage] [--scale=N]       замер скорости на копии настоящего storage
                                         (без каталога — на синтетических данных)
)";

template <class F>
double ms(F&& f) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string when(std::int64_t unix_ms) {
    const std::int64_t ticks = (unix_ms + 11'644'473'600'000ll) * fs::kTicksPerMs;
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}, local{};
    SYSTEMTIME st{};
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond);
}

Options options(const fs::Path& db, bool owner, const fs::Path& storage = {}) {
    Options o;
    o.db_path = db;
    o.storage_dir = storage;
    o.exclusive = owner;
    o.settle_ms = 0;
    o.players_period_ms = 0;
    o.log = [](std::string_view s) { std::cout << "  " << s << "\n"; };
    return o;
}

void need_base(const fs::Path& db) {
    std::error_code ec;
    if (!std::filesystem::exists(db, ec))
        throw std::runtime_error("нет файла базы «" + fs::to_utf8(db.wstring()) + "»");
}

// Сессия-хозяин для офлайн-операций: storage из аргумента или из базы.
Session offline(const fs::Path& db, const std::wstring& storage_arg) {
    need_base(db);
    if (Session::in_use(db))
        throw std::runtime_error("база открыта работающим сервером: остановите его");
    fs::Path storage = storage_arg;
    if (storage.empty()) {
        Session peek(options(db, false));
        storage = fs::to_wide(peek.store().meta("storage_dir"));
    }
    if (storage.empty())
        throw std::runtime_error("каталог storage не записан в базе — укажите его аргументом");
    std::cout << "storage: " << fs::to_utf8(storage.wstring()) << "\n";
    return Session(options(db, true, storage));
}

int cmd_status(const fs::Path& db) {
    need_base(db);
    Session s(options(db, false));
    Store& st = s.store();
    const auto gens = st.generations(1);
    std::cout << std::format("движок SQLite {}\nпоколений: голова {}, всего {}, закреплено {}\n",
                             sql::version(), st.head_gen(),
                             st.db().scalar("SELECT count(*) FROM generations"),
                             st.db().scalar("SELECT count(*) FROM generations WHERE pinned = 1"));
    std::cout << std::format("файлов {}, персонажей {}, блобов {}, занято {:.1f} МБ\n", st.files().size(),
                             st.chars().size(), st.db().scalar("SELECT count(*) FROM blobs"),
                             static_cast<double>(st.used_bytes()) / 1048576.0);
    std::cout << std::format("storage: {}\nсборка игры: {}\nоткат возможен до поколения: {}\n", st.meta("storage_dir"),
                             st.meta("build"), st.meta("floor").empty() ? "1" : st.meta("floor"));
    if (!gens.empty())
        std::cout << "последняя запись: " << when(gens[0].ts) << "\n";
    std::cout << (Session::in_use(db) ? "сервер работает\n" : "сервер остановлен\n");
    return 0;
}

int cmd_gens(const fs::Path& db, int limit) {
    need_base(db);
    Session s(options(db, false));
    std::cout << "    №  время                вид          файлов  персонажей  сборка        заметка\n";
    for (const GenInfo& g : s.store().generations(limit))
        std::cout << std::format("{:5}{} {}  {:<11}  {:>6}  {:>10}  {:<12}  {}\n", g.gen, g.pinned ? '*' : ' ',
                                 when(g.ts), g.kind, g.files, g.chars, g.build, g.note);
    std::cout << "(* — закреплено)\n";
    return 0;
}

int cmd_verify(const fs::Path& db) {
    need_base(db);
    Session s(options(db, false));
    const VerifyReport v = s.store().verify();
    for (const std::string& p : v.problems)
        std::cout << "ПРОБЛЕМА: " << p << "\n";
    std::cout << std::format("проверено блобов: {}, проблем: {}\n", v.blobs, v.problems.size());
    return v.problems.empty() ? 0 : 2;
}

int cmd_export(const fs::Path& db, std::int64_t gen, const fs::Path& dir) {
    need_base(db);
    Session s(options(db, false));
    if (!s.store().restorable(gen))
        throw std::runtime_error(std::format("поколение {} недоступно", gen));
    std::error_code ec;
    if (std::filesystem::exists(dir, ec) && !std::filesystem::is_empty(dir, ec))
        throw std::runtime_error("каталог выгрузки должен быть пустым");
    materialize(s.store(), gen, dir, s.options().log);
    return 0;
}

// ── бенчмарк ─────────────────────────────────────────────────────────────────

struct Bench {
    fs::Path work, root, db;
    std::int64_t bytes = 0;
    int files = 0;

    ~Bench() {   // и при сорванном замере копия storage не остаётся во временном каталоге
        std::error_code ec;
        std::filesystem::remove_all(work, ec);
    }

    void row(std::string_view what, double took, std::int64_t volume = 0) const {
        std::cout << std::format("  {:<46} {:>9.1f} мс", what, took);
        if (volume > 0 && took > 0)
            std::cout << std::format("  {:>7.0f} МБ/с", static_cast<double>(volume) / 1048576.0 / (took / 1000.0));
        std::cout << "\n";
    }
};

// Копия настоящего storage, размноженная scale раз (в копиях меняется байт, чтобы
// дедуп не сделал работу за нас), либо синтетика: 12 слоёв по 4 МБ на копию.
void fill(Bench& b, const fs::Path& source, int scale) {
    std::mt19937_64 rng(1);
    for (int k = 0; k < scale; ++k) {
        const fs::Path dst = k == 0 ? b.root : b.root / std::format("copy_{:03}", k);
        if (source.empty()) {
            fs::Bytes payload(4 << 20);
            for (int i = 0; i < 12; ++i) {
                for (std::size_t j = 0; j + 8 <= payload.size(); j += 8) {
                    const std::uint64_t v = rng();
                    std::memcpy(payload.data() + j, &v, 8);
                }
                fs::write_atomic(dst / std::format("data/dynamic_{:03}.bin", i),
                                 make_layer("STORAGE_DYNAMIC", 39, 144, 1, payload));
            }
            continue;
        }
        for (const fs::Entry& e : fs::scan(source)) {
            fs::Bytes data;
            if (is_players_path(e.rel) || fs::read_shared(source / fs::to_wide(e.rel), data) != fs::Read::Ok)
                continue;
            if (k > 0 && data.size() > 128)
                data[data.size() / 2] ^= static_cast<std::byte>(k);
            fs::write_atomic(dst / fs::to_wide(e.rel), data);
        }
    }
    for (const fs::Entry& e : fs::scan(b.root)) {
        b.bytes += e.size;
        ++b.files;
    }
}

// Третья часть файлов переписывается новым содержимым — как один слот за цикл CE.
std::int64_t rewrite_third(const Bench& b) {
    std::int64_t changed = 0;
    int i = 0;
    for (const fs::Entry& e : fs::scan(b.root)) {
        fs::Bytes data;
        if (is_players_path(e.rel) || i++ % 3 != 0 || fs::read_shared(b.root / fs::to_wide(e.rel), data) != fs::Read::Ok || data.size() <= 128)
            continue;
        data[data.size() / 3] ^= std::byte{0x5a};
        fs::write_atomic(b.root / fs::to_wide(e.rel), data);
        changed += e.size;
    }
    return changed;
}

void make_players(const fs::Path& pdb, int count, int touch_every) {
    std::filesystem::create_directories(pdb.parent_path());
    sql::Db e(pdb, sql::kReadWrite | sql::kCreate);
    e.exec("CREATE TABLE IF NOT EXISTS DB_Versions(Id INTEGER PRIMARY KEY NOT NULL, Structure INTEGER NOT NULL DEFAULT 0);"
           "CREATE TABLE IF NOT EXISTS Players(Id INTEGER PRIMARY KEY NOT NULL, Alive INTEGER NOT NULL,"
           " UID CHAR(64) UNIQUE NOT NULL, Data BLOB NULL); INSERT OR IGNORE INTO DB_Versions VALUES(1, 0)");
    static std::mt19937_64 rng(7);
    fs::Bytes blob(2048);
    sql::Tx tx(e);
    auto q = e.prepare("INSERT OR REPLACE INTO Players(Id, Alive, UID, Data) VALUES(?, 1, ?, ?)");
    for (int i = 1; i <= count; i += touch_every) {
        for (std::size_t j = 0; j + 8 <= blob.size(); j += 8) {
            const std::uint64_t v = rng();
            std::memcpy(blob.data() + j, &v, 8);
        }
        q.bind(1, std::int64_t{i}).bind(2, std::format("uid-{:08}", i)).bind(3, std::span<const std::byte>(blob));
        q.step();
        q.reset();
    }
    tx.commit();
}

// Сессия не бросает наружу, а считает ошибки: без этой проверки замер на движке, который
// не справился, показал бы красивые цифры ни о чём.
void must_be_clean(Session& s) {
    if (const Status st = s.status(); st.errors)
        throw std::runtime_error("замер недействителен, движок не справился: " + st.last_error);
}

int cmd_bench(const fs::Path& source, int scale) {
    Bench b;
    b.work = std::filesystem::temp_directory_path() / std::format("dpdb_bench_{}", GetCurrentProcessId());
    b.root = b.work / "storage_1";
    b.db = b.work / "bench.db";
    std::filesystem::remove_all(b.work);
    std::cout << "подготовка данных…\n";
    fill(b, source, scale);
    const int players = 5000;
    make_players(b.root / "players.db", players, 1);
    std::cout << std::format("\nмир: {} файлов, {:.1f} МБ ({}); игроки: {} персонажей по 2 КБ\nдвижок SQLite {}\n\n",
                             b.files, static_cast<double>(b.bytes) / 1048576.0,
                             source.empty() ? "синтетика" : "копия настоящего storage", players,
                             sql::version());
    Options o = options(b.db, true, b.root);
    o.log = [](std::string_view) {};
    {
        Session s(o);
        b.row("первый захват (мир + все игроки)", ms([&] { s.boot(); }), b.bytes + players * 2048ll);
        b.row("холостой проход (ничего не менялось)", ms([&] { s.tick(); }));
        const std::int64_t changed = rewrite_third(b);
        b.row("цикл сохранения (переписана треть файлов)", ms([&] { s.tick(); }), changed);
        make_players(b.root / "players.db", players, 80);   // ~60 игроков онлайн сохранились
        b.row("сохранение игроков (изменилось ~60 из 5000)", ms([&] { s.tick(true); }));
        b.row("удержание (схлопнуть историю)", ms([&] { s.store().retain(fs::unix_ms(), 0, INT64_MAX); }));
        VerifyReport v;
        b.row("проверка базы (integrity + хеши всех блобов)", ms([&] { v = s.store().verify(); }), b.bytes);
        std::cout << std::format("  размер базы: {:.1f} МБ\n", static_cast<double>(s.store().used_bytes()) / 1048576.0);
        must_be_clean(s);
        if (!v.problems.empty())
            throw std::runtime_error("замер недействителен, проверка базы: " + v.problems.front());
    }
    std::filesystem::remove_all(b.root);
    {
        Session s(o);
        b.row("старт после потери storage (всё из базы)", ms([&] { s.boot(); }), b.bytes + players * 2048ll);
        s.tick();   // восстановленные файлы получили новый mtime — запомнить его
        must_be_clean(s);
        if (const Status st = s.status(); st.boot_repair.restored != b.files || !st.boot_repair.players)
            throw std::runtime_error(std::format("замер недействителен: из базы вернулось {} файлов из {}",
                                                 st.boot_repair.restored, b.files));
    }
    {
        Session s(o);
        b.row("обычный старт (диск совпадает с базой)", ms([&] { s.boot(); }));
        must_be_clean(s);
    }

    watch::configure(L"D:\\DayZServer\\mpmissions\\dayzOffline.chernarusplus\\storage_1", L"D:\\DayZServer");
    const wchar_t* inside = L"\\\\?\\D:\\DayZServer\\mpmissions\\dayzOffline.chernarusplus\\storage_1\\data\\dynamic_004.001";
    const wchar_t* outside = L"\\\\?\\D:\\DayZServer\\profiles\\DayZServer_x64_2026-11-17_18-00-00.RPT";
    constexpr int n = 5'000'000;
    volatile int sink = 0;
    const double in = ms([&] { for (int i = 0; i < n; ++i) sink = sink + static_cast<int>(watch::classify(inside)); });
    const double out = ms([&] { for (int i = 0; i < n; ++i) sink = sink + static_cast<int>(watch::classify(outside)); });
    std::cout << std::format("\nглавный поток игры (перехват CreateFileW, только открытия на запись):\n"
                             "  проверка пути внутри storage   {:>6.0f} нс\n"
                             "  проверка пути вне storage      {:>6.0f} нс\n",
                             in * 1e6 / n, out * 1e6 / n);
    return 0;
}

int run(std::vector<std::wstring> a) {
    int scale = 1;
    std::erase_if(a, [&](const std::wstring& s) {
        if (s.starts_with(L"--scale="))
            scale = std::max(1, _wtoi(s.c_str() + 8));
        else
            return false;
        return true;
    });
    const auto at = [&](std::size_t i) { return i < a.size() ? a[i] : std::wstring(); };
    const std::wstring cmd = at(0);
    if (cmd == L"bench")
        return cmd_bench(at(1), scale);
    const fs::Path db = at(1);
    if (db.empty())
        return std::cout << kUsage, 1;
    if (cmd == L"status")
        return cmd_status(db);
    if (cmd == L"gens")
        return cmd_gens(db, at(2).empty() ? 20 : _wtoi(at(2).c_str()));
    if (cmd == L"verify")
        return cmd_verify(db);
    if (cmd == L"export" && !at(3).empty())
        return cmd_export(db, _wtoi64(at(2).c_str()), at(3));
    if ((cmd == L"pin" || cmd == L"unpin") && !at(2).empty()) {
        need_base(db);
        Session s(options(db, false));
        s.store().pin(_wtoi64(at(2).c_str()), cmd == L"pin", fs::to_utf8(at(3)));
        return 0;
    }
    if (cmd == L"restore" && !at(2).empty()) {
        Session s = offline(db, at(3));
        s.restore_generation(_wtoi64(at(2).c_str()));
        std::cout << "готово: хранилище соответствует поколению " << fs::to_utf8(at(2)) << "\n";
        return 0;
    }
    if (cmd == L"wipe" && (at(2) == L"world" || at(2) == L"players" || at(2) == L"all")) {
        Session s = offline(db, at(3));
        s.wipe(at(2) == L"world" ? WipeScope::World : at(2) == L"players" ? WipeScope::Players : WipeScope::All);
        std::cout << "готово; прежнее состояние осталось в истории (dpdb gens)\n";
        return 0;
    }
    return std::cout << kUsage, 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        return run({argv + 1, argv + argc});
    } catch (const std::exception& e) {
        std::cout << "ошибка: " << e.what() << "\n";
        return 1;
    }
}
