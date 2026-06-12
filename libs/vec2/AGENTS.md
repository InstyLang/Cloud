# AGENTS.md

Project: vec2

Agent-facing guide for working in this Insty project.

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

