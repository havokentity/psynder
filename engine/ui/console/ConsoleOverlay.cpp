// SPDX-License-Identifier: MIT
// Psynder — software drop-down developer console overlay (impl).
//
// State is a single file-static singleton (the UI is main-thread only, like
// the debug HUD — DESIGN.md §3.4). The overlay never allocates per render
// frame; std::string/vector mutate only on user input events (typing,
// submit, history recall), not in draw().

#include "ConsoleOverlay.h"

#include "core/console/Console.h"
#include "ui/imm/Imm.h"

#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace psynder::ui::console {
namespace {

using platform::KeyCode;

// ─── Look & layout ─────────────────────────────────────────────────────────
// imm's 5x7 glyph sits in a 6x8 cell; draw_text advances 6 px / char. We add
// 2 px of leading for legibility. Colours are 0xRRGGBBAA (filled_rect writes
// verbatim, so the panel reads as a solid dark slab — the classic Quake look).
constexpr i32 kCharW = 6;
constexpr i32 kLineH = 10;
constexpr f32 kPad = 6.0f;
constexpr f32 kPanelFrac = 0.55f;  // fraction of framebuffer height when open
constexpr f32 kAnimSpeed = 9.0f;   // open/close slide, units of "anim" per sec
constexpr f32 kRepeatDelay = 0.40f;
constexpr f32 kRepeatRate = 0.045f;

constexpr u32 kColPanel = 0x0A0F1AF2u;   // dark navy slab
constexpr u32 kColBorder = 0x36D9F0FFu;  // cyan accent (bottom edge + prompt)
constexpr u32 kColInput = 0xECECF0FFu;   // prompt text
constexpr u32 kColGhost = 0x5E727EFFu;   // dim autocomplete ghost
constexpr u32 kColText = 0xC2CCD4FFu;    // scrollback
constexpr u32 kColEcho = 0x8FE3FFFFu;    // echoed "] command" lines
constexpr u32 kColError = 0xFF6B6BFFu;   // error output
constexpr u32 kColBannerA = 0x36D9F0FFu;
constexpr u32 kColBannerB = 0xFF5EA3FFu;

constexpr usize kMaxScrollback = 512;

// A scrollback line + its colour. Echo / error / banner get distinct tints.
struct Line {
    std::string text;
    u32 colour = kColText;
};

struct State {
    bool open = false;
    f32 anim = 0.0f;  // 0 = closed, 1 = fully dropped
    f32 blink = 0.0f;

    std::string input;  // current prompt (UTF-8 bytes; ASCII is the norm)
    usize cursor = 0;   // byte index into `input`

    std::vector<Line> scrollback;
    u32 scroll = 0;  // lines scrolled up from the newest

    int history_pos = -1;    // -1 = editing live line; else index into backend history
    std::string saved_live;  // live line stashed while browsing history

    // Per-key auto-repeat bookkeeping for the editing keys.
    std::array<f32, static_cast<usize>(KeyCode::Count)> held{};
    std::array<f32, static_cast<usize>(KeyCode::Count)> next_fire{};

    bool initialised = false;
};

State& state() noexcept {
    static State s;
    return s;
}

void push_line(State& s, std::string text, u32 colour) noexcept {
    s.scrollback.push_back(Line{std::move(text), colour});
    if (s.scrollback.size() > kMaxScrollback)
        s.scrollback.erase(s.scrollback.begin(),
                           s.scrollback.begin() +
                               static_cast<long>(s.scrollback.size() - kMaxScrollback));
}

// Split a (possibly multi-line) blob into scrollback lines, dropping a single
// trailing newline so we don't emit a blank line per command.
void push_blob(State& s, std::string_view blob, u32 colour) noexcept {
    if (!blob.empty() && blob.back() == '\n')
        blob.remove_suffix(1);
    usize start = 0;
    for (usize i = 0; i <= blob.size(); ++i) {
        if (i == blob.size() || blob[i] == '\n') {
            push_line(s, std::string{blob.substr(start, i - start)}, colour);
            start = i + 1;
        }
    }
}

// ─── Built-in overlay commands ──────────────────────────────────────────────
// Registered once. `clear` wipes the scrollback; `help` lists every command +
// cvar the backend knows. Output goes through the ExecuteResult, which the
// submit path tints and pushes into scrollback.
void ensure_init(State& s) noexcept {
    if (s.initialised)
        return;
    s.initialised = true;

    auto& con = psynder::console::Console::Get();
    con.RegisterCommand("clear",
                        "Clear the console scrollback.",
                        [](std::span<const std::string_view>, psynder::console::Output&) {
                            State& st = state();
                            st.scrollback.clear();
                            st.scroll = 0;
                        });
    con.RegisterCommand("help",
                        "List all registered commands and cvars.",
                        [](std::span<const std::string_view>, psynder::console::Output& out) {
                            auto& c = psynder::console::Console::Get();
                            out.PrintLine("commands:");
                            c.EnumerateCommands("", [&](psynder::console::Command& cmd) {
                                out.FormatLine("  {:<16} {}", cmd.name, cmd.description);
                            });
                            out.PrintLine("cvars:");
                            c.EnumerateCVars("", [&](psynder::console::CVar& v) {
                                out.FormatLine("  {:<16} = {}", v.name, v.value);
                            });
                        });

    push_line(s, "Psynder console.  `~` close   Tab complete   Up/Down history   `help`", kColBannerA);
    push_line(s, "------------------------------------------------------------", kColBannerB);
}

// ─── UTF-8 helpers (ASCII fast path; multibyte tolerated) ───────────────────
int utf8_encode(u32 cp, char out[4]) noexcept {
    if (cp < 0x80u) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = static_cast<char>(0xC0u | (cp >> 6));
        out[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = static_cast<char>(0xE0u | (cp >> 12));
        out[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
        return 3;
    }
    out[0] = static_cast<char>(0xF0u | (cp >> 18));
    out[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
    return 4;
}

// Byte length of the UTF-8 char ending just before `pos` (>=1).
usize utf8_prev_len(const std::string& s, usize pos) noexcept {
    if (pos == 0)
        return 0;
    usize n = 1;
    while (pos - n > 0 && (static_cast<unsigned char>(s[pos - n]) & 0xC0u) == 0x80u)
        ++n;
    return n;
}

// Byte length of the UTF-8 char starting at `pos` (>=1).
usize utf8_next_len(const std::string& s, usize pos) noexcept {
    if (pos >= s.size())
        return 0;
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80u)
        return 1;
    if ((c >> 5) == 0x6u)
        return 2;
    if ((c >> 4) == 0xEu)
        return 3;
    if ((c >> 3) == 0x1Eu)
        return 4;
    return 1;
}

bool is_word_char(char c) noexcept {
    const unsigned char u = static_cast<unsigned char>(c);
    return u > ' ';  // any non-whitespace, non-control byte
}

// The whitespace-delimited token that ends at the cursor (for autocomplete).
std::string_view token_at_cursor(const State& s) noexcept {
    usize start = s.cursor;
    while (start > 0 && is_word_char(s.input[start - 1]))
        --start;
    return std::string_view{s.input}.substr(start, s.cursor - start);
}

void replace_token_at_cursor(State& s, std::string_view replacement) noexcept {
    usize start = s.cursor;
    while (start > 0 && is_word_char(s.input[start - 1]))
        --start;
    s.input.replace(start, s.cursor - start, replacement.data(), replacement.size());
    s.cursor = start + replacement.size();
}

std::string longest_common_prefix(const std::vector<std::string>& v) noexcept {
    if (v.empty())
        return {};
    std::string p = v.front();
    for (usize i = 1; i < v.size(); ++i) {
        usize k = 0;
        while (k < p.size() && k < v[i].size() && p[k] == v[i][k])
            ++k;
        p.resize(k);
        if (p.empty())
            break;
    }
    return p;
}

// Gather command + cvar names matching the cursor token.
std::vector<std::string> gather_matches(const State& s) noexcept {
    std::vector<std::string> matches;
    const std::string_view tok = token_at_cursor(s);
    if (tok.empty())
        return matches;
    auto& con = psynder::console::Console::Get();
    con.EnumerateCommands(tok, [&](psynder::console::Command& c) { matches.push_back(c.name); });
    con.EnumerateCVars(tok, [&](psynder::console::CVar& v) { matches.push_back(v.name); });
    std::sort(matches.begin(), matches.end());
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    return matches;
}

// ─── Input handling ──────────────────────────────────────────────────────────
void process_text(State& s, const platform::Input& input) noexcept {
    for (u32 cp : input.text_input()) {
        if (cp == 0x60u || cp == 0x7Eu)  // '`' / '~' are the toggle key glyphs
            continue;
        if (cp < 0x20u || cp == 0x7Fu)  // controls (defensive — backends filter too)
            continue;
        char tmp[4];
        const int n = utf8_encode(cp, tmp);
        s.input.insert(s.cursor, tmp, static_cast<usize>(n));
        s.cursor += static_cast<usize>(n);
        s.history_pos = -1;  // typing forks off the live line
    }
}

void submit(State& s) noexcept {
    const std::string line = s.input;
    push_line(s, "] " + line, kColEcho);

    // Trim for the "is this blank" test, but execute the raw line.
    const auto first = line.find_first_not_of(" \t");
    if (first != std::string::npos) {
        auto& con = psynder::console::Console::Get();
        con.PushHistory(line);
        const psynder::console::ExecuteResult res = con.Execute(line);
        if (!res.output.empty())
            push_blob(s, res.output, res.ok ? kColText : kColError);
        if (!res.error.empty())
            push_blob(s, res.error, kColError);
    }

    s.input.clear();
    s.cursor = 0;
    s.history_pos = -1;
    s.scroll = 0;
}

void autocomplete(State& s) noexcept {
    const std::vector<std::string> matches = gather_matches(s);
    if (matches.empty())
        return;
    if (matches.size() == 1) {
        replace_token_at_cursor(s, matches.front() + " ");
        return;
    }
    const std::string lcp = longest_common_prefix(matches);
    const std::string_view tok = token_at_cursor(s);
    if (lcp.size() > tok.size())
        replace_token_at_cursor(s, lcp);
    // List the candidates so the user can see the branch.
    std::string list;
    for (const std::string& m : matches) {
        list += m;
        list += "  ";
    }
    push_line(s, std::move(list), kColText);
}

void history_prev(State& s) noexcept {  // Up
    const std::vector<std::string> h = psynder::console::Console::Get().History();
    if (h.empty())
        return;
    if (s.history_pos == -1) {
        s.saved_live = s.input;
        s.history_pos = static_cast<int>(h.size()) - 1;
    } else if (s.history_pos > 0) {
        --s.history_pos;
    }
    s.input = h[static_cast<usize>(s.history_pos)];
    s.cursor = s.input.size();
}

void history_next(State& s) noexcept {  // Down
    const std::vector<std::string> h = psynder::console::Console::Get().History();
    if (s.history_pos == -1)
        return;
    if (s.history_pos + 1 < static_cast<int>(h.size())) {
        ++s.history_pos;
        s.input = h[static_cast<usize>(s.history_pos)];
    } else {
        s.history_pos = -1;
        s.input = s.saved_live;
    }
    s.cursor = s.input.size();
}

// Edge + auto-repeat for a held editing key. Returns the number of triggers
// this frame (1 on the initial edge, then ~kRepeatRate apart while held).
bool key_repeat(State& s, const platform::Input& input, KeyCode k, f32 dt) noexcept {
    const usize i = static_cast<usize>(k);
    const bool pressed = input.key_pressed(k);  // rising edge (read-and-clear)
    const bool down = input.key_down(k);
    if (pressed) {
        s.held[i] = 0.0f;
        s.next_fire[i] = kRepeatDelay;
        return true;
    }
    if (down) {
        s.held[i] += dt;
        if (s.held[i] >= s.next_fire[i]) {
            s.next_fire[i] += kRepeatRate;
            return true;
        }
        return false;
    }
    s.held[i] = 0.0f;
    s.next_fire[i] = kRepeatDelay;
    return false;
}

void process_edit_keys(State& s, const platform::Input& input, f32 dt) noexcept {
    if (input.key_pressed(KeyCode::Enter))
        submit(s);
    if (input.key_pressed(KeyCode::Escape))
        s.open = false;
    if (input.key_pressed(KeyCode::Tab))
        autocomplete(s);
    if (input.key_pressed(KeyCode::Up))
        history_prev(s);
    if (input.key_pressed(KeyCode::Down))
        history_next(s);

    if (key_repeat(s, input, KeyCode::Backspace, dt) && s.cursor > 0) {
        const usize n = utf8_prev_len(s.input, s.cursor);
        s.input.erase(s.cursor - n, n);
        s.cursor -= n;
        s.history_pos = -1;
    }
    if (key_repeat(s, input, KeyCode::Left, dt) && s.cursor > 0)
        s.cursor -= utf8_prev_len(s.input, s.cursor);
    if (key_repeat(s, input, KeyCode::Right, dt) && s.cursor < s.input.size())
        s.cursor += utf8_next_len(s.input, s.cursor);

    // Mouse wheel scrolls the scrollback (3 lines/notch). Clamp later in draw.
    const f32 wheel = input.mouse().wheel;
    if (wheel > 0.0f)
        s.scroll += 3;
    else if (wheel < 0.0f)
        s.scroll = (s.scroll > 3u) ? s.scroll - 3u : 0u;
}

}  // namespace

// ─── Public surface ──────────────────────────────────────────────────────────
bool is_open() noexcept {
    return state().open;
}
void set_open(bool open) noexcept {
    state().open = open;
}
void toggle() noexcept {
    state().open = !state().open;
}

void print_line(std::string_view text) noexcept {
    push_line(state(), std::string{text}, kColText);
}

void reset() noexcept {
    State& s = state();
    const bool was_init = s.initialised;
    s = State{};
    s.initialised = was_init;  // keep built-in commands registered
}

bool update(const platform::Input& input, f32 dt) noexcept {
    State& s = state();
    ensure_init(s);

    const bool was_open = s.open;
    const bool toggle_edge = input.key_pressed(KeyCode::Tilde);
    if (toggle_edge)
        s.open = !s.open;

    // Slide animation toward the open/closed target.
    const f32 target = s.open ? 1.0f : 0.0f;
    if (s.anim < target)
        s.anim = std::min(target, s.anim + kAnimSpeed * dt);
    else if (s.anim > target)
        s.anim = std::max(target, s.anim - kAnimSpeed * dt);

    // Skip input on the toggle frame so the backtick that opened us doesn't
    // land in the prompt (process_text also filters '`'/'~' belt-and-braces).
    if (s.open && !toggle_edge) {
        process_text(s, input);
        process_edit_keys(s, input, dt);
    }

    s.blink += dt;

    // Capturing if the console consumed input this frame: it was open at
    // entry (so it ate the keystrokes) or it is open now (just toggled on).
    // This also makes the Esc-to-close frame report capture, so the host's
    // escape-to-quit doesn't also fire.
    return was_open || s.open;
}

void draw(render::Framebuffer& fb) noexcept {
    State& s = state();
    if (s.anim <= 0.001f)
        return;
    if (fb.width == 0 || fb.height == 0)
        return;

    const f32 fw = static_cast<f32>(fb.width);
    const f32 fh = static_cast<f32>(fb.height);
    const f32 panel_h = std::round(s.anim * std::floor(fh * kPanelFrac));
    if (panel_h < static_cast<f32>(kLineH))
        return;

    imm::begin_frame(fb);

    // Panel + 2-px cyan bottom edge so it reads as a drawer.
    imm::filled_rect(math::Vec2{0.0f, 0.0f}, math::Vec2{fw, panel_h}, kColPanel);
    imm::filled_rect(math::Vec2{0.0f, panel_h - 2.0f}, math::Vec2{fw, 2.0f}, kColBorder);

    // Prompt sits on the bottom row of the panel; scrollback fills above it.
    const f32 prompt_y = panel_h - static_cast<f32>(kLineH) - kPad;
    const f32 prompt_x = kPad;

    // ── Scrollback (newest at the bottom, scrolled up by s.scroll) ──────────
    const i32 area_top = static_cast<i32>(kPad);
    const i32 area_bot = static_cast<i32>(prompt_y) - 4;
    const i32 rows = (area_bot - area_top) / kLineH;
    if (rows > 0 && !s.scrollback.empty()) {
        const u32 total = static_cast<u32>(s.scrollback.size());
        const u32 max_scroll = (total > static_cast<u32>(rows)) ? total - static_cast<u32>(rows) : 0u;
        s.scroll = std::min(s.scroll, max_scroll);
        // Bottom visible line index = last - scroll.
        const i32 bottom_idx = static_cast<i32>(total) - 1 - static_cast<i32>(s.scroll);
        for (i32 r = 0; r < rows; ++r) {
            const i32 idx = bottom_idx - r;
            if (idx < 0)
                break;
            const Line& ln = s.scrollback[static_cast<usize>(idx)];
            const f32 y = prompt_y - 4.0f - static_cast<f32>((r + 1) * kLineH);
            if (y < static_cast<f32>(area_top))
                break;
            imm::label(math::Vec2{prompt_x, y}, ln.text, ln.colour);
        }
        // "more below" hint when scrolled up.
        if (s.scroll > 0)
            imm::label(math::Vec2{fw - 9.0f * kCharW, prompt_y - 4.0f - kLineH}, "v v v", kColBannerB);
    }

    // ── Prompt line: "> " + input + ghost completion + blinking caret ───────
    imm::label(math::Vec2{prompt_x, prompt_y}, ">", kColBorder);
    const f32 text_x = prompt_x + 2.0f * static_cast<f32>(kCharW);
    imm::label(math::Vec2{text_x, prompt_y}, s.input, kColInput);

    // Ghost: show the remainder of the best single match past the cursor.
    {
        const std::vector<std::string> matches = gather_matches(s);
        const std::string_view tok = token_at_cursor(s);
        if (!matches.empty() && matches.front().size() > tok.size() && s.cursor == s.input.size()) {
            const std::string_view ghost = std::string_view{matches.front()}.substr(tok.size());
            const f32 gx = text_x + static_cast<f32>(s.input.size()) * static_cast<f32>(kCharW);
            imm::label(math::Vec2{gx, prompt_y}, ghost, kColGhost);
        }
    }

    // Caret: a 1-px vertical bar at the cursor column, blinking at ~1.5 Hz.
    if (std::fmod(s.blink, 0.7f) < 0.45f) {
        const f32 cx = text_x + static_cast<f32>(s.cursor) * static_cast<f32>(kCharW);
        imm::filled_rect(math::Vec2{cx, prompt_y - 1.0f},
                         math::Vec2{1.0f, static_cast<f32>(kLineH)},
                         kColBorder);
    }

    imm::end_frame();
}

bool step(const platform::Input& input, render::Framebuffer& fb, f32 dt) noexcept {
    const bool capturing = update(input, dt);
    draw(fb);
    return capturing;
}

}  // namespace psynder::ui::console
