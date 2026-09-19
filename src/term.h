// cloud: terminal sugar (zig-style build output).
//
// A tiny, dependency-free layer: ANSI colors when they work (Windows Terminal
// gets VT processing enabled; legacy consoles fall back to plain), [i/n]
// step lines, an ASCII spinner that lives on stderr while the compiler runs,
// and elapsed-time summaries. Plain output in CI / non-TTY / NO_COLOR.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace term {

namespace detail {
    inline bool ttyStdout() {
#if defined(_WIN32)
        return _isatty(_fileno(stdout)) != 0;
#else
        return isatty(fileno(stdout)) != 0;
#endif
    }
    inline bool ttyStderr() {
#if defined(_WIN32)
        return _isatty(_fileno(stderr)) != 0;
#else
        return isatty(fileno(stderr)) != 0;
#endif
    }

#if defined(_WIN32)
inline UINT& oldOutputCp() {
    static UINT v = 0;
    return v;
}
inline void restoreOutputCp() {
    if (oldOutputCp() != 0) SetConsoleOutputCP(oldOutputCp());
}
#endif

    inline bool consoleSetup() {
#if defined(_WIN32)
        // Tree glyphs are UTF-8; legacy consoles default to CP437/850 and
        // render them as mojibake ("Γö£ΓöÇ" for "├─"). Switch to UTF-8 for
        // the session (restored on exit), and enable VT processing for color.
        const UINT oldCp = GetConsoleOutputCP();
        if (oldCp != 65001 /* CP_UTF8 */) {
            if (!SetConsoleOutputCP(65001)) return false;
            oldOutputCp() = oldCp;
            std::atexit(restoreOutputCp);
        }
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) return false;
        return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
        return true;
#endif
    }

    inline bool colorDefault() {
        if (std::getenv("NO_COLOR") || std::getenv("CLOUD_NO_COLOR")) return false;
        if (std::getenv("CI")) return false;
        return ttyStdout() && consoleSetup();
    }
} // namespace detail

// The project's [diagnostics] colored_output may veto colors; default is on.
inline bool& colorPref() {
    static bool v = true;
    return v;
}
inline void setColorPref(bool enabled) { colorPref() = enabled; }

// --silent: cloud prints nothing (steps, tree, summaries, its own error
// line). The compiler's diagnostics still flow, and `run` lets the program
// print -- only cloud's own output is silenced. Exit codes are unaffected.
inline bool& silentMode() {
    static bool v = false;
    return v;
}
inline void setSilent(bool enabled) { silentMode() = enabled; }
inline bool silent() { return silentMode(); }

inline bool color() {
    static const bool dflt = detail::colorDefault();
    return dflt && colorPref() && !silent();
}

inline const char* kReset  = "\x1b[0m";
inline const char* kBold   = "\x1b[1m";
inline const char* kDim    = "\x1b[2m";
inline const char* kRed    = "\x1b[31m";
inline const char* kGreen  = "\x1b[32m";
inline const char* kYellow = "\x1b[33m";
inline const char* kCyan   = "\x1b[36m";

inline const char* style(const char* code) { return color() ? code : ""; }

// [ i/n] text -- legacy flat step line (kept for non-build callers).
inline void step(int index, int total, const std::string& text) {
    if (silent()) return;
    std::fprintf(stdout, "%s[%d/%d]%s %s%s%s\n", style(kDim), index, total,
                 style(kReset), style(kCyan), text.c_str(), style(kReset));
    std::fflush(stdout);
}

inline void ok(const std::string& text) {
    if (silent()) return;
    std::fprintf(stdout, "%sOK%s   %s\n", style(kGreen), style(kReset), text.c_str());
    std::fflush(stdout);
}

inline void err(const std::string& text) {
    if (silent()) return;
    std::fprintf(stderr, "%serror%s %s\n", style(kRed), style(kReset), text.c_str());
    std::fflush(stderr);
}

inline void note(const std::string& text) {
    if (silent()) return;
    std::fprintf(stderr, "%snote%s %s\n", style(kYellow), style(kReset), text.c_str());
}

inline void dim(const std::string& text) {
    if (silent()) return;
    std::fprintf(stdout, "%s%s%s\n", style(kDim), text.c_str(), style(kReset));
}

// A live build tree, zig-style: a parent line, then child steps as
//   parent
//   ├─ step one (0.0s)
//   ├─ step two  <- ticks elapsed time in place while running
//   └─ step three
// The running step's line is rewritten in place until it finishes (the
// "down and back up" behavior); instant steps print directly. No-ops to
// plain text lines on non-TTY/CI.
class Stepper {
public:
    explicit Stepper(std::string project) : project_(std::move(project)) {
        if (silent()) return;
        std::fprintf(stdout, "%s%s%s\n", style(kBold), project_.c_str(), style(kReset));
        std::fflush(stdout);
    }
    Stepper(const Stepper&) = delete;
    Stepper& operator=(const Stepper&) = delete;
    ~Stepper() { stopTicker(); }

    // An instant child step (already done).
    void instant(const std::string& text, bool last = false) {
        printChild(text, "", last);
    }

    // A long-running child step: its line ticks elapsed time in place.
    void begin(const std::string& name, bool last = false) {
        if (silent()) return;
        name_ = name;
        last_ = last;
        t0_ = std::chrono::steady_clock::now();
        if (!detail::ttyStderr()) return;
        ticking_.store(true, std::memory_order_relaxed);
        ticker_ = std::thread([this] {
            static const char* frames[] = {"-", "\\", "|", "/"};
            int i = 0;
            while (ticking_.load(std::memory_order_relaxed)) {
                std::fprintf(stderr, "\r%s %s %s %s", frames[i++ % 4],
                             name_.c_str(), style(kDim), sinceShort(t0_).c_str());
                std::fprintf(stderr, "%s", style(kReset));
                std::fflush(stderr);
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
        });
    }

    void ok() { finish(true); }
    void fail() { finish(false); }

private:
    std::string project_;
    std::string name_;
    bool last_ = false;
    std::chrono::steady_clock::time_point t0_{};
    std::atomic<bool> ticking_{false};
    std::thread ticker_;

    void stopTicker() {
        if (!ticking_.load(std::memory_order_relaxed)) return;
        ticking_.store(false, std::memory_order_relaxed);
        if (ticker_.joinable()) ticker_.join();
        if (detail::ttyStderr()) {
            std::fprintf(stderr, "\r\x1b[2K"); // erase the ticking line
            std::fflush(stderr);
        }
    }

    void finish(bool good) {
        const std::string took = sinceShort(t0_);
        stopTicker();
        printChild(name_, " (" + took + ")", last_, good);
    }

    void printChild(const std::string& text, const std::string& suffix,
                    bool last, bool good = true) {
        if (silent()) return;
        const bool fancy = color();
        // UTF-8 tree glyphs as hex escapes: correct regardless of the source
        // encoding MSVC assumes (no /utf-8 in the build).
        const char* branch = last
            ? (fancy ? "\xE2\x94\x94\xE2\x94\x80 " : "`- ")
            : (fancy ? "\xE2\x94\x9C\xE2\x94\x80 " : "+- ");
        const char* mark = good ? (fancy ? "\xE2\x9C\x93" : "v")
                                : (fancy ? "\xE2\x9C\x97" : "x");
        std::fprintf(stdout, "%s%s %s%s%s %s%s%s%s\n", style(kDim), branch,
                     good ? style(kGreen) : style(kRed), mark, style(kReset),
                     text.c_str(), style(kDim), suffix.c_str(), style(kReset));
        std::fflush(stdout);
    }

    static std::string sinceShort(std::chrono::steady_clock::time_point t0) {
        const double s = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        char buf[24];
        std::snprintf(buf, sizeof buf, "%.1fs", s);
        return buf;
    }
};

// Elapsed-time helper: "0.4s", "1.9s", "2m 07s".
inline std::string since(std::chrono::steady_clock::time_point t0) {
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    if (secs < 60.0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1fs", secs);
        return buf;
    }
    const int mins = static_cast<int>(secs) / 60;
    const int rest = static_cast<int>(secs) - mins * 60;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%dm %02ds", mins, rest);
    return buf;
}

} // namespace term
