// Windows/libstdc++ shim for C++23 std::print / std::println.
//
// On Windows, libstdc++'s <print> implements std::vprint_unicode by asking
// whether the stream is a console and, if so, writing a native Unicode string
// to it:
//
//     void* __open_terminal(FILE*);
//     error_code __write_to_terminal(void*, span<char>);
//
// Those two helpers live in libstdc++ proper, and the mingw-w64 builds of
// libstdc++ (as shipped by Debian, for example) do not provide them, so linking
// anything that calls std::println fails with undefined references to
// std::__open_terminal / std::__write_to_terminal.
//
// Reporting "this stream is not a terminal" is enough: libstdc++ then falls back
// to the same std::fwrite path it uses for std::vprint_nonunicode, which is
// exactly the behaviour we want. The only thing given up is the native console
// Unicode path, and the CLI's output is ASCII.
//
// Scoped to libstdc++ on Windows: MSVC and clang-cl use Microsoft's STL, which
// has its own working implementation and must not see these definitions.

#if defined(_WIN32)

// __GLIBCXX__ is only defined once a libstdc++ header has been seen, so pull one
// in before testing which standard library this is.
#include <version>

#if defined(__GLIBCXX__)

#include <cstdio>
#include <span>
#include <system_error>

namespace std {

void* __open_terminal(FILE*) {
    return nullptr;  // never treat the stream as a console
}

error_code __write_to_terminal(void*, span<char>) {
    return error_code();  // unreachable while __open_terminal returns nullptr
}

}  // namespace std

#endif  // __GLIBCXX__
#endif  // _WIN32
