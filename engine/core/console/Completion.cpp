// SPDX-License-Identifier: MIT
// Psynder — completion engine impl. Ported from dmonte's
// `pt::console::Completion`. Mirrors the JS web-console implementation
// (kept in sync there) so the C++ frontend and the web UI produce
// identical match orderings from the same input.

#include "Completion.h"

#include "Console.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace psynder::console {

namespace {

// ASCII-only lowercase compare. Cvar / command names in this engine are
// pure ASCII identifiers (alpha + digit + `_`) so we deliberately skip
// locale-aware std::tolower -- it'd be both slower per char and
// surprising at non-ASCII inputs.
inline char to_lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

}  // namespace

TokenInfo CurrentToken(std::string_view input, std::size_t cursor) {
    TokenInfo t;
    if (cursor > input.size())
        cursor = input.size();
    // Treat both ' ' and '\t' as a token delimiter -- matches what
    // tokenize_line does at execute time.
    auto is_ws = [](char c) { return c == ' ' || c == '\t'; };

    std::size_t s = cursor;
    while (s > 0 && !is_ws(input[s - 1]))
        --s;
    std::size_t e = cursor;
    while (e < input.size() && !is_ws(input[e]))
        ++e;
    t.start = s;
    t.end = e;
    t.text = std::string(input.substr(s, e - s));

    std::size_t i = 0;
    while (i < input.size() && is_ws(input[i]))
        ++i;
    std::size_t t0_start = i;
    while (i < input.size() && !is_ws(input[i]))
        ++i;
    t.first_tok = std::string(input.substr(t0_start, i - t0_start));
    t.is_token0 = (s <= t0_start);
    return t;
}

int ScoreMatch(std::string_view name,
               std::string_view query,
               std::vector<std::pair<std::size_t, std::size_t>>* out_spans) {
    if (out_spans)
        out_spans->clear();
    if (query.empty())
        return 1;  // anything matches an empty query
    if (name.empty())
        return 0;

    // round-to-nearest integer division: matches JS's
    // Math.round((numerator) / denominator). Integer floor would diverge
    // C++ from JS at otherwise-close candidates.
    auto rdiv = [](std::size_t num, std::size_t den) -> int {
        if (den == 0)
            return 0;
        return static_cast<int>((num + den / 2) / den);
    };

    // PREFIX
    if (name.size() >= query.size()) {
        bool prefix = true;
        for (std::size_t i = 0; i < query.size(); ++i) {
            if (to_lower_ascii(name[i]) != to_lower_ascii(query[i])) {
                prefix = false;
                break;
            }
        }
        if (prefix) {
            const int tightness = rdiv(query.size() * 200, name.size());
            if (out_spans)
                out_spans->emplace_back(0, query.size());
            return 1000 + tightness;
        }
    }

    // SUBSTRING with word-boundary bonus
    {
        std::size_t found = std::string::npos;
        for (std::size_t i = 0; i + query.size() <= name.size(); ++i) {
            bool eq = true;
            for (std::size_t j = 0; j < query.size(); ++j) {
                if (to_lower_ascii(name[i + j]) != to_lower_ascii(query[j])) {
                    eq = false;
                    break;
                }
            }
            if (eq) {
                found = i;
                break;
            }
        }
        if (found != std::string::npos) {
            const bool word_start = (found == 0) || name[found - 1] == '_';
            const int tightness = rdiv(query.size() * 100, name.size());
            const int word_bonus = word_start ? 100 : 0;
            const int score = 500 + word_bonus + tightness - static_cast<int>(found);
            if (out_spans)
                out_spans->emplace_back(found, found + query.size());
            return score;
        }
    }

    // FUZZY: chars of query appear in order, possibly with gaps.
    {
        std::size_t qi = 0;
        int score = 0;
        long long last_match = -2;
        std::size_t run_start = std::string::npos;
        for (std::size_t i = 0; i < name.size() && qi < query.size(); ++i) {
            if (to_lower_ascii(name[i]) != to_lower_ascii(query[qi])) {
                if (run_start != std::string::npos && out_spans) {
                    out_spans->emplace_back(run_start, i);
                }
                run_start = std::string::npos;
                continue;
            }
            const bool word_start = (i == 0) || name[i - 1] == '_';
            score += word_start ? 8 : 4;
            if (static_cast<long long>(i) == last_match + 1)
                score += 4;
            last_match = static_cast<long long>(i);
            if (run_start == std::string::npos)
                run_start = i;
            ++qi;
        }
        if (qi < query.size()) {
            if (out_spans)
                out_spans->clear();
            return 0;
        }
        if (run_start != std::string::npos && out_spans) {
            out_spans->emplace_back(run_start, static_cast<std::size_t>(last_match) + 1);
        }
        const int density = rdiv(query.size() * 50, name.size());
        return 100 + score + density;
    }
}

std::vector<CompletionMatch> BuildCompletions(const TokenInfo& token,
                                              std::size_t max_results,
                                              std::size_t description_clip) {
    auto& C = Console::Get();
    std::vector<CompletionMatch> pool;

    if (token.is_token0) {
        // Token 0: every cvar + every command (platform-filter applies
        // inside EnumerateCVars).
        C.EnumerateCVars("", [&](CVar& v) {
            CompletionMatch m;
            m.name = v.name;
            m.kind = CompletionKind::Cvar;
            m.value = v.value;
            m.description = v.description;
            pool.push_back(std::move(m));
        });
        C.EnumerateCommands("", [&](Command& c) {
            CompletionMatch m;
            m.name = c.name;
            m.kind = CompletionKind::Command;
            m.value = c.default_args;
            m.description = c.description;
            pool.push_back(std::move(m));
        });
    } else if (token.first_tok == "toggle") {
        // `toggle` special-case: token 1 is itself a cvar name, and
        // only cvars with allowed_values are meaningful targets.
        C.EnumerateCVars("", [&](CVar& v) {
            if (v.allowed_values.empty())
                return;
            CompletionMatch m;
            m.name = v.name;
            m.kind = CompletionKind::Cvar;
            m.value = v.value;
            m.description = v.description;
            pool.push_back(std::move(m));
        });
    } else if (token.first_tok == "exec") {
        // Filesystem completion for `exec <path>`. Lists the directory
        // referenced by the typed prefix: subdirectories (suffixed with
        // '/') + .cfg files.
        namespace fs = std::filesystem;
        const std::string& typed = token.text;
        std::string dir_str;
        std::string partial;
        if (typed.empty()) {
            dir_str = ".";
            partial = "";
        } else if (typed.back() == '/' || typed.back() == '\\') {
            dir_str = typed.substr(0, typed.size() - 1);
            if (dir_str.empty())
                dir_str = "/";
            partial = "";
        } else {
            const auto slash = typed.find_last_of("/\\");
            if (slash == std::string::npos) {
                dir_str = ".";
                partial = typed;
            } else {
                dir_str = typed.substr(0, slash);
                if (dir_str.empty())
                    dir_str = "/";
                partial = typed.substr(slash + 1);
            }
        }
        std::error_code ec;
        fs::path dir_path(dir_str);
        if (fs::is_directory(dir_path, ec)) {
            for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
                const std::string fname = entry.path().filename().string();
                if (fname.empty() || fname[0] == '.')
                    continue;
                if (!partial.empty()) {
                    if (fname.size() < partial.size())
                        continue;
                    bool prefix_ok = true;
                    for (std::size_t i = 0; i < partial.size(); ++i) {
                        if (to_lower_ascii(fname[i]) != to_lower_ascii(partial[i])) {
                            prefix_ok = false;
                            break;
                        }
                    }
                    if (!prefix_ok)
                        continue;
                }
                const bool is_dir = entry.is_directory(ec);
                if (!is_dir && entry.path().extension() != ".cfg")
                    continue;
                std::string commit;
                if (dir_str == ".") {
                    commit = fname;
                } else {
                    commit = dir_str;
                    if (commit.back() != '/' && commit.back() != '\\')
                        commit += '/';
                    commit += fname;
                }
                if (is_dir)
                    commit += '/';
                CompletionMatch m;
                m.name = std::move(commit);
                m.kind = CompletionKind::Value;
                m.value = is_dir ? "(dir)" : "(cfg)";
                pool.push_back(std::move(m));
            }
        }
    } else {
        // Value position. Three sources, in priority order:
        //   1. CVar with allowed_values  -- list those, tag current /
        //      default via `value`.
        //   2. CVar without allowed_values -- offer [current, default]
        //      as one-shot suggestions.
        //   3. Command with default_args  -- show the default
        //      invocation as a single suggestion.
        auto* cv = C.FindCVar(token.first_tok);
        if (cv == nullptr) {
            auto rr = C.ResolveCommand(token.first_tok);
            if (!rr.canonical_name.empty()) {
                cv = C.FindCVar(rr.canonical_name);
            }
        }
        if (cv != nullptr) {
            if (!cv->allowed_values.empty()) {
                // Filter values whose per-value platform flag doesn't
                // permit the current build's host -- mirrors what
                // Execute() would reject anyway, so suggest + reject
                // stay consistent.
                for (std::size_t i = 0; i < cv->allowed_values.size(); ++i) {
                    const u32 mask =
                        (i < cv->allowed_value_flags.size()) ? cv->allowed_value_flags[i] : 0u;
                    if (!cvar_value_allowed_on_this_platform(mask))
                        continue;
                    const auto& v = cv->allowed_values[i];
                    CompletionMatch m;
                    m.name = v;
                    m.kind = CompletionKind::Value;
                    if (v == cv->value)
                        m.value = "current";
                    else if (v == cv->default_value)
                        m.value = "default";
                    pool.push_back(std::move(m));
                }
            } else {
                CompletionMatch cur;
                cur.name = cv->value;
                cur.kind = CompletionKind::Value;
                cur.value = "current";
                cur.description = cv->description;
                pool.push_back(std::move(cur));
                if (cv->default_value != cv->value) {
                    CompletionMatch dflt;
                    dflt.name = cv->default_value;
                    dflt.kind = CompletionKind::Value;
                    dflt.value = "default";
                    dflt.description = cv->description;
                    pool.push_back(std::move(dflt));
                }
            }
        } else if (auto* cmd = C.FindCommand(token.first_tok); cmd != nullptr) {
            if (!cmd->default_args.empty()) {
                CompletionMatch m;
                m.name = cmd->default_args;
                m.kind = CompletionKind::Value;
                m.value = "default";
                m.description = cmd->description;
                pool.push_back(std::move(m));
            }
        }
    }

    if (pool.empty())
        return {};

    // Score every candidate; keep score-bearing ones. Value-position
    // rows tagged "current" / "default" get a small score bonus so
    // they sort to the top of the popup when the user opens it at
    // `<cvar> ` (empty query, value position). Bonuses are small
    // enough that any real prefix / substring / fuzzy match still
    // beats them (prefix 1000+, substring 500+, fuzzy 100+).
    std::vector<CompletionMatch> ranked;
    ranked.reserve(pool.size());
    for (auto& m : pool) {
        const int s = ScoreMatch(m.name, token.text, &m.spans);
        if (s <= 0)
            continue;
        m.score = s;
        if (m.value == "current")
            m.score += 5;
        else if (m.value == "default")
            m.score += 2;
        ranked.push_back(std::move(m));
    }
    if (ranked.empty())
        return {};

    std::sort(ranked.begin(), ranked.end(), [](const CompletionMatch& a, const CompletionMatch& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.name.size() != b.name.size())
            return a.name.size() < b.name.size();
        return a.name < b.name;
    });

    if (max_results > 0 && ranked.size() > max_results) {
        ranked.resize(max_results);
    }
    if (description_clip > 0) {
        for (auto& m : ranked) {
            Utf8SafeTruncate(m.description, description_clip);
        }
    }
    return ranked;
}

}  // namespace psynder::console
