// Minimal, dependency-free CRC-32 (IEEE 802.3 / zlib polynomial). Header-only.
// Used to fingerprint a source file so an already-converted dataset can be
// reused instead of re-importing it. This is a cache/dedup check, not a security
// check, so a fast non-cryptographic checksum is the right tool.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace crc32_detail {

inline const std::uint32_t* table() {
    static const std::uint32_t* t = [] {
        static std::uint32_t table[256];
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        return table;
    }();
    return t;
}

struct Ctx {
    std::uint32_t crc = 0xFFFFFFFFu;

    void update(const void* data, std::size_t len) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        const std::uint32_t* t = table();
        std::uint32_t c = crc;
        for (std::size_t i = 0; i < len; ++i)
            c = t[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
        crc = c;
    }

    // Finalized value; does not mutate state, so updates may continue after.
    std::uint32_t value() const { return crc ^ 0xFFFFFFFFu; }

    std::string hex() const {
        char out[9];
        std::snprintf(out, sizeof(out), "%08x", value());
        return std::string(out, 8);
    }
};

} // namespace crc32_detail

// Hex-encoded CRC-32 of a byte buffer.
inline std::string crc32_hex(const void* data, std::size_t len) {
    crc32_detail::Ctx c;
    c.update(data, len);
    return c.hex();
}
