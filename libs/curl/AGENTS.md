# AGENTS.md

Package: @ecx/curl

FFI bindings to libcurl 8.22.0, plus the vendored native library and a small
shim DLL for the variadic interface. HTTP/HTTPS (and 20+ other protocols)
with TLS via Windows-native Schannel -- no C compiler, no OpenSSL, nothing
installed.

## Layout

- `config.toml` - Cloud package configuration (`@ecx/curl`).
- `src/curl.ins` - the generated bindings (`import ecx::curl`). insbind
  output from `curl/curl.h`; do not hand-edit.
- `src/curlshim.ins` - hand-maintained bindings for the shim DLL
  (`import ecx::curlshim` resolves as the submodule `ecx::curl::curlshim`).
- `bin/<target>/` - vendored binaries. Currently for x86_64_windows:
  `libcurl.dll` (MSVC `/MT`, Schannel TLS, static zlib) and
  `insty_curl_shim.dll` (the variadic shim; links libcurl.dll).

## The variadic shim (important)

`curl_easy_setopt`, `curl_easy_getinfo`, `curl_multi_setopt` and
`curl_formadd` are variadic: uncallable from Insty and skipped in the
generated bindings. Configuration goes through the shim (see
`src/curlshim.ins` for the full table):

```ecx
import ecx::curl
import ecx::curl::curlshim

u64 h = curl.curl_easy_init()
i32 rc = 0
unsafe {
    rc = curlshim.insty_setopt_str(h, curl.curlopt_url(), "https://example.com")
    rc = curlshim.insty_setopt_ptr(h, curl.curlopt_writefunction(),
                                   cast<void*>(&on_write))
    rc = curl.curl_easy_perform(h)
    i32 code = 0
    rc = curlshim.insty_getinfo_long(h, curl.curlinfo_response_code(), &code)
}
```

## Conventions (from insbind, mirroring windows::*/unix::*)

- `CURL*`/`CURLM*`/`CURLSH*` are `void*` (curl 8.x typedefs them as `void`).
  `curl_slist*` is a real struct. Callbacks (`curl_write_callback`,
  `curl_read_callback`, progress/seek/debug/socket callbacks) are `u64`;
  pass `cast<u64>(&fn)` or `cast<void*>(&fn)` to `_ptr` shim slots.
- C strings are `text` (const) or `u8*` (mutable).
- Constants are zero-argument functions: `curl.curle_ok()`,
  `curl.curlopt_url()`, `curl.curlinfo_response_code()`, ...
- The write/read callback ABI: `fun on_write(u8* ptr, u64 size, u64 nmemb,
  void* ud) -> u64`, return `size * nmemb` (see the consumer demo).

## Regenerating the bindings

```bash
insbind bind curl/curl.h -I include -I <insbind>/include \
    --module curl --dll libcurl.dll --abi-check > src/curl.ins
```

`curlshim.ins` is maintained by hand (7 functions; keep it in sync with
`insty_shim.c` when adding slots). Regenerate `curl.ins` when bumping the
vendored curl version (and match `[project].version`).
