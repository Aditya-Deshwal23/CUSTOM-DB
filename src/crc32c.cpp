#include "crc32c.h"

#include <array>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <nmmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_acle.h>
#if defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

namespace relational_db {
namespace {

constexpr uint32_t CrcTableEntry(uint32_t value) {
    for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0U ? (value >> 1U) ^ 0x82F63B78U : value >> 1U;
    }
    return value;
}

constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (size_t index = 0; index < table.size(); ++index) {
        table[index] = CrcTableEntry(static_cast<uint32_t>(index));
    }
    return table;
}

constexpr auto kCrcTable = MakeCrcTable();

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("sse4.2"))) uint32_t Crc32cX86(const uint8_t* data, size_t length) {
    uint64_t crc = 0xFFFFFFFFU;
#if defined(__x86_64__)
    while (length >= sizeof(uint64_t)) {
        uint64_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        crc = _mm_crc32_u64(crc, value);
        data += sizeof(uint64_t);
        length -= sizeof(uint64_t);
    }
#endif
    uint32_t crc32 = static_cast<uint32_t>(crc);
    while (length >= sizeof(uint32_t)) {
        uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        crc32 = _mm_crc32_u32(crc32, value);
        data += sizeof(uint32_t);
        length -= sizeof(uint32_t);
    }
    while (length > 0) {
        crc32 = _mm_crc32_u8(crc32, *data++);
        --length;
    }
    return crc32 ^ 0xFFFFFFFFU;
}
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
bool HasX86Crc32() {
    int registers[4]{};
    __cpuid(registers, 1);
    return (registers[2] & (1 << 20)) != 0;
}

uint32_t Crc32cX86(const uint8_t* data, size_t length) {
    uint64_t crc = 0xFFFFFFFFU;
#if defined(_M_X64)
    while (length >= sizeof(uint64_t)) {
        uint64_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        crc = _mm_crc32_u64(crc, value);
        data += sizeof(value);
        length -= sizeof(value);
    }
#endif
    uint32_t crc32 = static_cast<uint32_t>(crc);
    while (length >= sizeof(uint32_t)) {
        uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        crc32 = _mm_crc32_u32(crc32, value);
        data += sizeof(value);
        length -= sizeof(value);
    }
    while (length > 0) {
        crc32 = _mm_crc32_u8(crc32, *data++);
        --length;
    }
    return crc32 ^ 0xFFFFFFFFU;
}
#endif

#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
uint64_t LoadLittleEndian64(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<uint64_t>(data[index]) << (index * 8U);
    }
    return value;
}

__attribute__((target("+crc"))) uint32_t Crc32cArm(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    while (length >= sizeof(uint64_t)) {
        crc = __crc32cd(crc, LoadLittleEndian64(data));
        data += sizeof(uint64_t);
        length -= sizeof(uint64_t);
    }
    while (length > 0) {
        crc = __crc32cb(crc, *data++);
        --length;
    }
    return crc ^ 0xFFFFFFFFU;
}

bool HasArmCrc32() {
#if defined(__linux__) && defined(HWCAP_CRC32)
    return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0U;
#elif defined(__APPLE__)
    int available = 0;
    size_t size = sizeof(available);
    return sysctlbyname("hw.optional.armv8_crc32", &available, &size, nullptr, 0) == 0 &&
           available != 0;
#else
    return false;
#endif
}
#endif

bool HasHardwareCrc32c() {
#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
    __builtin_cpu_init();
    return __builtin_cpu_supports("sse4.2");
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    return HasX86Crc32();
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    return HasArmCrc32();
#else
    return false;
#endif
}

}  // namespace

uint32_t Crc32cSoftware(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t index = 0; index < length; ++index) {
        const uint8_t table_index = static_cast<uint8_t>(crc ^ data[index]);
        crc = (crc >> 8U) ^ kCrcTable[table_index];
    }
    return crc ^ 0xFFFFFFFFU;
}

uint32_t Crc32c(const uint8_t* data, size_t length) {
    static const bool has_hardware = HasHardwareCrc32c();
#if ((defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))) || \
    (defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86)))
    if (has_hardware) {
        return Crc32cX86(data, length);
    }
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    if (has_hardware) {
        return Crc32cArm(data, length);
    }
#else
    (void)has_hardware;
#endif
    return Crc32cSoftware(data, length);
}

}  // namespace relational_db
