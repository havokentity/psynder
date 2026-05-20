// SPDX-License-Identifier: MIT
// Psynder — shared completion engine for the console (web UI + native
// overlays, when they ship). Ported from dmonte's
// `pt::console::Completion`. Three scoring layers, highest score wins
// per candidate; equal-score ties broken by name length ascending then
// lex order:
//
//   1. PREFIX     candidate starts with query                top rank
//   2. SUBSTRING  query is a contiguous run inside name      middle
//                 bonus when the run begins at a word boundary
//                 (position 0 or right after `_`)
//   3. FUZZY     chars of query appear in order in name,    bottom
//                 possibly with gaps; word-boundary bonus per char
//                 + streak bonus for consecutive matches.
//
// All comparisons are case-insensitive ASCII (cvar / command names in
// this engine are ASCII identifiers + digits + `_`).
//
// Two-step call pattern: a frontend first calls
//   TokenInfo t = CurrentToken(input, cursor);
// to identify the word at the cursor + first-token context, then
//   std::vector<CompletionMatch> matches = BuildCompletions(t);
// to get the ranked candidate list. Splitting the two lets the caller
// reuse the same TokenInfo for both the popup build AND the eventual
// commit-time splice (start/end byte indices for replacement).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psynder::console {

enum class CompletionKind : std::uint8_t {
    Cvar,
    Command,
    Value,
};

// One ranked match. `spans` is a list of [first, second) half-open
// char-index ranges within `name` that matched the query -- the UI uses
// these to bold / colour matched chars. Empty `spans` means an empty
// query matched everything trivially.
struct CompletionMatch {
    std::string name;
    CompletionKind kind = CompletionKind::Cvar;
    std::string value;        // current value or "current"/"default" tag
    std::string description;  // truncated by BuildCompletions
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    int score = 0;
};

// Word at the cursor plus enough context to pick the right candidate
// pool. `start..end` are byte indices into the original input string --
// ready for splice-style replacement on commit.
struct TokenInfo {
    std::size_t start = 0;
    std::size_t end = 0;
    std::string text;  // input.substr(start, end - start)
    bool is_token0 = true;
    std::string first_tok;  // first non-space token (for value-position picks)
};

TokenInfo CurrentToken(std::string_view input, std::size_t cursor);

// Score a candidate against the query. Returns 0 for no-match, > 0
// otherwise. `out_spans`, when non-null, is overwritten with the
// matched-char ranges within `name`. Spans are sorted ascending and
// non-overlapping.
int ScoreMatch(std::string_view name,
               std::string_view query,
               std::vector<std::pair<std::size_t, std::size_t>>* out_spans);

// Build the ranked candidate list for the current token context.
// Pulls the candidate pool from the live Console singleton (cvars +
// commands for token 0; cvar->allowed_values for value position; etc).
// Returns at most `max_results` matches sorted by (score desc, name
// length asc, name asc). `description_clip` truncates each candidate's
// description before returning so the caller doesn't have to. 0 = keep
// full text. Truncation is UTF-8 safe.
std::vector<CompletionMatch> BuildCompletions(const TokenInfo& token,
                                              std::size_t max_results = 60,
                                              std::size_t description_clip = 120);

// Truncate a UTF-8 string to AT MOST `max_bytes` bytes WITHOUT
// splitting a multibyte codepoint. If the byte at position `max_bytes`
// is a continuation byte (high bits 10xxxxxx), back up until we land on
// a codepoint START byte. Worst-case backup is 3 bytes (UTF-8 codepoints
// are at most 4 bytes long).
inline void Utf8SafeTruncate(std::string& s, std::size_t max_bytes) {
    if (s.size() <= max_bytes)
        return;
    while (max_bytes > 0 && (static_cast<unsigned char>(s[max_bytes]) & 0xC0) == 0x80) {
        --max_bytes;
    }
    s.resize(max_bytes);
}

}  // namespace psynder::console
