# AGENTS.md

Package: @ecx/glad

OpenGL 3.3 core bindings: GL types (GLenum, GLint, GLfloat, ...) and ~2000
constants (GL_TRIANGLES, GL_COLOR_BUFFER_BIT, GL_TEXTURE0, shader enums, ...)
generated from the glad-produced `gl.h` by insbind, plus the glad loader
library. Pair with @ecx/glfw for windows and GL contexts.

## Layout

- `config.toml` - Cloud package configuration (`@ecx/glad`).
- `src/gl.ins` - the bindings module (`import ecx::gl`). Generated; do not
  hand-edit.
- `bin/<target>/` - vendored binaries. Currently
  `bin/x86_64_windows/gl.dll` (glad's `gl.c`, MSVC `/MT`).

## How GL entry points work here (read this)

glad's C API is ~3000 function-pointer GLOBALS (`glad_glClearColor`, ...).
Insty module-level and extern globals don't cross modules, so those are not
bound (you will see one aggregated warning when regenerating). Instead, GL
entry points are resolved at runtime and invoked through `fnCall`:

```ecx
extern fun [dll("opengl32.dll")] wglGetProcAddress(text name) -> u64
extern fun [dll("kernel32.dll")] GetProcAddress(u64 module, text name) -> u64
extern fun [dll("kernel32.dll")] GetModuleHandleA(text name) -> u64

// Resolve: GetProcAddress(opengl32, name) for GL <= 1.1, wglGetProcAddress
// for anything newer (a current GL context is required first).
unsafe {
    u64 cc = wglGetProcAddress("glClearColor")
    f32 r = 0.1
    f32 g = 0.35
    f32 b = 0.6
    f32 a = 1.0
    fnCall<void>(cc, r, g, b, a)
    u64 cl = wglGetProcAddress("glClear")
    fnCall<void>(cl, gl.gl_color_buffer_bit())
}
```

`gladLoadGL`/`gladLoadGLUserPtr` ARE bound for completeness (they fill the
C globals, which are useless from Insty); skip them. A future accessor-shim
DLL could expose the globals by index if profiling ever shows fnCall
overhead mattering (it does not).

## Conventions (from insbind, mirroring windows::*/unix::*)

- GL scalar types map 1:1 (GLenum u32, GLint i32, GLfloat f32, GLchar i8,
  GLsizeiptr i64, GLuint64 u64, ...).
- Constants are zero-argument functions: `gl.gl_triangles()`,
  `gl.gl_color_buffer_bit()`, `gl.gl_texture0()`, `gl.gl_compile_status()`.

## Regenerating the bindings

glad is a generator, not a downloaded library: with Python + jinja2,

```bash
python -m glad --api gl:core=3.3 --out-path gen     # from the glad repo
insbind bind gen/include/glad/gl.h -I gen/include -I <insbind>/include \
    --module gl --dll gl.dll --abi-check > src/gl.ins
```

Rebuild `gl.dll` from `gen/src/gl.c` with `GLAD_API_CALL_EXPORT` and
`GLAD_API_CALL_EXPORT_BUILD` defined. Bump the GL version/profile with the
glad invocation and match `[project].version`.
