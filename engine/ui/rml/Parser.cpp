// SPDX-License-Identifier: MIT
// Psynder — RML/RCSS subset parser. Lane 17 owns.
//
// The full RmlUi parser lives upstream; we ship a small in-tree variant so
// Wave-A builds green without a multi-megabyte FetchContent. Surface kept
// identical: parse_rml + parse_rcss + apply_cascade. When the vendor flag
// flips on, this parser steps aside and RmlUi's own parser drives the same
// public load_document/show/hide.
//
// Grammar covered (Wave-A):
//   - RML: `<tag id="…" class="…" attr="…" style="…" onclick="…">…</tag>`
//          self-closing tags `<img src="…"/>`, text nodes, doctype + comments
//          skipped, whitespace collapsed within text.
//   - RCSS: `selector { prop: value; … }` with selector ∈
//           { *, tag, #id, .class }. Properties: left, top, width, height,
//           background-color (#rrggbb or rgba(...)), color, font-size,
//           display.
//
// Bugs in this subset are diagnostic-only (warn + ParseResult.error) —
// the goal is "the obvious HUDs parse correctly," not full conformance.

#include "Rml_internal.h"

#include "core/Log.h"

#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace psynder::ui::rml::detail {

namespace {

// ─── Token helpers ───────────────────────────────────────────────────────
struct Cursor {
    const char* p   = nullptr;
    const char* end = nullptr;
    u32 line = 1;

    bool eof() const noexcept { return p >= end; }
    char peek() const noexcept { return eof() ? '\0' : *p; }
    char peek_at(usize off) const noexcept {
        return (p + off >= end) ? '\0' : p[off];
    }

    void advance() noexcept {
        if (eof()) return;
        if (*p == '\n') ++line;
        ++p;
    }

    void skip_ws() noexcept {
        while (!eof() && std::isspace(static_cast<unsigned char>(*p))) advance();
    }

    bool starts_with(std::string_view s) const noexcept {
        return (static_cast<usize>(end - p) >= s.size())
            && std::memcmp(p, s.data(), s.size()) == 0;
    }

    void consume(usize n) noexcept {
        for (usize i = 0; i < n && !eof(); ++i) advance();
    }
};

[[nodiscard]] bool is_name_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'
        || c == ':' || c == '.';
}

std::string trim(std::string_view s) {
    usize a = 0;
    usize b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

[[nodiscard]] bool parse_float(std::string_view s, f32& out) {
    std::string trimmed = trim(s);
    if (trimmed.empty()) return false;
    // Strip trailing units: px, %, em
    auto strip_unit = [&](std::string_view unit) {
        if (trimmed.size() >= unit.size()
            && trimmed.compare(trimmed.size() - unit.size(), unit.size(), unit) == 0) {
            trimmed.resize(trimmed.size() - unit.size());
        }
    };
    strip_unit("px");
    strip_unit("em");
    strip_unit("%");
    try {
        usize idx = 0;
        out = std::stof(trimmed, &idx);
        return idx > 0;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool parse_hex_byte(const char* p, u8& out) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
        return -1;
    };
    const int hi = hex(p[0]);
    const int lo = hex(p[1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<u8>((hi << 4) | lo);
    return true;
}

// rgba()/rgb()/#rgb/#rrggbb → packed RGBA8.  Returns 0 on parse failure
// (which is also the "unset" sentinel — callers should check first).
[[nodiscard]] u32 parse_color(std::string_view raw, bool& ok) {
    std::string s = trim(raw);
    ok = false;
    if (s.empty()) return 0;

    if (s[0] == '#') {
        // #rgb or #rrggbb
        if (s.size() == 4) {  // #rgb
            auto expand = [](char c) -> u8 {
                u8 b = 0;
                if (parse_hex_byte((std::string{c, c}).c_str(), b)) return b;
                return 0;
            };
            u8 r = expand(s[1]);
            u8 g = expand(s[2]);
            u8 b = expand(s[3]);
            ok = true;
            return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | 0xFFu;
        }
        if (s.size() == 7) {  // #rrggbb
            u8 r = 0, g = 0, b = 0;
            if (!parse_hex_byte(s.c_str() + 1, r)) return 0;
            if (!parse_hex_byte(s.c_str() + 3, g)) return 0;
            if (!parse_hex_byte(s.c_str() + 5, b)) return 0;
            ok = true;
            return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | 0xFFu;
        }
        if (s.size() == 9) {  // #rrggbbaa
            u8 r = 0, g = 0, b = 0, a = 0;
            if (!parse_hex_byte(s.c_str() + 1, r)) return 0;
            if (!parse_hex_byte(s.c_str() + 3, g)) return 0;
            if (!parse_hex_byte(s.c_str() + 5, b)) return 0;
            if (!parse_hex_byte(s.c_str() + 7, a)) return 0;
            ok = true;
            return (u32(r) << 24) | (u32(g) << 16) | (u32(b) << 8) | u32(a);
        }
        return 0;
    }

    // rgba(r, g, b, a) / rgb(r, g, b) — both branches map to RGBA8.
    u8   comps[4] = {0, 0, 0, 0xFF};
    auto extract = [&](std::string_view prefix, bool has_alpha) -> bool {
        if (s.size() < prefix.size() + 2) return false;
        if (s.compare(0, prefix.size(), prefix) != 0) return false;
        if (s.back() != ')') return false;
        std::string_view body{ s.data() + prefix.size(), s.size() - prefix.size() - 1 };
        u8  out[4]    = {0, 0, 0, 0xFF};
        int parsed    = 0;
        usize start   = 0;
        const usize n = body.size();
        for (usize i = 0; i <= n; ++i) {
            if (i == n || body[i] == ',') {
                if (parsed >= 4) return false;
                std::string token = trim(body.substr(start, i - start));
                f32 fv;
                if (!parse_float(token, fv)) return false;
                if (parsed == 3) {
                    if (fv < 0.f) fv = 0.f;
                    if (fv > 1.f) fv = 1.f;
                    out[3] = static_cast<u8>(std::round(fv * 255.f));
                } else {
                    if (fv < 0.f) fv = 0.f;
                    if (fv > 255.f) fv = 255.f;
                    out[parsed] = static_cast<u8>(std::round(fv));
                }
                ++parsed;
                start = i + 1;
            }
        }
        const int want = has_alpha ? 4 : 3;
        if (parsed != want) return false;
        std::memcpy(comps, out, 4);
        return true;
    };

    // rgba first, since rgb is a prefix
    bool found = false;
    if (extract("rgba(", true))      found = true;
    else if (extract("rgb(", false)) found = true;

    if (found) {
        ok = true;
        return (u32(comps[0]) << 24) | (u32(comps[1]) << 16)
             | (u32(comps[2]) << 8)  | u32(comps[3]);
    }

    // Named colors — small set is enough for the test corpus.
    static const struct { const char* name; u32 rgba; } kNames[] = {
        { "black",       0x000000FFu },
        { "white",       0xFFFFFFFFu },
        { "red",         0xFF0000FFu },
        { "green",       0x008000FFu },
        { "blue",        0x0000FFFFu },
        { "yellow",      0xFFFF00FFu },
        { "transparent", 0x00000000u },
    };
    std::string lower = to_lower(s);
    for (const auto& kv : kNames) {
        if (lower == kv.name) {
            ok = true;
            return kv.rgba;
        }
    }
    return 0;
}

// ─── Attribute string ────────────────────────────────────────────────────
//
// Parses `name="value"` or `name='value'` style.  Returns false on
// malformed input.
bool read_attr(Cursor& c, std::string& name, std::string& value) {
    c.skip_ws();
    if (c.eof()) return false;
    if (c.peek() == '>' || c.peek() == '/') return false;

    const char* name_begin = c.p;
    while (!c.eof() && is_name_char(c.peek())) c.advance();
    if (c.p == name_begin) return false;
    name.assign(name_begin, static_cast<usize>(c.p - name_begin));

    c.skip_ws();
    if (c.eof() || c.peek() != '=') {
        value.clear();  // bare attribute (allowed)
        return true;
    }
    c.advance();   // '='
    c.skip_ws();
    if (c.eof()) return false;

    const char quote = c.peek();
    if (quote != '"' && quote != '\'') return false;
    c.advance();   // open quote
    const char* val_begin = c.p;
    while (!c.eof() && c.peek() != quote) c.advance();
    if (c.eof()) return false;
    value.assign(val_begin, static_cast<usize>(c.p - val_begin));
    c.advance();   // close quote
    return true;
}

}  // namespace (anon)

// ─── Inline-style parser (`style="prop: val; prop: val"`) ───────────────
//
// Used both for `<tag style="…">` and as the body parser for RCSS rules.
// Also exposed at `detail::` scope (declared in Rml_internal.h) so the
// DataBind setters can re-derive `inline_style` from an updated `style`
// attribute without reparsing the whole document.
//
// Returns ok=false on malformed declarations (warn-only — we still set
// every prop we recognized).
bool apply_declarations(std::string_view body, StyleBlock& out) {
    bool any_failed = false;
    usize start = 0;
    const usize n = body.size();
    for (usize i = 0; i <= n; ++i) {
        if (i == n || body[i] == ';') {
            std::string_view decl{ body.data() + start, i - start };
            start = i + 1;

            // split on first ':'
            usize colon = decl.find(':');
            if (colon == std::string_view::npos) {
                if (!trim(decl).empty()) any_failed = true;
                continue;
            }
            std::string prop  = to_lower(trim(decl.substr(0, colon)));
            std::string value = trim(decl.substr(colon + 1));
            if (prop.empty() || value.empty()) {
                any_failed = true;
                continue;
            }

            if (prop == "left") {
                f32 v;
                if (parse_float(value, v)) out.left = v; else any_failed = true;
            } else if (prop == "top") {
                f32 v;
                if (parse_float(value, v)) out.top = v; else any_failed = true;
            } else if (prop == "width") {
                f32 v;
                if (parse_float(value, v)) out.width = v; else any_failed = true;
            } else if (prop == "height") {
                f32 v;
                if (parse_float(value, v)) out.height = v; else any_failed = true;
            } else if (prop == "background-color" || prop == "background") {
                bool ok = false;
                u32 c = parse_color(value, ok);
                if (ok) out.background_color = c; else any_failed = true;
            } else if (prop == "color") {
                bool ok = false;
                u32 c = parse_color(value, ok);
                if (ok) out.text_color = c; else any_failed = true;
            } else if (prop == "font-size") {
                f32 v;
                if (parse_float(value, v)) out.font_size = v; else any_failed = true;
            } else if (prop == "display") {
                std::string lv = to_lower(value);
                if (lv == "none")  out.display_none = true;
                else if (lv == "block" || lv == "inline" || lv == "flex") out.display_none = false;
                else any_failed = true;
            }
            // Unknown properties are silently ignored (warn-only in v.0).
        }
    }
    return !any_failed;
}

namespace {

// ─── Children parse ──────────────────────────────────────────────────────
ParseResult parse_element(Cursor& c, Element& out);

// Splits a class="a b c" attribute into the classes vector.
void parse_classes(std::string_view raw, std::vector<std::string>& out) {
    usize start = 0;
    for (usize i = 0; i <= raw.size(); ++i) {
        if (i == raw.size() || std::isspace(static_cast<unsigned char>(raw[i]))) {
            if (i > start) {
                out.emplace_back(raw.substr(start, i - start));
            }
            start = i + 1;
        }
    }
}

ParseResult parse_text_node(Cursor& c, std::string& out_text) {
    // Reads until we hit a '<'.
    const char* begin = c.p;
    while (!c.eof() && c.peek() != '<') c.advance();
    // Collapse trailing/leading whitespace, keep interior spaces.
    std::string_view raw(begin, static_cast<usize>(c.p - begin));
    out_text = trim(raw);
    return { /*ok=*/ true, "", 0 };
}

// Parses a tag from '<' through '</tag>' (or self-closing).  c.p must
// point at '<' on entry.
ParseResult parse_element(Cursor& c, Element& out) {
    ParseResult r{};
    r.ok = true;

    if (c.peek() != '<') {
        r.ok = false; r.error = "expected '<' at element start"; r.error_line = c.line;
        return r;
    }
    out.source_line = c.line;
    c.advance(); // '<'

    // Skip doctype / comment / processing-instruction
    while (!c.eof() && (c.peek() == '!' || c.peek() == '?')) {
        // skip until '>'
        while (!c.eof() && c.peek() != '>') c.advance();
        if (!c.eof()) c.advance();
        // we consumed the leading '<' too — need a fresh next tag
        c.skip_ws();
        if (c.eof() || c.peek() != '<') {
            r.ok = false; r.error = "unexpected end after directive"; r.error_line = c.line;
            return r;
        }
        c.advance();
    }

    // tag name
    const char* name_begin = c.p;
    while (!c.eof() && is_name_char(c.peek())) c.advance();
    if (c.p == name_begin) {
        r.ok = false; r.error = "empty tag name"; r.error_line = c.line;
        return r;
    }
    out.tag.assign(name_begin, static_cast<usize>(c.p - name_begin));
    out.tag = to_lower(out.tag);

    // attributes
    while (!c.eof()) {
        c.skip_ws();
        if (c.eof()) {
            r.ok = false; r.error = "unterminated tag"; r.error_line = c.line;
            return r;
        }
        const char p0 = c.peek();
        if (p0 == '>' || p0 == '/') break;

        std::string aname, aval;
        if (!read_attr(c, aname, aval)) {
            r.ok = false; r.error = "malformed attribute"; r.error_line = c.line;
            return r;
        }
        std::string lname = to_lower(aname);
        if (lname == "id") {
            out.id = aval;
        } else if (lname == "class") {
            parse_classes(aval, out.classes);
        } else if (lname == "style") {
            apply_declarations(aval, out.inline_style);
        } else if (lname.size() > 2 && lname.compare(0, 2, "on") == 0) {
            // onclick / onmouseover / etc.
            out.handlers[lname] = aval;
        } else {
            out.attributes[lname] = aval;
        }
    }

    // self-closing?
    if (c.peek() == '/') {
        c.advance();
        if (c.peek() != '>') {
            r.ok = false; r.error = "expected '>' after '/'"; r.error_line = c.line;
            return r;
        }
        c.advance();
        return r;
    }

    if (c.peek() != '>') {
        r.ok = false; r.error = "expected '>'"; r.error_line = c.line;
        return r;
    }
    c.advance(); // '>'

    // Void elements (no closing tag)
    static const char* kVoid[] = { "br", "hr", "img", "input", "meta", "link" };
    for (const char* v : kVoid) {
        if (out.tag == v) return r;
    }

    // Children + text
    while (!c.eof()) {
        if (c.peek() != '<') {
            std::string text_run;
            parse_text_node(c, text_run);
            if (!text_run.empty()) {
                if (!out.text.empty()) out.text.push_back(' ');
                out.text.append(text_run);
            }
            continue;
        }

        // Look ahead for </
        if (c.p + 1 < c.end && c.p[1] == '/') {
            // close tag
            c.advance(); c.advance();  // </
            const char* close_begin = c.p;
            while (!c.eof() && is_name_char(c.peek())) c.advance();
            std::string close_name(close_begin, static_cast<usize>(c.p - close_begin));
            close_name = to_lower(close_name);
            c.skip_ws();
            if (c.peek() != '>') {
                r.ok = false; r.error = "expected '>' on close tag"; r.error_line = c.line;
                return r;
            }
            c.advance();
            if (close_name != out.tag) {
                // be forgiving: warn but accept
                PSY_LOG_WARN("rml: close </{}> at line {} doesn't match <{}>",
                             close_name, c.line, out.tag);
            }
            return r;
        }

        // Skip comments inline
        if (c.starts_with("<!--")) {
            c.consume(4);
            while (!c.eof() && !c.starts_with("-->")) c.advance();
            if (!c.eof()) c.consume(3);
            continue;
        }

        // Child element
        Element child;
        ParseResult sub = parse_element(c, child);
        if (!sub.ok) return sub;
        out.children.emplace_back(std::move(child));
    }

    r.ok = false; r.error = "unterminated element"; r.error_line = c.line;
    return r;
}

}  // namespace

// ─── StyleBlock ──────────────────────────────────────────────────────────
bool StyleBlock::any_set() const noexcept {
    return !std::isnan(left) || !std::isnan(top)
        || !std::isnan(width) || !std::isnan(height)
        || background_color != 0
        || text_color != 0
        || !std::isnan(font_size)
        || display_none;
}

// ─── parse_rml ───────────────────────────────────────────────────────────
ParseResult parse_rml(std::string_view src, Element& out_root) {
    Cursor c{ src.data(), src.data() + src.size(), 1 };
    // Skip leading whitespace / BOM / doctype / comments before root.
    while (!c.eof()) {
        c.skip_ws();
        if (c.eof()) break;
        if (c.starts_with("\xEF\xBB\xBF")) { c.consume(3); continue; }
        if (c.starts_with("<!--")) {
            c.consume(4);
            while (!c.eof() && !c.starts_with("-->")) c.advance();
            if (!c.eof()) c.consume(3);
            continue;
        }
        if (c.starts_with("<!") || c.starts_with("<?")) {
            // doctype / processing instruction
            while (!c.eof() && c.peek() != '>') c.advance();
            if (!c.eof()) c.advance();
            continue;
        }
        break;
    }
    if (c.eof()) {
        return { false, "empty document", c.line };
    }
    return parse_element(c, out_root);
}

// ─── parse_rcss ──────────────────────────────────────────────────────────
ParseResult parse_rcss(std::string_view src, std::vector<Rule>& out_rules) {
    Cursor c{ src.data(), src.data() + src.size(), 1 };
    ParseResult r{};
    r.ok = true;

    while (!c.eof()) {
        c.skip_ws();
        if (c.eof()) break;

        // comment
        if (c.starts_with("/*")) {
            c.consume(2);
            while (!c.eof() && !c.starts_with("*/")) c.advance();
            if (!c.eof()) c.consume(2);
            continue;
        }

        // selector list — one selector per rule for Wave-A; commas split.
        const char* sel_begin = c.p;
        while (!c.eof() && c.peek() != '{') c.advance();
        if (c.eof()) {
            r.ok = false; r.error = "expected '{' for rule body"; r.error_line = c.line;
            return r;
        }
        std::string_view sel_raw{ sel_begin, static_cast<usize>(c.p - sel_begin) };
        c.advance(); // '{'

        const char* body_begin = c.p;
        while (!c.eof() && c.peek() != '}') c.advance();
        if (c.eof()) {
            r.ok = false; r.error = "expected '}' to close rule"; r.error_line = c.line;
            return r;
        }
        std::string_view body{ body_begin, static_cast<usize>(c.p - body_begin) };
        c.advance(); // '}'

        // Split selector list on commas, emit one rule per selector.
        std::string sel_str(sel_raw);
        usize start = 0;
        for (usize i = 0; i <= sel_str.size(); ++i) {
            if (i == sel_str.size() || sel_str[i] == ',') {
                std::string sel = trim(std::string_view(sel_str).substr(start, i - start));
                start = i + 1;
                if (sel.empty()) continue;

                Rule rule;
                if (sel == "*") {
                    rule.kind = Rule::SelectorKind::Universal;
                } else if (sel[0] == '#') {
                    rule.kind = Rule::SelectorKind::Id;
                    rule.name = sel.substr(1);
                } else if (sel[0] == '.') {
                    rule.kind = Rule::SelectorKind::Class;
                    rule.name = sel.substr(1);
                } else {
                    rule.kind = Rule::SelectorKind::Tag;
                    rule.name = to_lower(sel);
                }
                apply_declarations(body, rule.style);
                out_rules.emplace_back(std::move(rule));
            }
        }
    }

    return r;
}

// ─── apply_cascade ───────────────────────────────────────────────────────
namespace {

bool selector_matches(const Rule& rule, const Element& el) {
    switch (rule.kind) {
        case Rule::SelectorKind::Universal: return true;
        case Rule::SelectorKind::Tag:       return rule.name == el.tag;
        case Rule::SelectorKind::Id:        return rule.name == el.id;
        case Rule::SelectorKind::Class:
            for (const auto& c : el.classes) if (c == rule.name) return true;
            return false;
    }
    return false;
}

void merge_into(StyleBlock& dst, const StyleBlock& src) {
    if (!std::isnan(src.left))   dst.left   = src.left;
    if (!std::isnan(src.top))    dst.top    = src.top;
    if (!std::isnan(src.width))  dst.width  = src.width;
    if (!std::isnan(src.height)) dst.height = src.height;
    if (src.background_color != 0) dst.background_color = src.background_color;
    if (src.text_color != 0)       dst.text_color       = src.text_color;
    if (!std::isnan(src.font_size)) dst.font_size = src.font_size;
    if (src.display_none) dst.display_none = true;
}

void cascade_node(Element& el, const std::vector<Rule>& sheet, const StyleBlock& parent) {
    // Initialize with inheritable parent props.
    el.computed_style = StyleBlock{};
    if (parent.text_color != 0)        el.computed_style.text_color = parent.text_color;
    if (!std::isnan(parent.font_size)) el.computed_style.font_size  = parent.font_size;

    // 1. Universal rules (lowest specificity)
    for (const auto& r : sheet)
        if (r.kind == Rule::SelectorKind::Universal && selector_matches(r, el))
            merge_into(el.computed_style, r.style);

    // 2. Tag selectors
    for (const auto& r : sheet)
        if (r.kind == Rule::SelectorKind::Tag && selector_matches(r, el))
            merge_into(el.computed_style, r.style);

    // 3. Class selectors
    for (const auto& r : sheet)
        if (r.kind == Rule::SelectorKind::Class && selector_matches(r, el))
            merge_into(el.computed_style, r.style);

    // 4. Id selectors (highest specificity among Wave-A subset)
    for (const auto& r : sheet)
        if (r.kind == Rule::SelectorKind::Id && selector_matches(r, el))
            merge_into(el.computed_style, r.style);

    // 5. Inline style wins over everything.
    merge_into(el.computed_style, el.inline_style);

    // Recurse — pass the freshly-computed style as the new parent so
    // children inherit color / font-size.
    for (auto& child : el.children) cascade_node(child, sheet, el.computed_style);
}

}  // namespace

void apply_cascade(Element& root, const std::vector<Rule>& sheet) {
    cascade_node(root, sheet, StyleBlock{});
}

}  // namespace psynder::ui::rml::detail
