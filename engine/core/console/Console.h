// SPDX-License-Identifier: MIT
// Psynder — console / cvar / command registry.
//
// Ported from dmonte (pt::console → psynder::console). Adds, on top of the
// Wave-A scaffold: undo / redo, favourites (f1..fN magic), input history
// (kMaxHistoryDepth, persisted), smart-resolve, bracket-batch transactions,
// cvar archive (SaveArchivedCvars) and load (LoadFromFile), per-allowed-
// value platform tags (CVAR_VALUE_*), and cross-cvar dependency predicates
// (requires_predicate + requires_hint).
//
// Naming: the Wave-A surface uses snake_case (`tokenize_line`,
// `cvar_value_allowed_on_this_platform`). The dmonte port adds PascalCase
// aliases (`TokenizeLine`, `CVarValueAllowedOnThisPlatform`) that forward
// to the snake_case originals, so existing call sites keep working AND
// dmonte-style code is a copy-paste fit.
//
// Hot-path note: `Console::Execute` runs on whatever thread calls it. The
// network / IPC layers should call `QueueExecute` instead, then `Drain()`
// from the main thread per Engine::Tick.

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

    // Dependency-warning suppression (issue #161 in dmonte). Toggled on
    // around cfg replays so a cfg that sets a dependent cvar before its
    // dependency doesn't fire a spurious warning per line.
    void SetSuppressDepWarnings(bool suppress) { dep_warn_suppressed_ = suppress; }
    bool DepWarningsSuppressed() const { return dep_warn_suppressed_; }

    // Smart command resolution (issue #162 in dmonte). When the typed
    // first token isn't an exact match, score every registered name
    // against it and return the top-ranked canonical. Tie-break order
    // matches the autocomplete listing (score desc, length asc, lex).
    struct Resolution {
        std::string canonical_name;                  // non-empty on success
        std::vector<std::string> ambiguous_matches;  // populated when no winner
        bool is_exact_match = false;
    };
    Resolution ResolveCommand(std::string_view typed);

    ExecuteResult Execute(std::string_view line);
    ExecuteResult ExecuteScript(std::string_view body);

    void QueueExecute(std::string line, Responder responder);
    void Drain();

    void EnumerateCVars(std::string_view prefix,
                        const std::function<void(CVar&)>& visitor);
    void EnumerateCommands(std::string_view prefix,
                           const std::function<void(Command&)>& visitor);

    // ─── Persistence ─────────────────────────────────────────────────
    // SaveArchivedCvars writes every CVAR_ARCHIVE cvar whose value
    // differs from its default to `path`, as `<name> <value>` lines
    // (values with spaces are double-quoted). Returns the line count
    // written, or -1 on file error.
    int  SaveArchivedCvars(const std::string& path);

    // LoadFromFile reads `path` as a script body and runs it through
    // ExecuteScript with dependency warnings suppressed. Returns the
    // aggregate ExecuteResult; missing file is reported as error. Lines
    // that don't parse to a known cvar/command emit a warning but don't
    // abort the load.
    ExecuteResult LoadFromFile(const std::string& path);

    std::size_t ResetAllCVarsToDefaults();

    // ─── Undo / Redo ─────────────────────────────────────────────────
    // Each ExecuteScript transaction snapshots the pre-values of every
    // cvar it touched and pushes them as one entry on the undo stack.
    // Stack capped at kMaxHistory entries (oldest dropped FIFO). Undo
    // pops the top and restores; Redo reapplies. Multi-cvar
    // transactions (semicolon-bundle, bracket-batch) come back as a
    // single Undo() call returning multiple entries.
    struct CvarChange {
        std::string name;
        std::string from;   // value before this undo step (was-current)
        std::string to;     // value after this undo step  (restored)
    };
    static constexpr std::size_t kMaxHistory = 50;
    std::vector<CvarChange> Undo();
    std::vector<CvarChange> Redo();
    std::size_t UndoDepth() const { return undo_stack_.size(); }
    std::size_t RedoDepth() const { return redo_stack_.size(); }

    // ─── Favourites (f1..fN magic dispatch) ──────────────────────────
    // AddFavorite stores the line at the next 1-based slot;
    // RemoveFavorite drops the slot at the given index; ClearFavorites
    // wipes the list. Typing `f1` / `f2` / ... `fN` in Execute() runs
    // the saved line at that slot. The interpretation happens AFTER
    // comment-strip / tokenise but BEFORE smart-resolve so a fN token
    // wins decisively. Recursion is guarded by in_fav_dispatch_.
    void AddFavorite(std::string line);
    void RemoveFavorite(std::size_t one_based_index);
    void ClearFavorites();
    std::vector<std::string> Favorites() const { return favorites_; }
    std::size_t FavoriteCount() const { return favorites_.size(); }
    const std::string& LastExecutedLine() const { return last_executed_line_; }
    void SetFavorites(std::vector<std::string> favs) { favorites_ = std::move(favs); }

    // ─── Console input history ───────────────────────────────────────
    // PushHistory appends a submitted line; History() returns the
    // current vector (snapshot for read-only iteration). Persisted by
    // the engine to console_history.txt next to psynder.cfg. Capped at
    // kMaxHistoryDepth entries (oldest dropped FIFO). Empty / dup-of-
    // last entries are rejected.
    static constexpr std::size_t kMaxHistoryDepth = 256;
    void PushHistory(std::string line);
    void ClearHistory();
    std::vector<std::string> History() const { return history_; }
    std::size_t HistoryCount() const { return history_.size(); }
    void SetHistory(std::vector<std::string> hist);

private:
    Console() = default;
    std::map<std::string, CVar, std::less<>>    cvars_;
    std::map<std::string, Command, std::less<>> commands_;

    struct Pending { std::string line; Responder responder; };
    std::mutex            queue_mutex_;
    std::deque<Pending>   queue_;

    // Single transaction snapshot: name -> pre-transaction value.
    using CvarSnapshot = std::map<std::string, std::string>;
    std::deque<CvarSnapshot> undo_stack_;
    std::deque<CvarSnapshot> redo_stack_;
    bool                     in_undo_redo_ = false;

    // Bracket-batch mode (`[` ... `]`).
    std::string              batch_buffer_;
    bool                     batch_active_ = false;

    // Cross-cvar dependency-warning suppression flag.
    bool                     dep_warn_suppressed_ = false;

    // Favourites + recursion guard + last-line tracking.
    std::vector<std::string> favorites_;
    std::string              last_executed_line_;
    bool                     in_fav_dispatch_ = false;

    // Console input history.
    std::vector<std::string> history_;
};

// Quote-aware tokenizer (handles `"a b"` as one token, `\n` / `\t` / `\\`
// escapes inside quotes, stops at `//` comment).
//
// The Wave-A scaffold introduced `tokenize_line` (snake_case); the dmonte
// port adds `TokenizeLine` (PascalCase) as a forwarder so existing call
// sites and dmonte-shaped call sites both link.
std::vector<std::string_view> tokenize_line(std::string_view line,
                                            std::string& storage);
inline std::vector<std::string_view> TokenizeLine(std::string_view line,
                                                  std::string& storage) {
    return tokenize_line(line, storage);
}

// Per-platform helpers. Snake_case is the Wave-A scaffold name; the
// PascalCase versions are the dmonte names and forward to them.
const char* current_platform_name();
bool cvar_value_allowed_on_this_platform(u32 value_flags);
inline bool        CVarValueAllowedOnThisPlatform(u32 value_flags) {
    return cvar_value_allowed_on_this_platform(value_flags);
}
inline const char* CurrentPlatformName() { return current_platform_name(); }

}  // namespace psynder::console

// Registration macros. Variables receive the registered object pointer
// so callers can read GetInt() / GetFloat() / GetBool() at runtime.
// Use C++20's __VA_OPT__ instead of GCC's `, ##__VA_ARGS__` extension --
// __VA_OPT__ is standard so Apple Clang doesn't warn -Wgnu-zero-variadic-
// macro-arguments on every PSY_CVAR call.
#define PSY_CVAR(varname, default_value, description, ...)                     \
    static ::psynder::console::CVar* varname =                                 \
        ::psynder::console::Console::Get().RegisterCVar(                       \
            #varname, default_value, description __VA_OPT__(,) __VA_ARGS__)

#define PSY_COMMAND(varname, description, lambda)                              \
    static ::psynder::console::Command* varname =                              \
        ::psynder::console::Console::Get().RegisterCommand(                    \
            #varname, description, lambda)
