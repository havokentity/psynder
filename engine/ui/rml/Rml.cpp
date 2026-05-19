// SPDX-License-Identifier: MIT
// Psynder — RmlUi binding entry points. Lane 17 owns.
//
// Implements the frozen public surface in `Rml.h`:
//
//   initialize / shutdown
//   load_document(virtual_path, name)
//   show(name) / hide(name)
//   render(target)
//   update(dt)
//
// Today the implementation drives an in-tree RML/RCSS subset parser +
// layout pass + rasterizer submitter (see Rml_internal.h, Parser.cpp,
// Layout.cpp, RenderInterface.cpp).  Flipping the CMake option
// `PSYNDER_VENDOR_RMLUI=ON` swaps the backend to upstream RmlUi via
// FetchContent + FreeType while keeping this exact public API.
//
// Hot reload (Wave-B): every successful load_document registers a VFS
// watch on both the .rml and the .rcss virtual paths via lane 05's
// `Vfs::watch`.  The watcher fires `needs_reload = true` from the asset
// poll thread; update() (on the engine main thread, between frames)
// reparses the source pair into temporaries and atomically swaps the
// DOM + stylesheet into place.  Document identity (the map slot keyed
// by name) is preserved across reload so Lua scripts that hold the
// document name keep working; show/hide state is preserved too.

#include "Rml.h"
#include "Rml_internal.h"

#include "asset/Vfs.h"
#include "core/Log.h"
#include "render/Framebuffer.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace psynder::ui::rml::detail {

// Defined in RenderInterface.cpp
void submit_boxes_to_rasterizer(const std::vector<LayoutBox>& boxes,
                                render::Framebuffer&          target);

// register_lua_surface + dispatch_handler are declared in Rml_internal.h
// and implemented in LuaBinding.cpp.

}  // namespace psynder::ui::rml::detail

namespace psynder::ui::rml {

namespace {

struct State {
    bool                                                       initialized = false;
    std::unordered_map<std::string, detail::Document>          documents;
    // Layout scratch reused across frames — keyed by document name so
    // hot-reload doesn't churn allocations.
    std::unordered_map<std::string, std::vector<detail::LayoutBox>> layout_cache;
    u64                                                        generation = 0;
};

State& state() {
    static State s;
    return s;
}

// Parse a .rml/.rcss source pair into a fresh DOM + stylesheet.  Used
// both by load_document (initial load) and the hot-reload sweep.  We
// build into a temporary so the caller can swap atomically on success
// without ever exposing a partially-parsed document to render().
struct ParsedPair {
    detail::Element             root;
    std::vector<detail::Rule>   sheet;
    bool                        ok = false;
};

[[nodiscard]] ParsedPair parse_pair_from_sources(std::string_view rml_src,
                                                 std::string_view rcss_src,
                                                 std::string_view diag_rml_path,
                                                 std::string_view diag_rcss_path) {
    ParsedPair pp{};
    auto res = detail::parse_rml(rml_src, pp.root);
    if (!res.ok) {
        PSY_LOG_WARN("rml: parse error in '{}' (line {}): {}",
                     std::string(diag_rml_path), res.error_line, res.error);
        return pp;
    }
    if (!rcss_src.empty()) {
        auto cssr = detail::parse_rcss(rcss_src, pp.sheet);
        if (!cssr.ok) {
            PSY_LOG_WARN("rml: rcss parse error in '{}' (line {}): {}",
                         std::string(diag_rcss_path), cssr.error_line, cssr.error);
            // Continue with whatever rules made it through.
        }
    }
    detail::apply_cascade(pp.root, pp.sheet);
    pp.ok = true;
    return pp;
}

// read_pair() outcomes — distinguishes "couldn't read source" (often
// transient, e.g. VFS stub or in-flight editor save) from "read source
// but it didn't parse" (designer error: log + give up until the next
// watcher tick).
enum class ReadOutcome : u8 { Ok, NoSource, ParseError };

// Read both .rml + .rcss for a document via the VFS.  The doc's
// rcss_virtual_path is derived by swapping the .rml extension; an .rml
// without an .rcss is still valid (no styles cascaded).
[[nodiscard]] ReadOutcome read_pair(detail::Document& doc) {
    auto& vfs = asset::Vfs::Get();

    // Read .rml
    asset::Blob rml_blob = vfs.read(doc.rml_virtual_path);
    if (!rml_blob.data || rml_blob.bytes == 0) {
        // VFS stub returns empty Blob — Wave-A note this and continue with
        // anything the caller might have pre-seeded.
        PSY_LOG_INFO("rml: VFS returned no bytes for '{}' (VFS stub)",
                     doc.rml_virtual_path);
        return ReadOutcome::NoSource;
    }
    std::string_view rml_src(reinterpret_cast<const char*>(rml_blob.data),
                             rml_blob.bytes);

    // Read companion .rcss if present.
    std::string_view rcss_src;
    asset::Blob css_blob{};
    if (!doc.rcss_virtual_path.empty()) {
        css_blob = vfs.read(doc.rcss_virtual_path);
        if (css_blob.data && css_blob.bytes > 0) {
            rcss_src = std::string_view(
                reinterpret_cast<const char*>(css_blob.data),
                css_blob.bytes);
        }
    }

    ParsedPair pp = parse_pair_from_sources(
        rml_src, rcss_src, doc.rml_virtual_path, doc.rcss_virtual_path);
    if (!pp.ok) return ReadOutcome::ParseError;

    // Atomic swap-in: blow away the old DOM + sheet, install the new
    // ones.  Caller is responsible for serialising with render() — we
    // only ever do this from update() on the main thread.
    doc.root  = std::move(pp.root);
    doc.sheet = std::move(pp.sheet);
    return ReadOutcome::Ok;
}

void watch_callback(std::string_view path, void* user) noexcept {
    auto* doc = static_cast<detail::Document*>(user);
    if (!doc) return;
    // Watch fires from the VFS poll thread — set the atomic flag so the
    // next update() pass on the main thread picks it up.  No mutex
    // necessary because update() is the sole consumer.
    doc->needs_reload.store(true, std::memory_order_release);
    (void)path;
}

// Derive the rcss path from an rml path: replace ".rml" with ".rcss".
// If the input doesn't end in .rml, append .rcss.  Returns the bare
// stem-suffix combo so designers can use either convention.
std::string companion_rcss_path(std::string_view rml_path) {
    constexpr std::string_view kRml = ".rml";
    if (rml_path.size() >= kRml.size()
        && rml_path.compare(rml_path.size() - kRml.size(), kRml.size(), kRml) == 0) {
        return std::string(rml_path.substr(0, rml_path.size() - kRml.size())) + ".rcss";
    }
    return std::string(rml_path) + ".rcss";
}

}  // namespace

// ─── Public surface ──────────────────────────────────────────────────────

bool initialize() {
    auto& s = state();
    if (s.initialized) return true;

    detail::register_lua_surface();

    s.initialized = true;
    PSY_LOG_INFO("rml: initialized (Wave-A in-tree backend)");
    return true;
}

void shutdown() {
    auto& s = state();
    if (!s.initialized) return;

    s.documents.clear();
    s.layout_cache.clear();
    s.initialized = false;
    PSY_LOG_INFO("rml: shutdown");
}

bool load_document(std::string_view virtual_path, std::string_view name) {
    auto& s = state();
    if (!s.initialized) {
        if (!initialize()) return false;
    }

    detail::Document doc;
    doc.name              = std::string(name);
    doc.rml_virtual_path  = std::string(virtual_path);
    doc.rcss_virtual_path = companion_rcss_path(virtual_path);

    const ReadOutcome outcome = read_pair(doc);
    const bool loaded         = (outcome == ReadOutcome::Ok);

    // Register in any case so show/hide on an unresolved-on-disk document
    // can be retried via hot-reload once the VFS lights up.
    auto [it, inserted] = s.documents.emplace(doc.name, std::move(doc));
    if (!inserted) {
        // Replace prior entry — designer overwrote.
        it->second = std::move(doc);
    }

    auto& vfs = asset::Vfs::Get();
    vfs.watch(it->second.rml_virtual_path,  watch_callback, &it->second);
    vfs.watch(it->second.rcss_virtual_path, watch_callback, &it->second);

    if (!loaded) {
        PSY_LOG_INFO("rml: load_document('{}', '{}') registered; "
                     "VFS read pending or backed by tests/inject",
                     std::string(virtual_path), std::string(name));
    } else {
        PSY_LOG_INFO("rml: load_document('{}', '{}') ok ({} rule{}, root <{}>)",
                     std::string(virtual_path), std::string(name),
                     it->second.sheet.size(),
                     it->second.sheet.size() == 1 ? "" : "s",
                     it->second.root.tag);
    }
    return loaded;
}

void show(std::string_view name) {
    auto& s = state();
    auto it = s.documents.find(std::string(name));
    if (it == s.documents.end()) {
        PSY_LOG_WARN("rml: show('{}') — no such document", std::string(name));
        return;
    }
    it->second.visible = true;
}

void hide(std::string_view name) {
    auto& s = state();
    auto it = s.documents.find(std::string(name));
    if (it == s.documents.end()) return;
    it->second.visible = false;
}

void update(f32 /*dt*/) {
    auto& s = state();
    ++s.generation;

    // Hot-reload sweep — any document whose VFS watcher flagged a change
    // gets reparsed.  Order:
    //
    //   1. Snapshot the dirty flag with exchange() so a watcher firing
    //      mid-update enqueues another pass next frame instead of being
    //      silently consumed.
    //   2. read_pair() parses into temporaries; on success it atomically
    //      replaces doc.root + doc.sheet.  The document's stable name
    //      (the map key) and `visible` flag are *preserved*, so any
    //      caller that holds the document name in script keeps working
    //      across the reload — DOM identity is at the document-name
    //      level, which is what designers' Lua scripts care about.
    //   3. Bump reload_generation so a script can observe the swap.
    //
    // We only do the work on the main thread, between frames, so
    // render() never sees a half-built DOM.
    for (auto& [name, doc] : s.documents) {
        bool dirty = doc.needs_reload.exchange(false, std::memory_order_acq_rel);
        if (!dirty) continue;
        const bool was_visible = doc.visible;
        const u64 gen_before   = doc.reload_generation;
        const ReadOutcome      outcome = read_pair(doc);
        if (outcome == ReadOutcome::Ok) {
            doc.visible           = was_visible;     // preserve show/hide
            doc.reload_generation = gen_before + 1;
            PSY_LOG_INFO("rml: hot-reloaded '{}' (gen {})", name,
                         doc.reload_generation);
        } else if (outcome == ReadOutcome::NoSource) {
            // File temporarily unreadable (in-flight editor save, or
            // VFS not yet mounted).  Re-arm so the next watcher tick
            // gets another shot.  Parse errors are *not* re-armed —
            // they're designer bugs and spamming the log every frame
            // helps nobody.
            doc.needs_reload.store(true, std::memory_order_release);
        }
    }
}

void render(render::Framebuffer& target) {
    auto& s = state();
    if (!s.initialized) return;
    if (target.width == 0 || target.height == 0) return;

    const math::Vec2 viewport{ static_cast<f32>(target.width),
                               static_cast<f32>(target.height) };

    for (const auto& [name, doc] : s.documents) {
        if (!doc.visible) continue;
        auto& boxes = s.layout_cache[name];
        detail::layout(doc, viewport, boxes);
        detail::submit_boxes_to_rasterizer(boxes, target);
    }
}

}  // namespace psynder::ui::rml

// ─── Test-only injection surface ─────────────────────────────────────────
//
// The unit test exercises parse + layout + show/hide directly from
// in-memory strings, without going through the asset VFS (which is
// stubbed in Wave-A).  Symbols live behind `psynder::ui::rml::test_only`
// so they don't appear in the public header; tests forward-declare them
// against the static lib.

namespace psynder::ui::rml::test_only {

bool inject_source(std::string_view name,
                   std::string_view rml_src,
                   std::string_view rcss_src) {
    auto& s = state();
    if (!s.initialized) {
        if (!initialize()) return false;
    }

    detail::Document doc;
    doc.name = std::string(name);
    doc.rml_virtual_path  = std::string(name) + ".rml";
    doc.rcss_virtual_path = std::string(name) + ".rcss";

    auto res = detail::parse_rml(rml_src, doc.root);
    if (!res.ok) {
        PSY_LOG_WARN("rml: test_only inject_source('{}') rml parse failed "
                     "line {}: {}", std::string(name), res.error_line, res.error);
        return false;
    }

    if (!rcss_src.empty()) {
        auto cssr = detail::parse_rcss(rcss_src, doc.sheet);
        if (!cssr.ok) {
            PSY_LOG_WARN("rml: test_only inject_source('{}') rcss parse failed "
                         "line {}: {}", std::string(name), cssr.error_line, cssr.error);
            // continue
        }
    }

    detail::apply_cascade(doc.root, doc.sheet);

    s.documents[doc.name] = std::move(doc);
    return true;
}

const detail::Document* find_document(std::string_view name) {
    auto& s = state();
    auto it = s.documents.find(std::string(name));
    if (it == s.documents.end()) return nullptr;
    return &it->second;
}

usize document_count() {
    return state().documents.size();
}

void render_layout(std::string_view name,
                   math::Vec2 viewport,
                   std::vector<detail::LayoutBox>& out) {
    auto* doc = find_document(name);
    if (!doc) { out.clear(); return; }
    detail::layout(*doc, viewport, out);
}

bool fire_handler(std::string_view name,
                  std::string_view element_id,
                  std::string_view event_name) {
    auto* doc = find_document(name);
    if (!doc) return false;

    // Tiny BFS for the element by id.
    std::vector<const detail::Element*> stack{ &doc->root };
    while (!stack.empty()) {
        const detail::Element* el = stack.back();
        stack.pop_back();
        if (el->id == element_id) {
            auto it = el->handlers.find(std::string(event_name));
            if (it == el->handlers.end()) return false;
            return detail::dispatch_handler(event_name, it->second);
        }
        for (const auto& c : el->children) stack.emplace_back(&c);
    }
    return false;
}

// Payload-aware variant — used by the Wave-B event-table tests.  Builds
// the `event` upvalue from `payload` before handing the body to the
// installed Lua backend.
bool fire_handler_with_payload(std::string_view name,
                                std::string_view element_id,
                                std::string_view event_name,
                                const detail::EventPayload& payload) {
    auto* doc = find_document(name);
    if (!doc) return false;
    std::vector<const detail::Element*> stack{ &doc->root };
    while (!stack.empty()) {
        const detail::Element* el = stack.back();
        stack.pop_back();
        if (el->id == element_id) {
            auto it = el->handlers.find(std::string(event_name));
            if (it == el->handlers.end()) return false;
            return detail::dispatch_handler(event_name, it->second, payload);
        }
        for (const auto& c : el->children) stack.emplace_back(&c);
    }
    return false;
}

// Mark the document dirty as if the VFS watcher had fired.  Lets unit
// tests exercise the update() hot-reload path without touching the
// filesystem (the asset Vfs test surface poll_watchers_now() is a
// separate integration story).
bool mark_dirty(std::string_view name) {
    auto& s = state();
    auto it = s.documents.find(std::string(name));
    if (it == s.documents.end()) return false;
    it->second.needs_reload.store(true, std::memory_order_release);
    return true;
}

// Run one tick of update() in-line so tests can assert what the
// hot-reload sweep does on the dirty bit without a real frame loop.
void run_update_tick() { ::psynder::ui::rml::update(0.f); }

// Inject a fresh source pair into an *already-registered* document and
// re-cascade — used by tests to simulate "designer edited the .rml,
// VFS watcher fired".  Preserves the document's visibility + reload
// generation.
bool reload_with_source(std::string_view name,
                        std::string_view rml_src,
                        std::string_view rcss_src) {
    auto& s = state();
    auto it = s.documents.find(std::string(name));
    if (it == s.documents.end()) return false;
    auto& doc = it->second;
    detail::Element              new_root;
    std::vector<detail::Rule>    new_sheet;
    auto rml_res = detail::parse_rml(rml_src, new_root);
    if (!rml_res.ok) return false;
    if (!rcss_src.empty()) {
        auto css_res = detail::parse_rcss(rcss_src, new_sheet);
        (void)css_res;
    }
    detail::apply_cascade(new_root, new_sheet);
    doc.root  = std::move(new_root);
    doc.sheet = std::move(new_sheet);
    ++doc.reload_generation;
    return true;
}

u64 reload_generation(std::string_view name) {
    const auto* doc = find_document(name);
    return doc ? doc->reload_generation : 0;
}

}  // namespace psynder::ui::rml::test_only
