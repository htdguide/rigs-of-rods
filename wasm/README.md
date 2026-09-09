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

Serve `build-emscripten/bin/` and open `RoR.html`.

## Serve

The build uses pthreads → needs `SharedArrayBuffer` → the page must be
[cross-origin isolated](https://web.dev/articles/coop-coep):

```
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Where the host can send those headers, it should. Where it can't — GitHub Pages,
any plain static host — `coi-serviceworker.js` installs them from a service
worker instead. `wasm/shell.html` loads it, and CMake stages it next to
`RoR.html`, so a dumb static server is enough:

```bash
python3 -m http.server -d build-emscripten/bin 8080
```

Caveats of the service-worker route: it costs one extra page reload on a cold
visit, it needs a secure context (`https://`, or `localhost`), and it cannot
work where service workers are unavailable (private/incognito windows in some
browsers). The shell detects `crossOriginIsolated === false` and says so rather
than failing with a bare `SharedArrayBuffer is not defined`.

## GitHub Pages

`.github/workflows/deploy-pages.yml` builds the port and publishes it on every
push to `wasm-port` (plus manual `workflow_dispatch`). Enable it once under
**Settings → Pages → Source: GitHub Actions**.

How it works around Pages' limits:

| Limit | Value | Handling |
| --- | --- | --- |
| Custom response headers | not supported | `coi-serviceworker.js` |
| Max file size in git | 100 MB | `RoR.data` is 127 MB, over the limit, so nothing is committed — the workflow uploads a Pages *artifact* instead, which has no per-file cap |
| Published site size | 1 GB | payload is ~150 MB |
| Bandwidth | 100 GB/month (soft) | ~650 cold loads/month; browser cache covers repeat visits |

The sysroot is cached against `setup-sysroot.sh` plus the resolved commit of
each engine fork (`htdguide/ogre@wasm-port`, `mygui`, `OIS`), so pushing to a
fork invalidates it and the next run rebuilds. A cold run builds everything and
takes hours; a warm one only rebuilds RoR.

`content/nhelens.zip` and `content/ChevyS1023.zip` are gitignored, so CI has no
copy. To include them, attach them to a release tagged `web-content` — the
workflow downloads that release when it exists and skips it otherwise.

### Payload

~150 MB cold: `RoR.data` 127 MB (already-compressed zips, gzip buys nothing) +
`RoR.wasm` 23 MB (~6 MB gzipped by Pages) + `RoR.js` 0.5 MB. Biggest items
inside `RoR.data`: `sounds.zip` 40 MB, `textures.zip` 17 MB, `wallpapers.zip`
16 MB. Fetching content packs on demand instead of preloading
them (see the TODO in `source/main/CMakeLists.txt`) is the main lever left.

Note also `-sINITIAL_MEMORY=1024MB`: every tab reserves a 1 GB `SharedArrayBuffer`
up front, which rules out most phones and low-memory machines regardless of host.
