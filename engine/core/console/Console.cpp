// SPDX-License-Identifier: MIT
// Psynder — console impl. Full port of dmonte's
// `pt::console::Console` (engine/core/console/Console.cpp in the
// PathTracer source tree). The public surface stays compatible with the
// Wave-A scaffold; the dmonte-only behaviour (undo/redo, favourites,
// history, smart-resolve, bracket-batch, platform tags, requires_predicate)
// rides on top.
//
// Threading: Execute / ExecuteScript / ResolveCommand / SaveArchivedCvars
// / LoadFromFile / Undo / Redo are NOT thread-safe -- they assume the
// caller is the main thread (or has otherwise serialised). The mutex-
// protected QueueExecute / Drain pair is the cross-thread on-ramp.

#include "Console.h"
#include "Completion.h"

#include "../Tracy.h"

#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace psynder::console {

// ─── CVar coercion ───────────────────────────────────────────────────────
int CVar::GetInt() const {
    int n = 0;
    auto [_, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
    if (ec != std::errc()) {
        std::from_chars(default_value.data(), default_value.data() + default_value.size(), n);
    }
    return n;
}

float CVar::GetFloat() const {
    char* end = nullptr;
    float f = std::strtof(value.c_str(), &end);
    if (end == value.c_str()) {
        f = std::strtof(default_value.c_str(), nullptr);
    }
    return f;
}

bool CVar::GetBool() const {
    if (value == "1" || value == "true" || value == "on" || value == "yes")
        return true;
    if (value == "0" || value == "false" || value == "off" || value == "no")
        return false;
    return GetInt() != 0;
}

// ─── Tokenizer ───────────────────────────────────────────────────────────
// Quote-aware, recognises `\n` / `\t` / `\"` / `\\` escapes inside
// quotes, stops at `//` line-comment. Returns string_views into the
// caller-provided `storage` buffer so callers control the lifetime.
std::vector<std::string_view> tokenize_line(std::string_view line, std::string& storage) {
    storage.clear();
    storage.reserve(line.size());
    std::vector<std::pair<std::size_t, std::size_t>> spans;

    std::size_t i = 0;
    auto skip_ws = [&]() {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
    };

    while (i < line.size()) {
        skip_ws();
        if (i >= line.size())
            break;
        if (i + 1 < line.size() && line[i] == '/' && line[i + 1] == '/')
            break;

        std::size_t start = storage.size();
        if (line[i] == '"') {
            ++i;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) {
                    char c = line[i + 1];
                    switch (c) {
                        case 'n':
                            storage.push_back('\n');
                            break;
                        case 't':
                            storage.push_back('\t');
                            break;
                        case '"':
                            storage.push_back('"');
                            break;
                        case '\\':
                            storage.push_back('\\');
                            break;
                        default:
                            storage.push_back(c);
                            break;
                    }
                    i += 2;
                } else {
                    storage.push_back(line[i++]);
                }
            }
            if (i < line.size())
                ++i;  // consume closing quote
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
                storage.push_back(line[i++]);
            }
        }
        spans.emplace_back(start, storage.size() - start);
    }

    std::vector<std::string_view> out;
    out.reserve(spans.size());
    for (auto [off, len] : spans) {
        out.emplace_back(storage.data() + off, len);
    }
    return out;
}

// ─── Singleton ───────────────────────────────────────────────────────────
Console& Console::Get() {
    static Console instance;
    return instance;
}

CVar* Console::RegisterCVar(std::string name,
                            std::string default_value,
                            std::string description,
                            CVarFlags flags,
                            std::function<void(const CVar&)> on_change) {
    auto [it, inserted] = cvars_.try_emplace(name);
    auto& v = it->second;
    if (inserted) {
        v.name = std::move(name);
        v.value = default_value;
        v.default_value = default_value;
        v.description = std::move(description);
        v.flags = flags;
        v.on_change = std::move(on_change);
    }
    return &v;
}

Command* Console::RegisterCommand(std::string name, std::string description, CommandCallback callback) {
    auto [it, inserted] = commands_.try_emplace(name);
    auto& c = it->second;
    if (inserted) {
        c.name = std::move(name);
        c.description = std::move(description);
        c.callback = std::move(callback);
    }
    return &c;
}

CVar* Console::FindCVar(std::string_view name) {
    auto it = cvars_.find(name);
    return it == cvars_.end() ? nullptr : &it->second;
}

Command* Console::FindCommand(std::string_view name) {
    auto it = commands_.find(name);
    return it == commands_.end() ? nullptr : &it->second;
}

bool Console::SetCVarOverride(std::string_view name, std::string_view value) {
    auto* v = FindCVar(name);
    if (v == nullptr)
        return false;
    v->value.assign(value);
    if (v->on_change)
        v->on_change(*v);
    return true;
}

// ─── Platform-visibility / per-value gate helpers (TU-local) ─────────────
namespace {

// A cvar carrying a platform bit that doesn't match the current
// build is hidden from listing + autocomplete. Registration / set /
// archive paths still work so a shared psynder.cfg round-trips cleanly
// across hosts.
inline bool cvar_visible_on_this_platform(CVarFlags flags) {
#if defined(__APPLE__)
    if ((flags & CVarFlags::PlatformWin) != 0u)
        return false;
    if ((flags & CVarFlags::PlatformLinux) != 0u)
        return false;
#elif defined(_WIN32)
    if ((flags & CVarFlags::PlatformMac) != 0u)
        return false;
    if ((flags & CVarFlags::PlatformLinux) != 0u)
        return false;
#else
    if ((flags & CVarFlags::PlatformMac) != 0u)
        return false;
    if ((flags & CVarFlags::PlatformWin) != 0u)
        return false;
#endif
    return true;
}

// Build a CSV of allowed values that pass the per-value platform filter
// on this host. Used inside the error message when the user picks a
// wrong-platform enum value, so the message lists only values that would
// actually be accepted.
std::string allowed_values_for_current_platform_csv(const CVar& v) {
    std::string out;
    for (std::size_t i = 0; i < v.allowed_values.size(); ++i) {
        CVarValueFlags mask =
            (i < v.allowed_value_flags.size()) ? v.allowed_value_flags[i] : CVarValueFlags::Any;
        if (!cvar_value_allowed_on_this_platform(mask))
            continue;
        if (!out.empty())
            out += ", ";
        out += v.allowed_values[i];
    }
    return out;
}

// Find the value-platform mask aligned with `value` inside the cvar's
// allowed set. Returns CVarValueFlags::Any if the value isn't in the set.
CVarValueFlags value_flags_for(const CVar& v, std::string_view value) {
    for (std::size_t i = 0; i < v.allowed_values.size(); ++i) {
        if (v.allowed_values[i] != value)
            continue;
        return (i < v.allowed_value_flags.size()) ? v.allowed_value_flags[i]
                                                  : CVarValueFlags::Any;
    }
    return CVarValueFlags::Any;
}

// Human-readable platform list (e.g. "macOS", "Windows/Linux") for the
// platform-mismatch error.
std::string platforms_from_mask(CVarValueFlags mask) {
    std::string s;
    auto add = [&](const char* p) {
        if (!s.empty())
            s += "/";
        s += p;
    };
    if ((mask & CVarValueFlags::Mac) != 0u)
        add("macOS");
    if ((mask & CVarValueFlags::Win) != 0u)
        add("Windows");
    if ((mask & CVarValueFlags::Linux) != 0u)
        add("Linux");
    return s;
}

}  // namespace

// ─── Public free functions ───────────────────────────────────────────────
bool cvar_value_allowed_on_this_platform(CVarValueFlags value_flags) {
    if (value_flags == CVarValueFlags::Any)
        return true;
#if defined(__APPLE__)
    return (value_flags & CVarValueFlags::Mac) != 0u;
#elif defined(_WIN32)
    return (value_flags & CVarValueFlags::Win) != 0u;
#else
    return (value_flags & CVarValueFlags::Linux) != 0u;
#endif
}

const char* current_platform_name() {
#if defined(__APPLE__)
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#else
    return "Linux";
#endif
}

// ─── Smart resolution ────────────────────────────────────────────────────
// Score every registered cvar / command against `typed`, sort by
// (score desc, length asc, lex), take the top. Cvars rank above
// commands at equal score (tie-break). The r_-stripped body is also
// scored so `deno` finds `r_denoiser`.
Console::Resolution Console::ResolveCommand(std::string_view typed) {
    Resolution r;
    if (typed.empty())
        return r;

    // Exact match wins immediately.
    if (auto it = cvars_.find(typed); it != cvars_.end()) {
        if (cvar_visible_on_this_platform(it->second.flags)) {
            r.canonical_name = it->second.name;
            r.is_exact_match = true;
            return r;
        }
    }
    if (auto it = commands_.find(typed); it != commands_.end()) {
        r.canonical_name = it->second.name;
        r.is_exact_match = true;
        return r;
    }

    auto score_name = [&](const std::string& name) -> int {
        int s = ScoreMatch(name, typed, /*spans=*/nullptr);
        if (name.size() >= 2 && name[0] == 'r' && name[1] == '_') {
            const std::string_view body(name.data() + 2, name.size() - 2);
            const int s2 = ScoreMatch(body, typed, /*spans=*/nullptr);
            if (s2 > s)
                s = s2;
        }
        return s;
    };

    struct Scored {
        int score;
        std::size_t length;
        std::string name;
    };
    std::vector<Scored> cvar_hits;
    std::vector<Scored> cmd_hits;
    for (const auto& [_, v] : cvars_) {
        if (!cvar_visible_on_this_platform(v.flags))
            continue;
        int s = score_name(v.name);
        if (s > 0)
            cvar_hits.push_back({s, v.name.size(), v.name});
    }
    for (const auto& [_, c] : commands_) {
        int s = score_name(c.name);
        if (s > 0)
            cmd_hits.push_back({s, c.name.size(), c.name});
    }

    auto cmp = [](const Scored& a, const Scored& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.length != b.length)
            return a.length < b.length;
        return a.name < b.name;
    };
    std::stable_sort(cvar_hits.begin(), cvar_hits.end(), cmp);
    std::stable_sort(cmd_hits.begin(), cmd_hits.end(), cmp);

    const std::size_t total = cvar_hits.size() + cmd_hits.size();
    if (total == 0)
        return r;

    // Cvars > commands at equal score.
    r.canonical_name = cvar_hits.empty() ? cmd_hits.front().name : cvar_hits.front().name;
    if (total > 1) {
        r.ambiguous_matches.reserve(total);
        for (auto& h : cvar_hits)
            r.ambiguous_matches.push_back(std::move(h.name));
        for (auto& h : cmd_hits)
            r.ambiguous_matches.push_back(std::move(h.name));
    }
    return r;
}

// ─── Execute ─────────────────────────────────────────────────────────────
ExecuteResult Console::Execute(std::string_view line) {
    PSY_TRACE_ZONE("console::Execute");
    ExecuteResult result;

    // Strip leading whitespace; skip blank / full-line `//` comments.
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    if (line.empty())
        return result;
    if (line.size() >= 2 && line[0] == '/' && line[1] == '/')
        return result;

    // Inline `#` comment stripping. Track quote state so a `#` inside a
    // quoted string is data, not a comment. Handle `\` escaping so
    // `\"` doesn't flip the quote state mid-string.
    {
        bool in_quote = false;
        bool escape = false;
        std::size_t cut = line.size();
        for (std::size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"') {
                in_quote = !in_quote;
                continue;
            }
            if (c == '#' && !in_quote) {
                cut = i;
                break;
            }
        }
        if (cut < line.size())
            line = line.substr(0, cut);
    }
    // Trim trailing whitespace including '\r' so CRLF-terminated input
    // doesn't end up with the carriage return swallowed into the last
    // token.
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
    if (line.empty())
        return result;

    std::string storage;
    auto tokens = tokenize_line(line, storage);
    if (tokens.empty())
        return result;

    auto name = tokens[0];

    // ─── Favourites magic invocation (f1..fN) ────────────────────────
    // Resolves BEFORE smart-resolve so a fN token wins decisively. The
    // in_fav_dispatch_ guard prevents infinite recursion if a saved
    // favourite happens to be `fN` itself.
    if (!in_fav_dispatch_ && !favorites_.empty() && name.size() >= 2 && name[0] == 'f') {
        bool all_digits = true;
        std::size_t idx = 0;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') {
                all_digits = false;
                break;
            }
            idx = idx * 10 + std::size_t(name[i] - '0');
        }
        if (all_digits && idx >= 1 && idx <= favorites_.size()) {
            const std::string saved = favorites_[idx - 1];
            in_fav_dispatch_ = true;
            ExecuteResult sub = Execute(saved);
            in_fav_dispatch_ = false;
            std::string log = fmt::format("[fav] f{} -> `{}`\n", idx, saved);
            if (!sub.output.empty()) {
                if (sub.output.back() != '\n')
                    sub.output.push_back('\n');
                log += sub.output;
                sub.output = std::move(log);
            } else {
                sub.output = std::move(log);
            }
            return sub;
        }
    }

    // ─── Smart command resolution ────────────────────────────────────
    bool smart_on = true;
    if (auto* sm = FindCVar("r_console_smart_resolve"); sm != nullptr) {
        smart_on = sm->GetBool();
    }

    std::string resolution_log;
    std::string canonical_storage;
    if (smart_on) {
        Resolution r = ResolveCommand(name);
        if (!r.canonical_name.empty() && !r.is_exact_match) {
            canonical_storage = r.canonical_name;
            if (r.ambiguous_matches.size() > 1) {
                // Show top match + up to 4 alternates so the user sees
                // what got chosen and what else was available.
                constexpr std::size_t kCap = 4;
                std::string alt;
                std::size_t i = 0;
                for (const auto& m : r.ambiguous_matches) {
                    if (m == canonical_storage)
                        continue;
                    if (i >= kCap) {
                        alt += ", ...";
                        break;
                    }
                    if (!alt.empty())
                        alt += ", ";
                    alt += m;
                    ++i;
                }
                resolution_log = fmt::format(
                    "[console] resolved `{}` -> `{}` (top of {} matches; "
                    "alt: {})\n",
                    std::string(name),
                    canonical_storage,
                    r.ambiguous_matches.size(),
                    alt);
            } else {
                resolution_log = fmt::format("[console] resolved `{}` -> `{}` (top match)\n",
                                             std::string(name),
                                             canonical_storage);
            }
            name = std::string_view(canonical_storage);
        }
        // else: zero match -- fall through to the "unknown" error path
        // below so a typo without any plausible match doesn't get a
        // bogus resolution.
    }

    // Track this line as "last executed" for the `fav` (no-args)
    // shortcut, EXCEPT when the line is itself a fav-management
    // command (otherwise `fav` would end up saving "fav" if you typed
    // it twice).
    auto is_fav_mgmt = [](std::string_view n) {
        return n == "fav" || n == "unfav" || n == "fav_clear" || n == "list_favs";
    };

    // Try command first.
    if (auto* cmd = FindCommand(name); cmd != nullptr) {
        Output out;
        std::span<const std::string_view> args(tokens.data() + 1, tokens.size() - 1);
        cmd->callback(args, out);
        result.output = resolution_log + out.Buffer();
        if (!is_fav_mgmt(name)) {
            last_executed_line_ = std::string(line);
        }
        return result;
    }

    // Then cvar: no argument = read; one or more = set.
    if (auto* v = FindCVar(name); v != nullptr) {
        if (tokens.size() == 1) {
            result.output =
                resolution_log +
                fmt::format("{} = \"{}\"  (default \"{}\")", v->name, v->value, v->default_value);
            return result;
        }
        if ((v->flags & CVarFlags::ReadOnly) != 0u) {
            result.ok = false;
            result.error = fmt::format("cvar '{}' is read-only", v->name);
            return result;
        }
        if ((v->flags & CVarFlags::Cheat) != 0u) {
            auto* cheat = FindCVar("dev_cheats");
            if (cheat == nullptr || !cheat->GetBool()) {
                result.ok = false;
                result.error = fmt::format("cvar '{}' requires dev_cheats 1", v->name);
                return result;
            }
        }

        // Join remaining tokens (handles `cvar a b c` and quoted
        // `cvar "0.05 0.05 0.06"` -- the tokenizer already stripped
        // quotes from the quoted form).
        std::string new_value;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (i > 1)
                new_value.push_back(' ');
            new_value.append(tokens[i]);
        }

        // "0" as off-shorthand. When a cvar has allowed_values and the
        // set contains exactly one of "off" / "none" / "disabled",
        // accept "0" as that token.
        if (new_value == "0" && !v->allowed_values.empty()) {
            auto ieq = [](std::string_view a, std::string_view b) {
                if (a.size() != b.size())
                    return false;
                for (std::size_t i = 0; i < a.size(); ++i) {
                    char ca = a[i], cb = b[i];
                    if (ca >= 'A' && ca <= 'Z')
                        ca = static_cast<char>(ca - 'A' + 'a');
                    if (cb >= 'A' && cb <= 'Z')
                        cb = static_cast<char>(cb - 'A' + 'a');
                    if (ca != cb)
                        return false;
                }
                return true;
            };
            for (const auto& a : v->allowed_values) {
                if (ieq(a, "off") || ieq(a, "none") || ieq(a, "disabled")) {
                    new_value = a;
                    break;
                }
            }
        }

        // Enforce allowed_values.
        if (!v->allowed_values.empty()) {
            bool ok2 = false;
            for (const auto& a : v->allowed_values) {
                if (a == new_value) {
                    ok2 = true;
                    break;
                }
            }
            if (!ok2) {
                std::string allowed;
                for (std::size_t i = 0; i < v->allowed_values.size(); ++i) {
                    if (i)
                        allowed += '|';
                    allowed += v->allowed_values[i];
                }
                result.ok = false;
                result.error = fmt::format("{}: invalid value '{}' (expected one of: {})",
                                           v->name,
                                           new_value,
                                           allowed);
                return result;
            }
        }

        // Per-value platform gate. The value still gets written so a
        // portable psynder.cfg round-trips, but Execute returns an
        // error so the interactive caller knows the value won't take
        // effect on this host.
        if (!v->allowed_value_flags.empty()) {
            CVarValueFlags mask = value_flags_for(*v, new_value);
            if (!cvar_value_allowed_on_this_platform(mask)) {
                std::string platforms = platforms_from_mask(mask);
                std::string available = allowed_values_for_current_platform_csv(*v);
                std::string old_value = v->value;
                v->value = new_value;
                result.ok = false;
                result.error = fmt::format(
                    "{}={} is {}-only; not available on {}.\n"
                    "        Available on this platform: {}",
                    v->name,
                    new_value,
                    platforms,
                    current_platform_name(),
                    available);
                result.output =
                    resolution_log + fmt::format("{}: \"{}\" -> \"{}\" (inactive: platform-gated)",
                                                 v->name,
                                                 old_value,
                                                 v->value);
                return result;
            }
        }

        std::string old_value = v->value;
        v->value = std::move(new_value);
        if (v->on_change)
            v->on_change(*v);

        if (!is_fav_mgmt(name)) {
            last_executed_line_ = std::string(line);
        }

        // Cross-cvar dependency warning. Evaluated AFTER the value is
        // committed; never blocks the set.
        std::string warn_line;
        if (!dep_warn_suppressed_ && v->requires_predicate && !v->requires_predicate()) {
            warn_line = fmt::format("[warn] {}", v->requires_hint);
        }

        result.output =
            resolution_log + fmt::format("{}: \"{}\" -> \"{}\"", v->name, old_value, v->value);
        if (!warn_line.empty()) {
            result.output.push_back('\n');
            result.output.append(warn_line);
        }
        return result;
    }

    result.ok = false;
    result.error = fmt::format("unknown command or cvar: '{}'", name);
    return result;
}

// ─── ExecuteScript ───────────────────────────────────────────────────────
namespace {

// Find the end of the next logical statement starting at body[i].
// Statements end at '\n' OR at ';' that is OUTSIDE of quoted strings AND
// OUTSIDE of '#' / '//' comments. The body slice [i, end) is then passed
// to Execute(). Mirrors the quote/escape handling Execute() does so cfg
// files can quote a literal ';' inside a string token.
//
// `//` is a comment marker ONLY at statement start (after optional
// leading whitespace), matching Execute()'s full-line `//` rule. Mid-
// statement `//` is data -- preserves URL-shaped values.
std::size_t scan_script_statement_end(std::string_view body, std::size_t i) {
    bool in_quote = false;
    bool escape = false;
    bool in_comment = false;
    bool at_stmt_start = true;
    std::size_t end = i;
    while (end < body.size()) {
        char c = body[end];
        if (c == '\n')
            break;
        if (in_comment) {
            ++end;
            continue;
        }
        if (escape) {
            escape = false;
            ++end;
            at_stmt_start = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            ++end;
            at_stmt_start = false;
            continue;
        }
        if (c == '"') {
            in_quote = !in_quote;
            ++end;
            at_stmt_start = false;
            continue;
        }
        if (!in_quote) {
            if (c == '#') {
                in_comment = true;
                ++end;
                continue;
            }
            if (at_stmt_start && c == '/' && end + 1 < body.size() && body[end + 1] == '/') {
                in_comment = true;
                end += 2;
                continue;
            }
            if (c == ';')
                break;
        }
        if (c != ' ' && c != '\t')
            at_stmt_start = false;
        ++end;
    }
    return end;
}

}  // namespace

ExecuteResult Console::ExecuteScript(std::string_view body) {
    ExecuteResult agg;

    // Capture pre-transaction values of every cvar the script might set.
    // Parse the first token of each statement; if it names a known cvar,
    // snapshot its value. Statements that target commands contribute
    // nothing to the snapshot.
    CvarSnapshot pre;
    if (!in_undo_redo_) {
        std::size_t i = 0;
        while (i < body.size()) {
            std::size_t end = scan_script_statement_end(body, i);
            auto line = body.substr(i, end - i);
            std::size_t a = 0;
            while (a < line.size() && (line[a] == ' ' || line[a] == '\t'))
                ++a;
            std::size_t b = a;
            while (b < line.size() && line[b] != ' ' && line[b] != '\t')
                ++b;
            if (b > a) {
                std::string_view name = line.substr(a, b - a);
                auto it = cvars_.find(name);
                if (it != cvars_.end() && pre.find(it->first) == pre.end()) {
                    pre[it->first] = it->second.value;
                }
            }
            i = (end < body.size()) ? end + 1 : end;
        }
    }

    std::size_t i = 0;
    while (i < body.size()) {
        std::size_t end = scan_script_statement_end(body, i);
        auto line = body.substr(i, end - i);
        auto r = Execute(line);
        if (!r.ok) {
            agg.ok = false;
            if (!agg.error.empty())
                agg.error.push_back('\n');
            agg.error.append(r.error);
        }
        if (!r.output.empty()) {
            if (!agg.output.empty() && agg.output.back() != '\n') {
                agg.output.push_back('\n');
            }
            agg.output.append(r.output);
        }
        i = (end < body.size()) ? end + 1 : end;
    }

    // Build the actual diff: only push cvars whose value really changed
    // during this script. Skip the entry if nothing changed.
    if (!in_undo_redo_) {
        CvarSnapshot diff;
        for (auto& [name, old] : pre) {
            auto it = cvars_.find(name);
            if (it != cvars_.end() && it->second.value != old) {
                diff[name] = old;
            }
        }
        if (!diff.empty()) {
            undo_stack_.push_back(std::move(diff));
            if (undo_stack_.size() > kMaxHistory)
                undo_stack_.pop_front();
            // Any new edit invalidates the redo branch.
            redo_stack_.clear();
        }
    }
    return agg;
}

// ─── Reset / Undo / Redo ────────────────────────────────────────────────
std::size_t Console::ResetAllCVarsToDefaults() {
    CvarSnapshot pre;
    for (auto& [name, cv] : cvars_) {
        if (cv.value != cv.default_value) {
            pre[name] = cv.value;
        }
    }
    if (pre.empty())
        return 0;

    in_undo_redo_ = true;
    for (auto& [name, cv] : cvars_) {
        if (cv.value == cv.default_value)
            continue;
        cv.value = cv.default_value;
        if (cv.on_change)
            cv.on_change(cv);
    }
    in_undo_redo_ = false;

    undo_stack_.push_back(pre);
    if (undo_stack_.size() > kMaxHistory)
        undo_stack_.pop_front();
    redo_stack_.clear();
    return pre.size();
}

std::vector<Console::CvarChange> Console::Undo() {
    std::vector<CvarChange> changes;
    if (undo_stack_.empty())
        return changes;
    in_undo_redo_ = true;
    CvarSnapshot snap = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    CvarSnapshot fwd;
    for (auto& [name, old] : snap) {
        auto it = cvars_.find(name);
        if (it == cvars_.end())
            continue;
        std::string current = it->second.value;
        fwd[name] = current;
        // Use Execute so any allowed_values check, on_change hook, and
        // cascading side effects fire as if the user typed the rollback.
        std::string line = name + " " + old;
        Execute(line);
        changes.push_back({name, std::move(current), old});
    }
    redo_stack_.push_back(std::move(fwd));
    if (redo_stack_.size() > kMaxHistory)
        redo_stack_.pop_front();
    in_undo_redo_ = false;
    return changes;
}

std::vector<Console::CvarChange> Console::Redo() {
    std::vector<CvarChange> changes;
    if (redo_stack_.empty())
        return changes;
    in_undo_redo_ = true;
    CvarSnapshot snap = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    CvarSnapshot back;
    for (auto& [name, val] : snap) {
        auto it = cvars_.find(name);
        if (it == cvars_.end())
            continue;
        std::string current = it->second.value;
        back[name] = current;
        std::string line = name + " " + val;
        Execute(line);
        changes.push_back({name, std::move(current), val});
    }
    undo_stack_.push_back(std::move(back));
    if (undo_stack_.size() > kMaxHistory)
        undo_stack_.pop_front();
    in_undo_redo_ = false;
    return changes;
}

// ─── Favourites ─────────────────────────────────────────────────────────
void Console::AddFavorite(std::string line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.erase(line.begin());
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    if (line.empty())
        return;
    favorites_.push_back(std::move(line));
}

void Console::RemoveFavorite(std::size_t one_based_index) {
    if (one_based_index == 0 || one_based_index > favorites_.size())
        return;
    using diff_t = std::vector<std::string>::difference_type;
    favorites_.erase(favorites_.begin() + static_cast<diff_t>(one_based_index - 1));
}

void Console::ClearFavorites() {
    favorites_.clear();
}

// ─── History ─────────────────────────────────────────────────────────────
void Console::PushHistory(std::string line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.erase(line.begin());
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    if (line.empty())
        return;
    // Bash-style HISTCONTROL=ignoredups: dedupe consecutive identical
    // submissions.
    if (!history_.empty() && history_.back() == line)
        return;
    history_.push_back(std::move(line));
    if (history_.size() > kMaxHistoryDepth) {
        using diff_t = std::vector<std::string>::difference_type;
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<diff_t>(history_.size() - kMaxHistoryDepth));
    }
}

void Console::ClearHistory() {
    history_.clear();
}

void Console::SetHistory(std::vector<std::string> hist) {
    history_ = std::move(hist);
    if (history_.size() > kMaxHistoryDepth) {
        using diff_t = std::vector<std::string>::difference_type;
        history_.erase(history_.begin(),
                       history_.begin() + static_cast<diff_t>(history_.size() - kMaxHistoryDepth));
    }
}

// ─── Queue + Drain (cross-thread on-ramp + bracket-batch handling) ─────
void Console::QueueExecute(std::string line, Responder responder) {
    std::lock_guard lock(queue_mutex_);
    queue_.push_back({std::move(line), std::move(responder)});
}

void Console::Drain() {
    PSY_TRACE_ZONE("console::Drain");
    std::deque<Pending> local;
    {
        std::lock_guard lock(queue_mutex_);
        std::swap(local, queue_);
    }
    for (auto& pe : local) {
        // Trim for `[` / `]` detection -- match the dmonte semantics.
        std::string_view trimmed = pe.line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' ||
                                    trimmed.front() == '\r' || trimmed.front() == '\n')) {
            trimmed.remove_prefix(1);
        }
        while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' ||
                                    trimmed.back() == '\r' || trimmed.back() == '\n')) {
            trimmed.remove_suffix(1);
        }

        if (!batch_active_) {
            if (trimmed == "[") {
                batch_active_ = true;
                batch_buffer_.clear();
                ExecuteResult r;
                r.output =
                    "batch: open  (send ']' on its own line to commit; "
                    "the whole bundle is one undo step)";
                if (pe.responder)
                    pe.responder(r);
                continue;
            }
            // Normal path: ExecuteScript splits on '\n' / ';' so multi-
            // line paste in one shot still works.
            auto result = ExecuteScript(pe.line);
            if (pe.responder)
                pe.responder(result);
            continue;
        }

        // batch_active_ == true: collect or commit
        if (trimmed == "]") {
            batch_active_ = false;
            std::string body = std::move(batch_buffer_);
            batch_buffer_.clear();
            ExecuteResult result;
            if (body.empty()) {
                result.output = "batch: empty (nothing committed)";
            } else {
                result = ExecuteScript(body);
                std::string header = "batch: committed (one undo step rolls back all)";
                if (result.output.empty()) {
                    result.output = std::move(header);
                } else {
                    result.output = header + "\n" + std::move(result.output);
                }
            }
            if (pe.responder)
                pe.responder(result);
        } else {
            // Cap the bundle so a misbehaving client that opens '[' but
            // never sends ']' can't grow the buffer without bound.
            constexpr std::size_t kBatchMaxBytes = 1u * 1024u * 1024u;
            const std::size_t projected = batch_buffer_.size() + pe.line.size() + 1u;
            if (projected > kBatchMaxBytes) {
                batch_active_ = false;
                batch_buffer_.clear();
                ExecuteResult r;
                r.output = fmt::format(
                    "batch: aborted (would exceed {} byte cap; nothing committed). "
                    "Send '[' again to retry with a smaller bundle.",
                    kBatchMaxBytes);
                if (pe.responder)
                    pe.responder(r);
                continue;
            }
            if (!batch_buffer_.empty())
                batch_buffer_.push_back('\n');
            batch_buffer_.append(pe.line);
            ExecuteResult r;
            r.output = fmt::format("batch: queued  ({} byte{} buffered, send ']' to commit)",
                                   batch_buffer_.size(),
                                   batch_buffer_.size() == 1 ? "" : "s");
            if (pe.responder)
                pe.responder(r);
        }
    }
}

void Console::AddExternalExecutionSink(ExternalExecutionSink sink) {
    if (!sink)
        return;
    if (std::find(external_execution_sinks_.begin(), external_execution_sinks_.end(), sink) !=
        external_execution_sinks_.end()) {
        return;
    }
    external_execution_sinks_.push_back(sink);
}

void Console::ClearExternalExecutionSinks() {
    external_execution_sinks_.clear();
}

void Console::NotifyExternalExecution(std::string_view line, const ExecuteResult& result) {
    for (ExternalExecutionSink sink : external_execution_sinks_) {
        if (sink)
            sink(line, result);
    }
}

// ─── Enumeration / persistence ──────────────────────────────────────────
void Console::EnumerateCVars(std::string_view prefix, const std::function<void(CVar&)>& visitor) {
    for (auto& [_, v] : cvars_) {
        if (!cvar_visible_on_this_platform(v.flags))
            continue;
        if (prefix.empty() || v.name.starts_with(prefix))
            visitor(v);
    }
}

void Console::EnumerateCommands(std::string_view prefix, const std::function<void(Command&)>& visitor) {
    for (auto& [_, c] : commands_) {
        if (prefix.empty() || c.name.starts_with(prefix))
            visitor(c);
    }
}

int Console::SaveArchivedCvars(const std::string& path) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open())
        return -1;

    f << "// Psynder Engine -- archived cvars (auto-generated on quit)\n";
    f << "// Hand edits get overwritten on the next clean exit.\n\n";

    int n = 0;
    for (auto& [name, cv] : cvars_) {
        if ((cv.flags & CVarFlags::Archive) == 0u)
            continue;
        if (cv.value == cv.default_value)
            continue;
        const bool needs_quotes = (cv.value.find(' ') != std::string::npos);
        f << cv.name << ' ';
        if (needs_quotes)
            f << '"' << cv.value << '"';
        else
            f << cv.value;
        f << '\n';
        ++n;
    }
    return n;
}

ExecuteResult Console::LoadFromFile(const std::string& path) {
    ExecuteResult r;
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        r.ok = false;
        r.error = fmt::format("LoadFromFile: cannot open '{}'", path);
        return r;
    }
    std::ostringstream body;
    body << f.rdbuf();
    // Suppress dependency warnings during replay -- a config that sets a
    // dependent cvar before its dependency would otherwise warn on every
    // line. Save / restore prior state so nested LoadFromFile calls don't
    // clobber an outer caller's setting.
    const bool prev = dep_warn_suppressed_;
    dep_warn_suppressed_ = true;
    r = ExecuteScript(body.str());
    dep_warn_suppressed_ = prev;
    return r;
}

}  // namespace psynder::console
