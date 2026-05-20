// SPDX-License-Identifier: MIT
// Psynder — virtual file system + .lmpak reader. Lane 05 owns.

#pragma once

#include "core/Types.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psynder::asset {

struct AssetTag {};
using AssetId = Handle<AssetTag>;

struct Blob {
    const u8* data = nullptr;
    usize bytes = 0;
};

class Vfs {
   public:
    static Vfs& Get();

    // Mount a .lmpak archive or a loose directory. Multiple mounts are searched
    // in mount order; later mounts shadow earlier ones (developer override pattern).
    bool mount_pak(std::string_view path);
    bool mount_directory(std::string_view path);

    // Synchronous read.
    Blob read(std::string_view virtual_path);

    // Async read; the on_loaded callback runs on a worker.
    void read_async(std::string_view virtual_path,
                    void (*on_loaded)(Blob, void* user) noexcept,
                    void* user);

    // Hot-reload watch (dev builds only).
    void watch(std::string_view virtual_path,
               void (*on_changed)(std::string_view path, void* user) noexcept,
               void* user);

    // Diagnostics
    usize mount_count() const noexcept;

   private:
    Vfs() = default;
};

}  // namespace psynder::asset
