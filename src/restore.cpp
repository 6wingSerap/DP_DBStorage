#include "restore.hpp"

#include <format>

#include "players.hpp"

namespace dpdb {

namespace {

fs::Path on_disk(const fs::Path& root, std::string_view rel) {
    return root / fs::to_wide(rel);
}

bool damaged_for_sure(const fs::Path& file, const HeadFile& head) {
    if (head.fmt < 0)
        return false;
    fs::Bytes disk;
    if (fs::read_shared(file, disk) != fs::Read::Ok)
        return false;
    const Facts f = inspect(disk);
    return f.shape == Shape::Blank || (f.shape == Shape::Unfinished && f.fmt == head.fmt && f.tag == head.tag);
}

}  // namespace

RestoreReport repair(Store& store, const fs::Path& root, const SkipCache& skip, PlayersHealth health,
                     const fs::Path& quarantine_dir, const Log& log) {
    RestoreReport r;
    std::error_code ec;
    for (const fs::Entry& e : fs::scan(root))
        if (e.rel.ends_with(fs::kTmpSuffix))
            std::filesystem::remove(on_disk(root, e.rel), ec);

    for (const auto& [path, head] : store.files()) {
        if (is_players_path(path))
            continue;
        const fs::Path file = on_disk(root, path);
        const bool missing = !std::filesystem::exists(file, ec);
        if (!missing) {
            const auto s = skip.find(path);
            if (s == skip.end() || !s->second.unfinished || !damaged_for_sure(file, head))
                continue;
        }
        try {
            // Слой проверяет себя сам (заголовок и хвост), поэтому пишется без сброса на
            // диск — иначе восстановление тысяч файлов упирается в fsync каждого. Файлы без
            // структуры сбрасываются: их повреждение после сбоя питания было бы не узнать.
            fs::write_atomic(file, store.blob(head.blob), /*durable=*/head.fmt < 0);
            ++(missing ? r.restored : r.repaired);
        } catch (const std::exception& e) {
            ++r.failed;
            log(std::format("восстановление «{}»: {}", path, e.what()));
        }
    }

    const fs::Path pdb = root / "players.db";
    const bool have = store.files().contains(std::string(kPlayersFile)) ||
                      store.files().contains(std::string(kPlayersSchema));
    if (have && health != PlayersHealth::Ok && health != PlayersHealth::Busy) {
        try {
            if (health == PlayersHealth::Corrupt) {
                std::filesystem::create_directories(quarantine_dir);
                const fs::Path aside = quarantine_dir / std::format("players.db.corrupt-{}", fs::unix_ms());
                std::filesystem::rename(pdb, aside);
                log(std::format("players.db испорчен, отложен как «{}»", fs::to_utf8(aside.wstring())));
            }
            r.players = rebuild_players(store, store.state(store.head_gen()), pdb);
        } catch (const std::exception& e) {
            ++r.failed;
            log(std::format("восстановление players.db: {}", e.what()));
        }
    }
    return r;
}

RestoreReport materialize(Store& store, std::int64_t gen, const fs::Path& dir, const Log& log) {
    RestoreReport r;
    const State st = store.state(gen);
    for (const auto& [path, blob] : st.files)
        store.blob(blob);   // сверка до первой записи на диск: бросит при порче
    for (const State::Char& c : st.chars)
        if (c.blob)
            store.blob(c.blob);

    for (const auto& [path, blob] : st.files) {
        if (is_players_path(path))
            continue;
        fs::write_atomic(on_disk(dir, path), store.blob(blob));
        ++r.restored;
    }
    std::error_code ec;
    for (const fs::Entry& e : fs::scan(dir))
        if (!is_players_path(e.rel) && !st.files.contains(e.rel) && std::filesystem::remove(on_disk(dir, e.rel), ec))
            ++r.removed;
    r.players = rebuild_players(store, st, dir / "players.db");
    if (!r.players)
        wipe(dir, WipeScope::Players);
    log(std::format("каталог приведён к поколению {}: файлов {}, удалено лишних {}, игроки {}", gen, r.restored,
                    r.removed, r.players ? "собраны" : "отсутствуют"));
    return r;
}

int wipe(const fs::Path& root, WipeScope scope) {
    int n = 0;
    std::error_code ec;
    for (const fs::Entry& e : fs::scan(root)) {
        const bool players = is_players_path(e.rel);
        if (scope == WipeScope::All || players == (scope == WipeScope::Players))
            n += std::filesystem::remove(on_disk(root, e.rel), ec);
    }
    return n;
}

}  // namespace dpdb
