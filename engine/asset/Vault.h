// SPDX-License-Identifier: MIT
// Psynder — public runtime asset vault facade.

#pragma once

#include "asset/Vfs.h"

namespace psynder::asset {

// Public asset namespace for runtime code. Vfs stays as the low-level backing
// implementation while samples/apps talk to the Vault API and virtual paths.
class Vault {
   public:
    static Vault& Get() {
        static Vault vault;
        return vault;
    }

    bool mount_vault(std::string_view path) { return Vfs::Get().mount_pak(path); }
    bool mount_archive(std::string_view path) { return mount_vault(path); }
    bool mount_directory(std::string_view path) { return Vfs::Get().mount_directory(path); }

    Blob read(std::string_view virtual_path) { return Vfs::Get().read(virtual_path); }

    void read_async(std::string_view virtual_path,
                    void (*on_loaded)(Blob, void* user) noexcept,
                    void* user) {
        Vfs::Get().read_async(virtual_path, on_loaded, user);
    }

    void watch(std::string_view virtual_path,
               void (*on_changed)(std::string_view path, void* user) noexcept,
               void* user) {
        Vfs::Get().watch(virtual_path, on_changed, user);
    }

    [[nodiscard]] usize mount_count() const noexcept { return Vfs::Get().mount_count(); }

   private:
    Vault() = default;
};

}  // namespace psynder::asset
