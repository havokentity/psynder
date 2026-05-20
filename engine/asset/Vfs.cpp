// SPDX-License-Identifier: MIT
// Psynder — virtual file system + .lmpak archive reader (Wave-A).
//
// What this file owns:
//   - Mount table: ordered list of directories and .lmpak archives.
//                  Search order is reverse (last mount wins) so the
//                  dev-time loose-directory overlay shadows the shipped
//                  pak. Matches the "developer override" pattern in the
//                  public header docs.
//   - mmap-based .lmpak reader. Uncompressed entries serve Blob views
//                  directly into the mapping (zero copy). Zstd entries
//                  decompress into a per-Vfs arena and Blob points at
//                  that.
//   - Async loader. Submits a job that runs `read()` then fires the
//                  user's callback on the worker. Currently the job
//                  system runs jobs synchronously (Phase-0 stub from
//                  lane 04), so on_loaded fires before submit returns;
//                  once lane 04 lands the work-stealing pool this code
//                  picks it up unchanged.
//   - Hot-reload watcher (dev builds only). Stat-polls every 250 ms on
//                  a background thread; fires `on_changed` from that
//                  thread. The public contract says "between frames" —
//                  the engine driver decides when to drain.
//
// Why a Pimpl-style file-scope state struct: the public `Vfs` declared
// in Vfs.h is frozen — no extra members, no destructor. We park the
// real state in a file-scope object whose destructor runs at process
// exit and joins the watcher thread cleanly.

#include "Vfs.h"

#include "LmpakFormat.h"
#include "core/Diag.h"
#include "core/Log.h"
#include "jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if __has_include(<zstd.h>)
#include <zstd.h>
#define PSYNDER_ASSET_HAS_ZSTD 1
#else
#define PSYNDER_ASSET_HAS_ZSTD 0
#endif

namespace psynder::asset {

using lmpak::LmpakEntry;
using lmpak::LmpakHeader;

// ─── Internal types ──────────────────────────────────────────────────────
namespace {

namespace fs = std::filesystem;

// Normalize a virtual path the same way the cooker did: forward slashes,
// lowercase ASCII. The hash uses the same normalization rule.
std::string normalize_path(std::string_view in) {
    std::string s;
    s.reserve(in.size());
    for (char c : in) {
        if (c == '\\')
            c = '/';
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
        s.push_back(c);
    }
    return s;
}

u64 hash_normalized(std::string_view norm) {
    return lmpak::fnv1a_path(norm.data(), norm.size());
}

// A platform-portable read-only memory mapping. The destructor unmaps.
struct Mmap {
    const u8* base = nullptr;
    usize bytes = 0;
#if defined(_WIN32)
    HANDLE file_handle = INVALID_HANDLE_VALUE;
    HANDLE map_handle = nullptr;
#else
    int fd = -1;
#endif

    Mmap() = default;
    Mmap(const Mmap&) = delete;
    Mmap& operator=(const Mmap&) = delete;
    Mmap(Mmap&& o) noexcept { swap(o); }
    Mmap& operator=(Mmap&& o) noexcept {
        if (this != &o) {
            reset();
            swap(o);
        }
        return *this;
    }
    ~Mmap() { reset(); }

    void swap(Mmap& o) noexcept {
        std::swap(base, o.base);
        std::swap(bytes, o.bytes);
#if defined(_WIN32)
        std::swap(file_handle, o.file_handle);
        std::swap(map_handle, o.map_handle);
#else
        std::swap(fd, o.fd);
#endif
    }

    void reset() noexcept {
#if defined(_WIN32)
        if (base) {
            ::UnmapViewOfFile(base);
            base = nullptr;
        }
        if (map_handle) {
            ::CloseHandle(map_handle);
            map_handle = nullptr;
        }
        if (file_handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(file_handle);
            file_handle = INVALID_HANDLE_VALUE;
        }
#else
        if (base && bytes) {
            ::munmap(const_cast<u8*>(base), bytes);
        }
        base = nullptr;
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
#endif
        bytes = 0;
    }

    static bool open(const fs::path& p, Mmap& out) {
#if defined(_WIN32)
        HANDLE fh = ::CreateFileW(p.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
        if (fh == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(fh, &size) || size.QuadPart <= 0) {
            ::CloseHandle(fh);
            return false;
        }
        HANDLE mh = ::CreateFileMappingW(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mh) {
            ::CloseHandle(fh);
            return false;
        }
        void* view = ::MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0);
        if (!view) {
            ::CloseHandle(mh);
            ::CloseHandle(fh);
            return false;
        }
        out.base = static_cast<const u8*>(view);
        out.bytes = static_cast<usize>(size.QuadPart);
        out.file_handle = fh;
        out.map_handle = mh;
        return true;
#else
        int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return false;
        struct stat st {};
        if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
            ::close(fd);
            return false;
        }
        void* m = ::mmap(nullptr, static_cast<usize>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (m == MAP_FAILED) {
            ::close(fd);
            return false;
        }
        out.base = static_cast<const u8*>(m);
        out.bytes = static_cast<usize>(st.st_size);
        out.fd = fd;
        return true;
#endif
    }
};

// A parsed .lmpak. The `mmap` owns the file mapping for its lifetime;
// `entries` / `names` are pointers into that mapping for the canonical
// writer (LmpakWriter / lane 05) layout. For the alternate layout emitted
// by `tools/lm_pak` we materialize the entry table + name pool into the
// owned `entries_owned` / `names_owned` buffers and point at those — same
// in-memory shape, different on-disk dialect.
struct PakMount {
    fs::path archive_path;
    Mmap map;
    const LmpakEntry* entries = nullptr;
    u32 entry_count = 0;
    const char* names = nullptr;  // base of name pool
    usize names_size = 0;
    bool sorted = false;

    // Owned storage for the tools/lm_pak dialect. Populated only when
    // `parse_lmpak_tools` succeeds; left empty for the canonical layout.
    std::vector<LmpakEntry> entries_owned;
    std::vector<char> names_owned;
};

struct DirMount {
    fs::path root;
};

struct Mount {
    enum class Kind { Directory, Pak };
    Kind kind = Kind::Directory;
    std::unique_ptr<DirMount> dir;
    std::unique_ptr<PakMount> pak;
};

// A watcher target. We store the resolved on-disk path and re-stat
// every poll cycle. The watcher fires on three transitions:
//   - mtime advance on an existing file
//   - file disappears (rename / delete) on something that previously existed
//   - file reappears at the watched vpath after a disappearance
//
// `valid` becomes true after the first poll records a baseline. `exists`
// tracks the most recently observed presence so we only fire delete /
// recreate edges, not every poll while the file is missing.
struct WatchTarget {
    std::string vpath;
    fs::path resolved;
    fs::file_time_type last_write{};
    // Track size alongside mtime: a rename-into-place on Windows/NTFS can
    // restore the *original* file's timestamp (filesystem tunneling), so an
    // mtime-only check misses content swaps where the new file happens to
    // land on the old mtime. Size advances catch those.
    std::uintmax_t last_size = 0;
    void (*cb)(std::string_view, void*) noexcept = nullptr;
    void* user = nullptr;
    bool valid = false;
    bool exists = false;
};

// ─── File-scope state ────────────────────────────────────────────────────
// Defined as a function-local static so its destructor runs at exit and
// can stop the watcher thread without changing the public header.
struct VfsState {
    std::mutex mtx;
    std::vector<Mount> mounts;

    // Backing storage for Blobs that don't live in the mmap (loose
    // files + zstd-decompressed pak entries). The vector retains every
    // buffer for the program lifetime; OK because asset reads happen
    // a bounded number of times per level and the level-scope allocator
    // resets between levels in higher-level lanes.
    std::vector<std::unique_ptr<u8[]>> backing;
    std::vector<usize> backing_sizes;

    // Hot-reload watcher state.
    std::vector<WatchTarget> watchers;
    std::thread watch_thread;
    std::atomic<bool> watch_stop{false};
    std::atomic<bool> watch_started{false};
    std::condition_variable watch_cv;
    std::mutex watch_mtx;
};

// Stop + join helper; safe to call from any thread.
void stop_watcher_locked(VfsState& s) {
    if (s.watch_started.load(std::memory_order_acquire)) {
        s.watch_stop.store(true, std::memory_order_release);
        s.watch_cv.notify_all();
        if (s.watch_thread.joinable())
            s.watch_thread.join();
        s.watch_started.store(false, std::memory_order_release);
        s.watch_stop.store(false, std::memory_order_release);
    }
}

// Wrap VfsState in a struct that joins the watcher thread on its own
// destruction. Living inside the State() Meyer's singleton means the
// thread is joined BEFORE the inner mutex / condvar are destroyed, no
// matter what other statics in other TUs do at shutdown.
struct VfsStateOwner {
    VfsState state;
    ~VfsStateOwner() { stop_watcher_locked(state); }
};

VfsState& State() {
    static VfsStateOwner owner;
    return owner.state;
}

// ─── .lmpak parse helpers ────────────────────────────────────────────────
//
// Two on-disk dialects share the same magic+version word but disagree on
// header layout after byte 16:
//
//   Canonical (LmpakWriter, lane 05): 64-byte header with explicit
//     entry_table_offset, name_table_offset, name_table_size,
//     blob_section_offset, blob_section_size, build_unix_time. Layout on
//     disk is [hdr | payloads (8-byte aligned) | entry_table | name_pool].
//     This is what `parse_lmpak_canonical` below understands.
//
//   Tools (tools/lm_pak/Lmpak.cpp): 56-byte header followed by 24 bytes of
//     reserved zero. Carries only index_offset + index_bytes; the index
//     region holds a packed array of 48-byte records (hash, offset, stored,
//     raw, path_off, path_len, flags, crc32) followed by the path-string
//     blob. Payloads are tightly packed (no 8-byte alignment). Parsed by
//     `parse_lmpak_tools`, which materializes a canonical LmpakEntry[] +
//     name pool into PakMount's owned buffers so the rest of the reader is
//     dialect-agnostic.
//
// Detection: try the canonical interpretation first; if any sanity check
// fails, fall through to the tools dialect. The discriminator is robust
// because the tools header zero-fills bytes 32..55, which makes the
// canonical reading see name_table_size=0 / blob_section_offset=0 — both
// invalid for the canonical layout whenever entry_count > 0 (and even for
// entry_count=0, entry_table_offset would equal kToolsHeaderSize=56 < the
// canonical 64-byte header size, also failing the canonical check).

// Tools-dialect constants — kept local to this TU so we don't reach across
// into the lane-24 namespace (which would force a header dependency).
constexpr usize kToolsHeaderSize = 56;
constexpr usize kToolsRecordBytes = 48;
constexpr u32 kToolsEntryZstd = 1u << 0;

template <class T>
bool read_le_local(const u8* base, usize bytes, usize offset, T& out) {
    if (offset + sizeof(T) > bytes)
        return false;
    using U = std::make_unsigned_t<T>;
    U u = 0;
    for (usize i = 0; i < sizeof(T); ++i) {
        u |= static_cast<U>(base[offset + i]) << (8 * i);
    }
    out = static_cast<T>(u);
    return true;
}

bool parse_lmpak_canonical(PakMount& m) {
    if (m.map.bytes < sizeof(LmpakHeader))
        return false;

    LmpakHeader hdr{};
    std::memcpy(&hdr, m.map.base, sizeof(LmpakHeader));
    if (hdr.magic != lmpak::kMagic)
        return false;
    if (hdr.version != lmpak::kVersion)
        return false;

    const u64 et_off = hdr.entry_table_offset;
    const u64 et_end = et_off + u64(hdr.entry_count) * sizeof(LmpakEntry);
    if (et_off < sizeof(LmpakHeader) || et_end > m.map.bytes)
        return false;

    const u64 nt_off = hdr.name_table_offset;
    const u64 nt_end = nt_off + hdr.name_table_size;
    if (nt_off < sizeof(LmpakHeader) || nt_end > m.map.bytes)
        return false;
    if (hdr.entry_count > 0 && hdr.name_table_size == 0)
        return false;

    m.entries = reinterpret_cast<const LmpakEntry*>(m.map.base + et_off);
    m.entry_count = hdr.entry_count;
    m.names = reinterpret_cast<const char*>(m.map.base + nt_off);
    m.names_size = static_cast<usize>(hdr.name_table_size);
    m.sorted = (hdr.flags & lmpak::kFlagSorted) != 0;

    // Range-check every entry payload.
    for (u32 i = 0; i < m.entry_count; ++i) {
        const LmpakEntry& e = m.entries[i];
        if (e.offset + e.size > m.map.bytes)
            return false;
        if (u64(e.name_offset) + u64(e.name_len) > hdr.name_table_size)
            return false;
    }
    return true;
}

// Parse the tools/lm_pak dialect. On success, fills the owned buffers in
// `m` with an entry table and name pool in canonical (LmpakEntry / lane
// 05) shape and points `m.entries` / `m.names` at them.
bool parse_lmpak_tools(PakMount& m) {
    const u8* const base = m.map.base;
    const usize bytes = m.map.bytes;
    if (bytes < kToolsHeaderSize)
        return false;

    u32 magic = 0;
    if (!read_le_local<u32>(base, bytes, 0, magic) || magic != lmpak::kMagic)
        return false;
    u32 version = 0;
    if (!read_le_local<u32>(base, bytes, 4, version) || version != lmpak::kVersion)
        return false;

    u32 archive_flags = 0;
    u32 entry_count = 0;
    u64 index_offset = 0;
    u64 index_bytes_field = 0;
    if (!read_le_local<u32>(base, bytes, 8, archive_flags))
        return false;
    if (!read_le_local<u32>(base, bytes, 12, entry_count))
        return false;
    if (!read_le_local<u64>(base, bytes, 16, index_offset))
        return false;
    if (!read_le_local<u64>(base, bytes, 24, index_bytes_field))
        return false;

    if (index_offset < kToolsHeaderSize)
        return false;
    if (index_offset + index_bytes_field > bytes)
        return false;
    const u64 records_bytes = u64(entry_count) * kToolsRecordBytes;
    if (records_bytes > index_bytes_field)
        return false;

    const usize cursor_base = static_cast<usize>(index_offset);
    const usize string_blob_off = cursor_base + static_cast<usize>(records_bytes);
    const u64 string_blob_bytes = index_bytes_field - records_bytes;
    if (string_blob_off + string_blob_bytes > bytes)
        return false;

    // Walk the records, build canonical entries + name pool.
    m.entries_owned.clear();
    m.entries_owned.reserve(entry_count);
    m.names_owned.clear();
    m.names_owned.reserve(static_cast<usize>(string_blob_bytes) + entry_count);

    bool sorted_check = true;
    u64 prev_hash = 0;

    for (u32 i = 0; i < entry_count; ++i) {
        const usize rec = cursor_base + static_cast<usize>(i) * kToolsRecordBytes;
        u64 hash = 0, offset = 0, stored = 0, raw = 0;
        u32 path_off = 0, path_len = 0, eflags = 0, crc32 = 0;
        if (!read_le_local<u64>(base, bytes, rec + 0, hash))
            return false;
        if (!read_le_local<u64>(base, bytes, rec + 8, offset))
            return false;
        if (!read_le_local<u64>(base, bytes, rec + 16, stored))
            return false;
        if (!read_le_local<u64>(base, bytes, rec + 24, raw))
            return false;
        if (!read_le_local<u32>(base, bytes, rec + 32, path_off))
            return false;
        if (!read_le_local<u32>(base, bytes, rec + 36, path_len))
            return false;
        if (!read_le_local<u32>(base, bytes, rec + 40, eflags))
            return false;
        if (!read_le_local<u32>(base, bytes, rec + 44, crc32))
            return false;
        (void)crc32;

        if (offset + stored > bytes)
            return false;
        if (u64(path_off) + u64(path_len) > string_blob_bytes)
            return false;
        if (path_len > 0xFFFFu)
            return false;  // doesn't fit in LmpakEntry::name_len

        // Reserve a slot in the owned name pool: copy path bytes + NUL.
        const u32 dst_off = static_cast<u32>(m.names_owned.size());
        m.names_owned.insert(m.names_owned.end(),
                             reinterpret_cast<const char*>(base + string_blob_off + path_off),
                             reinterpret_cast<const char*>(base + string_blob_off + path_off +
                                                           path_len));
        m.names_owned.push_back('\0');

        LmpakEntry ent{};
        ent.name_hash = hash;
        ent.offset = offset;
        ent.size = stored;
        // tools writer records `raw` = uncompressed size; if the entry is
        // uncompressed the writer sets stored == raw, so this is correct
        // either way and matches LmpakEntry::uncompressed semantics.
        ent.uncompressed = raw;
        ent.name_offset = dst_off;
        ent.name_len = static_cast<u16>(path_len);
        ent.flags = (eflags & kToolsEntryZstd) ? lmpak::kEntryZstd : u16{0};
        m.entries_owned.push_back(ent);

        if (i > 0 && hash < prev_hash)
            sorted_check = false;
        prev_hash = hash;
    }

    m.entries = m.entries_owned.data();
    m.entry_count = entry_count;
    m.names = m.names_owned.data();
    m.names_size = m.names_owned.size();
    // The tools writer always sorts records by hash before emitting, but
    // verify locally so a hand-tweaked archive doesn't break find_in_pak.
    m.sorted = sorted_check;
    return true;
}

bool parse_lmpak(PakMount& m) {
    if (parse_lmpak_canonical(m))
        return true;
    // Canonical parse failed — wipe any partial state and retry as tools dialect.
    m.entries = nullptr;
    m.entry_count = 0;
    m.names = nullptr;
    m.names_size = 0;
    m.sorted = false;
    return parse_lmpak_tools(m);
}

const LmpakEntry* find_in_pak(const PakMount& m, u64 path_hash) {
    if (!m.entries || m.entry_count == 0)
        return nullptr;
    if (m.sorted) {
        u32 lo = 0;
        u32 hi = m.entry_count;
        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2;
            u64 h = m.entries[mid].name_hash;
            if (h < path_hash)
                lo = mid + 1;
            else if (h > path_hash)
                hi = mid;
            else
                return &m.entries[mid];
        }
        return nullptr;
    }
    for (u32 i = 0; i < m.entry_count; ++i) {
        if (m.entries[i].name_hash == path_hash)
            return &m.entries[i];
    }
    return nullptr;
}

// Append `bytes` of `data` to the backing store and return a stable view.
Blob park_blob(VfsState& s, std::unique_ptr<u8[]> buf, usize bytes) {
    const u8* ptr = buf.get();
    s.backing.push_back(std::move(buf));
    s.backing_sizes.push_back(bytes);
    return {ptr, bytes};
}

Blob load_from_dir(VfsState& s, const DirMount& d, std::string_view norm_path) {
    fs::path full = d.root / fs::path(std::string(norm_path));
    std::error_code ec;
    if (!fs::is_regular_file(full, ec))
        return {};
    auto sz = fs::file_size(full, ec);
    if (ec)
        return {};
    auto buf = std::unique_ptr<u8[]>(new u8[sz ? sz : 1]);
    FILE* fp = std::fopen(full.string().c_str(), "rb");
    if (!fp)
        return {};
    usize read = std::fread(buf.get(), 1, sz, fp);
    std::fclose(fp);
    if (read != sz)
        return {};
    return park_blob(s, std::move(buf), sz);
}

Blob load_from_pak(VfsState& s, const PakMount& m, const LmpakEntry& e) {
    const u8* payload = m.map.base + e.offset;
    if ((e.flags & lmpak::kEntryZstd) == 0) {
        // Uncompressed: serve the mapping directly. Zero copy.
        return {payload, static_cast<usize>(e.size)};
    }
#if PSYNDER_ASSET_HAS_ZSTD
    auto buf = std::unique_ptr<u8[]>(new u8[e.uncompressed ? e.uncompressed : 1]);
    usize dec = ZSTD_decompress(buf.get(), e.uncompressed, payload, e.size);
    if (ZSTD_isError(dec) || dec != e.uncompressed) {
        ::psynder::log::warn("[asset] zstd decompress failed for entry hash={:#x}", e.name_hash);
        return {};
    }
    return park_blob(s, std::move(buf), e.uncompressed);
#else
    (void)s;
    (void)e;
    ::psynder::log::warn("[asset] entry is zstd-compressed but build lacks zstd support");
    return {};
#endif
}

// Walk mounts in reverse so later overlays shadow earlier ones.
// On a hit, returns the Blob and (if `out_resolved` is non-null) the
// on-disk path for watchers.
Blob lookup(VfsState& s, std::string_view vpath, fs::path* out_resolved) {
    const std::string norm = normalize_path(vpath);
    const u64 h = hash_normalized(norm);

    for (auto it = s.mounts.rbegin(); it != s.mounts.rend(); ++it) {
        switch (it->kind) {
            case Mount::Kind::Directory: {
                const auto& d = *it->dir;
                fs::path full = d.root / fs::path(norm);
                std::error_code ec;
                if (fs::is_regular_file(full, ec)) {
                    Blob b = load_from_dir(s, d, norm);
                    if (b.data && out_resolved)
                        *out_resolved = full;
                    if (b.data)
                        return b;
                }
                break;
            }
            case Mount::Kind::Pak: {
                const auto& p = *it->pak;
                const LmpakEntry* e = find_in_pak(p, h);
                if (e) {
                    Blob b = load_from_pak(s, p, *e);
                    if (b.data && out_resolved)
                        *out_resolved = p.archive_path;
                    if (b.data)
                        return b;
                }
                break;
            }
        }
    }
    return {};
}

// ─── Async loader job thunk ──────────────────────────────────────────────

struct AsyncReq {
    std::string vpath;
    void (*cb)(Blob, void*) noexcept = nullptr;
    void* user = nullptr;
};

void async_job_fn(void* user) noexcept {
    auto* req = static_cast<AsyncReq*>(user);
    Blob b = Vfs::Get().read(req->vpath);
    if (req->cb)
        req->cb(b, req->user);
    delete req;
}

// ─── Watcher thread loop ─────────────────────────────────────────────────

// Forward decl — the watcher poll re-resolves vpaths every cycle so
// rename / delete edges fire even after the mount layout shifts. The
// definition lives below to keep mount helpers grouped.
fs::path resolve_for_watch(VfsState& s, std::string_view vpath);

// Run one poll pass over every registered watcher. Shared between the
// background thread and the in-line `poll_watchers_now()` test hook so
// both paths obey the same delete/rename/recreate semantics.
//
// State machine per target:
//   (initial)                 valid=false, exists=?
//     → first poll, file is regular file       → valid=true, exists=true,
//                                                last_write=now, NO fire
//     → first poll, file is missing            → valid=true, exists=false,
//                                                NO fire (baseline says
//                                                "absent")
//   (valid && exists == true)
//     → poll says missing      → fire (delete/rename), exists=false
//     → poll says mtime moved  → fire, last_write=now
//   (valid && exists == false)
//     → poll says file is back → fire (re-create), exists=true,
//                                last_write=now
//
// If the underlying mount layout changes (e.g. a directory mount that
// previously shadowed a pak is unmounted), we re-resolve every cycle.
// That lets a "rename to a different mount" event still fire as a logical
// change of `vpath`'s backing file.
void poll_watchers_once(VfsState& s) {
    std::vector<WatchTarget> snapshot;
    {
        std::lock_guard<std::mutex> g(s.mtx);
        snapshot = s.watchers;
    }

    for (auto& w : snapshot) {
        // Re-resolve the vpath: a delete or a rename within a mounted
        // directory means `resolve_for_watch` returns empty (or a
        // different path). The resolved path is the live ground truth.
        fs::path live_path;
        {
            std::lock_guard<std::mutex> g(s.mtx);
            live_path = resolve_for_watch(s, w.vpath);
        }
        const bool has_live = !live_path.empty();

        std::error_code ec;
        fs::file_time_type t{};
        std::uintmax_t sz = 0;
        bool live_exists = false;
        if (has_live) {
            t = fs::last_write_time(live_path, ec);
            // If the path resolved but the stat just-now failed (e.g.
            // race with rename), treat it as missing this cycle.
            live_exists = !ec;
            if (live_exists) {
                std::error_code sec;
                sz = fs::file_size(live_path, sec);
                if (sec)
                    sz = 0;
            }
        }

        if (!w.valid) {
            // Baseline pass: record current state without firing.
            std::lock_guard<std::mutex> g(s.mtx);
            for (auto& real : s.watchers) {
                if (real.vpath == w.vpath && real.user == w.user) {
                    real.valid = true;
                    real.exists = live_exists;
                    if (live_exists) {
                        real.last_write = t;
                        real.last_size = sz;
                        real.resolved = live_path;
                    }
                    break;
                }
            }
            continue;
        }

        // Three edges:
        bool should_fire = false;
        bool new_exists = w.exists;
        fs::file_time_type new_mtime = w.last_write;
        std::uintmax_t new_size = w.last_size;
        fs::path new_path = w.resolved;

        if (w.exists && !live_exists) {
            // Disappeared: rename-away or delete.
            should_fire = true;
            new_exists = false;
        } else if (!w.exists && live_exists) {
            // Re-created at the watched vpath.
            should_fire = true;
            new_exists = true;
            new_mtime = t;
            new_size = sz;
            new_path = live_path;
        } else if (live_exists && (t != w.last_write || sz != w.last_size)) {
            // Content changed: mtime advanced, or size changed even when the
            // mtime didn't (NTFS rename-into-place tunneling).
            should_fire = true;
            new_mtime = t;
            new_size = sz;
            new_path = live_path;
        } else if (live_exists && live_path != w.resolved) {
            // The vpath now resolves through a different mount — treat as
            // a change so consumers can re-load.
            should_fire = true;
            new_mtime = t;
            new_size = sz;
            new_path = live_path;
        }

        if (should_fire) {
            {
                std::lock_guard<std::mutex> g(s.mtx);
                for (auto& real : s.watchers) {
                    if (real.vpath == w.vpath && real.user == w.user) {
                        real.exists = new_exists;
                        real.last_write = new_mtime;
                        real.last_size = new_size;
                        real.resolved = new_path;
                        break;
                    }
                }
            }
            if (w.cb)
                w.cb(w.vpath, w.user);
        }
    }
}

void watcher_loop(VfsState* sp) {
    using namespace std::chrono_literals;
    auto& s = *sp;
    while (!s.watch_stop.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(s.watch_mtx);
            s.watch_cv.wait_for(lk, 250ms, [&] {
                return s.watch_stop.load(std::memory_order_acquire);
            });
            if (s.watch_stop.load(std::memory_order_acquire))
                break;
        }
        poll_watchers_once(s);
    }
}

void ensure_watcher_started(VfsState& s) {
    bool expected = false;
    if (s.watch_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        s.watch_stop.store(false, std::memory_order_release);
        s.watch_thread = std::thread(watcher_loop, &s);
    }
}

// Resolve `vpath` to the on-disk path of the mount that currently
// provides it. Returns empty path if nothing matches.
fs::path resolve_for_watch(VfsState& s, std::string_view vpath) {
    const std::string norm = normalize_path(vpath);
    const u64 h = hash_normalized(norm);
    for (auto it = s.mounts.rbegin(); it != s.mounts.rend(); ++it) {
        switch (it->kind) {
            case Mount::Kind::Directory: {
                fs::path full = it->dir->root / fs::path(norm);
                std::error_code ec;
                if (fs::is_regular_file(full, ec))
                    return full;
                break;
            }
            case Mount::Kind::Pak: {
                if (find_in_pak(*it->pak, h))
                    return it->pak->archive_path;
                break;
            }
        }
    }
    return {};
}

}  // namespace

// ─── Public Vfs surface ──────────────────────────────────────────────────

Vfs& Vfs::Get() {
    static Vfs v;
    return v;
}

bool Vfs::mount_pak(std::string_view path) {
    auto& s = State();
    auto pak = std::make_unique<PakMount>();
    pak->archive_path = fs::path(std::string(path));
    if (!Mmap::open(pak->archive_path, pak->map)) {
        ::psynder::log::warn("[asset] mount_pak: failed to open {}", pak->archive_path.string());
        return false;
    }
    if (!parse_lmpak(*pak)) {
        ::psynder::log::warn("[asset] mount_pak: not a valid .lmpak v1 archive: {}",
                             pak->archive_path.string());
        return false;
    }
    Mount m;
    m.kind = Mount::Kind::Pak;
    m.pak = std::move(pak);
    std::lock_guard<std::mutex> g(s.mtx);
    s.mounts.push_back(std::move(m));
    PSY_DIAG_TIER1("asset", "mounted pak with {} entries", s.mounts.back().pak->entry_count);
    return true;
}

bool Vfs::mount_directory(std::string_view path) {
    auto& s = State();
    fs::path p{std::string(path)};
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        ::psynder::log::warn("[asset] mount_directory: not a directory: {}", p.string());
        return false;
    }
    Mount m;
    m.kind = Mount::Kind::Directory;
    m.dir = std::make_unique<DirMount>();
    m.dir->root = std::move(p);
    std::lock_guard<std::mutex> g(s.mtx);
    s.mounts.push_back(std::move(m));
    PSY_DIAG_TIER1("asset", "mounted directory {}", s.mounts.back().dir->root.string());
    return true;
}

Blob Vfs::read(std::string_view virtual_path) {
    auto& s = State();
    std::lock_guard<std::mutex> g(s.mtx);
    return lookup(s, virtual_path, nullptr);
}

void Vfs::read_async(std::string_view virtual_path, void (*on_loaded)(Blob, void*) noexcept, void* user) {
    auto* req = new AsyncReq{std::string(virtual_path), on_loaded, user};
    ::psynder::jobs::JobDesc desc{};
    desc.fn = &async_job_fn;
    desc.user = req;
    desc.name = "asset.read_async";
    ::psynder::jobs::JobSystem::Get().submit(desc);
}

void Vfs::watch(std::string_view virtual_path,
                void (*on_changed)(std::string_view, void*) noexcept,
                void* user) {
    auto& s = State();
    fs::path resolved;
    {
        std::lock_guard<std::mutex> g(s.mtx);
        resolved = resolve_for_watch(s, virtual_path);
        if (resolved.empty()) {
            ::psynder::log::warn("[asset] watch: {} not provided by any mount",
                                 std::string(virtual_path));
            return;
        }
        WatchTarget w{};
        w.vpath = std::string(virtual_path);
        w.resolved = std::move(resolved);
        w.cb = on_changed;
        w.user = user;
        w.valid = false;
        w.exists = false;  // set by the first poll
        s.watchers.push_back(std::move(w));
    }
    ensure_watcher_started(s);
}

usize Vfs::mount_count() const noexcept {
    auto& s = State();
    std::lock_guard<std::mutex> g(s.mtx);
    return s.mounts.size();
}

// ─── Internal test surface ───────────────────────────────────────────────
namespace internal {

void reset_for_tests() {
    auto& s = State();
    stop_watcher_locked(s);
    std::lock_guard<std::mutex> g(s.mtx);
    s.mounts.clear();
    s.backing.clear();
    s.backing_sizes.clear();
    s.watchers.clear();
}

bool watcher_thread_running() {
    return State().watch_started.load(std::memory_order_acquire);
}

void poll_watchers_now() {
    poll_watchers_once(State());
}

}  // namespace internal

}  // namespace psynder::asset
