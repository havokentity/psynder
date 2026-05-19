// SPDX-License-Identifier: MIT
// Psynder — log impl. Minimal first cut: stderr + sink list with a mutex.
// Lane 01 will swap this for a lock-free per-worker ring once the job system
// (lane 04) lands.

#include "Log.h"

#include <cstdio>
#include <mutex>
#include <vector>

namespace psynder::log {

namespace {
std::mutex& sinks_mutex() {
    static std::mutex m;
    return m;
}
std::vector<Sink>& sinks() {
    static std::vector<Sink> v;
    return v;
}

const char* level_prefix(Level l) {
    switch (l) {
        case Level::Info:  return "info";
        case Level::Warn:  return "warn";
        case Level::Error: return "error";
    }
    return "?";
}
}  // namespace

void emit(Level level, fmt::string_view fmt_str, fmt::format_args args) {
    std::string line = fmt::vformat(fmt_str, args);
    std::fprintf(level == Level::Error ? stderr : stdout, "[%s] %s\n",
                 level_prefix(level), line.c_str());
    std::fflush(level == Level::Error ? stderr : stdout);
    std::lock_guard<std::mutex> lock(sinks_mutex());
    for (auto& s : sinks()) s(level, line);
}

void add_sink(Sink sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lock(sinks_mutex());
    sinks().push_back(sink);
}

void remove_all_sinks() {
    std::lock_guard<std::mutex> lock(sinks_mutex());
    sinks().clear();
}

}  // namespace psynder::log
