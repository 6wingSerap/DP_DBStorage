// Конфиг плагина и пути сервера: где storage_<id>, профиль и база.
#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "fs.hpp"

namespace dpdb {

// $profile:dp_dbstorage.cfg — «ключ = значение», '#' — комментарий. Файла нет — всё по
// умолчанию, плагин работает из коробки.
struct Config {
    bool enabled = true;
    std::string db_dir;               // где лежит база; пусто — <профиль>\dp_dbstorage
    std::string storage_dir;          // каталог storage_<id>; пусто — определить самому
    int keep_hours = 24;              // глубина истории для отката
    int max_db_mb = 8192;             // потолок размера базы: при превышении история режется
    int players_period_sec = 30;      // не чаще — снимок players.db
    int rescan_sec = 30;              // страховочный обход каталога (и режим без перехвата)
};

struct ConfigResult {
    Config cfg;
    std::string error;   // пусто — успех; иначе cfg — значения по умолчанию
};
ConfigResult parse_config(std::string_view text);

// Значение ключа командной строки так, как его читает движок: «-ключ=значение» или
// «-ключ значение», регистр не важен, из повторов побеждает последний.
std::wstring arg(std::wstring_view cmdline, std::wstring_view key);

struct ServerCfg {
    std::string mission_template;
    std::optional<int> instance_id;
};
// template и instanceId из serverDZ.cfg; закомментированное не считается.
ServerCfg parse_server_cfg(std::string_view text);

struct ServerPaths {
    fs::Path exe_dir, profile, mission, storage;
    int instance_id = 0;
    bool resolved = false;   // storage определён
    std::string why;         // почему нет
};
// local_appdata — %LOCALAPPDATA%: профиль по умолчанию у игры там (папка DayZ).
ServerPaths resolve_paths(const fs::Path& exe_dir, std::wstring_view cmdline, const fs::Path& local_appdata);

// Имя файла базы по каталогу хранилища: «<миссия>.storage_<id>.db». Миссия в имени —
// чтобы смена карты при том же профиле не подсунула серверу чужой мир.
std::wstring db_file_name(const fs::Path& storage_dir);

// «1.30.164289» из ресурсов версии файла; пусто — прочитать не удалось.
std::string file_version(const fs::Path& file);

}  // namespace dpdb
