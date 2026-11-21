#include "config.hpp"

#include <windows.h>

#include <charconv>
#include <format>
#include <fstream>
#include <vector>

namespace dpdb {

namespace {

std::string_view trim(std::string_view s) {
    const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!s.empty() && ws(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && ws(s.back()))
        s.remove_suffix(1);
    return s;
}

bool to_int(std::string_view s, int& out) {
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
}

wchar_t lower(wchar_t c) {
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c + 32) : c;
}

bool iequals(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
        return lower(x) == lower(y);
    });
}

// Слова командной строки: пробелы внутри кавычек не делят, сами кавычки снимаются.
std::vector<std::wstring> tokens(std::wstring_view cmd) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool quoted = false, any = false;
    for (const wchar_t c : cmd) {
        if (c == L'"') {
            quoted = !quoted;
            any = true;
        } else if ((c == L' ' || c == L'\t') && !quoted) {
            if (any)
                out.push_back(std::move(cur));
            cur.clear();
            any = false;
        } else {
            cur += c;
            any = true;
        }
    }
    if (any)
        out.push_back(std::move(cur));
    return out;
}

fs::Path under(const fs::Path& base, const std::wstring& value) {
    const fs::Path p(value);
    return (p.is_absolute() ? p : base / p).lexically_normal();
}

std::string strip_comments(std::string_view text) {
    std::string out;
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!quoted && c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n')
                ++i;
            out += '\n';
        } else if (!quoted && c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 1;
            out += ' ';
        } else {
            if (c == '"')
                quoted = !quoted;
            out += c;
        }
    }
    return out;
}

// Текст после «key =» (key — целое слово); пусто — ключа нет.
std::string_view value_of(std::string_view text, std::string_view key) {
    const auto word = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    for (std::size_t pos = text.find(key); pos != std::string_view::npos; pos = text.find(key, pos + 1)) {
        if (pos > 0 && word(text[pos - 1]))
            continue;
        std::string_view rest = text.substr(pos + key.size());
        while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
            rest.remove_prefix(1);
        if (rest.empty() || rest.front() != '=')
            continue;
        rest.remove_prefix(1);
        return rest.substr(0, rest.find_first_of(";\n"));
    }
    return {};
}

}  // namespace

ConfigResult parse_config(std::string_view text) {
    ConfigResult r;
    Config cfg;
    if (text.starts_with("\xEF\xBB\xBF"))
        text.remove_prefix(3);
    const auto fail = [&](std::string msg) {
        r.error = std::move(msg);
        return r;
    };
    while (!text.empty()) {
        std::string_view line = text.substr(0, text.find('\n'));
        text.remove_prefix(std::min(text.size(), line.size() + 1));
        line = trim(line.substr(0, line.find('#')));
        if (line.empty())
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return fail(std::format("строка без «=»: «{}»", line));
        const std::string_view key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
        const auto number = [&](int& out, int min) {
            return to_int(val, out) && out >= min;
        };
        bool ok = true;
        int flag = 0;
        if (key == "enabled") {
            ok = number(flag, 0) && flag <= 1;
            cfg.enabled = flag == 1;
        } else if (key == "db_dir") {
            cfg.db_dir = val;
        } else if (key == "storage_dir") {
            cfg.storage_dir = val;
        } else if (key == "keep_hours") {
            ok = number(cfg.keep_hours, 0);
        } else if (key == "max_db_mb") {
            ok = number(cfg.max_db_mb, 1);
        } else if (key == "players_period_sec") {
            ok = number(cfg.players_period_sec, 1);
        } else if (key == "rescan_sec") {
            ok = number(cfg.rescan_sec, 1);
        } else {
            return fail(std::format("неизвестный ключ «{}»", key));
        }
        if (!ok)
            return fail(std::format("недопустимое значение «{}» у ключа «{}»", val, key));
    }
    r.cfg = cfg;
    return r;
}

std::wstring arg(std::wstring_view cmdline, std::wstring_view key) {
    std::wstring found;
    const std::vector<std::wstring> words = tokens(cmdline);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::wstring_view w = words[i];
        if (w.size() < 2 || w.front() != L'-')
            continue;
        const std::size_t eq = w.find(L'=');
        if (!iequals(w.substr(1, eq == std::wstring_view::npos ? eq : eq - 1), key))
            continue;
        if (eq != std::wstring_view::npos)
            found = w.substr(eq + 1);
        else if (i + 1 < words.size() && !words[i + 1].starts_with(L'-'))
            found = words[i + 1];
        else
            found.clear();
    }
    return found;
}

ServerCfg parse_server_cfg(std::string_view text) {
    ServerCfg c;
    const std::string clean = strip_comments(text);
    if (const std::string_view v = value_of(clean, "template"); !v.empty()) {
        const std::size_t a = v.find('"'), b = v.find('"', a + 1);
        if (a != std::string_view::npos && b != std::string_view::npos)
            c.mission_template = v.substr(a + 1, b - a - 1);
    }
    if (int id = 0; to_int(trim(value_of(clean, "instanceId")), id))
        c.instance_id = id;
    return c;
}

std::wstring db_file_name(const fs::Path& storage_dir) {
    fs::Path dir = storage_dir.lexically_normal();
    if (!dir.has_filename())   // путь с хвостовым разделителем
        dir = dir.parent_path();
    return dir.parent_path().filename().wstring() + L"." + dir.filename().wstring() + L".db";
}

ServerPaths resolve_paths(const fs::Path& exe_dir, std::wstring_view cmdline, const fs::Path& local_appdata) {
    ServerPaths p;
    p.exe_dir = exe_dir;
    const std::wstring profiles = arg(cmdline, L"profiles");
    p.profile = profiles.empty() ? local_appdata / "DayZ" : under(exe_dir, profiles);

    const std::wstring cfg_arg = arg(cmdline, L"config");
    const fs::Path cfg_path = under(exe_dir, cfg_arg.empty() ? L"serverDZ.cfg" : cfg_arg);
    std::ifstream f(cfg_path, std::ios::binary);
    if (!f) {
        p.why = "не прочитать конфиг сервера «" + fs::to_utf8(cfg_path.wstring()) + "»";
        return p;
    }
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const ServerCfg cfg = parse_server_cfg(text);
    if (!cfg.instance_id) {
        p.why = "в конфиге сервера нет instanceId";
        return p;
    }
    p.instance_id = *cfg.instance_id;

    if (const std::wstring mission = arg(cmdline, L"mission"); !mission.empty())
        p.mission = under(exe_dir, mission);
    else if (!cfg.mission_template.empty())
        p.mission = exe_dir / "mpmissions" / fs::to_wide(cfg.mission_template);
    else {
        p.why = "миссия не задана: нет ни -mission=, ни template в конфиге сервера";
        return p;
    }
    if (!arg(cmdline, L"storage").empty()) {
        p.why = "задан свой корень хранилища (-storage=): укажите storage_dir в dp_dbstorage.cfg";
        return p;
    }
    p.storage = p.mission / (L"storage_" + std::to_wstring(p.instance_id));
    p.resolved = true;
    return p;
}

std::string file_version(const fs::Path& file) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
    if (size == 0)
        return {};
    std::vector<std::byte> buf(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(file.c_str(), 0, size, buf.data()) ||
        !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info)
        return {};
    // Номер сборки DayZ не влезает в 16 бит и разложен на два поля: 1.30.164.289 = 1.30.164289.
    const unsigned a = HIWORD(info->dwFileVersionMS), b = LOWORD(info->dwFileVersionMS);
    const unsigned c = HIWORD(info->dwFileVersionLS), d = LOWORD(info->dwFileVersionLS);
    return std::format("{}.{}.{}{:03}", a, b, c, d);
}

}  // namespace dpdb
