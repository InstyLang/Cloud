# AGENTS.md

Package: @ecx/sqlite3

FFI bindings to SQLite 3.50.4, plus the vendored native library. A complete
SQL database engine (in-memory or file databases) with no C compiler or
system SQLite needed.

## Layout

- `config.toml` - Cloud package configuration (`@ecx/sqlite3`).
- `src/sqlite3.ins` - the bindings module (`import ecx::sqlite3`). Generated
  by insbind from the sqlite-amalgamation `sqlite3.h`; do not hand-edit.
- `src/db.ins` - the idiomatic RAII wrapper (`import ecx::sqlite3::db`):
  `db.Database` (self-closing), `db.Statement` (self-finalizing),
  `db.prepare`. Hand-maintained; unsafe blocks live inside it.
- `bin/<target>/` - vendored binaries. Currently
  `bin/x86_64_windows/sqlite3.dll` (MSVC `/MT`; no VC++ runtime dependency).

Prefer the wrapper for application code; drop to the raw module for anything
it does not cover.

## Conventions (from insbind, mirroring windows::*/unix::*)

- Opaque handles are `u64`: `sqlite3*`, `sqlite3_stmt*`, `sqlite3_context*`,
  `sqlite3_blob*`, `sqlite3_mutex*`, ... Output parameters like
  `sqlite3** ppDb` become `u64*`.
- C strings are `text` (const) or `u8*` (mutable). `sqlite3_column_text`
  returns `u8*` (UTF-8; treat as read-only).
- Constants are zero-argument functions: `sqlite3.sqlite_ok()`,
  `sqlite3.sqlite_row()`, `sqlite3.sqlite_done()`,
  `sqlite3.sqlite_open_readwrite()`, ...
- Callbacks (`sqlite3_exec`'s row callback, collation/busy/progress
  handlers, hooks) are `u64`: pass `cast<u64>(&my_fn)` with a C-compatible
  signature; the Insty function is invoked from C (see the consumer demo).
- Variadic functions are NOT bound by design (C shims territory):
  `sqlite3_mprintf`, `sqlite3_snprintf`, `sqlite3_config`,
  `sqlite3_db_config`. Everything else (open/exec/prepare/bind/step/
  column/finalize/close, blob, backup, mutex, vfs) is bound.
- Extern globals (`sqlite3_version`, `sqlite3_temp_directory`,
  `sqlite3_data_directory`) are skipped by design (shim territory); use
  `sqlite3_libversion()` for the version and PRAGMAs/defaults for the rest.

## Regenerating the bindings

```bash
insbind bind sqlite3.h -I . -I <insbind>/include \
    --module sqlite3 --dll sqlite3.dll --abi-check > src/sqlite3.ins
```

Regenerate when bumping the vendored SQLite version (and match
`[project].version`).
