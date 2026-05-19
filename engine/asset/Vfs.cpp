// SPDX-License-Identifier: MIT
// Psynder — asset VFS stub. Lane 05 fleshes out the .lmpak reader,
// async-loader job submission, and hot-reload file-system watcher.

#include "Vfs.h"

namespace psynder::asset {

Vfs& Vfs::Get() {
    static Vfs v;
    return v;
}

bool Vfs::mount_pak(std::string_view /*path*/)        { return false; }
bool Vfs::mount_directory(std::string_view /*path*/)  { return false; }
Blob Vfs::read(std::string_view /*virtual_path*/)      { return {}; }

void Vfs::read_async(std::string_view /*virtual_path*/,
                     void (*/*on_loaded*/)(Blob, void*) noexcept,
                     void* /*user*/) {}

void Vfs::watch(std::string_view /*virtual_path*/,
                void (*/*on_changed*/)(std::string_view, void*) noexcept,
                void* /*user*/) {}

usize Vfs::mount_count() const noexcept { return 0; }

}  // namespace psynder::asset
