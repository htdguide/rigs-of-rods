# Rigs of Rods — WebAssembly (emscripten) build

Work-in-progress port of RoR to wasm/WebGL2. See [`../WASM_PORTING_PLAN.md`](../WASM_PORTING_PLAN.md)
for the full plan and phase tracking.

> Status: **Phase 0** (toolchain + scaffolding). Does not build/boot yet.

## Engine fork (Path A)

RoR runs on Ogre3D 1.11.6. The wasm port uses a fork of the engine with its
existing Emscripten EGL / GLES2 backend enabled:

- Engine: `github.com/htdguide/ogre` @ `wasm-port` (forked from `OGRECave/ogre` v1.11.6)
- Game:   `github.com/htdguide/rigs-of-rods` @ `wasm-port`

## Prerequisites

- [emsdk](https://emscripten.org/docs/getting_started/downloads.html), activated
  (`source ./emsdk_env.sh`).
- `cmake` >= 3.16, `ninja`.
- A **wasm sysroot** holding the dependencies built for emscripten, passed to
  RoR via `CMAKE_PREFIX_PATH` (mirrors the Ship of Harkinian `wasm-sysroot`).

## Dependency stripping (Phase 1)

RoR's optional deps are `cmake_dependent_option`s that auto-disable when the
dependency is absent (`ROR_USE_SOCKETW`, `ROR_USE_CURL`, `ROR_USE_CAELUM`,
`ROR_USE_PAGED`, `ROR_USE_DISCORD_RPC`, `ROR_USE_ANGELSCRIPT`). So a minimal
sysroot — Ogre + core only — strips them automatically, no code edits needed.

Browser-incompatible pieces handled separately:
- **Cg** shaders → rewrite as GLSL ES (terrain PSSM, hydrax water).
- **OIS** input → SDL2 / HTML5 input.
- **SocketW** raw-TCP multiplayer → disabled (WebSocket proxy later).

## Build

```bash
# 1. build the wasm sysroot (Ogre fork + all deps) — see setup-sysroot.sh
./wasm/setup-sysroot.sh

# 2. configure + build RoR for wasm
emcmake cmake -S . -B build-emscripten -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/wasm/sysroot" \
  -DCMAKE_FIND_ROOT_PATH="$PWD/wasm/sysroot" \
  -DOGRE_DIR="$PWD/wasm/sysroot/lib/OGRE/cmake" \
  -DROR_USE_PCH=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5

# 3. stage the RTShaderSystem GLSL library into the resources (GLES2/WebGL2 has
#    no fixed-function pipeline, so RTSS auto-generates shaders from these).
cp -r wasm/ogre/Samples/Media/RTShaderLib/GLSL build-emscripten/bin/resources/RTShaderLib

# 4. build (Release is required: -O0 + -fexceptions overflows the wasm
#    per-function local limit). The 100MB+ resources are preloaded into MEMFS.
cmake --build build-emscripten -j4
```

Serve `build-emscripten/bin/` with COOP/COEP headers and open `RoR.html`.

## Serve

The build uses pthreads → needs `SharedArrayBuffer` → the page must be
[cross-origin isolated](https://web.dev/articles/coop-coep). Serve with:

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```
