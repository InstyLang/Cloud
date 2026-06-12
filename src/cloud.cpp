#include <cloudServer/core.hpp>

#include <curl/curl.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr const char* versionText = "0.2.0";
constexpr const char* defaultRegistryUrl = "https://ecliptix-web.insty.workers.dev";
constexpr std::size_t defaultMaxPackageBytes = 50 * 1024 * 1024;

struct ParsedArgs {
    std::string command;
    std::vector<std::string> positional;
    std::map<std::string, std::string> values;
    bool verbose = false;
    bool release = false;
};

struct ProjectConfig {
    std::string projectName = "app";
    std::string version = "0.1.0";
    std::string mainFile = "src/main.ins";
    std::string outputDir = ".cloud/objects";
    std::string outputFormat = "executable";
    std::vector<std::string> moduleSearchPaths;
    std::map<std::string, std::string> dependencies;
};

struct PackageSpec {
    std::string owner;
    std::string packageName;
    std::string scopedName;
    std::string range = "*";
};

struct HttpResult {
    long status = 0;
    std::string body;
    std::string finalUrl;
};

struct LockEntry {
    std::string scopedName;
    std::string owner;
    std::string packageName;
    std::string version;
    std::string range = "*";        // requested range that resolved to version
    std::string checksumSha256;     // sha256 of the source archive (may be empty)
    bool direct = true;             // listed in config.toml vs pulled transitively
};

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool isFlagWithValue(const std::string& value) {
    return value == "--config" ||
           value == "--registry" ||
           value == "--token" ||
           value == "--bootstrap-token" ||
           value == "--version" ||
           value == "--name" ||
           value == "--scope" ||
           value == "--expires-days" ||
           value == "--expires-seconds" ||
           value == "--prefix";
}

std::string getEnv(const char* key, const std::string& fallback = "") {
    const char* value = std::getenv(key);
    return value ? value : fallback;
}

std::string stripQuotes(std::string value) {
    value = CloudServer::trimCopy(value);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string removeInlineComment(const std::string& line) {
    bool inString = false;
    char quote = '\0';
    for (std::size_t index = 0; index < line.size(); ++index) {
        char ch = line[index];
        if ((ch == '"' || ch == '\'') && (index == 0 || line[index - 1] != '\\')) {
            if (!inString) {
                inString = true;
                quote = ch;
            } else if (quote == ch) {
                inString = false;
            }
        }
        if (ch == '#' && !inString) {
            return line.substr(0, index);
        }
    }
    return line;
}

std::string readTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("could not open file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

void writeTextFile(const fs::path& path, const std::string& content) {
    if (!path.parent_path().empty()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("could not write file: " + path.string());
    }
    file << content;
}

void createDirectory(const fs::path& path) {
    if (!fs::exists(path)) {
        fs::create_directories(path);
    }
}

std::string normalizeRegistryUrl(std::string registryUrl) {
    registryUrl = CloudServer::trimCopy(registryUrl);
    while (!registryUrl.empty() && registryUrl.back() == '/') {
        registryUrl.pop_back();
    }
    return registryUrl.empty() ? defaultRegistryUrl : registryUrl;
}

std::string shellQuote(const fs::path& path) {
    std::string value = path.string();
    std::string output = "'";
    for (char ch : value) {
        if (ch == '\'') {
            output += "'\\''";
        } else {
            output.push_back(ch);
        }
    }
    output += "'";
    return output;
}

ParsedArgs parseArgs(int argc, char** argv) {
    ParsedArgs args;
    if (argc > 1) {
        args.command = argv[1];
    }
    for (int index = 2; index < argc; ++index) {
        std::string value = argv[index];
        if (value == "--verbose") {
            args.verbose = true;
        } else if (value == "--release") {
            args.release = true;
        } else if (isFlagWithValue(value)) {
            if (index + 1 >= argc) {
                throw std::runtime_error("missing value for " + value);
            }
            args.values[value] = argv[++index];
        } else {
            args.positional.push_back(value);
        }
    }
    return args;
}

std::string optionValue(const ParsedArgs& args, const std::string& key, const std::string& fallback = "") {
    auto found = args.values.find(key);
    return found == args.values.end() ? fallback : found->second;
}

std::string configFilePath(const ParsedArgs& args) {
    return optionValue(args, "--config", getEnv("CLOUD_CONFIG", "config.toml"));
}

ProjectConfig loadProjectConfig(const fs::path& configPath) {
    ProjectConfig config;
    std::ifstream file(configPath);
    if (!file.is_open()) {
        throw std::runtime_error("No config.toml found. Run 'cloud init' to create a project.");
    }

    std::string section;
    std::string line;
    while (std::getline(file, line)) {
        line = CloudServer::trimCopy(removeInlineComment(line));
        if (line.empty()) {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = CloudServer::trimCopy(line.substr(1, line.size() - 2));
            continue;
        }

        std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = stripQuotes(line.substr(0, equals));
        std::string value = stripQuotes(line.substr(equals + 1));

        if (section == "project" && key == "name") {
            config.projectName = value;
        } else if (section == "project" && key == "version") {
            config.version = value;
        } else if (section == "project" && key == "main") {
            config.mainFile = value;
        } else if (section == "compiler" && key == "output_format") {
            config.outputFormat = value;
        } else if (section == "paths" && key == "output_dir") {
            config.outputDir = value;
        } else if (section == "paths" && key == "module_search_paths") {
            // Parse a TOML inline array: ["a", "b", ...]. Tolerant of spacing.
            std::string list = CloudServer::trimCopy(value);
            if (!list.empty() && list.front() == '[') {
                list = list.substr(1);
            }
            if (!list.empty() && list.back() == ']') {
                list.pop_back();
            }
            std::stringstream items(list);
            std::string item;
            while (std::getline(items, item, ',')) {
                std::string path = stripQuotes(CloudServer::trimCopy(item));
                if (!path.empty()) {
                    config.moduleSearchPaths.push_back(path);
                }
            }
        } else if (section == "dependencies") {
            config.dependencies[key] = value;
        }
    }

    return config;
}

void printHelp() {
    std::println("Cloud - Insty Package Manager v{}\n", versionText);
    std::println("Usage: cloud <command> [options]\n");
    std::println("Commands:");
    std::println("  init [name]                  Initialize a new Insty project");
    std::println("  build                        Build the current project");
    std::println("  run                          Build and run the project");
    std::println("  clean                        Clean build artifacts");
    std::println("  publish                      Upload this project to the registry");
    std::println("  install <@owner/package>     Download and install a package");
    std::println("  uninstall <@owner/package>   Remove an installed package");
    std::println("  update                       Install dependencies from config.toml");
    std::println("  list                         List installed packages");
    std::println("  yank <@owner/package>        Yank a published package version");
    std::println("  token <account>              Create a publish token using bootstrap auth");
    std::println("  token revoke                 Revoke a token (--token self, or --prefix with admin/bootstrap)");
    std::println("  test                         Run project tests");
    std::println("  version                      Show version information");
    std::println("  help                         Show this help message\n");
    std::println("Options:");
    std::println("  --registry <url>             Registry URL, default CLOUD_REGISTRY_URL or {}", defaultRegistryUrl);
    std::println("  --token <token>              Registry bearer token, default CLOUD_TOKEN");
    std::println("  --bootstrap-token <token>    Bootstrap token for cloud token, default CLOUD_BOOTSTRAP_TOKEN");
    std::println("  --name <@owner/package>      Publish name override");
    std::println("  --version <range|version>    Install range or publish/yank version override");
    std::println("  --scope <publish|admin>      Token scope");
    std::println("  --expires-days <n>           Token expiry in days (token create)");
    std::println("  --expires-seconds <n>        Token expiry in seconds (token create)");
    std::println("  --prefix <tokenPrefix>       Token prefix to revoke (token revoke, admin/bootstrap)");
    std::println("  --config <file>              Use specific config file");
    std::println("  --verbose                    Show detailed output");
    std::println("  --release                    Build in release mode");
}

void printVersion() {
    std::println("Cloud v{}", versionText);
    std::println("Insty Package Manager");
}

void initProject(const std::string& name) {
    std::println("Initializing Insty project: {}", name);

    createDirectory(name);
    createDirectory(fs::path(name) / "src");
    createDirectory(fs::path(name) / ".cloud/libs");
    createDirectory(fs::path(name) / ".cloud/objects");

    std::string configContent = R"(# Insty Project Configuration

[project]
name = ")" + name + R"("
version = "0.1.0"
description = "A new Insty project"
authors = ["Your Name"]
license = "MIT"
main = "src/main.ins"
module = "main"

[compiler]
optimization_level = 0
ast_optimization = true
output_format = "executable"
debug_info = true

[paths]
module_search_paths = [".", "src"]
output_dir = ".cloud/objects"

[features]
enable_imports = true
enable_nested_functions = true
enable_builtins = true
enable_syscalls = true
enable_pointers = true
enable_structs = true
enable_enums = true

[diagnostics]
error_verbosity = "normal"
show_error_context = true
colored_output = true

[dependencies]
# "@owner/package" = "^1.0.0"
)";

    writeTextFile(fs::path(name) / "config.toml", configContent);

    std::string mainContent = R"(module main

fun main() -> i32 {
    console::out("Hello from )" + name + R"(!")
    return 0
}
)";
    writeTextFile(fs::path(name) / "src/main.ins", mainContent);

    std::string gitignoreContent = R"(# Build artifacts
.cloud/objects/
.cloud/modules/
*.ll
*.out
*.exe

# IDE
.vscode/
.idea/

# OS
.DS_Store
Thumbs.db
)";
    writeTextFile(fs::path(name) / ".gitignore", gitignoreContent);

    std::string readmeContent = "# " + name + "\n\nAn Insty project.\n\n";
    readmeContent += "## Building\n\n```bash\ncloud build\n```\n\n";
    readmeContent += "## Running\n\n```bash\ncloud run\n```\n";
    writeTextFile(fs::path(name) / "README.md", readmeContent);

    std::string agentsContent = "# AGENTS.md\n\n";
    agentsContent += "Project: " + name + "\n\n";
    agentsContent += R"AGENTS(Agent-facing guide for working in this Insty project.

## Project Layout

- `config.toml` - Cloud project configuration.
- `src/main.ins` - Main Insty entry module.
- `.cloud/objects/` - Build outputs and object files.
- `.cloud/libs/` - Installed package source dependencies (`owner/package/version`).
- `.cloud/modules/` - Dependency modules staged in scoped layout for builds:
  `@owner/package` is staged as `.cloud/modules/owner/package.ins` and imported
  as `import owner::package`.
- `README.md` - Human project overview.
- `AGENTS.md` - Agent instructions and Insty reference.
- `CLAUDE.md` - Claude-specific project guide.

## Common Commands

```bash
cloud build
cloud run
cloud clean
cloud test
cloud install @owner/package
cloud update
cloud publish --name @owner/package --version 0.1.0
```

Useful environment variables:

- `INSTY_COMPILER` - Override compiler path used by Cloud.
- `CLOUD_CONFIG` - Override default `config.toml` path.
- `CLOUD_REGISTRY_URL` - Override package registry URL.
- `CLOUD_TOKEN` - Registry bearer token.

## Compiler Commands

```bash
insty src/main.ins -o app
insty -c src/main.ins --objects-dir .cloud/objects
insty --emit-llvm src/main.ins --objects-dir .cloud/objects
insty --target x86_64_linux src/main.ins -o app
insty --target targets/x86_64-unknown-none.toml --freestanding src/main.ins -o kernel.elf
```

Compiler flags currently useful for OS/dev work:

- `--target <name-or-file>` - Built-in target or custom target TOML file.
- `--freestanding`, `--no-std` - Disable hosted assumptions.
- `--runtime-start` - Explicitly generate runtime entry shim.
- `--allocator none|runtime|external` - Select heap strategy.
- `--entry <symbol>` - Linker entry symbol.
- `--linker <path>` - Override linker executable.
- `--linker-script <file>` - Use linker script.
- `--sysroot <dir>` - Use target sysroot.
- `--output-format executable|elf|raw-binary|pe|uefi` - Link output format.
- `--raw-binary` - Emit flat binary for ELF targets.
- `--multiboot2` - Add Multiboot2 header object for x86/x86_64 ELF kernels.
- `--panic abort|handler` and `--panic-handler <symbol>` - Panic strategy: `abort` lowers `@panic` to `llvm.trap`; `handler` calls the given noreturn symbol. Overrides target spec `panic`/`panic_handler` keys.

## `config.toml`

Cloud reads these sections:

```toml
[project]
name = "app"
version = "0.1.0"
main = "src/main.ins"
module = "main"

[compiler]
optimization_level = 0
output_format = "executable"

[paths]
module_search_paths = [".", "src"]
output_dir = ".cloud/objects"

[dependencies]
# "@owner/package" = "^1.0.0"
```

## Insty Basics

```ecx
module main

fun add(i32 a, i32 b) -> i32 {
    return a + b
}

fun main() -> i32 {
    i32 value = add(20, 22)
    return value
}
```

Core syntax:

- Modules start with `module name`.
- Functions use `fun name(args) -> type { ... }`.
- Variables require explicit types: `i32 count = 0`.
- Pointers use `T*`, address-of uses `&x`, dereference uses `~ptr`.
- Operators: arithmetic `+ - * /`, comparison `== != < > <= >=`, logical `&& || !`, bitwise `& | ^`, shifts `<< >>`.
- Unary `!` negates booleans and is C-like for integers (`!n` is true when `n == 0`).
- Control flow includes `if`, `while`, `loop`, `switch`, `break`, and `return`.
- Imports use `import module`; `::` separates scope/directory segments, so a
  dependency `@owner/package` is imported as `import owner::package`. C headers
  use `cimport header` in hosted mode.

Primitive types:

- Integers: `i8`, `i16`, `i32`, `i64`, `i128`, `u8`, `u16`, `u32`, `u64`, `u128`.
- Floats: `f16`, `f32`, `f64`, `f128`.
- Other: `bool`, `text`, `void`.

## Structs, Classes, Enums

```ecx
struct Point {
    i32 x,
    i32 y
}

enum Status : i32 {
    Pending,
    Ready
}

class Counter {
    i32 value

    constructor(i32 initial) {
        this.value = initial
    }

    fun inc() -> void {
        this.value = this.value + 1
    }
}
```

## Generics

```ecx
fun id<T>(T value) -> T {
    return value
}

struct Box<T> {
    T value
}
```

## Builtins

Common builtins:

- `@sizeof(Type)` / `sizeof<Type>()` - Type size.
- `@malloc(size[, align])`, `@realloc(ptr, size[, align])`, `@free(ptr[, size, align])` - Front-ends over the explicit allocator ABI when enabled.
- `@memcpy(dest, src, size)`, `@memset(ptr, value, size)` - Memory operations.
- `@readFile("path")` - Embed file content at compile time.
- `@system("cmd")` - Run command at compile time.
- `@getCurrentOS()` - Compile-time target OS code.
- `@syscall(...)` - Hosted Linux/InstantOS syscall helper; disabled in freestanding mode.
- `@print(...)`, `@println(...)` - Syscall-backed console helpers; disabled in freestanding mode.
- `@panic("msg")` - Stop via the configured panic strategy: `abort` traps; `handler` calls the configured `panic_handler` symbol with the message. Allowed in freestanding mode.

## Standard Library

Modules shipped next to the compiler (in `libs/`) and resolved with plain `import`:

- `io` (hosted Linux): `print`, `println`, `eprint`, `eprintln`, `print_int`, `println_int`, `eprintln_int` - raw write(2) syscalls, no libc.
- `str`: `len`, `eq`, `starts_with`, `byte_at`, `index_of`.
- `math`: `abs`, `min`, `max`, `clamp`, `sign`, `pow`.
- `mem`: `copy`, `set`, `zero`, `eq`.
- `uefi` (x86_64_efi target): firmware console output through `SystemTable->ConOut`. Call `uefi.init(image_handle, system_table)` first, then `print`, `println`, `print_int`, `println_int`, `clear`, `stall(us)`.

UEFI entry: when `main` declares two pointer parameters, the EFI entry shim passes `(ImageHandle, SystemTable)`:

```ecx
import uefi

fun main(u8* image_handle, u8* system_table) -> i64 {
    uefi.init(image_handle, system_table)
    uefi.println("hello from Insty")
    return 0
}
```

## Freestanding And OS Development

Use freestanding mode for kernels, boot code, UEFI apps, and custom OS targets:

```bash
insty --freestanding --target targets/x86_64-unknown-none.toml src/main.ins -o kernel.elf
```

Freestanding guarantees:

- No libc/CRT/dynamic linker assumptions.
- No hidden syscalls.
- No generated `_start` unless `--runtime-start` is passed.
- No allocator runtime unless `--allocator runtime` is passed.
- Heap use is rejected unless `--allocator runtime` or `--allocator external` is selected.
- `cimport` is rejected because it implies hosted C library linkage.
- No stack unwinding: every function is emitted `nounwind` with unwind tables suppressed. Kernels that want panic recovery must set `panic = "handler"` and implement the handler themselves.

External allocator ABI when using `--allocator external`:

```ecx
// Provided by linked runtime/kernel objects.
// __ins_alloc(u64 size, u64 align) -> i8*
// __ins_free(i8* ptr, u64 size, u64 align) -> void
// Optional compatibility hook for @realloc:
// __ins_realloc(i8* ptr, u64 size, u64 align) -> i8*
```

Allocator mode defaults to `none`. Any heap-backed feature is a compile-time error until the build explicitly passes `--allocator runtime` or `--allocator external`.

Heap-backed features include `new`, `delete`, array literals that require heap storage, string interpolation buffers, `@malloc`, `@free`, and `@realloc`.

## Custom Target Specs

Create a target TOML and pass it to `--target`:

```toml
name = "x86_64_unknown_none"
arch = "x86_64"
vendor = "unknown"
os = "none"
abi = "kernel"
llvm_triple = "x86_64-unknown-none"
object_format = "elf"
output_format = "elf"
pointer_width = 64
endian = "little"
disable_red_zone = true
entry = "kernel_main"
linker_script = "linker.ld"
freestanding = true
multiboot2 = false
```

Supported target keys include:

- Identity: `name`, `arch`, `vendor`, `os`, `abi`, `llvm_triple`.
- Binary format: `object_format`, `output_format`, `pointer_width`, `endian`.
- Kernel controls: `disable_red_zone`, `entry`, `linker_script`, `freestanding`, `multiboot2`.
- Panic model: `panic = "abort"|"handler"`, `panic_handler = "<symbol>"` (required for `"handler"`).
- Toolchain controls: `linker`, `sysroot`, `dynamic_linker`, `supports_linux_syscalls`.

## Linker Features

The compiler uses argv-based linker execution, not shell command strings.

Supported linker paths:

- ELF executable/kernel via `ld.lld`.
- Raw flat binary via `--raw-binary` or `output_format = "raw-binary"`.
- PE/COFF UEFI app via `--target x86_64_efi` or `output_format = "uefi"`.
- Multiboot2 header generation via `--multiboot2`.
- Cross-linking by target object format and architecture.
- Sysroot-aware linking via `--sysroot` or target `sysroot`.
- Linker-script support via `--linker-script` or target `linker_script`.

## Low-Level Directives

Directives use `[...]` only in declaration contexts.

Functions:

```ecx
fun [name(kernel_main), mangle(off), conv(cdecl), section(".boot")] main() -> void {
    unsafe {
        asm("nop")
    }
}

fun [naked(on), mangle(off)] trampoline() -> void {
    unsafe {
        asm("ret")
    }
}

fun [interrupt(on), mangle(off)] irq() -> void {
    return
}
```

Supported function directives:

- `name(symbol)` - Export a specific symbol name.
- `mangle(off)` - Disable module name mangling.
- `extern(C)` - C ABI naming behavior.
- `conv(cdecl|fastcc|coldcc|stdcall|fastcall|sysv|win64)` - Calling convention.
- `section(".name")` - Place function in a section.
- `naked(on)` - LLVM naked function attribute.
- `interrupt(on)` - LLVM interrupt function attribute.
- `unsafe(on)` - Function may perform unsafe operations and requires unsafe calls.

Structs:

```ecx
struct [repr(C), packed(on), align(16)] Descriptor {
    u16 limit,
    u64 base
}
```

Supported struct directives:

- `repr(C)` - Preserved in AST; Insty already keeps declared field order.
- `packed(on)` - Emits packed LLVM struct layout.
- `align(N)` - Applies alignment to stack allocations of the struct.

Sections:

```ecx
section ".boot" {
    fun [mangle(off), name(kernel_main)] main() -> void {
        return
    }
}
```

## Unsafe Boundary

Insty requires explicit `unsafe { ... }` blocks for operations that can break memory, ABI, or platform safety.

```ecx
fun [unsafe(on), mangle(off)] outb(u16 port, u8 value) -> void {
    unsafe {
        asm("outb $$al, $$dx")
    }
}

fun kernel(volatile u32* mmio, i32* counter) -> void {
    unsafe {
        u32 value = volatileLoad<u32>(mmio)
        volatileStore<u32>(mmio, value)
        u32 again = ~mmio
        atomicFetchAdd<i32>(counter, 1)
        atomicFence()
        asm("cli")
        outb(0x3f8, 0)
    }
}
```

Requires `unsafe { ... }`:

- Raw pointer dereference: `~ptr`, `~ptr = value`, and raw pointer indexing.
- Inline assembly: `asm(...)`.
- Volatile memory: `volatileLoad<T>`, `volatileStore<T>`, and volatile pointer dereference.
- Atomics and fences: `atomicLoad`, `atomicStore`, `atomicCompareExchange`, `atomicFetchAdd`, `atomicFence`.
- Indirect calls through function pointers: `fnCall`.
- Explicit casts: `cast<T>(value)`.
- Target syscalls and raw memory builtins: `@syscall`, `@memcpy`, `@memset`.
- Calls to functions marked `[unsafe(on)]`.
- Calls to C import functions.
- Port I/O and CPU-state changes when implemented through `asm(...)` or unsafe wrappers.

Unsafe functions:

```ecx
fun [unsafe(on), mangle(off)] privileged() -> void {
    unsafe {
        asm("hlt")
    }
}

fun caller() -> void {
    unsafe {
        privileged()
    }
}
```

## Volatile MMIO

Use volatile pointers for MMIO:

```ecx
fun mmio(volatile u32* reg) -> u32 {
    unsafe {
        u32 value = volatileLoad<u32>(reg)
        volatileStore<u32>(reg, value)
        return ~reg
    }
}
```

Supported operations:

- `volatileLoad<T>(ptr)`
- `volatileStore<T>(ptr, value)`
- `volatile T* ptr` tracked through pointer dereference where possible.

## Atomics

```ecx
fun increment(i32* ptr) -> i32 {
    unsafe {
        return atomicFetchAdd<i32>(ptr, 1)
    }
}

fun cas(i32* ptr, i32 old, i32 newValue) -> bool {
    unsafe {
        return atomicCompareExchange<i32>(ptr, old, newValue)
    }
}

fun barrier() -> void {
    unsafe {
        atomicFence()
    }
}
```

Supported operations:

- `atomicLoad<T>(ptr[, "ordering"])`
- `atomicStore<T>(ptr, value[, "ordering"])`
- `atomicCompareExchange<T>(ptr, expected, desired[, "ordering"])`
- `atomicFetchAdd<T>(ptr, value[, "ordering"])`
- `atomicFence(["ordering"])`

## Indirect calls (function pointers)

`fnCall` invokes a function through a pointer value, which is required for
firmware tables (UEFI) and runtime-resolved entry points. The first argument
is the target (a pointer or an integer address); remaining arguments are
passed through as-is. The optional generic argument is the return type
(defaults to `void`). The call uses the target's default calling convention,
so on `x86_64_efi` it matches EFIAPI function pointers.

```ecx
fun double_it(i64 n) -> i64 {
    return n * 2
}

fun demo() -> i64 {
    unsafe {
        u64 fn = cast<u64>(&double_it)   // &function yields its address
        return fnCall<i64>(fn, 21)       // 42
    }
    return 0
}
```

Orderings: `relaxed`, `acquire`, `release`, `acq_rel`, `seq_cst`.

## Inline Assembly

```ecx
unsafe {
    asm("nop")
    asm<i64>("rdtsc", "={rax}")
    asm("outb %al, %dx", "{al},{dx}", value, port)
    u8 v = asm<u8>("inb %dx, %al", "={al},{dx}", port)
    asm("movq $0, %cr3", "r", pml4_phys)
}
```

Syntax:

- `asm("template")` emits side-effecting void inline asm.
- `asm<T>("template", "constraints")` emits side-effecting inline asm returning `T`.
- `asm("template", "constraints", input...)` / `asm<T>(...)` pass input operands.

The constraint string is LLVM-style: optional output constraint first (only
with `asm<T>`), then one constraint per input argument in order, then
clobbers (e.g. `~{memory}`, `~{cc}`). Register classes (`r`) and specific
registers (`{dx}`) are supported. Template placeholders are `$0`, `$1`, ...
(output first, then inputs); a literal `$` is written `$$`.

## Compile-Time Constants And Arrays

- `comptime` is accepted as an alias for the current constant declaration context.
- Numeric fixed array types lower to LLVM arrays: `u8[4096]`.
- Named compile-time constants in array sizes are not fully lowered yet; use numeric sizes for now.

## Agent Workflow

- Prefer small, targeted changes.
- Run `cloud build` or direct `insty` commands after modifying Insty code.
- Use `--emit-llvm` to inspect ABI, section, volatile, atomic, and target behavior.
- Do not assume hosted APIs in freestanding projects.
- Keep unsafe operations inside explicit `unsafe { ... }` blocks.
- For kernels, prefer explicit `--target <spec.toml> --freestanding --entry <symbol>`.
- Keep target-specific boot/linker details in target specs and linker scripts.

)AGENTS";
    writeTextFile(fs::path(name) / "AGENTS.md", agentsContent);

    std::string claudeContent = "# CLAUDE.md\n\n";
    claudeContent += "Project: " + name + "\n\n";
    claudeContent += R"CLAUDE(Claude-specific instructions for this Insty project.

Read `AGENTS.md` first. It is the canonical project and language reference generated by `cloud init`.

## Default Behavior

- Use `cloud build` for normal verification.
- Use `cloud run` only when the program is expected to be a hosted executable.
- Use `insty --emit-llvm ...` when checking low-level ABI, target, section, volatile, atomic, or inline assembly behavior.
- Do not add libc, syscalls, heap allocation, `cimport`, or runtime startup to freestanding/kernel code unless explicitly requested.

## Useful Commands

```bash
cloud build
cloud run
cloud clean
insty --emit-llvm src/main.ins --objects-dir .cloud/objects
insty --freestanding --target targets/x86_64-unknown-none.toml src/main.ins -o kernel.elf
```

## Low-Level Insty Quick Reference

```ecx
section ".boot" {
    fun [name(kernel_main), mangle(off), conv(cdecl)] main() -> void {
        unsafe {
            asm("nop")
        }
    }
}

struct [repr(C), packed(on), align(16)] Descriptor {
    u16 limit,
    u64 base
}

fun mmio(volatile u32* reg) -> u32 {
    unsafe {
        u32 value = volatileLoad<u32>(reg)
        volatileStore<u32>(reg, value)
        return ~reg
    }
}

fun atomic_inc(i32* ptr) -> i32 {
    unsafe {
        return atomicFetchAdd<i32>(ptr, 1)
    }
}

fun indirect(u64 fn_addr) -> i64 {
    unsafe {
        // Indirect call through a function pointer (e.g. UEFI firmware tables).
        return fnCall<i64>(fn_addr, 21)
    }
}
```

## Unsafe Boundary

- Use `unsafe { ... }` for raw pointer dereference, inline asm, volatile memory, atomics/fences, indirect calls (`fnCall`), explicit casts, raw memory builtins, target syscalls, and calls to `[unsafe(on)]` or C-imported functions.
- Mark functions that expose privileged or memory-unsafe behavior with `[unsafe(on)]`.
- Keep the unsafe block as small as practical and leave safe setup/validation outside it.

## Allocator Boundary

- Allocator mode defaults to `none`; heap-backed features are compile errors unless the build passes `--allocator runtime` or `--allocator external`.
- External allocators must provide `__ins_alloc(u64 size, u64 align) -> i8*` and `__ins_free(i8* ptr, u64 size, u64 align) -> void`.
- `@malloc`, `@free`, `new`, `delete`, and heap-backed array/string helpers lower through that ABI.

## Notes

- `AGENTS.md` contains the fuller docs for Cloud, compiler flags, target specs, freestanding mode, linker behavior, directives, unsafe boundaries, atomics, volatile, and inline asm.
- Standard library modules ship next to the compiler in `libs/` and are imported by name: `io`, `str`, `math`, `mem` (hosted Linux) and `uefi` (firmware console output on the `x86_64_efi` target; call `uefi.init(image_handle, system_table)` first).
- Keep generated build output under `.cloud/objects/`.
- Keep package sources under `.cloud/libs/`; do not vendor generated objects into source.

)CLAUDE";
    writeTextFile(fs::path(name) / "CLAUDE.md", claudeContent);

    std::println("\nProject created successfully!");
    std::println("Next steps:");
    std::println("  cd {}", name);
    std::println("  cloud build");
    std::println("  cloud run");
}

std::string compilerPath() {
    std::string configured = getEnv("INSTY_COMPILER");
    if (!configured.empty()) {
        return configured;
    }
    if (fs::exists("./insty")) {
        return fs::absolute("./insty").string();
    }
    if (fs::exists("build/insty")) {
        return fs::absolute("build/insty").string();
    }
    if (fs::exists("Compiler/build/insty")) {
        return fs::absolute("Compiler/build/insty").string();
    }
    if (fs::exists("../Compiler/build/insty")) {
        return fs::absolute("../Compiler/build/insty").string();
    }
    if (fs::exists("../../Compiler/build/insty")) {
        return fs::absolute("../../Compiler/build/insty").string();
    }
    return "insty";
}

// Lockfile format v2 (whitespace-separated, one entry per line):
//   @owner/package <version> <range> <checksumSha256> <direct|transitive>
// A leading "# cloud-lock v2" comment marks the format. Legacy v1 lines
// (`@owner/package <version>`) are still accepted for backward compatibility:
// missing fields default to range "*", empty checksum, and direct=true.
std::vector<LockEntry> readLockFile() {
    std::vector<LockEntry> entries;
    std::ifstream file(".cloud/packages.lock");
    if (!file.is_open()) {
        return entries;
    }

    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = CloudServer::trimCopy(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        std::stringstream parts(trimmed);
        std::string scopedName, version, range, checksum, origin;
        parts >> scopedName >> version;
        parts >> range >> checksum >> origin; // optional (v2)

        std::size_t slash = scopedName.find('/');
        if (!CloudServer::isValidScopedPackageName(scopedName) ||
            slash == std::string::npos || version.empty()) {
            continue;
        }

        LockEntry entry;
        entry.scopedName = scopedName;
        entry.owner = scopedName.substr(1, slash - 1);
        entry.packageName = scopedName.substr(slash + 1);
        entry.version = version;
        entry.range = range.empty() ? "*" : range;
        entry.checksumSha256 = (checksum == "-") ? "" : checksum;
        entry.direct = (origin != "transitive");
        entries.push_back(entry);
    }
    return entries;
}

void writeLockFile(std::vector<LockEntry> entries) {
    createDirectory(".cloud");
    std::ofstream file(".cloud/packages.lock", std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("could not write .cloud/packages.lock");
    }
    // Deterministic ordering so the lockfile is stable across runs.
    std::sort(entries.begin(), entries.end(), [](const LockEntry& a, const LockEntry& b) {
        return a.scopedName < b.scopedName;
    });
    file << "# cloud-lock v2\n";
    for (const auto& entry : entries) {
        file << entry.scopedName << " " << entry.version << " "
             << (entry.range.empty() ? "*" : entry.range) << " "
             << (entry.checksumSha256.empty() ? "-" : entry.checksumSha256) << " "
             << (entry.direct ? "direct" : "transitive") << "\n";
    }
}

void upsertLockEntry(const LockEntry& newEntry) {
    std::vector<LockEntry> entries = readLockFile();
    bool updated = false;
    for (auto& entry : entries) {
        if (entry.scopedName == newEntry.scopedName) {
            // Preserve direct=true if it was ever requested directly.
            bool direct = entry.direct || newEntry.direct;
            entry = newEntry;
            entry.direct = direct;
            updated = true;
        }
    }
    if (!updated) {
        entries.push_back(newEntry);
    }
    writeLockFile(entries);
}

void removeLockEntry(const std::string& scopedName) {
    std::vector<LockEntry> entries = readLockFile();
    entries.erase(
        std::remove_if(entries.begin(), entries.end(), [&](const LockEntry& entry) {
            return entry.scopedName == scopedName;
        }),
        entries.end()
    );
    writeLockFile(entries);
}

std::optional<LockEntry> findLockEntry(const std::string& scopedName) {
    for (const auto& entry : readLockFile()) {
        if (entry.scopedName == scopedName) {
            return entry;
        }
    }
    return std::nullopt;
}

void syncInstalledModules(bool verbose = false) {
    fs::path moduleDir = ".cloud/modules";
    fs::remove_all(moduleDir);
    createDirectory(moduleDir);

    // Stage each installed package into a scoped layout so the compiler's
    // `::` module resolver finds them: `import owner::packageName` resolves to
    // `<moduleDir>/owner/packageName.ins`, and submodules resolve to
    // `<moduleDir>/owner/packageName/<sub>.ins` (-> owner::packageName::sub).
    //
    // Mapping of a package's `src/` tree:
    //   src/<packageName>.ins  -> owner/<packageName>.ins        (package entry)
    //   src/main.ins           -> owner/<packageName>.ins        (entry fallback)
    //   src/<other>.ins        -> owner/<packageName>/<other>.ins (submodule)
    for (const auto& entry : readLockFile()) {
        fs::path sourceRoot = fs::path(".cloud/libs") / entry.owner / entry.packageName / entry.version / "src";
        if (!fs::exists(sourceRoot)) {
            continue;
        }

        const fs::path ownerDir = moduleDir / entry.owner;
        const fs::path packageEntry = ownerDir / (entry.packageName + ".ins");
        const fs::path packageSubdir = ownerDir / entry.packageName;
        bool hasNamedEntry = fs::exists(sourceRoot / (entry.packageName + ".ins"));

        for (const auto& file : fs::recursive_directory_iterator(sourceRoot)) {
            if (!file.is_regular_file() || file.path().extension() != ".ins") {
                continue;
            }
            fs::path relative = fs::relative(file.path(), sourceRoot);
            fs::path target;

            const bool isNamedEntry = (relative == fs::path(entry.packageName + ".ins"));
            const bool isMainEntry = (relative == fs::path("main.ins"));

            if (isNamedEntry || (isMainEntry && !hasNamedEntry)) {
                // Package entry module -> owner/packageName.ins (owner::packageName)
                target = packageEntry;
            } else {
                // Submodule -> owner/packageName/<relative> (owner::packageName::...)
                target = packageSubdir / relative;
            }

            createDirectory(target.parent_path());
            fs::copy_file(file.path(), target, fs::copy_options::overwrite_existing);
            if (verbose) {
                std::println("Staged dependency module: {}", target.string());
            }
        }
    }
}

void buildProject(const ParsedArgs& args) {
    ProjectConfig config = loadProjectConfig(configFilePath(args));
    fs::path mainPath = fs::absolute(config.mainFile);
    fs::path outputDir = fs::absolute(config.outputDir);

    if (!fs::exists(mainPath)) {
        throw std::runtime_error("Main file not found: " + mainPath.string());
    }

    createDirectory(outputDir);
    syncInstalledModules(args.verbose);

    fs::path executablePath = outputDir / config.projectName;
    fs::path moduleDir = fs::absolute(".cloud/modules");

    // Build from the project root and hand the compiler explicit module search
    // paths via `-L`, so scoped dependency imports (`import owner::package`)
    // resolve against `.cloud/modules/owner/package.ins`. This replaces the
    // older `cd .cloud/modules` hack, which only worked for flat modules.
    std::string command = shellQuote(compilerPath());
    if (!args.release) {
        command += " -O0";
    }
    command += " " + shellQuote(mainPath);
    command += " --objects-dir " + shellQuote(outputDir);

    // Staged dependency modules first, then any user-configured search paths.
    command += " -L " + shellQuote(moduleDir);
    for (const auto& searchPath : config.moduleSearchPaths) {
        if (searchPath.empty()) {
            continue;
        }
        command += " -L " + shellQuote(fs::absolute(searchPath));
    }

    if (config.outputFormat == "executable") {
        command += " -o " + shellQuote(executablePath);
    } else {
        command += " -c";
    }

    if (args.verbose) {
        std::println("Running: {}", command);
    }

    int result = std::system(command.c_str());
    if (result != 0) {
        throw std::runtime_error("build failed with exit code: " + std::to_string(result));
    }

    std::println("Build successful!");
    if (config.outputFormat == "executable") {
        std::println("Executable: {}", executablePath.string());
    } else {
        std::println("Object files in: {}", outputDir.string());
    }
}

void runProject(const ParsedArgs& args) {
    ProjectConfig config = loadProjectConfig(configFilePath(args));
    fs::path executablePath = fs::path(config.outputDir) / config.projectName;
    if (!fs::exists(executablePath)) {
        throw std::runtime_error("Executable not found. Run 'cloud build' first.");
    }

    std::string command = shellQuote(executablePath);
    if (args.verbose) {
        std::println("Executing: {}", command);
    }
    int result = std::system(command.c_str());
    if (result != 0) {
        std::println(stderr, "Program exited with code: {}", result);
    }
}

void cleanProject(const ParsedArgs& args) {
    ProjectConfig config = loadProjectConfig(configFilePath(args));
    fs::path outputDir = config.outputDir;
    std::println("Cleaning build artifacts...");
    if (fs::exists(outputDir)) {
        fs::remove_all(outputDir);
        createDirectory(outputDir);
        std::println("Cleaned {}", outputDir.string());
    } else {
        std::println("Nothing to clean.");
    }
}

void testProject(const ParsedArgs& args) {
    std::string testDir = "tests";
    if (!fs::exists(testDir)) {
        throw std::runtime_error("Test directory not found: " + testDir);
    }

    int passed = 0;
    int failed = 0;
    for (const auto& entry : fs::directory_iterator(testDir)) {
        if (entry.path().extension() != ".ins") {
            continue;
        }
        std::print("  Testing: {} ... ", entry.path().string());
        std::string command = shellQuote(compilerPath()) + " " + shellQuote(entry.path()) + " -o .cloud/objects/test.o";
        int result = std::system(command.c_str());
        if (result == 0) {
            std::println("PASS");
            ++passed;
        } else {
            std::println("FAIL");
            ++failed;
        }
    }

    std::println("\nTest Results: {} passed, {} failed", passed, failed);
    if (failed > 0) {
        throw std::runtime_error("tests failed");
    }
}

std::string scopedNameFromConfig(const ProjectConfig& config, const ParsedArgs& args) {
    std::string name = optionValue(args, "--name", config.projectName);
    if (!CloudServer::isValidScopedPackageName(name)) {
        throw std::runtime_error("package name must be scoped like @owner/package; set [project].name or pass --name");
    }
    return name;
}

PackageSpec parsePackageSpec(std::string value, const std::string& explicitRange = "") {
    std::string range = explicitRange.empty() ? "*" : explicitRange;
    std::size_t versionMarker = value.find('@', 1);
    if (versionMarker != std::string::npos) {
        range = value.substr(versionMarker + 1);
        value = value.substr(0, versionMarker);
    }
    if (!CloudServer::isValidScopedPackageName(value)) {
        throw std::runtime_error("package name must be scoped like @owner/package");
    }
    std::size_t slash = value.find('/');
    return PackageSpec{
        value.substr(1, slash - 1),
        value.substr(slash + 1),
        value,
        range.empty() ? "*" : range
    };
}

std::string tokenValue(const ParsedArgs& args) {
    return optionValue(args, "--token", getEnv("CLOUD_TOKEN"));
}

std::string registryUrl(const ParsedArgs& args) {
    return normalizeRegistryUrl(optionValue(args, "--registry", getEnv("CLOUD_REGISTRY_URL", defaultRegistryUrl)));
}

std::size_t writeCurlBody(char* data, std::size_t size, std::size_t count, void* context) {
    auto* output = static_cast<std::string*>(context);
    output->append(data, size * count);
    return size * count;
}

HttpResult httpGet(const std::string& url, const std::vector<std::string>& headers = {}) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to initialize curl");
    }

    HttpResult result;
    curl_slist* headerList = nullptr;
    for (const auto& header : headers) {
        headerList = curl_slist_append(headerList, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    }

    CURLcode code = curl_easy_perform(curl);
    char* finalUrl = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
    if (finalUrl) {
        result.finalUrl = finalUrl;
    }
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(code));
    }
    return result;
}

HttpResult httpPostJson(const std::string& url, const std::string& body, const std::vector<std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to initialize curl");
    }

    HttpResult result;
    curl_slist* headerList = nullptr;
    headerList = curl_slist_append(headerList, "Content-Type: application/json");
    for (const auto& header : headers) {
        headerList = curl_slist_append(headerList, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(code));
    }
    return result;
}

HttpResult httpDelete(const std::string& url, const std::string& body,
                      const std::vector<std::string>& headers) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to initialize curl");
    }

    HttpResult result;
    curl_slist* headerList = nullptr;
    headerList = curl_slist_append(headerList, "Content-Type: application/json");
    for (const auto& header : headers) {
        headerList = curl_slist_append(headerList, header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(code));
    }
    return result;
}

HttpResult httpPostMultipart(
    const std::string& url,
    const std::string& token,
    const std::string& manifest,
    const std::string& archive
) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        throw std::runtime_error("failed to initialize curl");
    }

    HttpResult result;
    curl_slist* headers = nullptr;
    std::string authorization = "Authorization: Bearer " + token;
    headers = curl_slist_append(headers, authorization.c_str());

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* manifestPart = curl_mime_addpart(mime);
    curl_mime_name(manifestPart, "manifest");
    curl_mime_data(manifestPart, manifest.data(), manifest.size());
    curl_mime_type(manifestPart, "application/json");

    curl_mimepart* archivePart = curl_mime_addpart(mime);
    curl_mime_name(archivePart, "archive");
    curl_mime_filename(archivePart, "package.tar.gz");
    curl_mime_data(archivePart, archive.data(), archive.size());
    curl_mime_type(archivePart, "application/gzip");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCurlBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);

    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(code));
    }
    return result;
}

std::string octalValue(std::uint64_t value, std::size_t width) {
    std::string output(width, '0');
    std::string digits;
    do {
        digits.insert(digits.begin(), static_cast<char>('0' + (value & 7)));
        value >>= 3;
    } while (value > 0);
    std::size_t start = width > digits.size() + 1 ? width - digits.size() - 1 : 0;
    std::copy(digits.begin(), digits.end(), output.begin() + start);
    output[width - 1] = '\0';
    return output;
}

std::string tarHeader(const std::string& name, std::size_t size, char type) {
    std::string header(512, '\0');
    std::memcpy(header.data(), name.data(), std::min<std::size_t>(name.size(), 100));
    std::string mode = octalValue(0644, 8);
    std::string owner = octalValue(0, 8);
    std::string sizeText = octalValue(size, 12);
    std::string timeText = octalValue(0, 12);
    std::memcpy(header.data() + 100, mode.data(), 8);
    std::memcpy(header.data() + 108, owner.data(), 8);
    std::memcpy(header.data() + 116, owner.data(), 8);
    std::memcpy(header.data() + 124, sizeText.data(), 12);
    std::memcpy(header.data() + 136, timeText.data(), 12);
    std::memset(header.data() + 148, ' ', 8);
    header[156] = type;
    std::memcpy(header.data() + 257, "ustar", 5);

    unsigned int checksum = 0;
    for (unsigned char ch : header) {
        checksum += ch;
    }
    std::string checksumText = octalValue(checksum, 8);
    std::memcpy(header.data() + 148, checksumText.data(), 8);
    return header;
}

std::string gzipBytes(const std::string& input) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("failed to initialize gzip compression");
    }

    std::string output;
    std::array<char, 32768> buffer{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    int code = Z_OK;
    while (code == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        code = deflate(&stream, Z_FINISH);
        output.append(buffer.data(), buffer.size() - stream.avail_out);
    }
    deflateEnd(&stream);
    if (code != Z_STREAM_END) {
        throw std::runtime_error("gzip compression failed");
    }
    return output;
}

std::optional<std::string> gzipDecompress(const std::string& input, std::size_t maxOutputBytes) {
    z_stream stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        return std::nullopt;
    }

    std::string output;
    std::array<char, 32768> buffer{};
    int code = Z_OK;
    while (code == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
        stream.avail_out = static_cast<uInt>(buffer.size());
        code = inflate(&stream, Z_NO_FLUSH);
        std::size_t produced = buffer.size() - stream.avail_out;
        if (output.size() + produced > maxOutputBytes) {
            inflateEnd(&stream);
            return std::nullopt;
        }
        output.append(buffer.data(), produced);
    }
    inflateEnd(&stream);
    if (code != Z_STREAM_END) {
        return std::nullopt;
    }
    return output;
}

bool allowedPublishPath(const fs::path& relative) {
    std::string path = relative.generic_string();
    if (path == "config.toml") return true;
    if (path == "AGENTS.md" || path == "CLAUDE.md") return true;
    if (startsWith(path, "src/")) return true;
    if (startsWith(path, "include/")) return true;
    if (startsWith(path, "docs/")) return true;
    if (path.find('/') == std::string::npos) {
        return startsWith(path, "README") || startsWith(path, "LICENSE");
    }
    return false;
}

std::vector<fs::path> collectPublishFiles() {
    std::vector<fs::path> files;
    for (const fs::path& root : {fs::path("config.toml"), fs::path("README.md"), fs::path("README"), fs::path("AGENTS.md"), fs::path("CLAUDE.md"), fs::path("LICENSE"), fs::path("LICENSE.md")}) {
        if (fs::is_regular_file(root)) {
            files.push_back(root);
        }
    }
    for (const fs::path& root : {fs::path("src"), fs::path("include"), fs::path("docs")}) {
        if (!fs::exists(root)) {
            continue;
        }
        for (const auto& entry : fs::recursive_directory_iterator(root)) {
            if (entry.is_regular_file() && !entry.is_symlink() && allowedPublishPath(entry.path())) {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string createSourceArchive() {
    std::string tar;
    std::vector<fs::path> files = collectPublishFiles();
    if (files.empty()) {
        throw std::runtime_error("no publishable files found");
    }

    for (const auto& path : files) {
        std::string content = readTextFile(path);
        std::string relative = path.generic_string();
        tar += tarHeader(relative, content.size(), '0');
        tar += content;
        tar.append((512 - (content.size() % 512)) % 512, '\0');
    }
    tar.append(1024, '\0');
    return gzipBytes(tar);
}

std::uint64_t readTarOctal(const char* data, std::size_t size) {
    std::uint64_t value = 0;
    std::size_t index = 0;
    while (index < size && (data[index] == ' ' || data[index] == '\0')) {
        ++index;
    }
    for (; index < size; ++index) {
        char ch = data[index];
        if (ch == ' ' || ch == '\0') {
            break;
        }
        if (ch < '0' || ch > '7') {
            return 0;
        }
        value = (value << 3) + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

std::string tarString(const char* data, std::size_t size) {
    std::size_t length = 0;
    while (length < size && data[length] != '\0') {
        ++length;
    }
    return std::string(data, length);
}

bool zeroTarBlock(const char* data) {
    for (std::size_t index = 0; index < 512; ++index) {
        if (data[index] != '\0') {
            return false;
        }
    }
    return true;
}

bool safeExtractPath(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos) {
        return false;
    }
    std::string segment;
    for (char ch : path) {
        if (ch != '/') {
            segment.push_back(ch);
            continue;
        }
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        segment.clear();
    }
    return !segment.empty() && segment != "." && segment != "..";
}

void extractArchive(const std::string& archive, const fs::path& destination) {
    auto validation = CloudServer::validateSourceArchive(archive, defaultMaxPackageBytes);
    if (!validation.ok) {
        throw std::runtime_error("downloaded package archive is invalid: " + validation.errorMessage);
    }

    auto tarBytes = gzipDecompress(archive, defaultMaxPackageBytes * 20);
    if (!tarBytes) {
        throw std::runtime_error("downloaded package archive could not be decompressed");
    }

    fs::remove_all(destination);
    createDirectory(destination);

    std::size_t offset = 0;
    while (offset + 512 <= tarBytes->size()) {
        const char* header = tarBytes->data() + offset;
        if (zeroTarBlock(header)) {
            return;
        }
        std::string name = tarString(header, 100);
        std::string prefix = tarString(header + 345, 155);
        std::string path = prefix.empty() ? name : prefix + "/" + name;
        std::uint64_t size = readTarOctal(header + 124, 12);
        char type = header[156];
        offset += 512;

        if (!safeExtractPath(path)) {
            throw std::runtime_error("package archive contains unsafe path: " + path);
        }

        fs::path target = destination / path;
        if (type == '5') {
            createDirectory(target);
        } else if (type == '0' || type == '\0') {
            createDirectory(target.parent_path());
            std::ofstream file(target, std::ios::binary);
            file.write(tarBytes->data() + offset, static_cast<std::streamsize>(size));
        } else {
            throw std::runtime_error("package archive contains unsupported entry type");
        }

        offset += static_cast<std::size_t>((size + 511) / 512 * 512);
    }
}

std::string manifestJson(const ProjectConfig& config, const std::string& scopedName, const std::string& version) {
    CloudServer::JsonValue::Object dependencies;
    for (const auto& [name, range] : config.dependencies) {
        dependencies[name] = range;
    }

    CloudServer::JsonValue::Object manifest;
    manifest["name"] = scopedName;
    manifest["version"] = version;
    manifest["main"] = config.mainFile;
    manifest["dependencies"] = dependencies;
    return CloudServer::JsonValue(manifest).serialize();
}

void ensureHttpSuccess(const HttpResult& result, const std::string& action) {
    if (result.status >= 200 && result.status < 300) {
        return;
    }
    std::string message = action + " failed with HTTP " + std::to_string(result.status);
    if (!result.body.empty()) {
        message += ": " + result.body;
    }
    throw std::runtime_error(message);
}

void updateDependencyInConfig(const fs::path& configPath, const std::string& scopedName, const std::string& range) {
    std::vector<std::string> lines;
    {
        std::ifstream file(configPath);
        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(line);
        }
    }

    std::string dependencyLine = "\"" + scopedName + "\" = \"" + range + "\"";
    bool inDependencies = false;
    bool sawDependencies = false;
    bool replaced = false;
    std::vector<std::string> output;

    for (std::size_t index = 0; index < lines.size(); ++index) {
        std::string trimmed = CloudServer::trimCopy(lines[index]);
        bool sectionStart = trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
        if (sectionStart && inDependencies && !replaced) {
            output.push_back(dependencyLine);
            replaced = true;
        }
        if (sectionStart) {
            inDependencies = trimmed == "[dependencies]";
            sawDependencies = sawDependencies || inDependencies;
        }

        if (inDependencies && trimmed.find('=') != std::string::npos) {
            std::string key = stripQuotes(trimmed.substr(0, trimmed.find('=')));
            if (key == scopedName) {
                output.push_back(dependencyLine);
                replaced = true;
                continue;
            }
        }
        output.push_back(lines[index]);
    }

    if (!sawDependencies) {
        output.push_back("");
        output.push_back("[dependencies]");
        output.push_back(dependencyLine);
    } else if (inDependencies && !replaced) {
        output.push_back(dependencyLine);
    }

    std::ostringstream content;
    for (const auto& line : output) {
        content << line << "\n";
    }
    writeTextFile(configPath, content.str());
}

void removeDependencyFromConfig(const fs::path& configPath, const std::string& scopedName) {
    std::ifstream file(configPath);
    if (!file.is_open()) {
        return;
    }

    bool inDependencies = false;
    std::vector<std::string> output;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = CloudServer::trimCopy(line);
        bool sectionStart = trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
        if (sectionStart) {
            inDependencies = trimmed == "[dependencies]";
        }
        if (inDependencies && trimmed.find('=') != std::string::npos) {
            std::string key = stripQuotes(trimmed.substr(0, trimmed.find('=')));
            if (key == scopedName) {
                continue;
            }
        }
        output.push_back(line);
    }

    std::ostringstream content;
    for (const auto& outputLine : output) {
        content << outputLine << "\n";
    }
    writeTextFile(configPath, content.str());
}

void publishPackage(const ParsedArgs& args) {
    ProjectConfig config = loadProjectConfig(configFilePath(args));
    std::string scopedName = scopedNameFromConfig(config, args);
    std::string publishVersion = optionValue(args, "--version", config.version);
    if (!CloudServer::SemVer::parse(publishVersion)) {
        throw std::runtime_error("publish version must be SemVer");
    }
    std::string token = tokenValue(args);
    if (token.empty()) {
        throw std::runtime_error("publishing requires --token or CLOUD_TOKEN");
    }

    PackageSpec spec = parsePackageSpec(scopedName);
    std::string manifest = manifestJson(config, scopedName, publishVersion);
    std::string archive = createSourceArchive();
    auto validation = CloudServer::validateSourceArchive(archive, defaultMaxPackageBytes);
    if (!validation.ok) {
        throw std::runtime_error("source archive is invalid: " + validation.errorMessage);
    }

    std::string url = registryUrl(args) + "/v1/packages/" +
        CloudServer::urlEncode(spec.owner) + "/" +
        CloudServer::urlEncode(spec.packageName) + "/versions/" +
        CloudServer::urlEncode(publishVersion);
    HttpResult result = httpPostMultipart(url, token, manifest, archive);
    ensureHttpSuccess(result, "publish");
    std::println("Published {}@{}", scopedName, publishVersion);
}

// Outcome of resolving a single package spec against the registry: the chosen
// version, its source-archive checksum, and the package's own (transitive)
// dependency requirements parsed from its published manifest.
struct ResolvedPackage {
    std::string version;
    std::string checksumSha256;
    std::vector<PackageSpec> dependencies;
};

ResolvedPackage resolvePackage(const ParsedArgs& args, const PackageSpec& spec) {
    std::string url = registryUrl(args) + "/v1/packages/" +
        CloudServer::urlEncode(spec.owner) + "/" +
        CloudServer::urlEncode(spec.packageName) +
        "/resolve?range=" + CloudServer::urlEncode(spec.range);
    HttpResult result = httpGet(url);
    ensureHttpSuccess(result, "resolve");

    CloudServer::JsonValue json = CloudServer::JsonValue::parse(result.body);
    auto version = json.getString("version");
    if (!version) {
        throw std::runtime_error("registry resolve response did not include a version");
    }

    ResolvedPackage resolved;
    resolved.version = *version;
    if (auto checksum = json.getString("checksumSha256")) {
        resolved.checksumSha256 = *checksum;
    }

    // Parse transitive dependencies from the manifest's "dependencies" object,
    // shaped as { "@owner/package": "range", ... }.
    if (auto manifest = json.getValue("manifest"); manifest && manifest->isObject()) {
        if (auto deps = manifest->getValue("dependencies"); deps && deps->isObject()) {
            for (const auto& [name, range] : deps->asObject()) {
                if (!range.isString()) {
                    continue;
                }
                try {
                    resolved.dependencies.push_back(parsePackageSpec(name, range.asString()));
                } catch (const std::exception&) {
                    // Skip malformed dependency entries rather than aborting the
                    // whole resolution; surfaced later if the build needs them.
                }
            }
        }
    }
    return resolved;
}

// Downloads a specific package version's source archive, verifies its checksum
// (when known), and extracts it into the per-version libs directory.
void downloadAndInstall(const ParsedArgs& args, const PackageSpec& spec,
                        const std::string& version, const std::string& expectedChecksum) {
    std::string url = registryUrl(args) + "/v1/packages/" +
        CloudServer::urlEncode(spec.owner) + "/" +
        CloudServer::urlEncode(spec.packageName) + "/versions/" +
        CloudServer::urlEncode(version) + "/source";
    HttpResult result = httpGet(url);
    ensureHttpSuccess(result, "download");

    if (!expectedChecksum.empty()) {
        std::string actual = CloudServer::sha256Hex(result.body);
        if (!CloudServer::constantTimeEquals(actual, expectedChecksum)) {
            throw std::runtime_error(
                "checksum mismatch for " + spec.scopedName + "@" + version +
                " (expected " + expectedChecksum + ", got " + actual + ")");
        }
    }

    fs::path installDir = fs::path(".cloud/libs") / spec.owner / spec.packageName / version;
    extractArchive(result.body, installDir);
}

// Resolves the full transitive dependency graph rooted at `roots`, installs
// every package, and records the result in the lockfile. Conflict policy
// (milestone): when a package is reached via multiple ranges, the highest
// resolved SemVer wins. Returns the resolved entries keyed by scoped name.
std::map<std::string, LockEntry> resolveAndInstallGraph(
    const ParsedArgs& args,
    const std::vector<PackageSpec>& roots,
    const std::set<std::string>& directNames
) {
    std::map<std::string, LockEntry> resolved; // scopedName -> chosen entry
    std::vector<PackageSpec> worklist = roots;

    while (!worklist.empty()) {
        PackageSpec spec = worklist.back();
        worklist.pop_back();

        ResolvedPackage info = resolvePackage(args, spec);

        auto existing = resolved.find(spec.scopedName);
        if (existing != resolved.end()) {
            // Already chosen; keep the higher version. If the new resolution is
            // not greater, skip re-installing and re-walking its deps.
            auto chosen = CloudServer::SemVer::parse(existing->second.version);
            auto candidate = CloudServer::SemVer::parse(info.version);
            if (!candidate || (chosen && chosen->compare(*candidate) >= 0)) {
                continue;
            }
        }

        LockEntry entry;
        entry.scopedName = spec.scopedName;
        entry.owner = spec.owner;
        entry.packageName = spec.packageName;
        entry.version = info.version;
        entry.range = spec.range;
        entry.checksumSha256 = info.checksumSha256;
        entry.direct = directNames.count(spec.scopedName) > 0;
        resolved[spec.scopedName] = entry;

        downloadAndInstall(args, spec, info.version, info.checksumSha256);

        // Enqueue transitive dependencies.
        for (const auto& dep : info.dependencies) {
            worklist.push_back(dep);
        }
    }

    return resolved;
}

void installPackage(const ParsedArgs& args, const PackageSpec& spec, bool updateConfig) {
    std::set<std::string> directNames{spec.scopedName};
    auto resolved = resolveAndInstallGraph(args, {spec}, directNames);

    // Merge into the existing lockfile (preserve unrelated entries).
    for (const auto& [name, entry] : resolved) {
        upsertLockEntry(entry);
    }
    syncInstalledModules(args.verbose);

    if (updateConfig) {
        updateDependencyInConfig(configFilePath(args), spec.scopedName, spec.range);
    }

    const LockEntry& root = resolved.at(spec.scopedName);
    fs::path installDir = fs::path(".cloud/libs") / spec.owner / spec.packageName / root.version;
    std::println("Installed {}@{} -> {}", spec.scopedName, root.version, installDir.string());
    std::size_t transitive = resolved.size() - 1;
    if (transitive > 0) {
        std::println("  (+{} transitive {})", transitive, transitive == 1 ? "dependency" : "dependencies");
    }
}

void installCommand(const ParsedArgs& args) {
    if (args.positional.empty()) {
        throw std::runtime_error("install requires a package name");
    }
    (void)loadProjectConfig(configFilePath(args));
    PackageSpec spec = parsePackageSpec(args.positional[0], optionValue(args, "--version"));
    installPackage(args, spec, true);
}

void updateCommand(const ParsedArgs& args) {
    ProjectConfig config = loadProjectConfig(configFilePath(args));
    if (config.dependencies.empty()) {
        std::println("No dependencies configured.");
        return;
    }

    // Resolve the entire graph from the direct dependencies in one pass so
    // shared transitive deps are deduplicated and the lockfile is coherent.
    std::vector<PackageSpec> roots;
    std::set<std::string> directNames;
    for (const auto& [name, range] : config.dependencies) {
        PackageSpec spec = parsePackageSpec(name, range);
        roots.push_back(spec);
        directNames.insert(spec.scopedName);
    }

    auto resolved = resolveAndInstallGraph(args, roots, directNames);

    // Rewrite the lockfile to exactly the resolved graph (drops stale entries).
    std::vector<LockEntry> entries;
    entries.reserve(resolved.size());
    for (const auto& [name, entry] : resolved) {
        entries.push_back(entry);
    }
    writeLockFile(entries);
    syncInstalledModules(args.verbose);

    std::println("Resolved {} package{} ({} direct).",
                 resolved.size(), resolved.size() == 1 ? "" : "s", directNames.size());
}

void uninstallCommand(const ParsedArgs& args) {
    if (args.positional.empty()) {
        throw std::runtime_error("uninstall requires a package name");
    }
    PackageSpec spec = parsePackageSpec(args.positional[0]);
    fs::remove_all(fs::path(".cloud/libs") / spec.owner / spec.packageName);
    removeLockEntry(spec.scopedName);
    removeDependencyFromConfig(configFilePath(args), spec.scopedName);
    syncInstalledModules(args.verbose);
    std::println("Uninstalled {}", spec.scopedName);
}

void listCommand() {
    std::vector<LockEntry> entries = readLockFile();
    if (entries.empty()) {
        std::println("No packages installed.");
        return;
    }
    for (const auto& entry : entries) {
        std::println("{} {} ({}{})", entry.scopedName, entry.version,
                     entry.direct ? "direct" : "transitive",
                     entry.range == "*" || entry.range.empty() ? "" : ", " + entry.range);
    }
}

void yankCommand(const ParsedArgs& args) {
    if (args.positional.empty()) {
        throw std::runtime_error("yank requires a package name");
    }
    PackageSpec spec = parsePackageSpec(args.positional[0]);
    std::string version = optionValue(args, "--version");
    if (version.empty()) {
        auto locked = findLockEntry(spec.scopedName);
        if (locked) {
            version = locked->version;
        }
    }
    if (version.empty()) {
        throw std::runtime_error("yank requires --version when the package is not installed");
    }

    std::string token = tokenValue(args);
    if (token.empty()) {
        throw std::runtime_error("yanking requires --token or CLOUD_TOKEN");
    }

    std::string url = registryUrl(args) + "/v1/packages/" +
        CloudServer::urlEncode(spec.owner) + "/" +
        CloudServer::urlEncode(spec.packageName) + "/versions/" +
        CloudServer::urlEncode(version) + "/yank";
    HttpResult result = httpPostJson(url, "{}", {"Authorization: Bearer " + token});
    ensureHttpSuccess(result, "yank");
    std::println("Yanked {}@{}", spec.scopedName, version);
}

void tokenRevoke(const ParsedArgs& args) {
    // Revoke the caller's own token, or (with --prefix) revoke by prefix using
    // an admin/bootstrap token.
    std::string token = tokenValue(args);
    std::string bootstrap = optionValue(args, "--bootstrap-token", getEnv("CLOUD_BOOTSTRAP_TOKEN"));
    std::string prefix = optionValue(args, "--prefix");

    std::string authToken = !token.empty() ? token : bootstrap;
    if (authToken.empty()) {
        throw std::runtime_error("token revoke requires --token/CLOUD_TOKEN, or --bootstrap-token with --prefix");
    }

    std::string body;
    if (!prefix.empty()) {
        CloudServer::JsonValue::Object obj;
        obj["tokenPrefix"] = prefix;
        body = CloudServer::JsonValue(obj).serialize();
    }

    HttpResult result = httpDelete(
        registryUrl(args) + "/v1/tokens",
        body,
        {"Authorization: Bearer " + authToken}
    );
    ensureHttpSuccess(result, "token revoke");

    CloudServer::JsonValue json = CloudServer::JsonValue::parse(result.body);
    std::println("Revoked {} token(s).", json.getValue("revoked").value_or(CloudServer::JsonValue(0)).asInt());
}

void tokenCommand(const ParsedArgs& args) {
    // `token revoke ...` is a subcommand; otherwise create a token.
    if (!args.positional.empty() && args.positional[0] == "revoke") {
        tokenRevoke(args);
        return;
    }

    if (args.positional.empty()) {
        throw std::runtime_error("token requires an account name");
    }
    std::string bootstrapToken = optionValue(args, "--bootstrap-token", getEnv("CLOUD_BOOTSTRAP_TOKEN"));
    if (bootstrapToken.empty()) {
        throw std::runtime_error("token creation requires --bootstrap-token or CLOUD_BOOTSTRAP_TOKEN");
    }

    std::string accountName = args.positional[0];
    std::string scope = optionValue(args, "--scope", "publish");
    CloudServer::JsonValue::Object body;
    body["accountName"] = accountName;
    body["scope"] = scope;

    // Optional expiry: --expires-seconds takes precedence over --expires-days.
    std::string expiresSeconds = optionValue(args, "--expires-seconds");
    std::string expiresDays = optionValue(args, "--expires-days");
    if (!expiresSeconds.empty()) {
        body["expiresInSeconds"] = static_cast<std::int64_t>(std::stoll(expiresSeconds));
    } else if (!expiresDays.empty()) {
        body["expiresInDays"] = static_cast<std::int64_t>(std::stoll(expiresDays));
    }

    HttpResult result = httpPostJson(
        registryUrl(args) + "/v1/tokens",
        CloudServer::JsonValue(body).serialize(),
        {"Authorization: Bearer " + bootstrapToken}
    );
    ensureHttpSuccess(result, "token creation");

    CloudServer::JsonValue json = CloudServer::JsonValue::parse(result.body);
    auto token = json.getString("token");
    if (!token) {
        throw std::runtime_error("registry token response did not include a token");
    }
    std::println("{}", *token);
}

} // namespace

int main(int argc, char** argv) {
    try {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        ParsedArgs args = parseArgs(argc, argv);
        if (args.command.empty()) {
            printHelp();
            return 1;
        }

        if (args.command == "init") {
            initProject(args.positional.empty() ? "my-insty-project" : args.positional[0]);
        } else if (args.command == "build") {
            buildProject(args);
        } else if (args.command == "run") {
            buildProject(args);
            runProject(args);
        } else if (args.command == "clean") {
            cleanProject(args);
        } else if (args.command == "publish") {
            publishPackage(args);
        } else if (args.command == "install") {
            installCommand(args);
        } else if (args.command == "uninstall") {
            uninstallCommand(args);
        } else if (args.command == "update") {
            updateCommand(args);
        } else if (args.command == "list") {
            listCommand();
        } else if (args.command == "yank") {
            yankCommand(args);
        } else if (args.command == "token") {
            tokenCommand(args);
        } else if (args.command == "test") {
            testProject(args);
        } else if (args.command == "version" || args.command == "-v" || args.command == "--version") {
            printVersion();
        } else if (args.command == "help" || args.command == "-h" || args.command == "--help") {
            printHelp();
        } else {
            throw std::runtime_error("unknown command: " + args.command);
        }

        curl_global_cleanup();
        return 0;
    } catch (const std::exception& error) {
        curl_global_cleanup();
        std::println(stderr, "Error: {}", error.what());
        return 1;
    }
}
