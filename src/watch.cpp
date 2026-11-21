#include "watch.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <string_view>

namespace dpdb::watch {

namespace {

constexpr std::size_t kMaxRoot = 1024;
constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE;
constexpr int kSpins = 64;         // первые повторы — без сна: читатель отпускает файл за миллисекунды
constexpr int kMaxRetries = kSpins + 200;   // дальше по ~1 мс: потолок ожидания своего читателя

// Корни в свёрнутом виде (нижний регистр ASCII, '\\'), без хвостового разделителя.
// Пишутся один раз в configure() до установки перехвата, дальше только читаются.
wchar_t g_abs[kMaxRoot], g_rel[kMaxRoot];
std::size_t g_abs_len = 0, g_rel_len = 0;

std::atomic<std::uint64_t> g_epoch{0}, g_foreign{0}, g_retries{0};
wchar_t g_sample[260];
std::atomic<int> g_sample_state{0};   // 0 — пусто, 1 — пишется, 2 — готово

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn g_real = nullptr;
int g_slots = 0;

constexpr wchar_t fold(wchar_t c) {
    return c == L'/' ? L'\\' : (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
}

std::size_t set_root(wchar_t* dst, std::wstring_view src) {
    while (!src.empty() && (src.back() == L'\\' || src.back() == L'/'))
        src.remove_suffix(1);
    if (src.size() >= kMaxRoot)
        return 0;
    for (std::size_t i = 0; i < src.size(); ++i)
        dst[i] = fold(src[i]);
    dst[src.size()] = 0;
    return src.size();
}

bool under(const wchar_t* p, const wchar_t* root, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (fold(p[i]) != root[i])   // конец строки p тоже не совпадёт: в root нулей нет
            return false;
    return p[n] == L'\\' || p[n] == L'/';
}

// «…\storage_<число>\…» где угодно в пути.
bool names_some_storage(const wchar_t* p) {
    static constexpr std::wstring_view key = L"storage_";
    for (; *p; ++p) {
        if (*p != L'\\' && *p != L'/')
            continue;
        const wchar_t* q = p + 1;
        std::size_t i = 0;
        while (i < key.size() && fold(q[i]) == key[i])
            ++i;
        if (i != key.size())
            continue;
        q += i;
        if (*q == L'-')
            ++q;
        const wchar_t* digits = q;
        while (*q >= L'0' && *q <= L'9')
            ++q;
        if (q != digits && (*q == L'\\' || *q == L'/'))
            return true;
    }
    return false;
}

void remember_foreign(const wchar_t* path) {
    g_foreign.fetch_add(1, std::memory_order_relaxed);
    int empty = 0;
    if (g_sample_state.compare_exchange_strong(empty, 1)) {
        wcsncpy_s(g_sample, path, _TRUNCATE);
        g_sample_state.store(2, std::memory_order_release);
    }
}

HANDLE WINAPI detour(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                     DWORD flags, HANDLE tmpl) {
    // Чтения — подавляющее большинство вызовов: для них перехват стоит одну проверку.
    if (!(access & kWriteAccess) || !name)
        return g_real(name, access, share, sa, disposition, flags, tmpl);
    const Kind kind = classify(name);
    if (kind == Kind::Foreign)
        remember_foreign(name);
    HANDLE h = g_real(name, access, share, sa, disposition, flags, tmpl);
    if (kind != Kind::Storage)
        return h;
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        // Возможно, файл держит наш читатель. Просим его не начинать новых чтений (идущее
        // кончится за одно чтение файла) и повторяем открытие. Если перед попыткой наших
        // чтений уже не было, а отказ повторился — блокировка чужая (антивирус, бэкап):
        // возвращаем движку его ошибку, как было бы без плагина.
        fs::writer_waiting(+1);
        for (int i = 0; i < kMaxRetries && h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
             ++i) {
            const bool ours_gone = fs::active_readers() == 0;
            g_retries.fetch_add(1, std::memory_order_relaxed);
            Sleep(i < kSpins ? 0 : 1);
            h = g_real(name, access, share, sa, disposition, flags, tmpl);
            if (ours_gone)
                break;
        }
        const DWORD err = GetLastError();
        fs::writer_waiting(-1);
        SetLastError(err);
    }
    if (h != INVALID_HANDLE_VALUE)
        g_epoch.fetch_add(1, std::memory_order_release);   // GetLastError не трогает
    return h;
}

}  // namespace

void configure(const fs::Path& storage_dir, const fs::Path& cwd) {
    const std::wstring abs = std::filesystem::absolute(storage_dir).lexically_normal().wstring();
    g_abs_len = set_root(g_abs, abs);
    g_rel_len = 0;
    const std::wstring base = std::filesystem::absolute(cwd).lexically_normal().wstring();
    wchar_t folded[kMaxRoot];
    const std::size_t n = set_root(folded, base);
    if (n && under(abs.c_str(), folded, n))
        g_rel_len = set_root(g_rel, std::wstring_view(abs).substr(n + 1));
}

Kind classify(const wchar_t* p) {
    if (p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\')
        p += 4;
    if ((g_abs_len && under(p, g_abs, g_abs_len)) || (g_rel_len && under(p, g_rel, g_rel_len)))
        return Kind::Storage;
    return names_some_storage(p) ? Kind::Foreign : Kind::Other;
}

int install(void* module) {
    if (g_slots)
        return g_slots;
    auto* base = static_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return 0;
    for (auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name;
         ++desc) {
        // Имена функций лежат в таблице OriginalFirstThunk; FirstThunk уже переписан загрузчиком.
        if (!desc->OriginalFirstThunk)
            continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto** slot = reinterpret_cast<void**>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(by_name->Name, "CreateFileW") != 0)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
                continue;
            if (!g_real)
                g_real = reinterpret_cast<CreateFileWFn>(*slot);   // до подмены: детур зовёт его сразу
            *slot = reinterpret_cast<void*>(&detour);
            VirtualProtect(slot, sizeof(void*), old, &old);
            ++g_slots;
        }
    }
    return g_slots;
}

std::uint64_t epoch() {
    return g_epoch.load(std::memory_order_acquire);
}

std::uint64_t foreign() {
    return g_foreign.load(std::memory_order_relaxed);
}

std::wstring foreign_sample() {
    return g_sample_state.load(std::memory_order_acquire) == 2 ? std::wstring(g_sample) : std::wstring();
}

std::uint64_t retries() {
    return g_retries.load(std::memory_order_relaxed);
}

}  // namespace dpdb::watch
