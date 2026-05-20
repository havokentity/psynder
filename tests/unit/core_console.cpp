// SPDX-License-Identifier: MIT
// Unit tests for psynder::console.
//
// Coverage:
//   - Register cvars / commands; FindCVar; SetCVarOverride.
//   - Tokenizer: quotes, escapes, // comment.
//   - Execute: read vs write a cvar, allowed_values gate, READONLY block,
//     CHEAT gate.
//   - Smart-resolve: unique prefix, r_-stripped body match.
//   - Undo / Redo via ExecuteScript transactions.
//   - Favourites: AddFavorite + fN magic dispatch + in-range / out-of-range.
//   - History: PushHistory dedupe, kMaxHistoryDepth cap.
//   - Bracket-batch via QueueExecute / Drain.
//   - SaveArchivedCvars writes only CVAR_ARCHIVE cvars that diverged from
//     default; LoadFromFile reads them back.
//   - requires_predicate warning fires on interactive Execute.
//
// All tests assume a single Console::Get() singleton; we re-register
// cvars per test so state from earlier tests doesn't leak (RegisterCVar
// is no-op when the name is already present, so we explicitly
// SetCVarOverride to known values).

#include "core/console/Console.h"
#include "core/console/Completion.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace cn = psynder::console;

TEST_CASE("tokenize_line handles quotes, escapes, // comment", "[core][console][token]") {
    std::string storage;
    auto t = cn::tokenize_line("foo \"bar baz\" qux", storage);
    REQUIRE(t.size() == 3);
    REQUIRE(t[0] == "foo");
    REQUIRE(t[1] == "bar baz");
    REQUIRE(t[2] == "qux");

    auto u = cn::tokenize_line("a b // c d", storage);
    REQUIRE(u.size() == 2);
    REQUIRE(u[0] == "a");
    REQUIRE(u[1] == "b");

    auto v = cn::tokenize_line(R"(name "she said \"hi\"")", storage);
    REQUIRE(v.size() == 2);
    REQUIRE(v[0] == "name");
    REQUIRE(v[1] == "she said \"hi\"");
}

TEST_CASE("RegisterCVar / FindCVar / Execute read+write", "[core][console][cvar]") {
    auto& C = cn::Console::Get();
    auto* cv = C.RegisterCVar("test_r_clear_color", "0.1 0.1 0.1", "clear color (xyz floats)");
    REQUIRE(cv != nullptr);
    REQUIRE(C.FindCVar("test_r_clear_color") == cv);

    auto read = C.Execute("test_r_clear_color");
    REQUIRE(read.ok);
    REQUIRE(read.output.find("0.1") != std::string::npos);

    auto write = C.Execute("test_r_clear_color 0.2 0.3 0.4");
    REQUIRE(write.ok);
    REQUIRE(cv->value == "0.2 0.3 0.4");
}

TEST_CASE("RegisterCommand executes callback with args", "[core][console][cmd]") {
    auto& C = cn::Console::Get();
    int saw = 0;
    C.RegisterCommand("test_echo",
                      "echo argc",
                      [&saw](std::span<const std::string_view> args, cn::Output& out) {
                          saw = static_cast<int>(args.size());
                          out.Format("argc={}", args.size());
                      });
    auto r = C.Execute("test_echo a b c");
    REQUIRE(r.ok);
    REQUIRE(saw == 3);
    REQUIRE(r.output.find("argc=3") != std::string::npos);
}

TEST_CASE("Cvar allowed_values gate rejects unknown values", "[core][console][cvar]") {
    auto& C = cn::Console::Get();
    auto* cv = C.RegisterCVar("test_r_denoiser", "svgf_atrous", "denoiser backend");
    cv->allowed_values = {"svgf_atrous", "metalfx", "nrd_relax"};
    auto bad = C.Execute("test_r_denoiser blurinator");
    REQUIRE_FALSE(bad.ok);
    REQUIRE(bad.error.find("invalid value") != std::string::npos);

    auto good = C.Execute("test_r_denoiser metalfx");
    REQUIRE(good.ok);
    REQUIRE(cv->value == "metalfx");
}

TEST_CASE("Cvar CVAR_READONLY blocks writes", "[core][console][cvar]") {
    auto& C = cn::Console::Get();
    C.RegisterCVar("test_engine_version", "0.1.0", "build version", cn::CVAR_READONLY);
    auto bad = C.Execute("test_engine_version 9.9.9");
    REQUIRE_FALSE(bad.ok);
    REQUIRE(bad.error.find("read-only") != std::string::npos);
    // SetCVarOverride is the engine escape hatch.
    REQUIRE(C.SetCVarOverride("test_engine_version", "9.9.9"));
    REQUIRE(C.FindCVar("test_engine_version")->value == "9.9.9");
}

TEST_CASE("Smart-resolve picks unique prefix and r_-stripped body", "[core][console][resolve]") {
    auto& C = cn::Console::Get();
    C.RegisterCVar("test_uniqueprefix_alpha", "1", "alpha");

    auto r = C.ResolveCommand("test_uniquep");
    REQUIRE(r.canonical_name == "test_uniqueprefix_alpha");
    REQUIRE_FALSE(r.is_exact_match);

    // r_-stripped body match: a cvar named `r_test_denoiser_body` should
    // match the body-prefix `test_denoiser`.
    C.RegisterCVar("r_test_denoiser_body", "off", "test body match");
    auto r2 = C.ResolveCommand("test_denoiser");
    REQUIRE(r2.canonical_name == "r_test_denoiser_body");
}

TEST_CASE("Execute through smart-resolve dispatches the canonical command",
          "[core][console][resolve]") {
    auto& C = cn::Console::Get();
    int sentinel = 0;
    C.RegisterCommand("test_resolve_long_command",
                      "long name",
                      [&](std::span<const std::string_view>, cn::Output& out) {
                          sentinel = 42;
                          out.Print("ok");
                      });
    auto r = C.Execute("test_resolve_long");
    REQUIRE(r.ok);
    REQUIRE(sentinel == 42);
    REQUIRE(r.output.find("resolved") != std::string::npos);
}

TEST_CASE("Undo / Redo round-trip a cvar mutation", "[core][console][undo]") {
    auto& C = cn::Console::Get();
    auto* cv = C.RegisterCVar("test_undo_target", "alpha", "undo round-trip");
    REQUIRE(cv->value == "alpha");

    // ExecuteScript creates one transaction = one undo entry per script.
    auto e = C.ExecuteScript("test_undo_target beta");
    REQUIRE(e.ok);
    REQUIRE(cv->value == "beta");

    auto undone = C.Undo();
    REQUIRE_FALSE(undone.empty());
    REQUIRE(cv->value == "alpha");

    auto redone = C.Redo();
    REQUIRE_FALSE(redone.empty());
    REQUIRE(cv->value == "beta");
}

TEST_CASE("Favourites: AddFavorite + fN magic dispatch", "[core][console][fav]") {
    auto& C = cn::Console::Get();
    C.RegisterCVar("test_fav_target", "off", "favourite-dispatch target");

    C.ClearFavorites();
    C.AddFavorite("test_fav_target on");
    REQUIRE(C.FavoriteCount() == 1);

    auto r = C.Execute("f1");
    REQUIRE(r.ok);
    REQUIRE(C.FindCVar("test_fav_target")->value == "on");
    REQUIRE(r.output.find("[fav]") != std::string::npos);

    // Out-of-range fN: falls through to "unknown" because the saved list
    // doesn't have a slot 9.
    auto bad = C.Execute("f9");
    REQUIRE_FALSE(bad.ok);
}

TEST_CASE("Console input history dedupes and caps at kMaxHistoryDepth", "[core][console][history]") {
    auto& C = cn::Console::Get();
    C.ClearHistory();

    C.PushHistory("alpha");
    C.PushHistory("alpha");  // dedupe-of-last
    C.PushHistory("beta");
    REQUIRE(C.HistoryCount() == 2);

    // Bulk-load past the cap; SetHistory must trim to kMaxHistoryDepth.
    std::vector<std::string> big;
    for (std::size_t i = 0; i < cn::Console::kMaxHistoryDepth + 10; ++i) {
        big.push_back("line_" + std::to_string(i));
    }
    C.SetHistory(big);
    REQUIRE(C.HistoryCount() == cn::Console::kMaxHistoryDepth);
    // Oldest entries got dropped: the first surviving entry is line_10
    // (10 = 10-entry overflow).
    REQUIRE(C.History().front() == "line_10");
}

TEST_CASE("Bracket-batch collects then commits as one undo step", "[core][console][batch]") {
    auto& C = cn::Console::Get();
    auto* a = C.RegisterCVar("test_batch_a", "0", "");
    auto* b = C.RegisterCVar("test_batch_b", "0", "");
    C.SetCVarOverride("test_batch_a", "0");
    C.SetCVarOverride("test_batch_b", "0");

    bool got_open = false, got_queued = false, got_commit = false;
    C.QueueExecute("[", [&](const cn::ExecuteResult& r) {
        REQUIRE(r.output.find("batch: open") != std::string::npos);
        got_open = true;
    });
    C.QueueExecute("test_batch_a 1", [&](const cn::ExecuteResult& r) {
        REQUIRE(r.output.find("batch: queued") != std::string::npos);
        got_queued = true;
    });
    C.QueueExecute("test_batch_b 2", [&](const cn::ExecuteResult&) {});
    C.QueueExecute("]", [&](const cn::ExecuteResult& r) {
        REQUIRE(r.output.find("batch: committed") != std::string::npos);
        got_commit = true;
    });
    C.Drain();

    REQUIRE(got_open);
    REQUIRE(got_queued);
    REQUIRE(got_commit);
    REQUIRE(a->value == "1");
    REQUIRE(b->value == "2");

    // One Undo rolls back both cvars (single-transaction snapshot).
    auto changes = C.Undo();
    REQUIRE(changes.size() == 2);
    REQUIRE(a->value == "0");
    REQUIRE(b->value == "0");
}

TEST_CASE("SaveArchivedCvars writes only diverged-from-default archive cvars",
          "[core][console][archive]") {
    auto& C = cn::Console::Get();
    C.RegisterCVar("test_archive_kept", "5", "archived numeric", cn::CVAR_ARCHIVE);
    C.RegisterCVar("test_archive_default", "5", "archived but at default", cn::CVAR_ARCHIVE);
    C.RegisterCVar("test_archive_volatile", "tmp", "not archived");
    C.SetCVarOverride("test_archive_kept", "42");
    C.SetCVarOverride("test_archive_volatile", "modified");

    auto path = std::filesystem::temp_directory_path() / "psynder_test_archive.cfg";
    int n = C.SaveArchivedCvars(path.string());
    REQUIRE(n >= 1);

    // Re-read the file; expect exactly the kept cvar line, no default
    // or volatile line.
    std::FILE* f = std::fopen(path.string().c_str(), "rb");
    REQUIRE(f != nullptr);
    std::string content;
    char buf[256];
    while (auto got = std::fread(buf, 1, sizeof buf, f))
        content.append(buf, got);
    std::fclose(f);
    REQUIRE(content.find("test_archive_kept 42") != std::string::npos);
    REQUIRE(content.find("test_archive_default") == std::string::npos);
    REQUIRE(content.find("test_archive_volatile") == std::string::npos);

    // LoadFromFile round-trip: reset, then re-apply.
    C.SetCVarOverride("test_archive_kept", "999");
    auto load = C.LoadFromFile(path.string());
    REQUIRE(load.ok);
    REQUIRE(C.FindCVar("test_archive_kept")->value == "42");
}

TEST_CASE(
    "requires_predicate warning fires on interactive set, "
    "but is suppressed during a script replay",
    "[core][console][deps]") {
    auto& C = cn::Console::Get();
    auto* gate = C.RegisterCVar("test_deps_dependent", "off", "dep cvar");
    auto* dep = C.RegisterCVar("test_deps_dependency", "off", "the prerequisite");
    dep->requires_predicate = [&]() { return gate->GetBool(); };
    dep->requires_hint = "set test_deps_dependent first";

    // gate is off; the predicate returns false; we expect a warning.
    auto r = C.Execute("test_deps_dependency on");
    REQUIRE(r.ok);
    REQUIRE(r.output.find("[warn]") != std::string::npos);

    // Now flip the gate; the predicate returns true; no warning.
    C.SetCVarOverride("test_deps_dependent", "on");
    auto r2 = C.Execute("test_deps_dependency off");
    REQUIRE(r2.ok);
    REQUIRE(r2.output.find("[warn]") == std::string::npos);

    // SuppressDepWarnings flag (the cfg-replay path) silences the warn
    // even when the predicate is false.
    C.SetCVarOverride("test_deps_dependent", "off");
    C.SetSuppressDepWarnings(true);
    auto r3 = C.Execute("test_deps_dependency on");
    REQUIRE(r3.output.find("[warn]") == std::string::npos);
    C.SetSuppressDepWarnings(false);
}
