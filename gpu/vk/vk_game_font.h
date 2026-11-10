// (hookshot) The game's own menu font, read from the player's game files, in memory only (vk_imgui.cpp; never written anywhere).
// romfs/Font2/Scalable/<name>.Nin_Ounce_NVN2.bfotf.zs is zstd with the dictionary zs.zsdic, kept in the SARC archive
// romfs/Pack/ZsDic.pack.zs (zstd, no dictionary). The .bfotf in it is an OpenType font behind an 8-byte header, XOR-ed with a
// 4-byte key: the first little-endian word is 0x18029a7f XOR the key, the second the font's size (big-endian) XOR the key.
// No Vulkan or ImGui here: tests/test_game_font.py checks it on files of its own making.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "zstd.h"

namespace mkwvk::game_font {

inline bool read_file(const std::string& path, std::vector<unsigned char>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    const bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}
/// A zstd frame's content (its size in its header, at most 256 MB), with a dictionary or none
inline bool unzstd(const std::vector<unsigned char>& in, ZSTD_DDict* dict, std::vector<unsigned char>& out) {
    const unsigned long long size = ZSTD_getFrameContentSize(in.data(), in.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (256u << 20)) return false;
    out.resize((size_t)size);
    ZSTD_DCtx* dc = ZSTD_createDCtx();
    if (!dc) return false;
    const size_t got = dict ? ZSTD_decompress_usingDDict(dc, out.data(), out.size(), in.data(), in.size(), dict)
                            : ZSTD_decompressDCtx(dc, out.data(), out.size(), in.data(), in.size());
    ZSTD_freeDCtx(dc);
    return !ZSTD_isError(got) && got == out.size();
}
/// A file of a little-endian SARC archive by its exact name
inline bool sarc_file(const std::vector<unsigned char>& a, const char* want, std::vector<unsigned char>& out) {
    auto u16 = [&](size_t o) { return (unsigned)(a[o] | a[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (unsigned)(a[o] | a[o + 1] << 8 | a[o + 2] << 16 | (unsigned)a[o + 3] << 24); };
    if (a.size() < 0x20 || memcmp(a.data(), "SARC", 4) || u16(6) != 0xFEFF) return false;
    const size_t data = u32(0xC), sfat = 0x14, n = u16(sfat + 6), names = sfat + 12 + 16 * n + 8, len = strlen(want);
    for (size_t i = 0; i < n; i++) {
        const size_t node = sfat + 12 + 16 * i;
        if (node + 16 > a.size()) return false;
        const size_t name = names + (size_t)(u32(node + 4) & 0xFFFFFF) * 4;
        if (name > a.size() || a.size() - name < len + 1 || memcmp(a.data() + name, want, len + 1)) continue;   // (with its 0)
        const size_t s0 = u32(node + 8), s1 = u32(node + 12);
        if (data + s1 > a.size() || s1 < s0) return false;
        out.assign(a.begin() + data + s0, a.begin() + data + s1);
        return true;
    }
    return false;
}
/// The OpenType font in a .bfotf (header checked: its size word, the font's own tag); empty when it is not one
inline std::vector<unsigned char> bfotf_font(const std::vector<unsigned char>& raw) {
    if (raw.size() < 16) return {};
    const uint32_t key = ((uint32_t)raw[0] | raw[1] << 8 | raw[2] << 16 | (uint32_t)raw[3] << 24) ^ 0x18029a7fu;
    unsigned char k[4];
    for (int i = 0; i < 4; i++) k[i] = (unsigned char)(key >> (8 * i));
    const uint32_t size = (uint32_t)(raw[4] ^ k[0]) << 24 | (uint32_t)(raw[5] ^ k[1]) << 16 | (uint32_t)(raw[6] ^ k[2]) << 8 |
                          (uint32_t)(raw[7] ^ k[3]);
    if (size != raw.size() - 8) return {};
    std::vector<unsigned char> font(raw.size() - 8);
    for (size_t i = 0; i < font.size(); i++) font[i] = raw[8 + i] ^ k[i & 3];
    if (memcmp(font.data(), "OTTO", 4) && memcmp(font.data(), "\0\1\0\0", 4)) return {};
    return font;
}

/// The game's fonts from a romfs folder: open() once (the dictionary), then read() each
struct Reader {
    std::string romfs;
    ZSTD_DDict* dict = nullptr;
    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader() {
        if (dict) ZSTD_freeDDict(dict);
    }
    bool open(std::string dir) {
        if (dir.empty()) return false;
        if (dir.back() != '/' && dir.back() != '\\') dir += '/';
        romfs = dir;
        std::vector<unsigned char> packed, pack, zsdic;
        if (read_file(romfs + "Pack/ZsDic.pack.zs", packed) && unzstd(packed, nullptr, pack) && sarc_file(pack, "zs.zsdic", zsdic))
            dict = ZSTD_createDDict(zsdic.data(), zsdic.size());
        return dict != nullptr;
    }
    /// A font by name ("ClarimoUDPE-Regular"); empty when the files do not have it
    std::vector<unsigned char> read(const char* name) const {
        std::vector<unsigned char> packed, raw;
        if (!dict || !read_file(romfs + "Font2/Scalable/" + name + ".Nin_Ounce_NVN2.bfotf.zs", packed) || !unzstd(packed, dict, raw))
            return {};
        return bfotf_font(raw);
    }
};

} // namespace mkwvk::game_font
