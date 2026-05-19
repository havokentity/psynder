// SPDX-License-Identifier: MIT
// Psynder — minimal console impl. Lane 01 ports the full dmonte console
// (cvar archive/load, undo/redo, favorites, history, smart-resolve, platform
// tags, bracket-batch transactions). This file is intentionally small so
// the rest of the engine can already register cvars / commands in Phase 0
// without crashing.

#include "Console.h"

#include <charconv>
#include <cstdlib>

namespace psynder::console {

// ─── CVar accessors ──────────────────────────────────────────────────────
int CVar::GetInt() const {
    if (value.empty()) return 0;
    int v = 0;
    auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    return ec == std::errc{} ? v : 0;
}

float CVar::GetFloat() const {
    if (value.empty()) return 0.0f;
    return static_cast<float>(std::atof(value.c_str()));
}

bool CVar::GetBool() const {
    if (value == "1" || value == "true" || value == "on") return true;
    return false;
}

// ─── Singleton ───────────────────────────────────────────────────────────
Console& Console::Get() {
    static Console c;
    return c;
}

// ─── Register ────────────────────────────────────────────────────────────
CVar* Console::RegisterCVar(std::string name, std::string default_value,
                            std::string description, u32 flags,
                            std::function<void(const CVar&)> on_change) {
    auto [it, inserted] = cvars_.try_emplace(name);
    if (inserted) {
        it->second.name          = std::move(name);
        it->second.default_value = default_value;
        it->second.value         = std::move(default_value);
        it->second.description   = std::move(description);
        it->second.flags         = flags;
        it->second.on_change     = std::move(on_change);
    }
    return &it->second;
}

Command* Console::RegisterCommand(std::string name, std::string description,
                                  CommandCallback callback) {
    auto [it, inserted] = commands_.try_emplace(name);
    if (inserted) {
        it->second.name        = std::move(name);
        it->second.description = std::move(description);
        it->second.callback    = std::move(callback);
    }
    return &it->second;
}

// ─── Find ────────────────────────────────────────────────────────────────
CVar* Console::FindCVar(std::string_view name) {
    auto it = cvars_.find(std::string{name});
    return it == cvars_.end() ? nullptr : &it->second;
}

Command* Console::FindCommand(std::string_view name) {
    auto it = commands_.find(std::string{name});
    return it == commands_.end() ? nullptr : &it->second;
}

bool Console::SetCVarOverride(std::string_view name, std::string_view value) {
    if (auto* cv = FindCVar(name)) {
        cv->value = std::string{value};
        if (cv->on_change) cv->on_change(*cv);
        return true;
    }
    return false;
}

// ─── Tokenize / execute (minimal — lane 01 ports full semantics) ─────────
std::vector<std::string_view> tokenize_line(std::string_view line,
                                            std::string& storage) {
    storage.clear();
    storage.reserve(line.size());

    std::vector<std::string_view> tokens;
    bool in_quote = false;
    usize start = storage.size();

    for (char c : line) {
        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && (c == ' ' || c == '\t')) {
            if (storage.size() != start) {
                tokens.emplace_back(storage.data() + start, storage.size() - start);
                start = storage.size();
            }
            continue;
        }
        storage.push_back(c);
    }
    if (storage.size() != start) {
        tokens.emplace_back(storage.data() + start, storage.size() - start);
    }
    return tokens;
}

ExecuteResult Console::Execute(std::string_view line) {
    std::string storage;
    auto tokens = tokenize_line(line, storage);
    if (tokens.empty()) return {};

    auto cmd_name = tokens.front();

    if (auto* cmd = FindCommand(cmd_name)) {
        Output out;
        cmd->callback(std::span(tokens.data() + 1, tokens.size() - 1), out);
        return {.ok = true, .output = out.Buffer()};
    }
    if (auto* cv = FindCVar(cmd_name)) {
        if (tokens.size() == 1) {
            return {.ok = true, .output = fmt::format("{} = \"{}\"\n", cv->name, cv->value)};
        }
        cv->value = std::string{tokens[1]};
        if (cv->on_change) cv->on_change(*cv);
        return {.ok = true, .output = fmt::format("{} = \"{}\"\n", cv->name, cv->value)};
    }
    return {.ok = false, .error = fmt::format("unknown: {}", cmd_name)};
}

ExecuteResult Console::ExecuteScript(std::string_view body) {
    ExecuteResult agg;
    usize start = 0;
    for (usize i = 0; i <= body.size(); ++i) {
        if (i == body.size() || body[i] == '\n' || body[i] == ';') {
            auto line = body.substr(start, i - start);
            if (!line.empty()) {
                auto r = Execute(line);
                agg.output += r.output;
                if (!r.ok) {
                    agg.ok = false;
                    agg.error += r.error;
                    agg.error.push_back('\n');
                }
            }
            start = i + 1;
        }
    }
    return agg;
}

void Console::QueueExecute(std::string line, Responder responder) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_.push_back({std::move(line), std::move(responder)});
}

void Console::Drain() {
    std::deque<Pending> pending;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        pending.swap(queue_);
    }
    for (auto& p : pending) {
        auto r = Execute(p.line);
        if (p.responder) p.responder(r);
    }
}

void Console::EnumerateCVars(std::string_view prefix,
                             const std::function<void(CVar&)>& visitor) {
    for (auto& [name, cv] : cvars_) {
        if (prefix.empty() || name.starts_with(prefix)) visitor(cv);
    }
}

void Console::EnumerateCommands(std::string_view prefix,
                                const std::function<void(Command&)>& visitor) {
    for (auto& [name, c] : commands_) {
        if (prefix.empty() || name.starts_with(prefix)) visitor(c);
    }
}

int Console::SaveArchivedCvars(const std::string& /*path*/) {
    // Lane 01: port from dmonte. Returns -1 to signal not-implemented.
    return -1;
}

std::size_t Console::ResetAllCVarsToDefaults() {
    std::size_t changed = 0;
    for (auto& [_, cv] : cvars_) {
        if (cv.value != cv.default_value) {
            cv.value = cv.default_value;
            if (cv.on_change) cv.on_change(cv);
            ++changed;
        }
    }
    return changed;
}

// ─── Platform tags ───────────────────────────────────────────────────────
const char* current_platform_name() {
#if PSYNDER_PLATFORM_MACOS
    return "mac";
#elif PSYNDER_PLATFORM_WIN32
    return "win";
#elif PSYNDER_PLATFORM_LINUX
    return "linux";
#else
    return "unknown";
#endif
}

bool cvar_value_allowed_on_this_platform(u32 value_flags) {
    if (value_flags == CVAR_VALUE_ANY) return true;
#if PSYNDER_PLATFORM_MACOS
    return (value_flags & CVAR_VALUE_MAC) != 0;
#elif PSYNDER_PLATFORM_WIN32
    return (value_flags & CVAR_VALUE_WIN) != 0;
#elif PSYNDER_PLATFORM_LINUX
    return (value_flags & CVAR_VALUE_LINUX) != 0;
#else
    return false;
#endif
}

}  // namespace psynder::console
