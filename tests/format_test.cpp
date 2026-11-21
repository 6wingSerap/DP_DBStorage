#include <gtest/gtest.h>

#include <cstring>

#include "format.hpp"
#include "support.hpp"

using namespace dpdb;

namespace {

fs::Bytes from_hex(std::string_view hex) {
    fs::Bytes out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::byte>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

void put_u32(fs::Bytes& b, std::size_t off, std::uint32_t v) {
    std::memcpy(b.data() + off, &v, 4);
}

}  // namespace

// Настоящие байты doors.bin с живого сервера (сборка 2026-11-04): формат 0, без stamp.
TEST(Format, RealDoorsFileIsComplete) {
    const auto b = from_hex("000000001b0000000d0053544f524147455f444f4f5253000000005f454f46");
    const auto f = inspect(b);
    EXPECT_EQ(f.shape, Shape::Complete);
    EXPECT_EQ(f.tag, "STORAGE_DOORS");
    EXPECT_EQ(f.fmt, 0);
    EXPECT_EQ(f.save, -1);
    EXPECT_EQ(f.stamp, -1);
}

TEST(Format, ModernLayerHeaderIsFullyRead) {
    const auto b = make_layer("STORAGE_DYNAMIC", 39, 144, 0x0d36, test::bytes("payload"));
    const auto f = inspect(b);
    EXPECT_EQ(f.shape, Shape::Complete);
    EXPECT_EQ(f.tag, "STORAGE_DYNAMIC");
    EXPECT_EQ(f.fmt, 39);
    EXPECT_EQ(f.save, 144);
    EXPECT_EQ(f.stamp, 0x0d36);
}

// Движок ставит eofOff последним: обрыв записи оставляет в поле ноль.
TEST(Format, ZeroEofOffsetMeansUnfinished) {
    auto b = make_layer("STORAGE_DYNAMIC", 39, 144, 7, test::bytes("payload"));
    put_u32(b, 20, 0);
    b.resize(b.size() - 9);
    const auto f = inspect(b);
    EXPECT_EQ(f.shape, Shape::Unfinished);
    EXPECT_EQ(f.fmt, 39);
    EXPECT_EQ(f.tag, "STORAGE_DYNAMIC");
}

TEST(Format, MissingTrailerMeansUnfinished) {
    auto b = make_layer("STORAGE_VEHICLES", 39, 144, 7, test::bytes("payload"));
    b.resize(b.size() - 4);   // eofOff уже проставлен, "_EOF" не дописан
    EXPECT_EQ(inspect(b).shape, Shape::Unfinished);
}

// spawnpoints.bin: тег есть, но раскладка другая и хвоста _EOF нет. Судить о таком
// файле нельзя — он хранится как есть и никогда не «чинится».
TEST(Format, SpawnpointsLayoutIsOpaque) {
    auto b = from_hex("08000000130053544f524147455f535041574e504f494e5453080000000000f041");
    const auto f = inspect(b);
    EXPECT_EQ(f.shape, Shape::Opaque);
    EXPECT_TRUE(f.tag.empty());
    EXPECT_EQ(f.fmt, -1);
}

// После отключения питания NTFS отдаёт недописанные области нулями; пустой файл остаётся,
// если движок успел только создать его. Ни один формат не начинается с пустого заголовка.
TEST(Format, EmptyOrZeroedHeaderIsBlank) {
    EXPECT_EQ(inspect({}).shape, Shape::Blank);
    EXPECT_EQ(inspect(fs::Bytes(4096)).shape, Shape::Blank);
    EXPECT_EQ(inspect(fs::Bytes(10)).shape, Shape::Blank);
    fs::Bytes late(4096);
    late[200] = std::byte{1};   // данные есть, но заголовок пуст
    EXPECT_EQ(inspect(late).shape, Shape::Blank);
}

TEST(Format, ForeignAndTinyFilesAreOpaque) {
    EXPECT_EQ(inspect(test::bytes("hello")).shape, Shape::Opaque);
    EXPECT_EQ(inspect(test::bytes("{\"json\": \"STORAGE_ не в заголовке\"}")).shape, Shape::Opaque);
}

// Патч добавил поля в заголовок ПЕРЕД eofOff: тег сдвинулся, но eofOff всё так же стоит
// перед длиной тега — файл остаётся узнаваемым без правки плагина.
TEST(Format, ExtraHeaderFieldsBeforeEofOffsetStillParse) {
    auto b = make_layer("STORAGE_DYNAMIC", 45, 150, 9, test::bytes("payload"));
    b.insert(b.begin() + 20, 8, std::byte{0});          // два новых u32
    put_u32(b, 28, static_cast<std::uint32_t>(b.size() - 4));
    const auto f = inspect(b);
    EXPECT_EQ(f.shape, Shape::Complete);
    EXPECT_EQ(f.fmt, 45);
    EXPECT_EQ(f.tag, "STORAGE_DYNAMIC");
}

// Незнакомая раскладка с мусором после eofOff: не «оборван», а «не знаю».
TEST(Format, InconsistentLengthIsOpaqueNotUnfinished) {
    auto b = make_layer("STORAGE_DYNAMIC", 39, 144, 7, test::bytes("payload"));
    b.insert(b.end(), 16, std::byte{1});
    EXPECT_EQ(inspect(b).shape, Shape::Opaque);
}

TEST(Format, MakeLayerProducesEngineLayout) {
    const auto b = make_layer("STORAGE_TYPES", 33, 144, 3, {});
    // ver, saveVer, stamp, 0, backupIdx, eofOff, u16 len, tag, u32 count, "_EOF"
    EXPECT_EQ(b.size(), 26u + 13u + 4u + 4u);
    EXPECT_EQ(test::str(std::span(b).last(4)), "_EOF");
}
