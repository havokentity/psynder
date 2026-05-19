// SPDX-License-Identifier: MIT
// Psynder — internal lua_State* RAII handle. NOT a public header; only the
// script lane's .cpp files include this. The public `Vm::Get()` indirection
// in script/Script.h is the user-facing contract.

#pragma once

#include "core/Types.h"

// Lua is a C library that uses macros expanding to old-style casts inside
// luaL_Buffer. Silence the warning at the include boundary — our own code
// stays Wold-style-cast clean.
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

#include <string>
#include <string_view>

namespace psynder::script::detail {

// LuaState owns the Lua interpreter. One state per Vm. Not copyable; not
// thread-safe — Lua is single-thread by design (per DESIGN.md §10.5 the VM
// runs on the dedicated game thread, not engine workers).
class LuaState {
public:
    LuaState() = default;
    ~LuaState() { close(); }

    LuaState(const LuaState&)            = delete;
    LuaState& operator=(const LuaState&) = delete;
    LuaState(LuaState&& o) noexcept : L_(o.L_) { o.L_ = nullptr; }
    LuaState& operator=(LuaState&& o) noexcept {
        if (this != &o) {
            close();
            L_ = o.L_;
            o.L_ = nullptr;
        }
        return *this;
    }

    // Opens a fresh interpreter, loads the standard library subset that is
    // safe inside a game thread (no `io.popen`, no `os.execute`). Returns
    // false on allocation failure.
    bool open();
    void close();

    [[nodiscard]] lua_State* handle() const noexcept { return L_; }
    [[nodiscard]] bool       opened() const noexcept { return L_ != nullptr; }

    // Convenience: pcall wrapper. Top of stack must be the function followed
    // by `nargs` arguments. Returns true on success; on failure writes the
    // Lua error message to `err_out` and pops the error value.
    bool pcall(int nargs, int nresults, std::string& err_out);

private:
    lua_State* L_ = nullptr;
};

}  // namespace psynder::script::detail
