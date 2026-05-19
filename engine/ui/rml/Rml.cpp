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
// Hot reload: every successful load_document registers a VFS watch on
// both the .rml and the .rcss virtual paths; update() reparses any
// document whose generation counter advanced between frames.

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

// Read both .rml + .rcss for a document via the VFS.  The doc's
// rcss_virtual_path is derived by swapping the .rml extension; an .rml
// without an .rcss is still valid (no styles cascaded).
[[nodiscard]] bool read_pair(detail::Document& doc) {
    auto& vfs = asset::Vfs::Get();

    // Read .rml
    asset::Blob rml_blob = vfs.read(doc.rml_virtual_path);
    if (!rml_blob.data || rml_blob.bytes == 0) {
        // VFS stub returns empty Blob — Wave-A note this and continue with
        // anything the caller might have pre-seeded.
        PSY_LOG_INFO("rml: VFS returned no bytes for '{}' (VFS stub)",
                     doc.rml_virtual_path);
        return false;
    }
    std::string_view rml_src(reinterpret_cast<const char*>(rml_blob.data),
                             rml_blob.bytes);

    detail::Element root;
    auto res = detail::parse_rml(rml_src, root);
    if (!res.ok) {
        PSY_LOG_WARN("rml: parse error in '{}' (line {}): {}",
                     doc.rml_virtual_path, res.error_line, res.error);
        return false;
    }
    doc.root = std::move(root);

    // Read companion .rcss if present.
    if (!doc.rcss_virtual_path.empty()) {
        asset::Blob css_blob = vfs.read(doc.rcss_virtual_path);
        if (css_blob.data && css_blob.bytes > 0) {
            std::string_view css_src(reinterpret_cast<const char*>(css_blob.data),
                                     css_blob.bytes);
            doc.sheet.clear();
            auto cssr = detail::parse_rcss(css_src, doc.sheet);
            if (!cssr.ok) {
                PSY_LOG_WARN("rml: rcss parse error in '{}' (line {}): {}",
                             doc.rcss_virtual_path, cssr.error_line, cssr.error);
                // Continue with whatever rules made it through.
            }
        }
    }

    detail::apply_cascade(doc.root, doc.sheet);
    return true;
}

void watch_callback(std::string_view path, void* user) noexcept {
    auto* doc = static_cast<detail::Document*>(user);
    if (!doc) return;
    doc->needs_reload = true;
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

    const bool loaded = read_pair(doc);

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
    // gets reparsed.
    for (auto& [name, doc] : s.documents) {
        if (!doc.needs_reload) continue;
        doc.needs_reload = false;
        ++doc.reload_generation;
        if (read_pair(doc)) {
            PSY_LOG_INFO("rml: hot-reloaded '{}'", name);
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

}  // namespace psynder::ui::rml::test_only
