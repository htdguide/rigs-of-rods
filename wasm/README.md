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


## Online repository (installing mods)

Browsing the repository works out of the box: `v2.api.rigsofrods.org` sends
`Access-Control-Allow-Origin: *`, so the browser can read it directly.

**Downloading** does not, and the reason is not what an earlier commit message
claimed. `forum.rigsofrods.org` serves the file correctly — a plain request for
`/resources/1401/download?file=31555` returns HTTP 200 and 11,089,634 valid ZIP
bytes, from nginx, with no bot challenge. What it does not send is an
`Access-Control-Allow-Origin` header, so the *browser* discards the response
before the game sees it. `emscripten_fetch` reports that as status 0 with no
detail, which is why it used to look like nothing happened at all.

So the web build needs a CORS proxy, named by the `remote_cors_proxy` cvar. The
web build ships with a working default (`https://cors.htdguide.com/?url=`, an
allowlisted proxy restricted to the two Rigs of Rods hosts), so the repository
installs mods out of the box.

There is no settings UI for the cvar and RoR.cfg lives in MEMFS (wiped every
reload), so on the web it is also readable from the page URL. Precedence is
**page URL > RoR.cfg > built-in default**:

```
https://<host>/?cors_proxy=https://my-proxy.example/?url=   # use another proxy
https://<host>/?cors_proxy=                                 # disable (present but empty)
https://<host>/                                             # built-in default
```

`WasmProxiedUrl` accepts three proxy shapes. A download URL carries its own
`?file=NNNN` query, so it has to be percent-encoded for the query-style ones:

| proxy string | result |
| --- | --- |
| `https://host/{url}` | target percent-encoded into the placeholder |
| `https://host/?url=` | trailing `=` → target percent-encoded and appended |
| `https://host/` | prefix style → target appended verbatim |

### Free public proxies do not work

Twelve were tested against a real 11 MB mod download (codetabs, allorigins,
corsproxy.io, cors.lol, corsfix, isomorphic-git, thingproxy, whateverorigin,
cors.eu.org, cors-anywhere, test.cors.workers.dev). **All twelve failed**, and
mostly not because of the size — codetabs and allorigins return Cloudflare's
`error code: 522` (their own origins are down) even for a one-line payload,
cors.lol and cors.eu.org rate-limit, corsfix and cors-anywhere require
registration, thingproxy does not resolve. Do not ship one as a default; it will
fail silently for every visitor.

### Use your own

Two ready-made options, both allowlisted to `forum.rigsofrods.org` and
`v2.api.rigsofrods.org` so neither is an open relay.

**On a VPS** — any small HTTPS service that: accepts `?url=<encoded>` and the
prefix form; answers `OPTIONS` with 204; sends `Access-Control-Allow-Origin: *`,
`Access-Control-Allow-Methods`, `Access-Control-Allow-Headers`,
`Access-Control-Expose-Headers` and `Cross-Origin-Resource-Policy: cross-origin`
on every reply; streams the body rather than buffering (mod zips run 10-100 MB);
and rejects any other host with 403. HTTPS is mandatory — the game page is
HTTPS, so an http:// proxy is blocked as mixed content. `Cross-Origin-Resource-Policy`
is needed because the page is cross-origin isolated.

**On Cloudflare** — `wasm/cors-proxy-worker.js` is a ready-to-deploy Worker (free tier is
100k requests/day, which is far more than this needs). It is restricted to
`forum.rigsofrods.org` and `v2.api.rigsofrods.org`, so it is not an open relay
that could be abused through your account.

```bash
npm i -g wrangler
wrangler deploy wasm/cors-proxy-worker.js
# then open:  https://<host>/?cors_proxy=https://<worker>.workers.dev/?url=
```

### Installing without any of this

The main menu's **Install mod (.zip)** button needs no proxy at all — the
visitor's own browser downloads the zip, and the picker hands the bytes straight
to MEMFS. Note the known limitation from `13d91a7`: installs land in MEMFS and
do not survive a reload (IDBFS persistence is still a follow-up).
