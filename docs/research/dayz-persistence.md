# DayZ Server: персистентность изнутри (REA-разбор под Turso-бэкенд)

Цель: `F:/SteamLibrary/steamapps/common/DayZServer/DayZServer_x64.exe` (сборка 2026-11-04,
sha256 `987fbc47bab1dab37f74d7f700aa2e5b6af936a7181a62f5ba9f0090aebba805`, image base `0x140000000`).
Сверка: `DayZServer_x64_130_164078.exe` (1.30), `DayZServer_x64_129_163709.exe` (1.29) — только точечно.

Инструменты: REA (провайдер IDA, headless: строки, xref, декомпиляция) + локальные Python/capstone/pefile
скрипты для сырых байт (индекс RIP-ссылок, таблицы указателей, IAT). Плюс read-only сверка с живыми файлами
`mpmissions/*/storage_*` и RPT-логами сервера.

Метки: **CONFIRMED** — видно в коде/байтах/живых данных; **INFERRED** — вывод из косвенных признаков;
**UNKNOWN** — не установлено (указано, что пробовали). Все адреса — текущая сборка, если не сказано иное;
адреса после патча меняются, якоря (строки/RTTI/IAT) — нет.

---

## TL;DR

1. **SQLite видит патчи IAT.** `aSyscall[].pCurrent` указывают не на сырые функции, а на thunk'и
   `jmp [IAT]` внутри образа. Каждый вызов SQLite перечитывает слот IAT → патч IAT в рантайме
   (до или после инициализации SQLite) действует и на SQLite. **CONFIRMED** во всех трёх сборках.
2. **Дыра в текущем перехвате:** SQLite проверяет существование файлов (hot-journal, `winAccess`, `winIsDir`)
   через `GetFileAttributesExW` — этого слота в списке перехвата нет. `SetFileAttributesW` движок не импортирует вовсе.
3. **.bin пишутся «на месте»:** `CreateFileW(\\?\<abs>, GENERIC_WRITE, share=0, CREATE_ALWAYS)` прямо в
   финальный слот (`.bin`/`.001`/`.002`). Временного файла нет, rename нет, FlushFileBuffers нет.
   Защита от обрыва — это ротация слотов + `stamp` + `eofOff`/`_EOF`, выставляемые последними.
4. **Версии:** в заголовке два номера. `ver` — версия формата файла, пара (min,cur) зашита в дескриптор
   каждого типа (например dynamic 11..39). `saveVer` — скриптовый `GAME_STORAGE_VERSION`
   (`StorageVersion()` → `g_Game+0xE0`; при чтении → `g_Game+0xDC` = `LoadVersion()`). CRC нет.
5. **В 1.29 файловый слой был другим** (A-API: `CreateDirectoryA`, `FindFirstFileA`, `RemoveDirectoryA`,
   без `MoveFileExW`). Перехват только W-слотов на 1.29 был бы неполным.

---

## Q1. Встроенный SQLite

### 1.1 Версия, статическая линковка — CONFIRMED

| Что | Адрес | Значение |
|---|---|---|
| `sqlite3_version` | `0x14125e828` | `"3.21.0"` |
| `sqlite3_sourceid` | `0x1412680e0` | `"2017-10-24 18:55:49 1a584e499906b5c87ec7d43d4abce641fdf017c42125b083109bc77c4de4alt2"` |

Суффикс `alt2` означает модифицированный исходник: последние символы хэша заменены, upstream так делает
при правке исходника. Экспортов `sqlite3_*` нет, импорта `sqlite3.dll` нет → линковка статическая.
Строки VFS: `win32` `0x141263698`, `win32-longpath` `0x1412636a0`, `win32-none` `0x1412636b0`,
`win32-longpath-none` `0x1412636c0`.

### 1.2 VFS — CONFIRMED

- `winOsInit` = `0x140c492a0`. Он регистрирует четыре `sqlite3_vfs`: `0x141659370` (win32),
  `0x141659440` (win32-longpath), `0x1416594f0` (win32-none), `0x1416595a0` (win32-longpath-none).
  Ссылки: `lea r8,[rip+…]` по адресам `0x140c49322` / `0x140c493c3` / `0x140c49481` / `0x140c4953f`.
- `winVfs` (`0x141659370`): `iVersion=3`, `szOsFile=104`, `mxPathname=1040`.
  Методы: `xOpen=0x140bceed0`, `xDelete=0x140bcf2f0`, `xAccess=0x140bcffb0`, `xFullPathname=0x140bd0170`, …
- Движок открывает БД с VFS по умолчанию, то есть `"win32"` (без `\\?\`-логики long-path VFS).

### 1.3 aSyscall и IAT — КЛЮЧЕВОЙ ВОПРОС — CONFIRMED

Как найти: строка `"AreFileApisANSI"` (`0x141262d88`) → единственный указатель на неё в `.data`
→ это `aSyscall[0].zName`. Таблица: `aSyscall` @ `0x1416587f0`, 81 запись по 24 байта
`{zName, pCurrent, pDefault}`. Скрипт `asys2.py` разбирает байты по адресу `pCurrent`:

```
idx  zName                 pCurrent      bytes@pCurrent        -> IAT slot   (import)
 3   CloseHandle           0x14101732d   ff 25 65 90 06 00     jmp [0x141080398] KERNEL32!CloseHandle
 5   CreateFileW           0x141017387   ff 25 8b 91 06 00     jmp [0x141080518] KERNEL32!CreateFileW
10   DeleteFileW           0x14101738d   ff 25 8d 91 06 00     jmp [0x141080520] KERNEL32!DeleteFileW
13   FlushFileBuffers      0x1410173cf   ff 25 03 93 06 00     jmp [0x1410806d8]
21   GetFileAttributesW    0x141017393   ff 25 a7 91 06 00     jmp [0x141080540]
22   GetFileAttributesExW  0x141017429   ff 25 41 93 06 00     jmp [0x141080770]
25   GetFullPathNameW      0x14101745f   ff 25 7b 8e 06 00     jmp [0x1410802e0]
48   LockFileEx            0x1410173ed   …                     jmp [0x141080720]
52   ReadFile              0x141017399   …                     jmp [0x141080550]
61   WriteFile             0x1410173a5   …                     jmp [0x141080578]
(pDefault = 0 у всех; CreateFile2/SetFilePointerEx/GetFileInformationByHandleEx/… = NULL)
```

Вывод: компилятор не мог положить в статический инициализатор значение из IAT, поэтому подставил
адрес thunk'а `jmp qword ptr [IAT]`. **Каждый вызов SQLite читает слот IAT заново.** Значит, патч
слота главного образа в любой момент — до или после `sqlite3_initialize` — перехватывает и SQLite.
`xSetSystemCall` движок не использует. На `aSyscall` ссылаются только `winSetSystemCall`,
`winGetSystemCall` и `winNextSystemCall` (`0x140bcac20`, `0x140bcb1f0`, `0x140bcb260`). Прямых
callers у них нет: они доступны лишь через указатели в `sqlite3_vfs`. Таблица лежит в `.data`, то есть writable; это альтернативный путь
перехвата, который не трогает IAT.

Какие слоты дёргает SQLite (места вызова через `aSyscall`, скрипт `asys2.py`/`q.py`):

| syscall | функция SQLite (callsite) | зачем |
|---|---|---|
| CreateFileW | winOpen `0x140bcf08d` | open db / `-journal` |
| DeleteFileW | winDelete `0x140bcf34d` | удаление `-journal` (journal_mode=DELETE) |
| GetFileAttributesW | winDelete `0x140bcf32d`, `0x140bcf397` | проверка удаления |
| **GetFileAttributesExW** | winOpen `0x140bcefca`, winAccess `0x140bd003a` | `winIsDir`, **детект hot-journal** |
| GetFullPathNameW | winFullPathname `0x140bd0235`, `0x140bd02b8` | абсолютизация пути |
| ReadFile / WriteFile | `0x140bcc7c0` / `0x140bcce6c` | |
| FlushFileBuffers | winSync `0x140bcd30e` | fsync |
| LockFileEx / UnlockFileEx | `0x140bcda18` … | блокировки |

`GetFileAttributesExW` (слот `0x141080770`) из кода самого движка не вызывается ни разу
(0 прямых callers). Его зовёт только SQLite через thunk. **Риск для текущего плагина (INFERRED):**
CreateFileW/DeleteFileW для `players.db-journal` уводятся в кэш, а `winAccess` смотрит по
исходному пути. Тогда после краха hot-journal не будет найден и откат не выполнится.

### 1.4 Как движок открывает players.db — CONFIRMED

```
0x140bd24c0  sqlite3_open:  xor r9d,r9d          ; zVfs = NULL (default "win32")
0x140bd24c3                 lea r8d,[r9+6]       ; flags = 6 = READWRITE|CREATE
0x140bd24c7                 jmp 0x140bf9740      ; openDatabase
```

- У `sqlite3_open` единственный caller — `0x1408fa410` (offlineDB open). Путь собирается так:
  `sub_1406D0870(&p, "players.db")` = `sprintf("%s%s", qword_141756308 /*storage root*/, "players.db")`,
  затем `sub_140AF9A30` меняет `/` на `\`. Корень абсолютный, по логу:
  `[StorageDirs] :: Selected storage directory: F:\…\dayzOffline.chernarusplus\storage_1\`.
- Обёртки SQLite API (по сигнатурам использования, INFERRED имена):
  - `exec=0x140bd09d0`, `prepare` (обёртка `0x1408fa500`), `reset=0x140bd40b0`
  - `bind_int=0x140bd2e20`, `bind_blob=0x140bd2d40`, `bind_text=0x140bd2f00`, `step=0x140bd34b0`
  - `column_int=0x140bd3ac0`, `column_blob=0x140bd39b0`, `column_bytes=0x140bd3df0`
  - `errmsg=0x140bd2880`, `finalize=0x140bd3fa0`, `close=0x140bd09b0`, `free=0x140bd1ee0`

### 1.5 SQL движка — CONFIRMED (строки + xref в кластере `0x1408f9970..0x1408faaa0`)

Инициализация `0x1408fa1c0` (вызывается из `ADBOffline` vt+0x10 `0x1408fb850`):

```c
sub_1408FA410(a1);                                  // open players.db
exec("SELECT name FROM sqlite_master WHERE type='table' AND name='DB_Versions';",
     cb=sub_1408F9960 /* если таблица есть -> a1[24]=0 (миграция не нужна) */);
exec("CREATE TABLE IF NOT EXISTS DB_Versions ( Id INTEGER PRIMARY KEY NOT NULL, Structure INTEGER NOT NULL DEFAULT 0 );");
prepare("INSERT OR REPLACE INTO DB_Versions (Id, Structure) VALUES (?, ?)");
bind_int(1, 1); bind_int(2, 0);                      // Id=1, Structure=0
step();  /* SQLITE_BUSY(5) -> Sleep(50), до 5 попыток */
exec("CREATE TABLE IF NOT EXISTS Players ( Id INTEGER PRIMARY KEY NOT NULL, Alive INTEGER NOT NULL, UID CHAR(64) UNIQUE NOT NULL, Data BLOB NULL );");
prepare(a1+8,  "UPDATE Players SET Alive=1, Data=? WHERE Id=?");   // постоянный stmt сохранения
prepare(a1+16, "UPDATE Players SET Alive=0 WHERE Id=?");           // постоянный stmt смерти
if (a1[24]) sub_1408FAAA0(a1);                      // миграция только если DB_Versions не было
```

- **`Structure` всегда 0**, `Id` всегда 1. В живой БД `[(1, 0)]` — CONFIRMED.
- Миграция `0x1408faaa0` выполняется только для старых БД без `DB_Versions`:
  `BEGIN TRANSACTION;CREATE TABLE Players_Backup (…);INSERT INTO Players_Backup SELECT Id, Alive, REPLACE( REPLACE( UID, '+', '-'), '/', '_' ), Data FROM Players;DROP TABLE Players;ALTER TABLE Players_Backup RENAME TO Players;COMMIT;VACUUM;`
  Затем каждый blob конвертируется (`SELECT Id, Alive FROM Players` → `SELECT Data … WHERE Id=?` →
  конвертер `0x1408f9970` → `UPDATE Players SET Data=?, Alive=1 WHERE Id=?`).
- Рантайм-SQL: `INSERT INTO Players (Alive, UID, Data) VALUES (0,?,NULL)`, `SELECT Id, Alive FROM Players WHERE UID=?`,
  `SELECT Alive, Data FROM Players WHERE Id=?`.
- **PRAGMA движок не выполняет** (INFERRED). Единственная строка `"PRAGMA "` (`0x141263cf0`) —
  внутренняя для SQLite. Среди SQL-строк offlineDB PRAGMA нет. Значит, `journal_mode=DELETE` и
  `synchronous` остаются по умолчанию для сборки 3.21.
- Явных транзакций в рантайме нет: каждый UPDATE — autocommit. На каждое сохранение SQLite создаёт
  `players.db-journal`, пишет его, делает fsync и удаляет (INFERRED из journal_mode=DELETE и слотов aSyscall).

---

## Q2. Игроки (players.db)

### 2.1 Классы и потоки — CONFIRMED (RTTI)

- `.?AVADBOffline@@` → vtable `0x1411caff8` (`.?AVADBOnline@@` → `0x1411ca068` — онлайн-хайв, в оффлайн не используется):

| слот | адрес | что |
|---|---|---|
| +0x10 | `0x1408fb850` | Init: стартует поток `"ADBOffline::SaveThread"` (тело `0x1408fe7e0`) и вызывает offlineDB init `0x1408fa1c0` |
| +0x18 | `0x1408fb950` | загрузка CE-XML (types/events/globals/messages) |
| +0x28 | `0x1408fb4e0` | `ADBOffline::Future1stPass` — поиск/создание по UID, поток `"ADBOffline::1stPass::Callback"` → `0x1408fb300` → `0x1408fa790` |
| +0x30 | `0x1408fb360` | `ADBOffline::Future2ndPass` — загрузка blob по Id, поток `"ADBOffline::2ndPass::Callback"` → `0x1408fb330` → `0x1408fa630` |
| +0x38 | `0x1408fb620` | **enqueue save**: опкод 0, charId, blob |
| +0x40 | `0x1408fb4a0` | **enqueue kill**: опкод 1, charId |
| +0x48 | `0x14015c540` | общий nullsub — `CharacterExit` в оффлайне ничего не делает |

- Скриптовые нативы (класс `CentralHive`, регистрация `0x1408f24e0`): `CharacterSave` → `0x1408f41e0`
  (`hive->vt[+0x38]`), `CharacterKill` → `0x1408f3d90`, `CharacterExit` → `0x1408f38c0` (`hive->vt[+0x48]`).

### 2.2 Когда сохраняется — CONFIRMED (скрипты) / INFERRED (периодика)

- Скрипт `PlayerBase.Save()` (PlayerBase.c:8062) вызывает `GetHive().CharacterSave(this)`, если игрок
  ALIVE и не связан.
- Дисконнект: `missionServer.c:710` `PlayerDisconnected` → `player.Save(); GetHive().CharacterExit(player);`.
- Смерть: PlayerBase.c:1229 → `CharacterKill`. Натив `0x1408f3d90` не декомпилировался;
  связь «натив → `vt[+0x40]`» INFERRED по аналогии с Save и Exit. Опкод 1 →
  `UPDATE Players SET Alive=0 WHERE Id=?` — CONFIRMED.
- Периодически: CE-job `"save_players"` (маска `0x8000`, float-параметр `120.0`), обработчик `0x1406aa940`.
  Если включена персистентность игроков (`CEroot+176` флаг `&4`), вызывается
  `sub_140897660(sub_140898B80())`. **INFERRED:** обходит игроков и сохраняет их. Тело не
  декомпилировалось; что именно значит 120.0 (интервал в секундах или бюджет), точно не установлено.

### 2.3 Путь записи — CONFIRMED

Нативный вызов в главном потоке сериализует персонажа в кольцевой буфер (4 буфера, `0x1408fe7e0`).
Поток SaveThread разбирает поток команд (`0x1408fe270`):

```c
op = next();                       // 0 = save, 1 = kill
if (op == 1) kill(id)              // 0x1408fa5c0 -> stmt "UPDATE Players SET Alive=0 WHERE Id=?"
else { id=u32; len=u32;
       if (len > remaining) log("Character saving stream corrupted, skipping and discarding current data.");
       save(id, data, len); }      // 0x1408fa990: reset; bind_blob(1,data,len); bind_int(2,id); step
```

Для save и kill используется одно постоянное соединение (открыто при init). Create и Load открывают
**новое соединение на каждый вызов** (`0x1408fa410` … `close 0x140bd09b0`). На `SQLITE_BUSY`
выполняется `Sleep(50)` в бесконечном цикле.

### 2.4 Путь загрузки при подключении — CONFIRMED (код) / INFERRED (порядок)

1stPass `0x1408fa790`: `SELECT Id, Alive FROM Players WHERE UID=?`. Если записи нет —
`INSERT … VALUES (0,?,NULL)` и Id = last_insert_rowid (`0x1402301b0`). 2ndPass `0x1408fa630`:
`SELECT Alive, Data FROM Players WHERE Id=?` → `malloc+memcpy` blob. Дальше идёт десериализация
персонажа (обратная к 2.5; подробно не разбиралась).

### 2.5 Раскладка Data blob — CONFIRMED (код `0x1405cabe0` + `0x1405caa70`, сверено с живой БД)

```
off   type        поле
0     u16         формат = 2                                   (0x1405cabe0: *(u16*)=2)
2     f32 x3      позиция X,Y,Z (visual state +44/+48/+52)     ("02 00 76 b8 03 46…" -> X=8430.1)
14    2 bytes     sub_140ADCE60/sub_1408D57B0 — назначение UNKNOWN
16    u8 + char[] имя типа персонажа ("SurvivorF_Frida")       (sub_1408D5800)
..    u32         gameBlockLen (патчится в конце, лимит 4096 -> "[ERROR][Person] BinSave() :: game data too long")
..      u32+...   блок статистики (sub_1408D81E0; "infected_killed", "playtime", …)
..      u32       saveVer = g_Game+0xE0 (GAME_STORAGE_VERSION; в живой БД 142)
..      u64 x2    persistent id (a1[175], a1[176])
..      ...       движковое состояние (sub_1405F4890) + скриптовый OnStoreSave (vt+2744)
..    ...         инвентарь (sub_14093C750: вложения/карго)
```

В сохраняемый blob `playerID/charID` не пишутся: ветка `a3==0` в `0x1405cabe0` пропускается, при save `a3=1`.
Сверка на `dbcopy/p.db`: id1 `gameBlockLen 377, statsLen 168, saveVer 142, total 1264`; id2/id3 аналогично.

---

## Q3. Мировые .bin: кто пишет, жизненный цикл, API

### 3.1 Каталоги — CONFIRMED (`StorageDirs` `0x1406d0f00`, вызывается из `0x1408f4b80`)

```c
sprintf(&root,   "%s%s%d%c", base, "storage_", instanceId /*CEroot+0x120*/, '\\');  // qword_141756308
sprintf(&data,   "%s%s%c",   root, "data",   '\\');                                // qword_141756310
sprintf(&backup, "%s%s%c",   root, "backup", '\\');                                // qword_141756318
log("[StorageDirs] :: Selected storage directory: %s", root);
fs->vt[+160](fs, root, 0, "storage", 1903, …);   // регистрация FS-алиаса "storage" -> $storage:
```

Проверка длины пути: `base + "storage_2147483647/backup/000/dynamic_000.bin"` не должна превысить max path,
иначе `"[StorageDirs] :: [ERROR] :: … will exceed max path."`. Каталоги создаются через `fs->vt[+400]`
(MakeDirectory) с абсолютным путём.

### 3.2 Имена слотов — CONFIRMED (`0x1406a82e0`)

| сегмент | слот | формат | пример |
|---|---|---|---|
| нет | ≤0 | `"%s.%s"` (+`"bin"`) | `types.bin` |
| нет | >0 | `"%s.%03d"` | `types.001`, `types.002` |
| есть | ≤0 | `"%s_%03d.%s"` | `dynamic_000.bin` |
| есть | >0 | `"%s_%03d.%03d"` | `dynamic_000.002` |

Каталог: при `backupIdx < 0` это `data\` (`0x1406d0830`), иначе `backup\NNN\` (`0x1406d0720`).
В бэкап пишется без слотового суффикса.

### 3.3 Кто и как открывает файл — CONFIRMED

Цепочка записи:

1. `0x1406a4000` (сохранить один тип/сегмент) создаёт `WRFileOut` (RTTI `.?AVWRFileOut@@`).
2. `0x1406ab5f0` (open + заголовок) вызывает `0x140afb150`.
3. `0x140afb150` переходит на объект `FileServerST` (RTTI `.?AVFileServerST@@`).
   - Объект лежит по `0x1417557d0`, первичная vtable `0x14124b1f8`.
   - Вызов идёт через `.rdata`-указатель `0x14115e3c0` на подобъект `+0x10` (вторичная vtable `0x14124b370`).
   - Слот 0 вторичной vtable — `0x140b06470`, он вызывает первичный `vt[28]` = `0x140b04bd0`
     с режимом `&unk_14124A8A8`.
4. Режим `0x14124A8A8` = `{ dwDesiredAccess=0x40000000 (GENERIC_WRITE), dwShareMode=0, lpSA=NULL, dwCreationDisposition=2 (CREATE_ALWAYS), dwFlags=0x80 }`.

```c
// 0x140b04bd0 (FileServerST::Open)
abs = path[0]=='\\' || (path[0]!='0' && path[1]==':');
n = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
if (abs)  wcscpy(buf, L"\\\\?\\");                         // "\\?\" + путь
else    { wcscpy(buf, L"\\\\?\\"); GetCurrentDirectoryW(...); append '\\'; }   // "\\?\" + CWD + "\" + путь
MultiByteToWideChar(CP_ACP, 0, path, -1, tail, n);
h = CreateFileW(buf, m->access, m->share, m->sa, m->disp, m->flags, NULL);   // call [IAT 0x141080518]
```

Итог: `CreateFileW(L"\\?\F:\…\storage_1\data\dynamic_000.001", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, NORMAL)`.
Обратите внимание: здесь `CP_ACP`, а не UTF-8, как в enf-бэкенде (3.6).

- **Нет temp-файла, нет rename/MoveFileEx, нет DeleteFile, нет FlushFileBuffers.** CONFIRMED.
  У `MoveFileExW` единственный caller — `0x140213600` (enf FS Rename), у `DeleteFileW` — `0x1402123e0`
  (enf FS Delete); CE-писатель их не использует. У `FlushFileBuffers` callers только лог-писатель
  RPT (`0x140b23b90`/`0x140b23c00`) и SQLite `winSync`.
- Запись данных идёт буферизованным потоком WRFileOut (`sub_1405581B0`). Финальный `WriteFile` —
  через FileServerST (INFERRED: `0x140af95c0` входит в callers WriteFile в той же группе функций).
- Чтение: `WRFileIn` (`0x1406ab380` → `0x140afaff0`). INFERRED, что путь тот же
  (FileServerST → CreateFileW через IAT); режим чтения не выписывался.

### 3.4 Последовательность записи файла — CONFIRMED (`0x1406ab5f0`, `0x1406a4000`, `0x1406a67a0`)

```c
// 0x1406ab5f0 — заголовок
w32(ver = desc->cur /*desc+44*/);
if (ver >= 31) w32(*(int*)(g_Game + 0xE0));        // saveVer = StorageVersion()
if (ver >= 32) { w32(stamp /*+48*/); w32(field52 /*+52, 0*/); }
if (ver >= 33)   w32(backupIdx /*+40*/);
eofPos = tell(); w32(0);                            // eofOff-заглушка
w16(len = min(strlen(tag), 0x1FFF)); write(tag, len);
// 0x1406a4000 — тело
cntPos = tell(); w32(0);                            // itemCount-заглушка
<payload по типу>;                                  // types:0x1406a0990 events:0x1406c8880 doors:0x1406be730 прочее:0x14054d580
seek(cntPos); w32(itemCount); seek(end);
// 0x1406a67a0 — финализация
end = tell(); seek(eofPos); w32(end); seek(end);
w32(0x464F455F /* "_EOF" */);                       // длина файла = eofOff + 4
```

### 3.5 Когда пишется — CONFIRMED (таблица задач `0x1406acf30`, обработчики, диспетчер `0x1406a3780`)

Задачи планировщика CE (имя, маска → тип):

| job | маска | тип / файл |
|---|---|---|
| `save_start` | – | `0x1406aa970` (no-op `return 1`) |
| `save_world` | `0x20` | dynamic, сегментами (`world_segments`) |
| `save_animal` | `0x80` | animals |
| `save_vehicle` | `0x200` | vehicles |
| `save_infected` | `0x100` | zombies |
| `save_building` | `0x4000` | building, сегментами |
| `save_de` | `0x10` | events |
| `save_type` | `0x4` | types |
| `save_players` | `0x8000` | игроки (Q2), float 120.0 |
| `save_doors` | `0x10000` | doors |
| `save_end` | – | `0x1406a36b0`: закрыть бэкап-цикл |
| `loop` | – | `0x1406aaa60`: конец CE-цикла |

Обработчик save-задачи `0x1406aa880`. Он вызывает диспетчер с текущим слотом задачи (`job+36`)
только при `CEroot+314` = «сохранение разрешено». Флаг выставляется в конце загрузки:
`= (mode != 4)` в `0x1408f5220`. Для каждого типа диспетчер дополнительно проверяет флаг
персистентности `CEroot+104+8*type`.

**Ротация слотов (`0x1406aaa60`, задача `loop`):** если бэкап в этом цикле не нужен
(`sub_1406A5240` → 0), то `++stamp` (`controller+20`), и у каждой задачи с `count>0` слот
сдвигается: `slot = slot+1 > 2 ? 0 : slot+1`. Получается цикл `.bin → .001 → .002 → .bin`,
по одному слоту на CE-цикл. Тот же сдвиг делается один раз при загрузке (`0x1406ab050`).

Прочие триггеры, все пишут **слот 0 (`.bin`)**:

```c
// IdleMode (0x1406aae60 / 0x1406ab0d0) — сервер пуст N секунд:
sub_1406A3780(ctl, 0x31C3B4 /* force|noBackup|все типы */, -1, -1, /*slot*/0);  log("[IdleMode] Entering IN - save processed");
// Shutdown 0x140a651d0 ("Destroying game"):
sub_1406A3780(ctl, 0x11C3B4 /* noBackup|все типы */, -1, -1, /*slot*/0);
```

Бит `0x200000` — «force», игнорирует флаги персистентности типов. Бит `0x100000` выключает
бэкап-режим (`ctl+1=0`).

**Бэкапы** (`0x1406a1770`, `0x1406a5240`, `0x1406a36b0`):

- `backup_period` в минутах, ограничен диапазоном 15..1440.
- `backup_count` ограничен 0..999; при 0 пишется
  `"[CE][Storage] \"backup_period\" set to:%0.0f but \"backup_count\" is zero!"` и бэкап выключается.
- `backup_startup` → первый цикл сохранения идёт в бэкап.
- Когда таймер истёк, `ctl+1=1`: следующий цикл пишет все файлы в `backup\NNN\` без суффикса,
  `stamp` не растёт. `save_end` записывает метаданные (индекс, stamp, время) и делает
  `backupIdx = (backupIdx+1) % backup_count`.
- Дескриптор `backup.xml` есть в таблице типов (`0x1406ac940`). INFERRED: в нём лежат эти метаданные.

### 3.6 Порядок загрузки при старте — CONFIRMED (`0x1408f5220` + RPT)

1. `sub_14057EEA0` — spawnpoints.bin (xref на строку `"spawnpoints.bin"`).
2. `"[CE][Hive] :: Loading map data ..."` — map XML (`mapgroupproto`, `mapclusterproto`, `mapgrouppos`, `mapgroupdirt`, `mapgroupcluster`).
3. `sub_1406A3230(ctl, 4, /*type*/0, 0xB, -1)` → **types** (все слоты, выбор по stamp).
4. `hive->vt[+0x18]` — economy XML (types.xml/events.xml/…; `"[CE][offlineDB] :: Loaded %d dynamic events %d total types."`).
5. `sub_1406A3230(ctl, 0x10, /*type*/1, 0xC, -1)` → **events**.
6. `sub_1406A3330(ctl, mask, -1)` → dynamic_000..N, building_000..N, doors, vehicles (+animals/zombies,
   если включены) → `"[CE][Hive] :: Storage load took:%0.2f seconds, recovery stamp:%d"` или
   `"Empty storage folder, reinitializing ..."`.
7. `0x1406ab050` — инициализация бэкапов + `++stamp` + сдвиг слотов.
8. `"Initializing ..."`, спавнеры (InitialSpawn/RestartSpawn идут **после** загрузки storage),
   `CEroot+314 = (mode != 4)`, `"Init sequence finished."`.

Фрагмент RPT (1.30, storage_7):

```
[CE][Storage] ver:21 stamp:64, valid:yes, name:"…\storage_7\data\types.bin"
[CE][Storage] ver:21 stamp:67, valid:yes, name:"…\storage_7\data\types.001"
[CE][Storage] ver:21 stamp:68, valid:yes, name:"…\storage_7\data\types.002"
[CE][Storage] Restoring file "…\storage_7\data\types.002".
… events.* … dynamic_000..011.* … building_000..011.* …
[CE][Storage] ver:0 stamp:0, valid:yes, name:"…\data\doors.bin"   -> Restoring "doors.bin"
… vehicles.002 280 items.
[CE][Hive] :: Storage load took:9.92 seconds, recovery stamp:68
[IdleMode] Entering IN - save processed
```

(`ver` печатается через `%x`: `21` = 33, `27` = 39.)

---

## Q4. Формат .bin и версии

### 4.1 Заголовок — CONFIRMED (писатель `0x1406ab5f0`, читатель `0x1406aa640`, живые файлы)

| поле | условие | размер | смысл |
|---|---|---|---|
| `ver` | всегда | u32 | версия **формата файла данного типа** = `desc.cur` |
| `saveVer` | ver≥31 | u32 | **скриптовая версия** = `g_Game+0xE0` (`StorageVersion(GAME_STORAGE_VERSION)`, сейчас 144=0x90) |
| `stamp` | ver≥32 | u32 | счётчик CE-циклов на момент записи (`controller+20`) |
| `field52` | ver≥32 | u32 | всегда 0 в наблюдаемых файлах — UNKNOWN |
| `backupIdx` | ver≥33 | u32 | `controller+24` (data-файлы: 3, бэкапы 000/001/002: 0/1/2) — INFERRED «номер бэкап-слота» |
| `eofOff` | всегда | u32 | смещение `"_EOF"`; 0 = файл не финализирован |
| tag | всегда | u16 + char[] | `STORAGE_TYPES` / `STORAGE_DYNAMIC` / … (≤0x1FFF) |
| `itemCount` | всегда | u32 | патчится после записи тела (animals.bin: 147) |
| payload | | | по типу |
| `"_EOF"` | | 4 | `5f 45 4f 46`, файл = `eofOff+4` |

- Живой пример: `types.bin ver=0x21 save=0x90 stamp=819 f52=0 f40=3 eofOff=len-4 tag=STORAGE_TYPES`.
- `doors.bin` (ver=0, без saveVer/stamp/backupIdx): `00000000 1b000000 0d00 "STORAGE_DOORS" 00000000 "_EOF"`, 31 байт.
- `spawnpoints.bin` — **другой писатель** (`0x14057eea0`/`0x140582b70`): `u32 8, u16 0x13 "STORAGE_SPAWNPOINTS", u32 8, float…`.
  `_EOF` нет. Формат тела UNKNOWN.

### 4.2 Какой номер «версия хранилища», какой «сборка» — CONFIRMED

- `StorageVersion(int)` @ `0x140a73910`: `mov eax,0x64; cmp edx,eax; cmovl edx,eax; mov [rcx+0xE0],edx`.
  Значения меньше 100 поднимаются до 100.
- `SaveVersion()` @ `0x140a318d0`: `mov eax,[rcx+0xE0]`.
- `LoadVersion()` @ `0x140a31620`: `mov eax,[rcx+0xDC]`.
- Читатель заголовка: `if (ver>=31) { hdr.saveVer = r32(); *(g_Game()+220 /*0xDC*/) = hdr.saveVer; }`.
  **`version`, которую видит скрипт в `OnStoreLoad`, — это `saveVer` из заголовка файла.** Это
  глобальная переменная, она перезаписывается при чтении каждого файла.
- В player-blob свой `saveVer` (Q2.5).
- `ver` — внутренняя версия формата C++-сериализатора конкретного типа, с допустимым диапазоном
  (`desc.min..desc.cur`). Дескрипторы строит `0x1406ac940`: `[rsp+0x28]=cur`, `[rsp+0x20]=min`.

| тип | 1.29 (cur/min) | 1.30 | текущая |
|---|---|---|---|
| types | 33/26 | 33/26 | 33/26 |
| events | 37/22 | 38/22 | 38/22 |
| dynamic, animals, zombies, vehicles | 38/11 | 39/11 | 39/11 |
| building | 38/37 | 39/37 | 39/37 |
| doors | — (нет типа) | версия не на стеке; в файлах 0 | то же |

- Значение «0x0d36/0x0cad» из постановки — это `stamp` (3382/3245), а не версия сборки.

### 4.3 Проверка и несовпадение версий — CONFIRMED (`0x1406aa640`, `0x1406ab380`, `0x1406aac10`, `0x1406a39c0`)

```c
if (eofOff <= 0 && CEroot[0x13F] /*storageAutoFix*/) return 1;  // "  File not closed..."
if (strcmp(tag, desc->name))                         return 2;  // "  File header corrupted..."
if (ver < desc->min)                                 return 3;  // "  File version too old..."
if (ver > desc->cur)                                 return 4;  // "  File version is from newer executable, not supported..."
// 6 = "File cannot be opened...", 8 = "File is empty..."
```

- Загрузчик `0x1406a39c0` открывает **все** слоты 0..2. Для каждого он логирует
  `"[CE][Storage] ver:%x stamp:%d, valid:%s, name:\"%s\""`, сортирует по `stamp` по убыванию и берёт
  **самый новый валидный**. Итоговый stamp = максимум из прочитанных (`recovery stamp`).
- Старые версии в диапазоне `[min,cur]` принимаются, и их читает тот же код с ветками по `ver`.
  Пример: `building.bin` ver 0x26 при cur 0x27 / min 0x25 → `valid:yes`. Конвертации на диске нет:
  следующий save просто пишет cur.
- Скриптовая миграция идёт через `LoadVersion()` (saveVer 142 в старом файле при движке 144).
- **CRC/контрольной суммы нет.** CONFIRMED: в заголовке нет такого поля, читатель ничего не считает.
  Целостность тела проверяется только парсингом: `"!!! Corrupted stream after \"%s\" at [%0.1f, %0.1f], file offset: %x"`
  (`0x1406a1e90`), `"Scripted variables corrupted upon \"%s\""`.

### 4.4 Раскладка записи сущности в dynamic_*.bin — UNKNOWN

Писатель `sub_14054D580(qword_141740770, typeCode, seg, …)` и читатель `0x1406a44d0`/`0x1406a1e90`
не разбирались — не хватило бюджета. Что известно косвенно: читатель сообщает тип, позицию
(`at [%0.1f, %0.1f]`) и смещение в файле, есть блок скриптовых переменных и блок
door/auxiliary (`"Storage uses full door persistence …, skipped auxiliary from [Storage] at 0x%x"`).
Длины скриптового OnStoreSave-блока и persistent-id в записи не проверялись.

---

## Q5. Файлы скриптовых модов (`$storage:`, `$mission:`)

- **Алиас `storage`** регистрирует `StorageDirs` через `fs->vt[+160](fs, root, 0, "storage", 1903, …)`.
  `$storage:` раскрывается в `<mission>\storage_<instanceId>\`. CONFIRMED.
- **Белый список путей для скриптов** — `0x1402b7580`. Он отвергает абсолютные пути (`X:\`, `/`, `\`, `.`)
  и `".."`, пропускает только префиксы `$profile:`, `$saves:`, `$logs:`, `$mission:`, `$storage:`.
  Его вызывают нативы `OpenFile` (`0x1402b3830`), `Open` (FileSerializer, `0x1402b3680`),
  `MakeDirectory` (`0x1402b2500`), `DeleteFile` (`0x1402a47e0`), `CopyFile` (`0x1402a30d0`),
  `FileExist` (`0x1402a7b10`), `FindFile` (`0x1402a7fb0`), `BeginParse` (`0x1402a0820`). CONFIRMED.
- Далее идёт синглтон enf FileSystem `sub_1401FF8E0()`: OpenFile → `vt[+312]`, MakeDirectory → `vt[+400]`.
  Это Win32-бэкенд `0x140212330..0x140214190`, **прямые `call [IAT]`** (CONFIRMED):

| backend | API (IAT слот) | параметры |
|---|---|---|
| `0x140213820` | CreateFileW `0x141080518` | маппинг режима: OPEN_EXISTING / OPEN_ALWAYS / CREATE_ALWAYS / CREATE_NEW, GENERIC_WRITE |
| `0x1402132d0` | CreateFileW | access 0, share 7, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS (атрибуты/каталоги) |
| `0x140213600` | MoveFileExW `0x141080598` | flags `0x29` = REPLACE_EXISTING\|WRITE_THROUGH\|FAIL_IF_NOT_TRACKABLE |
| `0x1402123e0` | DeleteFileW `0x141080520` | |
| `0x140213550` | CreateDirectoryW `0x141080510` | |
| `0x140212330` | RemoveDirectoryW | |
| `0x140213120`, `0x140213ae0` | GetFileAttributesW `0x141080540` | |
| `0x140212490` | FindFirstFileW | |
| `0x140214190` | WriteFile | |

- Конвертер путей `0x140213e60`: `MultiByteToWideChar(CP_UTF8=0xFDE9, MB_ERR_INVALID_CHARS)`.
  `\\?\` добавляется **только** для путей вида `X:\`/`X:/`; дальше нормализуются `/`→`\`, `.` и `..`.
  Относительные пути передаются как есть. CONFIRMED.
- Точное соответствие `fs->vt[+312]` → `0x140213820` не прослежено пошагово (INFERRED: других
  W-open'ов в бэкенде нет).
- **Итог:** записи модов (например `communityframework/modstorageplayers.*`) идут через тот же IAT
  главного образа, путь имеет вид `\\?\F:\…\storage_1\…`. CONFIRMED для слоя FS, INFERRED для
  конкретного vt-слота.
- UCRT динамический (`api-ms-win-crt-*`): `fopen`, `_wfopen`, `rename`, `remove`, `_unlink`.
  Патч слота `fopen` главного образа ловит только вызовы `fopen` из самого exe (12 функций; среди них
  storage-писателей не найдено, INFERRED). Внутри ucrtbase `CreateFileW` идёт через **IAT ucrtbase**
  и патчем главного образа не перехватывается.
- `SetFileAttributesW` **не импортируется** (во всех трёх сборках) — хук на него ничего не делает.

---

## Q6. Стабильность между патчами

Проверка байтов во всех трёх exe — CONFIRMED (Y = присутствует везде):

| якорь | cur | 1.30 | 1.29 |
|---|---|---|---|
| RTTI `WRFile` / `WRFileIn` / `WRFileOut` / `FileServerST` / `ADBOffline` / `CentralHive` | Y | Y | Y |
| SQLite source-id `…alt2`, `"AreFileApisANSI"`, `"win32-longpath"` | Y | Y | Y |
| aSyscall = thunk→IAT (CreateFileW, DeleteFileW, Flush, GFA(W/ExW), LockFileEx, Read/WriteFile) | Y `0x1416587f0` | Y `0x141648790` | Y `0x140e56830` |
| `players.db`, весь SQL offlineDB, `ADBOffline::SaveThread` | Y | Y | Y |
| `>storage_` (перед строкой байт `>`, не NUL), `$storage:`, `$mission:` | Y | Y | Y |
| `storage_2147483647/backup/000/dynamic_000.bin`, `DayZ_TransferStorage_%i_Lock` | Y | Y | Y |
| `[CE][Storage] ver:%x stamp:%d…`, `  File not closed...`, `Storage load took…recovery stamp` | Y | Y | Y |
| `%s_%03d.%03d`, `%s.%03d`, `backup.xml`, `save_players`, `[IdleMode] Entering IN - save processed` | Y | Y | Y |
| L`"\\?\"`, `StorageVersion`, `[StorageDirs] :: Selected storage directory: %s` | Y | Y | Y |
| `STORAGE_DOORS` | Y | Y | **нет** |
| `StorageVersion` пишет `[rcx+0xE0]` (байтовая сигнатура) | Y | Y | Y |

**Файловые импорты kernel32 — главное отличие.** CONFIRMED:

- текущая и 1.30: `CreateDirectoryW`, `RemoveDirectoryW`, `FindFirstFileW/NextFileW`, `MoveFileExW`, `_wfopen` — есть.
- 1.29: **нет** `MoveFileExW`, `CreateDirectoryW`, `RemoveDirectoryW`, `FindFirstFileW`, `_wfopen`.
  Вместо них `CreateDirectoryA`, `RemoveDirectoryA`, `FindFirstFileA`, `SetFileInformationByHandle`.
  Enf-бэкенд в 1.29 был на A-API (как именно он делал rename — не разбиралось).
- Во всех трёх: `CreateFileW`, `DeleteFileW`, `GetFileAttributesW`, `GetFileAttributesExW` — есть;
  `SetFileAttributesW`, `CopyFileW`, `ReplaceFileW` — нет.

Версии форматов по сборкам — в таблице 4.2. Между 1.29 и 1.30 выросли events (37→38), dynamic и
сущности (38→39), building (38→39); min не менялись. `GAME_STORAGE_VERSION` — скриптовая
константа (144 сейчас, 142 в старых файлах), растёт с патчами скриптов.

---

## Q7. Прочее для безопасной замены бэкенда

- **Lock-файлы.** Сам движок для storage лок не держит. CONFIRMED.
  - `DayZ_TransferStorage_<instanceId>_Lock` создаётся `StorageDirs` только при переносе storage в
    кастомный корень. Корень задаётся параметром `"storage"`: lookup в `0x1409be7d0`, setter
    `0x1406d1cc0` → `qword_141756300`. Имя ключа `-storage=` — INFERRED.
  - Если лок найден при старте, пишется `"A lock file from a previous run was found, indicating an unsuccessful run, forcing storage transfer."`,
    при ошибке — `"Failed to delete lock file, please manually delete it."`.
  - `communityframework/*.lock` — это скриптовый мод CF, не движок.
- **instanceId:** `CEroot+0x120`, валидность `CEroot+0x13B` (`>=0`). Иначе
  `"[ERROR][Server config] :: instanceId parameter is mandatory and must be valid 32-bit integer."`.
  Межпроцессной блокировки каталога нет. Два сервера с одинаковым `instanceId` пишут в одни файлы;
  `share=0` защищает только на время одного open. INFERRED.
- **`storageAutoFix`** → `CEroot+0x13F` (= value>0), разбор конфига `0x140833e80..0x140833e92`. CONFIRMED.
  Флаг используется только в правиле `"File not closed"`: при `storageAutoFix=0` нефинализированный
  файл (`eofOff=0`) **не отбрасывается** проверкой заголовка. INFERRED-риск: свежий оборванный слот
  с максимальным stamp может быть выбран для загрузки.
- **Бэкапы `backup\000..NNN`** пишет тот же писатель в бэкап-режиме (3.5). Это полный набор файлов
  без ротации, `backupIdx` в заголовке = номер каталога. Пустые каталоги 003..011 в storage_1 —
  INFERRED: их создаёт `sub_1406D0D70(backup_count)` заранее.
- **Предположения о crash-safety (CONFIRMED по коду, следствия INFERRED):**
  1. CE-цикл пишет в слот, отличный от предыдущего: из трёх слотов перезаписывается только один.
  2. Оборванная запись оставляет `eofOff=0`. При autofix файл отвергается, загрузчик берёт следующий
     по stamp.
  3. fsync нет. После сбоя ОС слот может содержать мусор; спасает только парсинг тела.
  4. IdleMode и shutdown **всегда пишут слот 0**, поэтому `.bin` часто новейший. В storage_1 `.bin`
     от 5 ноября, `.001`/`.002` — от апреля/мая.
  5. `doors.*` имеют ver 0 и stamp 0. Все слоты «равны», берётся первый (`.bin`).
- **players.db** держит своё постоянное соединение и плюс одно соединение на каждый Load/Create, в
  других потоках. Использует блокировки SQLite (`LockFileEx`). Внешний writer в этот файл во время
  работы сервера конфликтует с ними.

---

## Что это значит для дизайна плагина

Это наблюдения из кода выше, а не реализация.

1. Перехват IAT главного образа покрывает SQLite (через thunk'и) и оба файловых слоя движка (прямые `call [IAT]`).
   В список нужно добавить **`GetFileAttributesExW`**. Под вопросом `GetFullPathNameW`: SQLite абсолютизирует
   путь через него.
2. Путь приходит в трёх формах, сравнивать нужно после нормализации (снять `\\?\`, без учёта регистра):
   - `\\?\F:\…` — FileServerST и enf-бэкенд для абсолютных путей;
   - `\\?\<CWD>\rel` — FileServerST для относительных;
   - `F:\…` — SQLite win32 VFS (INFERRED: стандартный win32 VFS 3.21 `\\?\` не добавляет).
3. Естественная «точка коммита» .bin — финализация: патч `eofOff` + `_EOF`, затем CloseHandle.
   Писатель открывает файл с `CREATE_ALWAYS`/`share 0` и пишет последовательно, кроме двух seek-патчей
   (itemCount, eofOff).
4. Чтобы поддержать 1.29, перехват должен включать A-варианты (`CreateDirectoryA`, `FindFirstFileA`,
   `RemoveDirectoryA`, `SetFileInformationByHandle`) или работать уровнем выше (FileServerST / enf FS по RTTI).
   Индексы vtable проверены только в текущей сборке.

---

## Итоговая таблица: граница → стабильный якорь → уверенность → переживёт патч?

| граница | стабильный якорь (как искать в рантайме) | уверенность | переживёт патч? |
|---|---|---|---|
| Win32 IAT главного образа (CreateFileW, DeleteFileW, GetFileAttributesW/ExW, WriteFile, ReadFile, MoveFileExW, CreateDirectoryW) | импорт-директория PE по имени функции | CONFIRMED | Да для W-набора 1.30→cur. **Нет для 1.29** (A-API, нет MoveFileExW/CreateDirectoryW) |
| SQLite os_win `aSyscall` (thunk → IAT) | `"AreFileApisANSI"` + единственный `.data`-указатель на неё; записи по 24 байта | CONFIRMED | Да (SQLite 3.21.0 alt2 одинаков в 1.29/1.30/cur) |
| SQLite open/flags/VFS | xref `"players.db"` → `0x1408fa410` → `sqlite3_open` (`xor r9d,r9d; lea r8d,[r9+6]; jmp`) | CONFIRMED | Якорь да, адреса нет |
| offlineDB SQL / схема / `DB_Versions(1,0)` | точные SQL-строки | CONFIRMED | Да (все 3 сборки) |
| Очередь сохранения игроков | RTTI `.?AVADBOffline@@` → vtable (+0x38 save, +0x40 kill); `"ADBOffline::SaveThread"` | CONFIRMED (слоты — только cur) | RTTI да; индексы слотов — проверять |
| Скриптовые нативы Hive | имена `CharacterSave` / `CharacterKill` / `CharacterExit` в регистрации `CentralHive` | CONFIRMED | Да (стабильный скриптовый API) |
| Player blob формат | `u16 2` + pos + typename; `"[ERROR][Person] BinSave() :: game data too long"` | CONFIRMED | Формат 2 стабилен в 3 сборках по строкам; содержимое меняется с `GAME_STORAGE_VERSION` |
| Корень storage / instanceId | `"[StorageDirs] :: Selected storage directory: %s"`, `"%s%s%d%c"`+`">storage_"` | CONFIRMED | Да |
| Алиас `$storage:` + белый список | `"$storage:"` (xref → `0x1402b7580`), `fs->vt[+160](…,"storage",…)` | CONFIRMED | Да (строки во всех 3) |
| Писатель .bin (WRFileOut) | RTTI `.?AVWRFileOut@@`; `"!!! Failed to write [Storage] data to file \"%s\""` | CONFIRMED | RTTI да |
| FileServerST::Open (`\\?\` + CreateFileW, CREATE_ALWAYS, share 0) | RTTI `.?AVFileServerST@@`; L`"\\?\"` рядом с CreateFileW | CONFIRMED | RTTI да; индекс vt[28] — только cur |
| enf FS Win32-бэкенд (UTF-8, `\\?\` для `X:\`) | прямые `call [IAT]` на CreateDirectoryW/MoveFileExW | CONFIRMED (cur) | **Нет** для 1.29 (другой API) |
| Заголовок .bin (ver/saveVer/stamp/backupIdx/eofOff/tag/count/_EOF) | `"[CE][Storage] ver:%x stamp:%d, valid:%s, name:\"%s\""`, `"  File not closed..."` | CONFIRMED | Схема заголовка да (ver≥33); `ver` типов растёт (events/dynamic/building +1 в 1.30) |
| Версии типов (min/cur) | дескрипторы рядом со строками `STORAGE_*` (`[rsp+0x28]`=cur, `[rsp+0x20]`=min) | CONFIRMED | Нет — меняются; читать в рантайме |
| `saveVer` = `GAME_STORAGE_VERSION` | `StorageVersion` → `[g_Game+0xE0]`, `LoadVersion` ← `[+0xDC]` | CONFIRMED | Смещения те же в 3 сборках; значение меняется со скриптами |
| Ротация слотов / stamp | задачи `"save_world"`…`"save_end"`, `"loop"`; формат `"%s_%03d.%03d"` | CONFIRMED | Да (строки во всех 3) |
| Сейвы IdleMode / shutdown (слот 0) | `"[IdleMode] Entering IN - save processed"`, `"Destroying game"` | CONFIRMED | IdleMode да; `"Destroying game"` в старых сборках не проверялся |
| Бэкапы | `"backup_period"`, `"backup_count"`, `"backup_startup"`, `"backup.xml"` | CONFIRMED | Да |
| storageAutoFix | ключ `"storageAutoFix"` → `CEroot+0x13F` | CONFIRMED (cur) | Ключ да; смещение — проверять |
| Lock переноса storage | `"DayZ_TransferStorage_%i_Lock"` | CONFIRMED | Да |
| UCRT fopen/_wfopen/rename/remove | импорт `api-ms-win-crt-*` | CONFIRMED | Да, но мимо kernel32-IAT exe |
| Раскладка записей dynamic_*.bin | `sub_14054D580` / `0x1406a1e90` | **UNKNOWN** | — |
| Формат spawnpoints.bin | `"STORAGE_SPAWNPOINTS"` → `0x140582b70` | **UNKNOWN** (только заголовок) | — |

---

## UNKNOWN / что не сделано

- **Запись сущности в dynamic_*.bin.** Писатель `sub_14054D580` и читатели `0x1406a44d0`, `0x1406a1e90`
  не декомпилировались (объём). Как подступиться: строка `"!!! Corrupted stream after"` → `0x1406a1e90`.
- **Тело spawnpoints.bin.** Видели только заголовок (`u32 8`, tag, `u32 8`, float'ы, без `_EOF`).
- **`field52` заголовка** — всегда 0, смысл не найден.
- **Точная семантика `backupIdx`** (3 в data-файлах) — INFERRED.
- **`save_players`:** что значит float 120.0 и как `sub_140897660` выбирает игроков — тело не разбиралось.
- **Цепочка `fs->vt[+312]` → `0x140213820`** не прослежена пошагово. Режим чтения WRFileIn тоже.
- **Имя CLI-параметра корня storage** (`"storage"`; что это `-storage=`, INFERRED).
- **Поведение при «все слоты невалидны».** Видна только ветка `"Empty storage folder, reinitializing"`
  по результату `0x1406a3330`. При `storageAutoFix=1` замены битых файлов пустыми в коде заголовка
  не видно — это за пределами разобранного.
- **Где `0x1406a3330(mask)` перечисляет типы при загрузке.** Маска выведена по аналогии с
  диспетчером сохранения `0x1406a3780`.
