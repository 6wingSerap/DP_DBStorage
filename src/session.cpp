#include "session.hpp"

#include <windows.h>

#include <chrono>
#include <format>

namespace dpdb {

namespace {

// Имя замка — от пути базы; Global: сервер-служба и утилита администратора живут в
// разных сеансах Windows и должны видеть один и тот же объект.
std::wstring lock_name(const fs::Path& db_path) {
    std::string key = fs::to_utf8(std::filesystem::absolute(db_path).lexically_normal().wstring());
    for (char& c : key)
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
    const fs::Hash h = fs::sha256({reinterpret_cast<const std::byte*>(key.data()), key.size()});
    return L"Global\\DP_DBStorage_" + fs::to_wide(fs::hex(std::span(h).first(8)));
}

bool is_corruption(const sql::Error& e) {
    return e.code == sql::kCorrupt || e.code == sql::kNotADb;
}

enum class Players { Skip, Capture, Forget };

}  // namespace

std::string to_string(const Status& s) {
    std::string out = std::format(
        "поколение {}, файлов {}, персонажей {}, база {:.1f} МБ; проходов {}, новых поколений {}, "
        "последний проход {} мс (худший {} мс)",
        s.head_gen, s.files, s.chars, static_cast<double>(s.db_bytes) / (1024.0 * 1024.0), s.ticks, s.generations,
        s.last_ms, s.worst_ms);
    if (s.sync.deferred || s.sync.unfinished || s.sync.oversized || s.sync.failed)
        out += std::format("; отложено {}, оборванных слоёв {}, слишком больших {}, не прочитано {}", s.sync.deferred,
                           s.sync.unfinished, s.sync.oversized, s.sync.failed);
    if (s.players == PlayersMode::File)
        out += "; players.db незнакомой раскладки — хранится целиком";
    if (s.db_replaced)
        out += "; БАЗА БЫЛА ИСПОРЧЕНА и заменена новой";
    if (s.errors)
        out += std::format("; ошибок {}: {}", s.errors, s.last_error);
    return out;
}

Session::Session(Options opt) : opt_(std::move(opt)) {
    if (opt_.exclusive) {
        lock_ = CreateMutexW(nullptr, FALSE, lock_name(opt_.db_path).c_str());
        if (!lock_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (lock_)
                CloseHandle(lock_);
            throw std::runtime_error("база «" + fs::to_utf8(opt_.db_path.wstring()) +
                                     "» уже открыта другим процессом (работает сервер с тем же instanceId?)");
        }
    }
    try {
        open_store();
    } catch (...) {
        if (lock_)
            CloseHandle(lock_);
        throw;
    }
    status_.db_replaced = store_->was_quarantined();
}

void Session::open_store() {
    store_.emplace(opt_.db_path, opt_.log, /*may_replace=*/opt_.exclusive);
}

Session::~Session() {
    store_.reset();
    if (lock_)
        CloseHandle(lock_);
}

bool Session::in_use(const fs::Path& db_path) {
    const HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, lock_name(db_path).c_str());
    if (h)
        CloseHandle(h);
    return h || GetLastError() == ERROR_ACCESS_DENIED;
}

Status Session::status() const {
    std::lock_guard lk(mx_);
    return status_;
}

// Один проход «диск → база» одной транзакцией: файлы и (по запросу) игроки.
bool Session::pass(std::string_view kind, std::string_view note, bool deletions, std::int64_t settle_ms,
                   int players) {
    const auto t0 = std::chrono::steady_clock::now();
    SyncReport sr;
    PlayersReport pr;
    std::int64_t gen = 0;
    std::string players_error;
    {
        auto w = store_->begin(std::string(kind), opt_.build, std::string(note));
        sr = sync_files(*store_, w, opt_.storage_dir, {.deletions = deletions, .min_age = settle_ms * fs::kTicksPerMs},
                        skip_);
        if (players == static_cast<int>(Players::Forget)) {
            for (const auto& [uid, head] : store_->chars())
                w.del_char(uid);
            w.del_file(kPlayersSchema);
            w.del_file(kPlayersFile);
        } else if (players == static_cast<int>(Players::Capture)) {
            std::error_code ec;
            const auto size = static_cast<std::int64_t>(std::filesystem::file_size(players_db(), ec));
            const auto mtime = std::filesystem::last_write_time(players_db(), ec).time_since_epoch().count();
            try {
                pr = capture_players(*store_, w, players_db(), opt_.db_path.parent_path());
                if (!pr.busy) {
                    players_size_ = size;
                    players_mtime_ = mtime;
                    players_at_ = fs::unix_ms();
                }
            } catch (const std::exception& e) {
                // Нечитаемый players.db не должен стоить нам сохранения мира.
                players_error = std::format("players.db не прочитан: {}", e.what());
                opt_.log(players_error);
            }
        }
        gen = w.commit();
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const std::int64_t bytes = store_->used_bytes();
    std::lock_guard lk(mx_);
    status_.head_gen = store_->head_gen();
    status_.files = static_cast<std::int64_t>(store_->files().size());
    status_.chars = static_cast<std::int64_t>(store_->chars().size());
    status_.db_bytes = bytes;
    status_.sync = sr;
    if (pr.mode != PlayersMode::None)
        status_.players = pr.mode;
    ++status_.ticks;
    status_.generations += gen != 0;
    status_.last_ms = ms;
    status_.worst_ms = std::max(status_.worst_ms, static_cast<std::int64_t>(ms));
    if (!players_error.empty()) {
        ++status_.errors;
        status_.last_error = players_error;
    }
    return sr.pending() || pr.busy;
}

void Session::boot() {
    try {
        const PlayersHealth health = check_players(players_db());
        pass("boot", "", false, 0, static_cast<int>(health == PlayersHealth::Ok ? Players::Capture : Players::Skip));
        const RestoreReport rr =
            repair(*store_, opt_.storage_dir, skip_, health, opt_.db_path.parent_path() / "quarantine", opt_.log);

        // Обновление игры: всё, что записала старая сборка (включая сохранение при выходе,
        // только что забранное с диска), закрепляется — к нему можно вернуться всегда.
        const std::string prev = store_->meta("build");
        if (!opt_.build.empty() && prev != opt_.build) {
            if (!prev.empty() && store_->head_gen() > 0) {
                const std::string note = std::format("до обновления игры {} → {}", prev, opt_.build);
                store_->pin(store_->head_gen(), true, note);
                opt_.log(std::format("игра обновилась ({} → {}): поколение {} закреплено", prev, opt_.build,
                                     store_->head_gen()));
            }
            store_->set_meta("build", opt_.build);
        }
        store_->set_meta("storage_dir", fs::to_utf8(opt_.storage_dir.wstring()));

        std::lock_guard lk(mx_);
        status_.boot_repair = rr;
    } catch (const std::exception& e) {
        opt_.log(std::format("старт: {}", e.what()));
        std::lock_guard lk(mx_);
        ++status_.errors;
        status_.last_error = e.what();
    }
}

void Session::replace_broken_base(std::string_view why) {
    opt_.log(std::format("база испорчена ({}): откладываю и создаю новую, мир перезапишется с диска", why));
    store_.reset();
    Store::quarantine(opt_.db_path);
    skip_.clear();
    players_size_ = players_mtime_ = -1;
    open_store();
    std::lock_guard lk(mx_);
    status_.db_replaced = true;
}

bool Session::tick(bool force) {
    try {
        if (!store_)   // прошлая попытка пересоздать базу не удалась (нет места?) — пробуем снова
            open_store();
        bool due = force;
        if (!due && fs::unix_ms() - players_at_ >= opt_.players_period_ms) {
            std::error_code ec;
            const auto size = static_cast<std::int64_t>(std::filesystem::file_size(players_db(), ec));
            due = !ec && (size != players_size_ ||
                          std::filesystem::last_write_time(players_db(), ec).time_since_epoch().count() !=
                              players_mtime_);
        }
        return pass("cycle", "", true, opt_.settle_ms, static_cast<int>(due ? Players::Capture : Players::Skip));
    } catch (const std::exception& e) {
        const auto* se = dynamic_cast<const sql::Error*>(&e);
        try {
            if (se && is_corruption(*se) && opt_.exclusive)
                replace_broken_base(e.what());
            else
                opt_.log(std::format("проход: {}", e.what()));
        } catch (const std::exception& again) {
            opt_.log(std::format("база не пересоздана: {}", again.what()));
        }
        std::lock_guard lk(mx_);
        ++status_.errors;
        status_.last_error = e.what();
        return false;
    }
}

void Session::retention() {
    try {
        if (!store_)
            return;
        const RetentionReport r = store_->retain(fs::unix_ms(), opt_.keep_hours, opt_.max_db_bytes);
        if (r.versions || r.blobs)
            opt_.log(std::format("удержание: история до поколения {} схлопнута — версий {}, блобов {}, поколений {}",
                                 r.cutoff, r.versions, r.blobs, r.generations));
        const std::int64_t bytes = store_->used_bytes();
        std::lock_guard lk(mx_);
        status_.db_bytes = bytes;
    } catch (const std::exception& e) {
        opt_.log(std::format("удержание: {}", e.what()));
        std::lock_guard lk(mx_);
        ++status_.errors;
        status_.last_error = e.what();
    }
}

void Session::restore_generation(std::int64_t gen) {
    if (!store_->restorable(gen))
        throw std::runtime_error(std::format(
            "поколение {} недоступно для отката: его нет либо история до него уже схлопнута удержанием", gen));
    const auto players = [this] {
        std::error_code ec;
        return static_cast<int>(std::filesystem::exists(players_db(), ec) ? Players::Capture : Players::Forget);
    };
    pass("pre-restore", std::format("перед откатом к поколению {}", gen), true, 0, players());
    materialize(*store_, gen, opt_.storage_dir, opt_.log);
    skip_.clear();
    pass("restore", std::format("откат к поколению {}", gen), true, 0, players());
}

void Session::wipe(WipeScope scope) {
    const auto players = [this] {
        std::error_code ec;
        return static_cast<int>(std::filesystem::exists(players_db(), ec) ? Players::Capture : Players::Forget);
    };
    const char* what = scope == WipeScope::World ? "мир" : scope == WipeScope::Players ? "игроки" : "всё";
    pass("pre-wipe", std::format("перед вайпом ({})", what), true, 0, players());
    const int removed = dpdb::wipe(opt_.storage_dir, scope);
    skip_.clear();
    pass("wipe", std::format("вайп: {}", what), true, 0, players());
    opt_.log(std::format("вайп ({}): удалено файлов {}", what, removed));
}

}  // namespace dpdb
