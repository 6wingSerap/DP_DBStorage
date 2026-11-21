# CONTRIBUTING — DP_DBStorage

Работаем через PR, как в [KR_GRAFTED](https://github.com/KRdayzmodding/KR_GRAFTED):
`main` стабильный, прямой push закрыт, каждая правка — отдельной веткой и PR.

## Цикл

1. Ветка от `main`: `feat|fix|docs|test|refactor/<тема>`. Долгоживущих веток нет —
   влилась и удалена.
2. PR = одна мысль. Заголовок PR становится текстом squash-коммита.
3. Формат коммитов — [Conventional Commits](https://www.conventionalcommits.org)
   (`feat:`, `fix:`, `docs:`, `test:`, `refactor:`), описание на русском.
4. Тесты вперёд кода: новая логика без кейса в `tests/` не принимается. Вся логика
   живёт в `dpdb_core` и проверяется без игры на настоящем SQLite и настоящих файлах.

## Проверка перед PR

```bat
build.cmd tests                 :: gtest без игры (ядро graft не нужно)
build.cmd release               :: плагин + dpdb.exe
out\release\dpdb.exe bench      :: замеры; правка горячего пути — приложить «до/после»
powershell -File deploy.ps1 -Game "C:\DayZServer"   :: сборка + PBO + раскладка
```

Требуется: VS Build Tools 2022+ (C++), LLVM (clang-cl 17+), CMake 3.30+, Ninja, Mikero
MakePbo (только для `deploy.ps1`). Исходник SQLite, googletest и KR_GRAFTED качаются при
первой настройке. Без сети: `SQLITE_SOURCE_DIR` — каталог с `sqlite3.c` и `sqlite3.h`,
`GRAFT_SOURCE_DIR` — локальный клон KR_GRAFTED, `GTEST_SOURCE_DIR` — локальный googletest.

## Правила

- **Знание формата файлов движка — только в `src/format.cpp`** и только то, что нужно,
  чтобы отличить целый слой от оборванного. Содержимое слоёв и персонажей плагин не
  разбирает: это то, что ломает патч.
- **Факты о движке — из `docs/research/dayz-persistence.md`**, с номером сборки. Новый
  факт сначала попадает туда (адрес, сборка, как проверено), потом в код.
- **Главный поток игры** исполняет одну нашу функцию — перехват `CreateFileW` в
  `src/watch.cpp`. В ней нет выделений памяти, блокировок, диска и базы; любая правка
  там — с замером из `dpdb bench`.
- **Схема базы меняется только новой миграцией** (`kMigrations` в `src/store.cpp`),
  существующие не правятся. База с версией новее плагина не открывается.
- **Ничего не пишется на диск без сверки хеша и не под настоящим именем наполовину**
  (`fs::write_atomic`, `Store::blob`).
- **Движок базы — закреплённый исходник SQLite** (ссылка и хеш в `CMakeLists.txt`).
  Обновление: новая ссылка и хеш, версия в `tests/sqlite_test.cpp`, `dpdb bench` «до/после».
- **Строки в горячем пути пишутся обычным `INSERT`**, не `INSERT OR REPLACE` и не
  `ON CONFLICT`: при включённых внешних ключах оба в 16 раз медленнее (см. README,
  «Движок базы»). Новый оператор в проходе — с замером.
- Известный предел оформляется комментарием `shortcut: <предел>, <когда снимать>`.
- E2E-стенд: миссия-копия и отдельный `instanceId` — рабочий storage не трогаем.

## Структура

| Файл | Что в нём |
|---|---|
| `src/watch.cpp` | перехват `CreateFileW`: счётчик записей в storage, уступка движку |
| `src/sync.cpp` | диск → база: что изменилось, что оборвано, что удалено |
| `src/players.cpp` | `players.db` ↔ база: построчно или целым файлом |
| `src/restore.cpp` | база → диск: починка на старте, откат, вайп |
| `src/store.cpp` | схема, поколения, блобы, удержание, проверка |
| `src/format.cpp` | распознавание заголовка слоя CE |
| `src/session.cpp` | сценарии целиком: старт, проход, откат, вайп |
| `src/plugin.cpp` | graft-плагин: старт, фоновый поток, натив `DbStatus` |
| `src/cli.cpp` | `dpdb.exe` |
| `src/sqlite.cpp`, `src/fs.cpp`, `src/config.cpp` | обёртки SQLite C API, файлов, конфига |
