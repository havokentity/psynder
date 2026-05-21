// SPDX-License-Identifier: MIT
// Psynder — software drop-down developer console overlay (impl).
//
// State is a single file-static singleton (the UI is main-thread only, like
// the debug HUD — DESIGN.md §3.4). The overlay never allocates per render
// frame; std::string/vector mutate only on user input events (typing,
// submit, history recall), not in draw().

#include "ConsoleOverlay.h"

#include "core/console/Completion.h"
#include "core/console/Console.h"
#include "ui/imm/DebugHud.h"
#include "ui/imm/Imm.h"

#include "core/Types.h"
#include "math/Math.h"
#include "platform/Platform.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
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
// 2 px of leading for legibility. Colours are packed via rgba() below
// (0xAABBGGRR, the live framebuffer byte order). The panel is a top-lit
// slate-blue gradient + blueprint grid; the palette is a muted "Nord"-ish set.
constexpr i32 kCharW = 6;
constexpr i32 kLineH = 10;
constexpr f32 kPad = 6.0f;
constexpr f32 kPanelFrac = 0.55f;  // fraction of framebuffer height when open
constexpr f32 kAnimSpeed = 9.0f;   // open/close slide, units of "anim" per sec
constexpr f32 kRepeatDelay = 0.40f;
constexpr f32 kRepeatRate = 0.045f;

// Background: a top-lit slate-blue gradient (brighter at the top edge, fading
// darker toward the prompt) shaped by a vignette, a faint blueprint grid, a
// bright top "lip", scanlines, and dither. Milder + clearly textured vs the
// old near-black, still dark enough that the light text stays crisp.
constexpr f32 kBgTopR = 38.0f, kBgTopG = 48.0f, kBgTopB = 68.0f;  // #263044 (top, lit)
constexpr f32 kBgBotR = 16.0f, kBgBotG = 21.0f, kBgBotB = 32.0f;  // #101520 (bottom)
constexpr u32 kGridSpacing = 32u;                                 // faint grid cell size in px

// The live framebuffer stores pixels as 0xAABBGGRR (R in the LOW byte) — the
// same packing clear_framebuffer, the samples, and the platform present use.
// (imm's own rgba() literals are the opposite byte order, which is why a flat
// fill came out red — the alpha byte landed in red.) We pack our colours the
// right way and hand the u32s straight to imm::label / filled_rect, which
// write them verbatim, so the console renders true colour.
constexpr u32 rgba(u8 r, u8 g, u8 b, u8 a = 0xFFu) noexcept {
    return static_cast<u32>(r) | (static_cast<u32>(g) << 8) | (static_cast<u32>(b) << 16) |
           (static_cast<u32>(a) << 24);
}

constexpr u32 kColBorder = rgba(0x7F, 0xB3, 0xC4);  // soft frost cyan (edge + prompt)
constexpr u32 kColInput = rgba(0xE0, 0xE5, 0xEE);   // prompt text (snow)
constexpr u32 kColGhost = rgba(0x4C, 0x56, 0x6A);   // dim slate autocomplete ghost
constexpr u32 kColText = rgba(0xBF, 0xC6, 0xD2);    // scrollback grey
constexpr u32 kColEcho = rgba(0x8F, 0xBC, 0xBB);    // echoed "] command" lines (teal)
constexpr u32 kColError = rgba(0xBF, 0x61, 0x6A);   // muted aurora red
constexpr u32 kColBannerA = rgba(0x7F, 0xB3, 0xC4);
constexpr u32 kColBannerB = rgba(0xB4, 0x8E, 0xAD);  // soft muted purple

constexpr usize kMaxScrollback = 512;

// Draw the panel background straight into the framebuffer (per-pixel work the
// imm row-fills can't do): slate-blue vertical gradient + a gentle 2D vignette
// that focuses the centre so text pops + faint scanlines + a little hash
// dither to kill banding. Packs via rgba() (0xAABBGGRR — the live framebuffer
// byte order), so colours come out true rather than channel-swapped.
void draw_panel_bg(render::Framebuffer& fb, f32 panel_h) noexcept {
    if (fb.format != render::PixelFormat::RGBA8 && fb.format != render::PixelFormat::BGRA8) {
        // Non-32-bit target: flat fill via the public API and bail.
        imm::filled_rect(math::Vec2{0.0f, 0.0f},
                         math::Vec2{static_cast<f32>(fb.width), panel_h},
                         rgba(0x1A, 0x20, 0x30));
        return;
    }
    const u32 ph = std::min(static_cast<u32>(panel_h), fb.height);
    const u32 w = fb.width;
    const f32 inv_w = (w > 1u) ? 1.0f / static_cast<f32>(w - 1u) : 1.0f;
    const f32 inv_h = (panel_h > 1.0f) ? 1.0f / panel_h : 1.0f;
    auto clamp8 = [](f32 v) noexcept -> u8 {
        const i32 i = static_cast<i32>(v + 0.5f);
        return static_cast<u8>(i < 0 ? 0 : (i > 255 ? 255 : i));
    };
    for (u32 y = 0; y < ph; ++y) {
        const f32 ty = static_cast<f32>(y) * inv_h;  // 0..1 top -> bottom
        const f32 baseR = kBgTopR + (kBgBotR - kBgTopR) * ty;
        const f32 baseG = kBgTopG + (kBgBotG - kBgTopG) * ty;
        const f32 baseB = kBgTopB + (kBgBotB - kBgTopB) * ty;
        const f32 dy = ty * 2.0f - 1.0f;                 // -1..1 (vertical)
        const f32 scan = (y % 3u == 0u) ? 0.96f : 1.0f;  // very faint scanline
        const bool grid_h = (y % kGridSpacing) == 0u;
        // Bright lip on the top 2 rows so the drawer reads as catching light.
        const f32 lip = (y < 2u) ? 26.0f : 0.0f;
        u32* row = reinterpret_cast<u32*>(fb.pixels + static_cast<usize>(fb.pitch) * y);
        for (u32 x = 0; x < w; ++x) {
            const f32 dx = static_cast<f32>(x) * inv_w * 2.0f - 1.0f;  // -1..1
            const f32 vig = 1.0f - 0.26f * (dx * dx * 0.55f + dy * dy * 0.45f);
            // Faint blueprint grid (slightly brighter on cell lines).
            const f32 grid = (grid_h || (x % kGridSpacing) == 0u) ? 8.0f : 0.0f;
            u32 hsh = (x * 374761393u) ^ (y * 668265263u);
            hsh ^= hsh >> 13;
            const f32 dith = static_cast<f32>(hsh & 3u) - 1.5f;  // ~ -1.5..+1.5
            const f32 m = vig * scan;
            const f32 add = grid + lip + dith;
            row[x] = rgba(clamp8(baseR * m + add),
                          clamp8(baseG * m + add * 1.1f),
                          clamp8(baseB * m + add * 1.3f));
        }
    }
}

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

    // Navigable completion popup (Up/Down select, Tab accepts). Rebuilt each
    // frame from the cursor token; while it's active Up/Down drive the list
    // instead of command history.
    std::vector<std::string> comp_items;
    int comp_sel = 0;
    bool comp_active = false;
    std::string comp_token;  // token the list was built for (detect changes)

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

// Defined below (near the autocomplete helpers); used by r_resolution's
// on_change handler registered in ensure_init.
bool parse_resolution(std::string_view s, u32& w, u32& h) noexcept;

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
    con.RegisterCommand("echo",
                        "Print the arguments back to the console.",
                        [](std::span<const std::string_view> args, psynder::console::Output& out) {
                            std::string line;
                            for (usize i = 0; i < args.size(); ++i) {
                                if (i != 0)
                                    line.push_back(' ');
                                line.append(args[i]);
                            }
                            out.PrintLine(line);
                        });
    con.RegisterCommand("quit",
                        "Exit the application.",
                        [](std::span<const std::string_view>, psynder::console::Output&) {
                            std::exit(0);  // dev console in sample apps; runs atexit + static dtors
                        });
    con.RegisterCommand("exit",
                        "Alias for quit.",
                        [](std::span<const std::string_view>, psynder::console::Output&) {
                            std::exit(0);
                        });

    // The debug HUD cvar everything documents but nothing actually registered:
    // off | compact | full, wired straight to the lane-16 HUD mode (the same
    // state F1 cycles). Lets `r_debug_hud full` work from the prompt.
    con.RegisterCVar("r_debug_hud",
                     "off",
                     "Debug HUD overlay: off | compact | full.",
                     0,
                     [](const psynder::console::CVar& v) {
                         using ui::imm::DebugHudMode;
                         ui::imm::set_debug_hud_mode(v.value == "full"      ? DebugHudMode::Full
                                                     : v.value == "compact" ? DebugHudMode::Compact
                                                                            : DebugHudMode::Off);
                     });

    // ── Display control (drives the platform's active window) ──────────────
    // r_resolution carries a preset list so `r_resolution <Tab>` lists the
    // modes; on change it resizes the window (the framebuffer is unchanged —
    // present stretches it to fit). r_fullscreen flips borderless full-screen.
    if (auto* res = con.RegisterCVar("r_resolution",
                                     "1280x720",
                                     "Window resolution (windowed): WIDTHxHEIGHT.",
                                     0,
                                     [](const psynder::console::CVar& v) {
                                         u32 w = 0, h = 0;
                                         if (parse_resolution(v.value, w, h))
                                             platform::request_window_size(w, h);
                                     })) {
        res->allowed_values = {"640x360", "1280x720", "1600x900", "1920x1080", "2560x1440"};
    }
    if (auto* fsv =
            con.RegisterCVar("r_fullscreen",
                             "0",
                             "Borderless full-screen (frame stretched to fit): 0 | 1.",
                             0,
                             [](const psynder::console::CVar& v) {
                                 platform::request_fullscreen(v.value == "1" || v.value == "true");
                             })) {
        fsv->allowed_values = {"0", "1"};
    }
    con.RegisterCommand("fullscreen",
                        "Toggle borderless full-screen.",
                        [](std::span<const std::string_view>, psynder::console::Output& out) {
                            platform::toggle_fullscreen();
                            out.PrintLine(platform::is_fullscreen() ? "fullscreen: on"
                                                                    : "fullscreen: off");
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

// Parse "WIDTHxHEIGHT" (e.g. "1920x1080"). Returns false if malformed.
bool parse_resolution(std::string_view s, u32& w, u32& h) noexcept {
    const usize x = s.find_first_of("xX");
    if (x == std::string_view::npos)
        return false;
    auto to_u = [](std::string_view t, u32& out) noexcept {
        if (t.empty())
            return false;
        u32 v = 0;
        for (char c : t) {
            if (c < '0' || c > '9')
                return false;
            v = v * 10u + static_cast<u32>(c - '0');
        }
        out = v;
        return true;
    };
    return to_u(s.substr(0, x), w) && to_u(s.substr(x + 1), h);
}

// Candidate completions for the cursor token, via the shared dmonte-ported
// Completion engine: prefix > substring > fuzzy scoring (so `res` matches
// `r_resolution`), and value-position aware (so once the first token is a
// cvar with presets, it lists those — e.g. `r_resolution <Tab>`).
std::vector<std::string> gather_matches(const State& s) noexcept {
    const auto token = psynder::console::CurrentToken(s.input, s.cursor);
    const auto ranked = psynder::console::BuildCompletions(token, /*max_results*/ 24);
    std::vector<std::string> out;
    out.reserve(ranked.size());
    for (const auto& m : ranked)
        out.push_back(m.name);
    return out;
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
    s.comp_active = false;
    s.comp_token.clear();
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

// Byte offset where the cursor's token begins (for popup placement).
usize token_start(const State& s) noexcept {
    usize start = s.cursor;
    while (start > 0 && is_word_char(s.input[start - 1]))
        --start;
    return start;
}

// Rebuild the completion popup from the current cursor token. Resets the
// highlighted row when the token changes so a fresh prefix starts at the top.
void refresh_completion(State& s) noexcept {
    const auto token = psynder::console::CurrentToken(s.input, s.cursor);
    s.comp_items = gather_matches(s);
    // Show the popup while typing a token, OR whenever the cursor is in a
    // value position (so right after "r_resolution " the option list appears
    // before any value char is typed). Hidden for an empty FIRST token so
    // merely opening the console doesn't dump the whole registry.
    s.comp_active = !s.comp_items.empty() && (!token.text.empty() || !token.is_token0);
    if (token.text != s.comp_token) {
        s.comp_token = token.text;
        s.comp_sel = 0;
    }
    const int n = static_cast<int>(s.comp_items.size());
    if (s.comp_sel >= n)
        s.comp_sel = n - 1;
    if (s.comp_sel < 0)
        s.comp_sel = 0;
}

void process_edit_keys(State& s, const platform::Input& input, f32 dt) noexcept {
    refresh_completion(s);  // popup mirrors the current prompt token

    if (input.key_pressed(KeyCode::Enter))
        submit(s);
    if (input.key_pressed(KeyCode::Escape)) {
        // Esc never quits while the console owns input: dismiss the popup,
        // else clear the prompt, else (already empty) close the console.
        if (s.comp_active)
            s.comp_active = false;
        else if (!s.input.empty()) {
            s.input.clear();
            s.cursor = 0;
            s.history_pos = -1;
        } else {
            s.open = false;
        }
    }
    if (input.key_pressed(KeyCode::Tab)) {
        if (s.comp_active && s.comp_sel >= 0 && s.comp_sel < static_cast<int>(s.comp_items.size())) {
            replace_token_at_cursor(s, s.comp_items[static_cast<usize>(s.comp_sel)] + " ");
            s.comp_active = false;
            s.history_pos = -1;
        } else {
            autocomplete(s);
        }
    }
    // Up/Down navigate the completion list when it's showing; otherwise they
    // walk command history.
    if (input.key_pressed(KeyCode::Up)) {
        if (s.comp_active) {
            const int n = static_cast<int>(s.comp_items.size());
            s.comp_sel = (s.comp_sel > 0) ? s.comp_sel - 1 : n - 1;
        } else {
            history_prev(s);
        }
    }
    if (input.key_pressed(KeyCode::Down)) {
        if (s.comp_active) {
            const int n = static_cast<int>(s.comp_items.size());
            s.comp_sel = (n > 0) ? (s.comp_sel + 1) % n : 0;
        } else {
            history_next(s);
        }
    }

    if (key_repeat(s, input, KeyCode::Backspace, dt) && s.cursor > 0) {
        const usize n = utf8_prev_len(s.input, s.cursor);
        s.input.erase(s.cursor - n, n);
        s.cursor -= n;
        s.history_pos = -1;
    }
    // Forward delete: remove the char AT the caret (the one to its right).
    if (key_repeat(s, input, KeyCode::Delete, dt) && s.cursor < s.input.size()) {
        const usize n = utf8_next_len(s.input, s.cursor);
        s.input.erase(s.cursor, n);
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

    // Tell the platform we're capturing text while open, so the backend's
    // default Escape-closes-the-window is suppressed (Esc is ours: clear the
    // prompt / dismiss the popup). Cleared when the console is shut.
    platform::set_text_input_capturing(s.open);

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

    // Slate-blue gradient + vignette + faint scanline background, then a 2-px
    // cyan bottom edge so the panel reads as a drawer.
    draw_panel_bg(fb, panel_h);
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

    // Ghost: remainder of the HIGHLIGHTED completion, inline past the cursor
    // (so navigating the popup updates the inline hint too).
    if (s.comp_active && s.cursor == s.input.size() && s.comp_sel >= 0 &&
        s.comp_sel < static_cast<int>(s.comp_items.size())) {
        const std::string& sel = s.comp_items[static_cast<usize>(s.comp_sel)];
        const std::string_view tok = token_at_cursor(s);
        if (sel.size() > tok.size()) {
            const std::string_view ghost = std::string_view{sel}.substr(tok.size());
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

    // ── Completion popup: a navigable list above the prompt, anchored under
    // the token being typed. Up/Down move the highlight; Tab accepts it. ────
    if (s.comp_active && !s.comp_items.empty()) {
        const int n = static_cast<int>(s.comp_items.size());
        constexpr int kMaxVis = 8;
        const int vis = std::min(kMaxVis, n);
        int first = 0;
        if (n > kMaxVis)
            first = std::clamp(s.comp_sel - kMaxVis / 2, 0, n - kMaxVis);

        usize longest = 0;
        for (int i = 0; i < vis; ++i)
            longest = std::max(longest, s.comp_items[static_cast<usize>(first + i)].size());

        const f32 box_w = static_cast<f32>(longest) * static_cast<f32>(kCharW) + 8.0f;
        const f32 box_h = static_cast<f32>(vis) * static_cast<f32>(kLineH) + 4.0f;
        const f32 box_x = text_x + static_cast<f32>(token_start(s)) * static_cast<f32>(kCharW);
        const f32 box_y = std::max(kPad, prompt_y - box_h - 3.0f);

        imm::filled_rect(math::Vec2{box_x, box_y}, math::Vec2{box_w, box_h}, rgba(0x0E, 0x13, 0x20));
        imm::rect_outline(math::Vec2{box_x, box_y}, math::Vec2{box_w, box_h}, kColBorder);
        for (int i = 0; i < vis; ++i) {
            const int idx = first + i;
            const f32 iy = box_y + 2.0f + static_cast<f32>(i) * static_cast<f32>(kLineH);
            const std::string& item = s.comp_items[static_cast<usize>(idx)];
            if (idx == s.comp_sel) {
                imm::filled_rect(math::Vec2{box_x + 1.0f, iy - 1.0f},
                                 math::Vec2{box_w - 2.0f, static_cast<f32>(kLineH)},
                                 rgba(0x2A, 0x3A, 0x5A));  // selection highlight
                imm::label(math::Vec2{box_x + 4.0f, iy}, item, kColInput);
            } else {
                imm::label(math::Vec2{box_x + 4.0f, iy}, item, kColText);
            }
        }
    }

    imm::end_frame();
}

bool step(const platform::Input& input, render::Framebuffer& fb, f32 dt) noexcept {
    const bool capturing = update(input, dt);
    draw(fb);
    return capturing;
}

}  // namespace psynder::ui::console
