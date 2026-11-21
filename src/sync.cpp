#include "sync.hpp"

#include <set>

namespace dpdb {

bool is_players_path(std::string_view rel) {
    // players.db, его журнал/WAL и виртуальные записи вида «players.db:…»
    return rel.starts_with(kPlayersFile) &&
           (rel.size() == kPlayersFile.size() || rel[kPlayersFile.size()] == '-' || rel[kPlayersFile.size()] == ':');
}

SyncReport sync_files(Store& store, Store::Writer& w, const fs::Path& root, const SyncOptions& opt, SkipCache& skip) {
    SyncReport r;
    const std::int64_t now = opt.now ? opt.now : fs::now_ticks();
    std::set<std::string> seen;
    fs::Bytes data;
    for (const fs::Entry& e : fs::scan(root)) {
        if (is_players_path(e.rel) || e.rel.ends_with(fs::kTmpSuffix))
            continue;
        ++r.scanned;
        seen.insert(e.rel);
        if (const auto h = store.files().find(e.rel);
            h != store.files().end() && h->second.size == e.size && h->second.mtime == e.mtime)
            continue;
        if (const auto s = skip.find(e.rel); s != skip.end() && s->second.size == e.size && s->second.mtime == e.mtime) {
            ++(s->second.unfinished ? r.unfinished : r.oversized);
            continue;
        }
        // mtime из будущего (перевели часы) считаем старым, иначе файл не дождётся никогда.
        if (e.mtime <= now && now - e.mtime < opt.min_age) {
            ++r.deferred;
            continue;
        }
        switch (fs::read_shared(root / fs::to_wide(e.rel), data, opt.max_file)) {
            case fs::Read::Ok:
                break;
            case fs::Read::Busy:
                ++r.deferred;
                continue;
            case fs::Read::Missing:
                seen.erase(e.rel);   // исчез между обходом и чтением
                continue;
            case fs::Read::TooBig:
                skip[e.rel] = {e.size, e.mtime, false};
                ++r.oversized;
                continue;
            case fs::Read::Failed:
                ++r.failed;
                continue;
        }
        const Facts facts = inspect(data);
        // Пустышка на месте слоя — повреждение; пустой файл, слоем не бывший, — обычный файл.
        const auto head = store.files().find(e.rel);
        const bool was_layer = head != store.files().end() && head->second.fmt >= 0;
        if (facts.shape == Shape::Unfinished || (facts.shape == Shape::Blank && was_layer)) {
            skip[e.rel] = {e.size, e.mtime, true};
            ++r.unfinished;
            continue;
        }
        skip.erase(e.rel);
        if (w.put_file(e.rel, data, e.mtime, facts)) {
            ++r.stored;
            r.bytes += e.size;
        } else {
            ++r.same;
        }
    }
    std::erase_if(skip, [&](const auto& kv) { return !seen.contains(kv.first); });
    if (opt.deletions) {
        for (const auto& [path, head] : store.files())
            if (!seen.contains(path) && !is_players_path(path)) {
                w.del_file(path);
                ++r.deleted;
            }
    }
    return r;
}

}  // namespace dpdb
