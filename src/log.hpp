#pragma once

// Logging helpers. No-op under Emscripten (no terminal).
// Uses std::print/std::println on native builds.

#ifdef __EMSCRIPTEN__

#include <cstdio>

// Silence all progress/status output in WASM
template <typename... Args>
inline void log_print([[maybe_unused]] Args&&...) {}

template <typename... Args>
inline void log_println([[maybe_unused]] Args&&...) {}

inline void log_flush() {}

#else

#include <cstdio>
#include <print>

template <typename... Args>
inline void log_print(std::format_string<Args...> fmt, Args&&... args) {
    std::print(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_println(std::format_string<Args...> fmt, Args&&... args) {
    std::println(fmt, std::forward<Args>(args)...);
}

inline void log_println() { std::println(""); }

inline void log_flush() { std::fflush(stdout); }

#endif
