// Плагин KR_GRAFTED: всё движко-зависимое — в этом файле, логика — в ядре (session.hpp).
//
// GRAFT_ON_LOAD (главный поток, движок ещё не создал потоков и не читал storage):
//   пути → конфиг → база → Session::boot() → наблюдатель CreateFileW → фоновый поток.
// После этого главный поток игры на плагин не тратит ничего, кроме проверки пути в
// перехвате открытий на запись (десятки наносекунд, см. `dpdb bench`).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

#include "graft/native.hpp"

#include "config.hpp"
#include "session.hpp"
#include "watch.hpp"

GRAFT_PLUGIN("DP_DBStorage", 2);

namespace {

using namespace dpdb;
using Clock = std::chrono::steady_clock;

// Сессия не разрушается никогда: при выходе процесса фоновый поток обрывается посреди
// работы, и деструктор трогал бы наполовину занятое соединение. Оборванную транзакцию
// SQLite откатит сам при следующем открытии, а недописанное заберёт следующий старт.
Session* g_session = nullptr;
int g_hook_slots = 0;
std::string g_idle_reason = "плагин ещё не загружен";
std::atomic<bool> g_hook_blind{false};

std::mutex g_log_mx;
fs::Path g_log_file;

void log_line(std::string_view msg) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::lock_guard lk(g_log_mx);
    std::ofstream f(g_log_file, std::ios::app | std::ios::binary);
    if (f)
        f << std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} {}\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                         st.wMinute, st.wSecond, msg);
}

fs::Path env_path(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    return n && n < std::size(buf) ? fs::Path(buf) : fs::Path();
}

Config load_config(const fs::Path& profile) {
    std::ifstream f(profile / "dp_dbstorage.cfg", std::ios::binary);
    if (!f)
        return {};   // конфига нет — значения по умолчанию рабочие
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    ConfigResult r = parse_config(text);
    if (!r.error.empty())
        log_line("dp_dbstorage.cfg: " + r.error + " — файл проигнорирован, действуют значения по умолчанию");
    return r.cfg;
}

// Фоновый поток: единственное место, где плагин работает с диском и базой после старта.
void worker(Config cfg) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto settle = std::chrono::milliseconds(g_session->options().settle_ms);
    const auto max_wait = std::chrono::seconds(10);   // непрерывный поток записей не откладывает проход вечно
    const auto rescan = std::chrono::seconds(cfg.rescan_sec);

    std::uint64_t seen = watch::epoch();
    auto now = Clock::now();
    auto changed_at = now, dirty_since = now, last_pass = now, last_retention = now - std::chrono::minutes(59);
    bool dirty = false, pending = false, foreign_reported = false;
    for (;;) {
        Sleep(250);
        now = Clock::now();
        if (const std::uint64_t e = watch::epoch(); e != seen) {
            seen = e;
            changed_at = now;
            if (!std::exchange(dirty, true))
                dirty_since = now;
        }
        // Движок пишет цикл сохранения пачкой файлов: ждём паузы, чтобы забрать его целиком.
        const bool quiet = dirty && (now - changed_at >= settle || now - dirty_since >= max_wait);
        if (quiet || pending || now - last_pass >= rescan) {
            const std::uint64_t before = g_session->status().generations;
            dirty = dirty && !quiet;
            pending = g_session->tick();
            last_pass = now;
            // Обход нашёл изменения, а перехват за всю жизнь процесса не увидел ни одной
            // записи: после патча движок пишет storage другим путём. Данные в безопасности
            // (обход работает), но задержка выросла до rescan_sec — сообщаем один раз.
            if (g_hook_slots && watch::epoch() == 0 && g_session->status().generations != before &&
                !g_hook_blind.exchange(true))
                log_line("ВНИМАНИЕ: изменения storage найдены обходом, перехват их не видел — вероятно, игра "
                         "обновилась и пишет файлы иначе. Плагин работает в режиме обхода, обновите его.");
        }
        if (!foreign_reported && watch::foreign() > 0) {
            foreign_reported = true;
            log_line("ВНИМАНИЕ: движок пишет в хранилище, которое плагин не охраняет: «" +
                     fs::to_utf8(watch::foreign_sample()) + "». Укажите storage_dir в dp_dbstorage.cfg.");
        }
        if (now - last_retention >= std::chrono::hours(1)) {
            last_retention = now;
            g_session->retention();
        }
    }
}

void at_load() {
    const auto t0 = Clock::now();
    wchar_t exe[2048];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    const fs::Path exe_path(std::wstring(exe, n));
    const ServerPaths paths = resolve_paths(exe_path.parent_path(), GetCommandLineW(), env_path(L"LOCALAPPDATA"));
    std::error_code ec;
    std::filesystem::create_directories(paths.profile, ec);
    g_log_file = paths.profile / "dp_dbstorage.log";

    const Config cfg = load_config(paths.profile);
    if (!cfg.enabled) {
        g_idle_reason = "выключен в конфиге (enabled = 0)";
        log_line(g_idle_reason);
        return;
    }
    const fs::Path storage = cfg.storage_dir.empty() ? paths.storage : fs::Path(fs::to_wide(cfg.storage_dir));
    if (storage.empty()) {
        g_idle_reason = "каталог storage не определён: " + paths.why;
        log_line(g_idle_reason + " — плагин не работает, сервер не затронут");
        return;
    }

    Options o;
    o.storage_dir = storage;
    o.db_path = (cfg.db_dir.empty() ? paths.profile / "dp_dbstorage" : fs::Path(fs::to_wide(cfg.db_dir))) /
                db_file_name(storage);
    o.build = file_version(exe_path);
    o.keep_hours = cfg.keep_hours;
    o.max_db_bytes = std::int64_t{cfg.max_db_mb} << 20;
    o.players_period_ms = std::int64_t{cfg.players_period_sec} * 1000;
    o.log = log_line;
    g_session = new Session(o);
    log_line(std::format("старт: игра {}, storage «{}», база «{}», SQLite {}", o.build.empty() ? "?" : o.build,
                         fs::to_utf8(storage.wstring()), fs::to_utf8(o.db_path.wstring()),
                         sql::version()));
    g_session->boot();

    watch::configure(storage, std::filesystem::current_path(ec));
    g_hook_slots = watch::install(GetModuleHandleW(nullptr));
    const Status st = g_session->status();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    log_line(std::format("готов за {} мс: {}; восстановлено файлов {}, заменено оборванных {}, игроки {}; {}", ms,
                         to_string(st), st.boot_repair.restored, st.boot_repair.repaired,
                         st.boot_repair.players ? "собраны из базы" : "на месте",
                         g_hook_slots ? std::format("перехват CreateFileW стоит ({} сл.)", g_hook_slots)
                                      : "перехвата нет — режим обхода по таймеру"));
    g_idle_reason.clear();
    std::thread(worker, cfg).detach();
}

// Сводка для администратора и скриптов мода.
std::string DbStatus() {
    if (!g_idle_reason.empty())
        return "DP_DBStorage не работает: " + g_idle_reason;
    return std::format("DP_DBStorage: {}; перехват: {}, записей замечено {}, уступок движку {}{}",
                       to_string(g_session->status()),
                       !g_hook_slots ? "нет (обход по таймеру)" : g_hook_blind ? "СЛЕП (обход по таймеру)" : "стоит",
                       watch::epoch(), watch::retries(),
                       watch::foreign() ? "; ВНИМАНИЕ: движок пишет в другой storage" : "");
}

}  // namespace

GRAFT_ON_LOAD() {
    try {
        at_load();
    } catch (const std::exception& e) {
        // Что бы ни случилось с плагином, сервер стартует как обычно — на своём storage.
        g_idle_reason = e.what();
        log_line(std::string("плагин не запущен: ") + e.what());
    }
}

GRAFT_BINDINGS("3_Game") {
    bind.global<&DbStatus>("DbStatus");
}
