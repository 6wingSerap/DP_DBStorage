cmake_minimum_required(VERSION 3.30)
project(dp_dbstorage C CXX)

# DP_DBStorage — хранилище DayZ-сервера в SQLite.
#   dpdb_core   движко-независимое ядро (тестируется без игры)
#   dpdb        офлайн-утилита и бенчмарк
#   dp_dbstorage graft-плагин (нужен KR_GRAFTED)
#
#   build.cmd release          плагин + утилита
#   build.cmd tests            gtest без игры и без graft
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Статический рантайм — требование ABI плагинов graft; форма с $<CONFIG> обязательна.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(DPDB_BUILD_PLUGIN "graft-плагин (нужен KR_GRAFTED)" ON)
option(DPDB_BUILD_TESTS  "юнит-тесты (gtest)" OFF)

# SQLite собирается из исходника внутрь плагина: один и тот же движок на любой Windows,
# а не тот, что оказался в системе. Обновление — новая ссылка и хеш со страницы
# https://www.sqlite.org/download.html. Без сети: -DFETCHCONTENT_SOURCE_DIR_SQLITE=<каталог>.
include(FetchContent)
FetchContent_Declare(sqlite
    URL      https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
    URL_HASH SHA3_256=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
    DOWNLOAD_EXTRACT_TIMESTAMP OFF)
FetchContent_MakeAvailable(sqlite)
add_library(dpdb_sqlite STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(dpdb_sqlite SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
# Набор «Recommended Compile-time Options» (sqlite.org/compile.html) под нашу работу:
# соединение не делится между потоками, расширения и устаревшие API не нужны.
target_compile_definitions(dpdb_sqlite PRIVATE
    SQLITE_THREADSAFE=2              # потоки — да, одно соединение из двух потоков — нет
    SQLITE_DEFAULT_MEMSTATUS=0       # без учёта памяти: это мьютекс на каждый malloc
    SQLITE_OMIT_AUTOINIT             # sqlite3_initialize() зовём сами (sql::Db)
    SQLITE_DQS=0
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_STRICT_SUBTYPE=1
    SQLITE_USE_ALLOCA
    SQLITE_OMIT_DECLTYPE
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_PROGRESS_CALLBACK
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_OMIT_LOAD_EXTENSION)
target_compile_options(dpdb_sqlite PRIVATE /w)

add_library(dpdb_core STATIC
    src/sqlite.cpp
    src/fs.cpp
    src/format.cpp
    src/store.cpp
    src/sync.cpp
    src/players.cpp
    src/restore.cpp
    src/config.cpp
    src/watch.cpp
    src/session.cpp)
target_include_directories(dpdb_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(dpdb_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(dpdb_core PRIVATE /W4)
# bcrypt — SHA-256, version — версия exe игры.
target_link_libraries(dpdb_core PUBLIC bcrypt version PRIVATE dpdb_sqlite)

add_executable(dpdb src/cli.cpp)
target_link_libraries(dpdb PRIVATE dpdb_core)

if(DPDB_BUILD_PLUGIN)
    set(GRAFT_BOOT_FILE "${CMAKE_BINARY_DIR}/graft.boot.cmake" CACHE FILEPATH "graft.boot.cmake")
    if(NOT EXISTS "${GRAFT_BOOT_FILE}")
        file(DOWNLOAD https://raw.githubusercontent.com/KRdayzmodding/KR_GRAFTED/main/cmake/graft.boot.cmake "${GRAFT_BOOT_FILE}")
    endif()
    include("${GRAFT_BOOT_FILE}")
    # Тег релиза, не main. Локальный клон: -DGRAFT_SOURCE_DIR=<путь>.
    graft_import(graft https://github.com/KRdayzmodding/KR_GRAFTED TAG v0.5.1)

    # Объявления нативов печатаются прямо в мод — PBO собирается из него же.
    graft_plugin(dp_dbstorage
        NAME    DP_DBStorage
        VERSION 2
        SOURCES src/plugin.cpp
        MODULES 3_Game
        SCRIPTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mod/DP_DBStorage_Server/scripts")
    target_link_libraries(dp_dbstorage PRIVATE dpdb_core)
endif()

if(DPDB_BUILD_TESTS)
    enable_testing()
    # Без сети: -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<готовый исходник>.
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        EXCLUDE_FROM_ALL)
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    foreach(t gtest gtest_main gmock gmock_main)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE -clang:-Wno-character-conversion)
        endif()
    endforeach()

    add_executable(dpdb_tests
        tests/sqlite_test.cpp
        tests/fs_test.cpp
        tests/format_test.cpp
        tests/store_test.cpp
        tests/sync_test.cpp
        tests/players_test.cpp
        tests/restore_test.cpp
        tests/config_test.cpp
        tests/watch_test.cpp
        tests/session_test.cpp
        tests/perf_test.cpp)
    target_include_directories(dpdb_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(dpdb_tests PRIVATE dpdb_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(dpdb_tests)
endif()
