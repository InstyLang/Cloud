# Building Cloud

`cloud` is the Insty package manager / build-tool CLI. `cloud-server` is the
registry server (Unix/Docker only; needs libpq + libevent).

## Windows (cloud CLI)

The CLI needs **libcurl** and **zlib**; SHA-256 / HMAC / secure-random come from
the built-in Windows CNG (`bcrypt`), so **no OpenSSL is required** on Windows.

1. Install the dependencies with vcpkg:

   ```pwsh
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install curl zlib --triplet x64-windows
   ```

2. Configure + build with the vcpkg toolchain (clang + ninja shown; any
   generator works):

   ```pwsh
   cmake -S . -B build -G Ninja `
     -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang++.exe" `
     -DCMAKE_MAKE_PROGRAM="C:/Program Files/LLVM/bin/ninja.exe" `
     -DCMAKE_TOOLCHAIN_FILE="C:/vcpkg/scripts/buildsystems/vcpkg.cmake" `
     -DVCPKG_TARGET_TRIPLET=x64-windows
   cmake --build build --target cloud
   ```

   vcpkg copies `libcurl` / `zlib` DLLs next to `build\cloud.exe` automatically.

3. Point `cloud` at the Insty compiler and use it:

   ```pwsh
   $env:INSTY_COMPILER = "C:\path\to\Compiler\build\insty.exe"
   build\cloud.exe init myapp          # interactive wizard (kind + target)
   # or: build\cloud.exe init myapp -y  # accept all defaults, no prompts
   cd myapp
   ..\build\cloud.exe build
   ..\build\cloud.exe run
   ```

   `cloud init` runs an interactive wizard (project name, kind:
   executable/library/freestanding/uefi, and target). Use `-y`/`--yes` for the
   defaults (executable, host target), or drive it non-interactively with
   `--kind` and `--target`.

On Windows, `cloud build`/`run`/`test` default to the host target
(`x86_64_windows`); the compiler picks the matching linker automatically.
Override per project in `config.toml`:

```toml
[compiler]
target = "x86_64_linux"   # or x86_64_instantos, etc.
linker = "ld.lld"
```

## Unix (cloud CLI + cloud-server)

```bash
sudo apt-get install build-essential cmake pkg-config \
  libcurl4-openssl-dev libssl-dev zlib1g-dev libpq-dev libevent-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build            # builds cloud, cloud-server, tests
```

The registry server is also available via the `Dockerfile` / `docker-compose.yml`.
