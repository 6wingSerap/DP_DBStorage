// Общие помощники тестов: временный каталог, запись файлов, сдвиг mtime.
#pragma once
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "fs.hpp"

namespace test {

namespace stdfs = std::filesystem;

// Уникальный каталог на тест; удаляется в деструкторе.
struct TempDir {
    stdfs::path path;
    TempDir() {
        static std::atomic<int> n{0};
        path = stdfs::temp_directory_path() /
               ("dpdb_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(n++));
        stdfs::remove_all(path);
        stdfs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        stdfs::remove_all(path, ec);
    }
    stdfs::path operator/(std::string_view rel) const { return path / rel; }
};

inline dpdb::fs::Bytes bytes(std::string_view s) {
    const auto* p = reinterpret_cast<const std::byte*>(s.data());
    return {p, p + s.size()};
}

inline std::string str(std::span<const std::byte> b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

inline void write(const stdfs::path& p, std::span<const std::byte> data) {
    stdfs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

inline void write(const stdfs::path& p, std::string_view text) {
    write(p, bytes(text));
}

inline std::string read(const stdfs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Состарить файл: mtime = сейчас − seconds (чтобы пройти порог «отстоялся»).
inline void age(const stdfs::path& p, int seconds) {
    stdfs::last_write_time(p, stdfs::file_time_type::clock::now() - std::chrono::seconds(seconds));
}

}  // namespace test
