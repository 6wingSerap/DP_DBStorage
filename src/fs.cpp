#include "fs.hpp"

#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <stdexcept>

namespace dpdb::fs {

Hash sha256(std::span<const std::byte> data) {
    static const BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE h = nullptr;
        if (BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
            throw std::runtime_error("SHA-256: провайдер BCrypt недоступен");
        return h;
    }();
    Hash out{};
    if (BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())),
                   static_cast<ULONG>(data.size()), reinterpret_cast<PUCHAR>(out.data()),
                   static_cast<ULONG>(out.size())) != 0)
        throw std::runtime_error("SHA-256: BCryptHash отказал");
    return out;
}

std::string hex(std::span<const std::byte> data) {
    static const char digits[] = "0123456789abcdef";
    std::string s;
    s.reserve(data.size() * 2);
    for (const std::byte b : data) {
        s += digits[std::to_integer<int>(b) >> 4];
        s += digits[std::to_integer<int>(b) & 15];
    }
    return s;
}

std::string to_utf8(std::wstring_view w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring to_wide(std::string_view s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::int64_t now_ticks() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::int64_t unix_ms() {
    return now_ticks() / kTicksPerMs - 11'644'473'600'000ll;   // 1601 → 1970
}

std::vector<Entry> scan(const Path& root) {
    std::vector<Entry> out;
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const std::filesystem::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe) || fe)
            continue;
        Entry e;
        e.rel = to_utf8(it->path().lexically_relative(root).wstring());
        for (char& c : e.rel)
            c = c == '\\' ? '/' : (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        e.size = static_cast<std::int64_t>(it->file_size(fe));
        // file_clock MSVC = тики FILETIME: сравнимо с now_ticks() напрямую.
        e.mtime = it->last_write_time(fe).time_since_epoch().count();
        if (!fe)
            out.push_back(std::move(e));
    }
    std::ranges::sort(out, {}, &Entry::rel);
    return out;
}

namespace {
std::atomic<int> g_readers{0}, g_writers_waiting{0};
}

int active_readers() {
    return g_readers.load(std::memory_order_acquire);
}

void writer_waiting(int delta) {
    g_writers_waiting.fetch_add(delta, std::memory_order_acq_rel);
}

Read read_shared(const Path& path, Bytes& out, std::int64_t max_size) {
    out.clear();
    // Счётчик поднят на всё время жизни хэндла: окно, в котором движок может получить
    // отказ из-за нас, целиком внутри него.
    struct Scope {
        Scope() { g_readers.fetch_add(1, std::memory_order_acq_rel); }
        ~Scope() { g_readers.fetch_sub(1, std::memory_order_acq_rel); }
    } scope;
    if (g_writers_waiting.load(std::memory_order_acquire) > 0)
        return Read::Busy;   // уступаем движку: прочитаем в следующий проход
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            return Read::Missing;
        return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION ? Read::Busy : Read::Failed;
    }
    Read result = Read::Ok;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) {
        result = Read::Failed;
    } else if (size.QuadPart > max_size) {
        result = Read::TooBig;
    } else {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        std::size_t done = 0;
        while (done < out.size()) {
            DWORD n = 0;
            const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 30));
            if (!ReadFile(h, out.data() + done, want, &n, nullptr) || n == 0) {
                result = Read::Failed;   // файл укоротили под нами — прочитаем в следующий раз
                break;
            }
            done += n;
        }
    }
    CloseHandle(h);
    if (result != Read::Ok)
        out.clear();
    return result;
}

void write_atomic(const Path& path, std::span<const std::byte> data, bool durable) {
    std::filesystem::create_directories(path.parent_path());
    Path tmp = path;
    tmp += to_wide(kTmpSuffix);
    const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("не создать файл «" + to_utf8(tmp.wstring()) + "» (ошибка " +
                                 std::to_string(GetLastError()) + ")");
    bool ok = true;
    for (std::size_t done = 0; ok && done < data.size();) {
        DWORD n = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 30));
        ok = WriteFile(h, data.data() + done, want, &n, nullptr) && n > 0;
        done += n;
    }
    ok = ok && (!durable || FlushFileBuffers(h));
    const DWORD err = GetLastError();
    CloseHandle(h);
    // Переименование без WRITE_THROUGH: данные уже сброшены, а потерянное при сбое питания
    // переименование — это «файла нет», что следующий старт чинит сам.
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD e = ok ? GetLastError() : err;
        DeleteFileW(tmp.c_str());
        throw std::runtime_error("не записать файл «" + to_utf8(path.wstring()) + "» (ошибка " + std::to_string(e) +
                                 ")");
    }
}

}  // namespace dpdb::fs
