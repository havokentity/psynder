// SPDX-License-Identifier: MIT
// Psynder — console / cvar / command registry.
//
// Lane 01 (core) ports the full dmonte console (cvar archive/load,
// undo/redo, favorites, history, smart-resolve, platform tags) into this
// header. Phase-0 scaffold leaves enough surface that other lanes can call
// PSY_CVAR / PSY_COMMAND right now without blocking; lane 01 then fills in
// real semantics.

#pragma once

#include "../Log.h"
#include "../Types.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace psynder::console {

enum CVarFlag : u32 {
    CVAR_NONE          = 0,
    CVAR_ARCHIVE       = 1u << 0,  // persist to psynder.cfg
    CVAR_READONLY      = 1u << 1,  // engine-set, user cannot mutate
    CVAR_CHEAT         = 1u << 2,  // requires dev_cheats 1
    CVAR_PLATFORM_MAC  = 1u << 3,
    CVAR_PLATFORM_WIN  = 1u << 4,
    CVAR_PLATFORM_LINUX= 1u << 5,
};

enum CVarValueFlag : u32 {
    CVAR_VALUE_ANY   = 0,
    CVAR_VALUE_MAC   = 1u << 0,
    CVAR_VALUE_WIN   = 1u << 1,
    CVAR_VALUE_LINUX = 1u << 2,
};

struct CVar {
    std::string   name;
    std::string   value;
    std::string   default_value;
    std::string   description;
    u32           flags = 0;

    std::vector<std::string> allowed_values;
    std::vector<u32>         allowed_value_flags;

    std::function<bool()> requires_predicate;
    std::string           requires_hint;

    f32 slider_min  = 0.0f;
    f32 slider_max  = 0.0f;
    f32 slider_step = 0.0f;

    std::function<void(const CVar&)> on_change;

    int   GetInt()   const;
    float GetFloat() const;
    bool  GetBool()  const;
};

class Output {
public:
    void Print(std::string_view s) { buf_.append(s.data(), s.size()); }
    void PrintLine(std::string_view s) { Print(s); buf_.push_back('\n'); }

    template <class... Args>
    void Format(fmt::format_string<Args...> f, Args&&... args) {
        Print(fmt::format(f, std::forward<Args>(args)...));
    }
    template <class... Args>
    void FormatLine(fmt::format_string<Args...> f, Args&&... args) {
        PrintLine(fmt::format(f, std::forward<Args>(args)...));
    }
    const std::string& Buffer() const { return buf_; }
private:
    std::string buf_;
};

using CommandCallback =
    std::function<void(std::span<const std::string_view> args, Output& out)>;

struct Command {
    std::string     name;
    std::string     description;
    CommandCallback callback;
    std::string     default_args;
};

struct ExecuteResult {
    bool        ok = true;
    std::string output;
    std::string error;
};

using Responder = std::function<void(const ExecuteResult&)>;

class Console {
public:
    static Console& Get();

    CVar* RegisterCVar(std::string name, std::string default_value,
                       std::string description, u32 flags = 0,
                       std::function<void(const CVar&)> on_change = {});
    Command* RegisterCommand(std::string name, std::string description,
                             CommandCallback callback);

    CVar*    FindCVar(std::string_view name);
    Command* FindCommand(std::string_view name);

    bool SetCVarOverride(std::string_view name, std::string_view value);

    ExecuteResult Execute(std::string_view line);
    ExecuteResult ExecuteScript(std::string_view body);

    void QueueExecute(std::string line, Responder responder);
    void Drain();

    void EnumerateCVars(std::string_view prefix,
                        const std::function<void(CVar&)>& visitor);
    void EnumerateCommands(std::string_view prefix,
                           const std::function<void(Command&)>& visitor);

    // Persistence (lane 01 port-in-progress)
    int SaveArchivedCvars(const std::string& path);
    std::size_t ResetAllCVarsToDefaults();

private:
    Console() = default;
    std::map<std::string, CVar, std::less<>>    cvars_;
    std::map<std::string, Command, std::less<>> commands_;
    struct Pending { std::string line; Responder responder; };
    std::mutex            queue_mutex_;
    std::deque<Pending>   queue_;
};

std::vector<std::string_view> tokenize_line(std::string_view line,
                                            std::string& storage);

const char* current_platform_name();
bool cvar_value_allowed_on_this_platform(u32 value_flags);

}  // namespace psynder::console

// Registration macros. Variable receives the registered object pointer so
// callers can read GetInt()/GetFloat()/GetBool() at runtime.
#define PSY_CVAR(varname, default_value, description, ...)                     \
    static ::psynder::console::CVar* varname =                                 \
        ::psynder::console::Console::Get().RegisterCVar(                       \
            #varname, default_value, description __VA_OPT__(,) __VA_ARGS__)

#define PSY_COMMAND(varname, description, lambda)                              \
    static ::psynder::console::Command* varname =                              \
        ::psynder::console::Console::Get().RegisterCommand(                    \
            #varname, description, lambda)
