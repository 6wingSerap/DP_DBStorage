// players.db движка ↔ база плагина.
//
// players.db — SQLite 3.21 внутри exe, журнал в режиме delete; схема одинакова в сборках
// 1.29–1.30+ (docs/research/dayz-persistence.md, разд. 1.5): Players(Id, Alive, UID, Data)
// и служебная DB_Versions. Читаем её системным SQLite одной короткой read-транзакцией:
// движок на занятую базу отвечает ожиданием в своём фоновом потоке сохранения.
//
// Два режима, выбор автоматический:
//   Rows — раскладка Players знакома: каждый персонаж отдельной строкой (дедуп, история
//          на персонажа), схема и прочие таблицы — SQL-скриптом как есть;
//   File — раскладка незнакома (патч изменил таблицу): база целиком одним файлом.
#pragma once
#include <string>

#include "store.hpp"

namespace dpdb {

enum class PlayersHealth {
    Missing,   // файла нет
    Ok,
    Empty,     // файл есть, таблиц движка в нём нет
    Corrupt,   // не открывается как SQLite или не проходит проверку
    Busy,      // занят или недоступен — не трогать
};

// Проверка на старте, до движка: открытие на запись заодно откатывает незавершённую
// транзакцию по журналу, оставшемуся после краша.
PlayersHealth check_players(const fs::Path& players_db);

enum class PlayersMode { None, Rows, File };

struct PlayersReport {
    PlayersMode mode = PlayersMode::None;
    int stored = 0;   // новых версий персонажей
    int gone = 0;     // исчезнувших строк
    bool busy = false;
};

PlayersReport capture_players(Store& store, Store::Writer& w, const fs::Path& players_db, const fs::Path& tmp_dir,
                              int busy_ms = 2000);

// Собрать players.db из состояния поколения. false — игроков в состоянии нет.
// Старый журнал рядом с новым файлом удаляется: иначе SQLite накатил бы его на новую базу.
bool rebuild_players(Store& store, const State& state, const fs::Path& players_db);

}  // namespace dpdb
