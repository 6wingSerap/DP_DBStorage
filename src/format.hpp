// Единственное место, где плагин знает формат файлов движка. Знание минимальное и
// проверяет само себя: всё, что не сошлось, считается непрозрачным (Opaque) — такой
// файл хранится байт-в-байт, и плагин никогда не берётся его «чинить».
//
// Заголовок слоя CE (docs/research/dayz-persistence.md, разд. 4.1; сборки 1.29–1.30+):
//   u32 ver | [u32 saveVer, ver>=31] | [u32 stamp, u32 0, ver>=32] | [u32 backupIdx, ver>=33]
//   | u32 eofOff | u16 len | "STORAGE_*" | u32 count | данные | "_EOF"
// Движок пишет eofOff нулём, а настоящее значение и "_EOF" ставит последними:
// длина готового файла = eofOff + 4. Контрольной суммы в формате нет.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "fs.hpp"

namespace dpdb {

enum class Shape {
    Opaque,       // не слой CE или незнакомая раскладка: судить нельзя
    Complete,     // слой CE, дописан до конца
    Unfinished,   // слой CE, запись оборвана (краш посреди сохранения)
    Blank,        // пустой файл или обнулённый заголовок (след отключения питания)
};

struct Facts {
    Shape shape = Shape::Opaque;
    std::string tag;            // "STORAGE_DYNAMIC"…; пусто у Opaque
    std::int64_t fmt = -1;      // версия формата файла (ver); -1 — неизвестно
    std::int64_t save = -1;     // GAME_STORAGE_VERSION скриптов (saveVer)
    std::int64_t stamp = -1;    // счётчик циклов сохранения
};

// Привязка к тегу, а не к смещениям: eofOff стоит сразу перед длиной тега в любой
// известной раскладке, поэтому новые поля в начале заголовка разбор не ломают.
Facts inspect(std::span<const std::byte> file);

// Слой в раскладке ver>=33 — для тестов и бенчмарка.
fs::Bytes make_layer(std::string_view tag, std::uint32_t fmt, std::uint32_t save, std::uint32_t stamp,
                     std::span<const std::byte> payload);

}  // namespace dpdb
