// SPDX-License-Identifier: MIT
// Psynder — golden-image renderer test harness.
//
// Walks tests/golden/cases/*.psyscene, renders each into a CPU framebuffer,
// and compares the result against tests/golden/references/<name>.ppm. A
// case passes when its per-pixel mismatch ratio (Manhattan distance > 8 on
// any channel counts as a mismatch) is ≤ 0.5% — the bar in DESIGN.md §14.
//
// File format (tests/golden/cases/*.psyscene):
//
//     # comment lines start with '#'
//     size 64 32                  — framebuffer dimensions
//     clear 30 30 60              — RGB clear color
//     rect 4 4 24 16 200 80 80    — axis-aligned filled rectangle
//     triangle  4 28   60 28   32 4   90 200 90   — filled triangle
//
// Reference images live next to cases/ under references/, one .ppm per case
// (binary P6, 255 max value). PPM keeps the dep surface zero — we'd need an
// extra third-party lib (stb_image, libpng) for PNG, and adding that is a
// cross-lane build-system change that lane 25 cannot make unilaterally.
// The tolerance bar and the SCIENCE — golden-image diff at ≤ 0.5% — match
// the spec exactly; the on-disk encoding is the only swap.
//
// CLI flags:
//   --update                       Write the rendered output as the new
//                                  reference for every case. Use this when
//                                  intentionally rebaselining; CI never
//                                  runs with this flag.
//   --cases-dir=PATH               Override the cases search root.
//   --references-dir=PATH          Override the references search root.
//   --tolerance-pct=F              Override the per-pixel mismatch budget
//                                  (default 0.5).

#include "core/Log.h"
#include "core/Types.h"
#include "image_diff.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace psynder;
using psynder::testing::Image;
using psynder::testing::CompareResult;
using psynder::testing::compare_images;

namespace {

bool read_ppm(const fs::path& path, Image& out) {
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f) return false;
    char magic[3] = {0,0,0};
    if (std::fread(magic, 1, 2, f) != 2 ||
        magic[0] != 'P' || magic[1] != '6') {
        std::fclose(f); return false;
    }
    int w = 0, h = 0, maxv = 0;
    if (std::fscanf(f, "%d %d %d", &w, &h, &maxv) != 3 ||
        maxv != 255 || w <= 0 || h <= 0) {
        std::fclose(f); return false;
    }
    std::fgetc(f);  // exactly one whitespace byte
    const usize uw = static_cast<usize>(w);
    const usize uh = static_cast<usize>(h);
    out.width  = static_cast<u32>(w);
    out.height = static_cast<u32>(h);
    out.rgb.resize(uw * uh * 3);
    if (std::fread(out.rgb.data(), 1, out.rgb.size(), f) != out.rgb.size()) {
        std::fclose(f); return false;
    }
    std::fclose(f);
    return true;
}

bool write_ppm(const fs::path& path, const Image& img) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
    const bool ok =
        std::fwrite(img.rgb.data(), 1, img.rgb.size(), f) == img.rgb.size();
    std::fclose(f);
    return ok;
}

// ─── Scene representation ────────────────────────────────────────────────
struct Rect { i32 x, y, w, h; u8 r, g, b; };
struct Tri  { f32 x0, y0, x1, y1, x2, y2; u8 r, g, b; };

struct Scene {
    u32 width  = 0;
    u32 height = 0;
    u8  clear_r = 0, clear_g = 0, clear_b = 0;
    std::vector<Rect> rects;
    std::vector<Tri>  tris;
};

bool parse_uint(std::string_view s, i32& out) {
    out = 0; bool neg = false;
    usize i = 0;
    if (!s.empty() && s[0] == '-') { neg = true; ++i; }
    if (i == s.size()) return false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    if (neg) out = -out;
    return true;
}
bool parse_float(std::string_view s, f32& out) {
    char* end = nullptr;
    std::string buf(s);
    out = std::strtof(buf.c_str(), &end);
    return end != nullptr && end != buf.c_str();
}

bool load_scene(const fs::path& path, Scene& out) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        // Strip CR for cross-platform line endings.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Strip comments.
        if (auto h = line.find('#'); h != std::string::npos) line.resize(h);
        // Tokenize.
        std::istringstream iss(line);
        std::vector<std::string> tok;
        for (std::string t; iss >> t; ) tok.push_back(std::move(t));
        if (tok.empty()) continue;

        const std::string& op = tok[0];
        if (op == "size" && tok.size() == 3) {
            i32 w, h;
            if (!parse_uint(tok[1], w) || !parse_uint(tok[2], h) || w <= 0 || h <= 0)
                return false;
            out.width  = static_cast<u32>(w);
            out.height = static_cast<u32>(h);
        } else if (op == "clear" && tok.size() == 4) {
            i32 r, g, b;
            if (!parse_uint(tok[1], r) || !parse_uint(tok[2], g) ||
                !parse_uint(tok[3], b)) return false;
            out.clear_r = static_cast<u8>(std::clamp(r, 0, 255));
            out.clear_g = static_cast<u8>(std::clamp(g, 0, 255));
            out.clear_b = static_cast<u8>(std::clamp(b, 0, 255));
        } else if (op == "rect" && tok.size() == 8) {
            Rect r{};
            i32 cr, cg, cb;
            if (!parse_uint(tok[1], r.x) || !parse_uint(tok[2], r.y) ||
                !parse_uint(tok[3], r.w) || !parse_uint(tok[4], r.h) ||
                !parse_uint(tok[5], cr)  || !parse_uint(tok[6], cg) ||
                !parse_uint(tok[7], cb)) return false;
            r.r = static_cast<u8>(std::clamp(cr, 0, 255));
            r.g = static_cast<u8>(std::clamp(cg, 0, 255));
            r.b = static_cast<u8>(std::clamp(cb, 0, 255));
            out.rects.push_back(r);
        } else if (op == "triangle" && tok.size() == 10) {
            Tri t{};
            i32 cr, cg, cb;
            if (!parse_float(tok[1], t.x0) || !parse_float(tok[2], t.y0) ||
                !parse_float(tok[3], t.x1) || !parse_float(tok[4], t.y1) ||
                !parse_float(tok[5], t.x2) || !parse_float(tok[6], t.y2) ||
                !parse_uint (tok[7], cr  ) || !parse_uint (tok[8], cg  ) ||
                !parse_uint (tok[9], cb  )) return false;
            t.r = static_cast<u8>(std::clamp(cr, 0, 255));
            t.g = static_cast<u8>(std::clamp(cg, 0, 255));
            t.b = static_cast<u8>(std::clamp(cb, 0, 255));
            out.tris.push_back(t);
        } else {
            PSY_LOG_WARN("golden: unknown directive '{}' in {}",
                         op, path.string());
            return false;
        }
    }
    return out.width > 0 && out.height > 0;
}

// ─── Renderer (CPU, deterministic — no jitter) ───────────────────────────
inline void put(Image& img, u32 x, u32 y, u8 r, u8 g, u8 b) {
    const usize idx = (static_cast<usize>(y) * img.width + x) * 3;
    img.rgb[idx + 0] = r;
    img.rgb[idx + 1] = g;
    img.rgb[idx + 2] = b;
}

void render_scene(const Scene& s, Image& out) {
    out.width  = s.width;
    out.height = s.height;
    out.rgb.assign(static_cast<usize>(s.width) * s.height * 3, 0);

    // Clear.
    for (u32 y = 0; y < s.height; ++y) {
        for (u32 x = 0; x < s.width; ++x) {
            put(out, x, y, s.clear_r, s.clear_g, s.clear_b);
        }
    }
    // Rects.
    for (const Rect& r : s.rects) {
        const i32 x0 = std::max(0, r.x);
        const i32 y0 = std::max(0, r.y);
        const i32 x1 = std::min(static_cast<i32>(s.width),  r.x + r.w);
        const i32 y1 = std::min(static_cast<i32>(s.height), r.y + r.h);
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = x0; x < x1; ++x) {
                put(out, static_cast<u32>(x), static_cast<u32>(y), r.r, r.g, r.b);
            }
        }
    }
    // Triangles via edge functions (pixel centers).
    auto edge = [](f32 ax, f32 ay, f32 bx, f32 by, f32 px, f32 py) {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };
    for (const Tri& t : s.tris) {
        const f32 area = edge(t.x0, t.y0, t.x1, t.y1, t.x2, t.y2);
        if (std::abs(area) < 1e-4f) continue;
        const i32 lo_x = std::max(0, static_cast<i32>(std::floor(std::min({t.x0,t.x1,t.x2}))));
        const i32 hi_x = std::min(static_cast<i32>(s.width)  - 1,
                                  static_cast<i32>(std::ceil (std::max({t.x0,t.x1,t.x2}))));
        const i32 lo_y = std::max(0, static_cast<i32>(std::floor(std::min({t.y0,t.y1,t.y2}))));
        const i32 hi_y = std::min(static_cast<i32>(s.height) - 1,
                                  static_cast<i32>(std::ceil (std::max({t.y0,t.y1,t.y2}))));
        for (i32 y = lo_y; y <= hi_y; ++y) {
            for (i32 x = lo_x; x <= hi_x; ++x) {
                const f32 px = static_cast<f32>(x) + 0.5f;
                const f32 py = static_cast<f32>(y) + 0.5f;
                const f32 w0 = edge(t.x1, t.y1, t.x2, t.y2, px, py);
                const f32 w1 = edge(t.x2, t.y2, t.x0, t.y0, px, py);
                const f32 w2 = edge(t.x0, t.y0, t.x1, t.y1, px, py);
                const bool ccw = w0 >= 0 && w1 >= 0 && w2 >= 0;
                const bool cw  = w0 <= 0 && w1 <= 0 && w2 <= 0;
                if (!ccw && !cw) continue;
                put(out, static_cast<u32>(x), static_cast<u32>(y), t.r, t.g, t.b);
            }
        }
    }
}

// ─── Path resolution ─────────────────────────────────────────────────────
// CI / tests can be invoked from many cwd's; walk up to find the repo-relative
// path. Stops at the first directory containing the expected subtree.
std::optional<fs::path> find_relative_root(std::string_view relpath) {
    fs::path start = fs::current_path();
    for (int up = 0; up < 8; ++up) {
        fs::path cand = start / fs::path{relpath};
        if (fs::exists(cand)) return cand;
        if (!start.has_parent_path() || start.parent_path() == start) break;
        start = start.parent_path();
    }
    return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
    bool update           = false;
    fs::path cases_dir;
    fs::path refs_dir;
    f64  tolerance_pct    = 0.5;

    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a == "--update") update = true;
        else if (a.starts_with("--cases-dir="))
            cases_dir = std::string(a.substr(std::string_view("--cases-dir=").size()));
        else if (a.starts_with("--references-dir="))
            refs_dir  = std::string(a.substr(std::string_view("--references-dir=").size()));
        else if (a.starts_with("--tolerance-pct="))
            tolerance_pct = std::strtod(
                std::string(a.substr(std::string_view("--tolerance-pct=").size())).c_str(),
                nullptr);
    }

    if (cases_dir.empty()) {
        if (auto found = find_relative_root("tests/golden/cases")) cases_dir = *found;
    }
    if (refs_dir.empty()) {
        if (auto found = find_relative_root("tests/golden/references")) refs_dir = *found;
    }

    if (cases_dir.empty() || !fs::exists(cases_dir)) {
        PSY_LOG_ERROR("golden: cases directory not found "
                      "(searched ./tests/golden/cases ...). cwd={}",
                      fs::current_path().string());
        return EXIT_FAILURE;
    }
    if (refs_dir.empty()) {
        // When updating, create the directory; otherwise refuse.
        if (update) {
            refs_dir = cases_dir.parent_path() / "references";
            std::error_code ec;
            fs::create_directories(refs_dir, ec);
        } else {
            PSY_LOG_ERROR("golden: references directory not found");
            return EXIT_FAILURE;
        }
    }

    PSY_LOG_INFO("golden: cases={} refs={} tolerance={:.3f}%",
                 cases_dir.string(), refs_dir.string(), tolerance_pct);

    std::vector<fs::path> cases;
    for (auto& e : fs::directory_iterator(cases_dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() == ".psyscene") cases.push_back(e.path());
    }
    std::sort(cases.begin(), cases.end());

    if (cases.empty()) {
        PSY_LOG_ERROR("golden: no .psyscene cases found under {}",
                      cases_dir.string());
        return EXIT_FAILURE;
    }

    usize passed = 0, failed = 0;
    for (const fs::path& case_path : cases) {
        const std::string name = case_path.stem().string();
        Scene scene{};
        if (!load_scene(case_path, scene)) {
            PSY_LOG_ERROR("golden: FAILED to parse {}", case_path.string());
            ++failed;
            continue;
        }
        Image rendered{};
        render_scene(scene, rendered);

        const fs::path ref_path = refs_dir / (name + ".ppm");
        if (update) {
            if (!write_ppm(ref_path, rendered)) {
                PSY_LOG_ERROR("golden: failed to write reference {}", ref_path.string());
                ++failed;
            } else {
                PSY_LOG_INFO("golden: [update] wrote reference {}", ref_path.string());
                ++passed;
            }
            continue;
        }

        Image ref{};
        if (!read_ppm(ref_path, ref)) {
            PSY_LOG_ERROR("golden: missing reference {}", ref_path.string());
            ++failed;
            continue;
        }
        const CompareResult cr = compare_images(rendered, ref);
        if (!cr.sizes_match) {
            PSY_LOG_ERROR("golden: case '{}' size mismatch (rendered {}x{}, ref {}x{})",
                          name, rendered.width, rendered.height,
                          ref.width, ref.height);
            ++failed;
            continue;
        }
        if (cr.mismatch_pct() > tolerance_pct) {
            PSY_LOG_ERROR("golden: case '{}' FAIL — {:.4f}% mismatch "
                          "({} / {} pixels, tolerance {:.3f}%)",
                          name, cr.mismatch_pct(),
                          cr.mismatch_count, cr.total_pixels, tolerance_pct);
            // Drop the diff image next to the build output for inspection.
            const fs::path actual_path =
                fs::current_path() / (name + ".actual.ppm");
            write_ppm(actual_path, rendered);
            PSY_LOG_INFO("golden: wrote actual to {}", actual_path.string());
            ++failed;
        } else {
            PSY_LOG_INFO("golden: case '{}' OK — {:.4f}% mismatch",
                         name, cr.mismatch_pct());
            ++passed;
        }
    }

    PSY_LOG_INFO("golden: {}/{} cases passed ({} failed)",
                 passed, passed + failed, failed);
    return failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
