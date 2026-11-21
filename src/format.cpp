#include "format.hpp"

#include <algorithm>
#include <cstring>

namespace dpdb {

namespace {

constexpr std::string_view kTagPrefix = "STORAGE_";
constexpr std::string_view kTrailer = "_EOF";
constexpr std::size_t kHeaderWindow = 96;   // тег ищется только в начале файла

std::uint32_t u32(std::span<const std::byte> b, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + off, 4);
    return v;
}

void put(fs::Bytes& out, const void* p, std::size_t n) {
    const auto* b = static_cast<const std::byte*>(p);
    out.insert(out.end(), b, b + n);
}

}  // namespace

Facts inspect(std::span<const std::byte> file) {
    Facts f;
    const std::string_view head(reinterpret_cast<const char*>(file.data()), std::min(file.size(), kHeaderWindow));
    if (head.find_first_not_of(char{0}) == std::string_view::npos) {
        f.shape = Shape::Blank;
        return f;
    }
    const std::size_t t = head.find(kTagPrefix);
    // Перед тегом обязаны поместиться ver, eofOff и длина тега.
    if (t == std::string_view::npos || t < 10)
        return f;
    std::uint16_t len = 0;
    std::memcpy(&len, file.data() + t - 2, 2);
    if (len < kTagPrefix.size() || len > 64 || t + len > file.size())
        return f;
    const std::string_view tag(reinterpret_cast<const char*>(file.data()) + t, len);
    if (!std::ranges::all_of(tag, [](char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }))
        return f;

    const std::uint64_t eof = u32(file, t - 6);
    const bool trailer = file.size() >= 4 &&
                         std::memcmp(file.data() + file.size() - 4, kTrailer.data(), 4) == 0;
    if (eof != 0 && eof + 4 == file.size() && trailer)
        f.shape = Shape::Complete;
    else if (eof == 0 || eof + 4 > file.size())
        f.shape = Shape::Unfinished;   // не финализирован либо хвост не дописан
    else
        return f;   // длина не сходится в большую сторону — раскладка незнакома

    f.tag = tag;
    f.fmt = u32(file, 0);
    if (t >= 14)
        f.save = u32(file, 4);
    if (t >= 22)
        f.stamp = u32(file, 8);
    return f;
}

fs::Bytes make_layer(std::string_view tag, std::uint32_t fmt, std::uint32_t save, std::uint32_t stamp,
                     std::span<const std::byte> payload) {
    fs::Bytes b;
    const std::uint32_t zero = 0;
    const auto len = static_cast<std::uint16_t>(tag.size());
    put(b, &fmt, 4);
    put(b, &save, 4);
    put(b, &stamp, 4);
    put(b, &zero, 4);
    put(b, &zero, 4);   // backupIdx
    put(b, &zero, 4);   // eofOff — ниже
    put(b, &len, 2);
    put(b, tag.data(), tag.size());
    put(b, &zero, 4);   // count
    put(b, payload.data(), payload.size());
    const auto eof = static_cast<std::uint32_t>(b.size());
    std::memcpy(b.data() + 20, &eof, 4);
    put(b, kTrailer.data(), kTrailer.size());
    return b;
}

}  // namespace dpdb
