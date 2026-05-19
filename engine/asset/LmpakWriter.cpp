// SPDX-License-Identifier: MIT
// Psynder — .lmpak writer support: thin zstd compression helper used by
// tools/lm_pak (lane 24) and in-lane unit tests. The struct layout and
// hashing logic stay in the header so the cooker can use them too, but
// the actual zstd call needs the optional dep so it lives here.

#include "LmpakWriter.h"

#if __has_include(<zstd.h>)
#   include <zstd.h>
#   define PSYNDER_ASSET_HAS_ZSTD 1
#else
#   define PSYNDER_ASSET_HAS_ZSTD 0
#endif

#include <vector>

namespace psynder::asset::lmpak {

bool zstd_available() noexcept {
    return PSYNDER_ASSET_HAS_ZSTD != 0;
}

bool zstd_compress(const u8* src, usize src_len, int level, std::vector<u8>& out) {
#if PSYNDER_ASSET_HAS_ZSTD
    usize bound = ZSTD_compressBound(src_len);
    out.assign(bound, 0);
    usize n = ZSTD_compress(out.data(), bound, src, src_len, level);
    if (ZSTD_isError(n)) {
        out.clear();
        return false;
    }
    out.resize(n);
    return true;
#else
    (void)src;
    (void)src_len;
    (void)level;
    out.clear();
    return false;
#endif
}

}  // namespace psynder::asset::lmpak
