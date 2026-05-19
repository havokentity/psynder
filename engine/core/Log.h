// SPDX-License-Identifier: MIT
// Psynder — log API. Ported from dmonte (pt::log → psynder::log).
//
// Lock-free per-worker ring buffer drained by the main thread; see Log.cpp.

#pragma once

#include <string>
#include <fmt/format.h>

namespace psynder::log {

enum class Level { Info, Warn, Error };

void emit(Level level, fmt::string_view fmt_str, fmt::format_args args);

// A sink receives every log line in addition to stderr. Used by the console
// server (and any future telemetry sink) to forward log events to subscribers.
using Sink = void (*)(Level, const std::string&);
void add_sink(Sink sink);
void remove_all_sinks();

template <class... Args>
inline void info(fmt::format_string<Args...> f, Args&&... args) {
    emit(Level::Info, f, fmt::make_format_args(args...));
}
template <class... Args>
inline void warn(fmt::format_string<Args...> f, Args&&... args) {
    emit(Level::Warn, f, fmt::make_format_args(args...));
}
template <class... Args>
inline void error(fmt::format_string<Args...> f, Args&&... args) {
    emit(Level::Error, f, fmt::make_format_args(args...));
}

}  // namespace psynder::log

#define PSY_LOG_INFO(...)  ::psynder::log::info(__VA_ARGS__)
#define PSY_LOG_WARN(...)  ::psynder::log::warn(__VA_ARGS__)
#define PSY_LOG_ERROR(...) ::psynder::log::error(__VA_ARGS__)
