# AGENTS.md

Project: mathx

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
- `AGENTS.md` - Agent instructions for this project.

## Common Commands

```bash
cloud build
cloud run
cloud clean
cloud test
cloud install @owner/package
cloud update
cloud publish --name @owner/package --version 0.1.0
cloud upgrade
```

`cloud update` installs this project's dependencies; `cloud upgrade` updates
the toolchain itself (`insty`, `cloud`, `insty-lsp` and the standard library).
Use `cloud upgrade --check` to only report what is available.

Useful environment variables:

- `INSTY_COMPILER` - Override compiler path used by Cloud.
- `CLOUD_CONFIG` - Override default `config.toml` path.
- `CLOUD_REGISTRY_URL` - Override package registry URL.
- `CLOUD_TOKEN` - Registry bearer token.
- `CLOUD_NO_UPDATE_CHECK` - Disable the daily toolchain update check.

## Compiler Commands

```bash
insty src/main.ins -o app
insty -c src/main.ins --objects-dir .cloud/objects
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
- `--panic abort|handler` and `--panic-handler <symbol>` - Panic strategy: `abort` traps; `handler` calls the given noreturn symbol. Overrides target spec `panic`/`panic_handler` keys.

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

## Insty Language Reference

This file deliberately does **not** restate the language. The reference lives with
the compiler, in the `AGENTS.md` beside the `insty` toolchain, and that copy is the
one kept current and checked against the compiler -- its examples are compiled as
part of the compiler's own test pass. A per-project copy would drift instead.

Look there for: types and literals, structs/classes/enums, sum types and `switch`,
generics, slices, `for`-in, `.insize` / `.inalign`, the builtin list, the standard
library layout, compile-time `#if` and `@targetIs`, the unsafe boundary, volatile
and atomics, inline `asm`, freestanding/OS development, custom target specs, and
the WebAssembly target.

A minimal module, so this file is not entirely abstract:

```ecx
module main

fun add(i32 a, i32 b) -> i32 {
    return a + b
}

fun main() -> i32 {
    return add(20, 22)
}
```

## Agent Workflow

- Prefer small, targeted changes.
- Run `cloud build` (or a direct `insty` command) after modifying Insty code.
- Use `--emit-tokens` or `--emit-ast` to inspect the front end; the backend emits
  machine code directly, so there is no IR to dump.
- Do not assume hosted APIs in freestanding projects.
- Keep unsafe operations inside explicit `unsafe { ... }` blocks.
- For kernels, prefer explicit `--target <spec.toml> --freestanding --entry <symbol>`.
- Keep target-specific boot/linker details in target specs and linker scripts.

