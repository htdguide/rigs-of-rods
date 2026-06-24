# Rigs of Rods → WebAssembly (emscripten) porting plan

Status: research / planning. No code changes yet.

This plan reuses the playbook from the Ship of Harkinian (SoH) emscripten port:
fork the engine, strip to a bootable minimum, get a blank canvas rendering,
then add subsystems back one at a time. RoR is a much bigger lift than SoH
because the heavy lifting is the **third-party dependency stack**, not RoR's
own ~150k LOC.

---

## 1. Why this is harder than the SoH port

| | Ship of Harkinian | Rigs of Rods |
|---|---|---|
| Engine | thin custom (libultraship + Fast3D), we control it | Ogre3D 1.11 — a large third-party C++ engine |
| Rendering | one small abstraction → retarget to WebGL2 directly | all rendering goes through Ogre `RenderSystem` plugins (GL/GL3Plus/D3D) |
| Deps | few | Ogre, MyGUI, OIS, OpenAL, AngelScript, libcurl, OpenSSL, SocketW, Caelum, PagedGeometry, fmt, rapidjson |
| Language | mostly C | heavy modern C++ |
| Main loop | blocking init + render loop | blocking `while(state != SHUTDOWN){ renderOneFrame() }` (main.cpp:328/2194) |

The SoH-transferable lessons still hold (pthreads-everywhere, fixed heap,
ASYNCIFY, COOP/COEP serving, preload archives into MEMFS, bundle from
user-supplied content). The new cost is **building every native dependency for
wasm** and **replacing the ones browsers can't support**.

## 2. Dependency triage (the real work)

| Dep | wasm path | Notes |
|---|---|---|
| **Ogre3D 1.11 (fork)** | Port GLES2/GL3Plus RS to emscripten, **or bump to modern OGRE 14.x** which ships official Emscripten support | Biggest single task. Recommend forking like SoH did with libultraship. |
| **Cg / Plugin_CgProgramManager** | **Drop.** No wasm, dead toolkit | Used by terrain PSSM material generator + hydrax water → must rewrite those shaders as GLSL ES |
| **OIS** (input) | **Replace** with SDL2 (emscripten built-in) or HTML5 input | OIS has no browser backend |
| **OpenAL** | emscripten `-lopenal` → Web Audio | Works out of the box |
| **MyGUI** | Compiles if Ogre compiles (Ogre-based GUI) | |
| **AngelScript** | Portable interpreter, compiles to wasm | No JIT needed; keep optional at first |
| **libcurl + OpenSSL** | emscripten Fetch API, or build curl for wasm | Optional; gate off initially |
| **SocketW** (raw TCP multiplayer) | **Drop / rewrite.** Browsers can't open raw TCP | Multiplayer needs a WebSocket proxy later, or stays disabled |
| **Caelum, PagedGeometry** | Ogre plugins; port only after Ogre boots | Optional; disable initially |
| **Bullet / beam physics** | RoR's own CPU beam solver compiles fine | Heavy; runs under pthreads |
| **Codec_FreeImage** | Build FreeImage for wasm, or use Ogre STB codec | Needed for texture loading |

Conan provides none of these for wasm. We stand up our own **wasm sysroot**
(prebuilt deps on `CMAKE_PREFIX_PATH`), exactly like the SoH `wasm-sysroot`.

## 3. Engine decision (pick before Phase 2)

- **Option A — Fork the existing Ogre 1.11 RoR fork** and add emscripten
  support (EmscriptenGLSupport + GLES2 RS over WebGL2). Keeps RoR's engine API
  usage unchanged. This mirrors SoH forking libultraship/ZAPDTR and pointing the
  submodule at a `wasm-port` branch. **Recommended for fastest boot.**
- **Option B — Bump to modern OGRE 14.x** (official Emscripten target). Most
  sustainable long-term, but RoR pins a custom 1.11 fork with patches → API
  migration cost across all of `source/main/gfx`.

Recommendation: **A first** to prove the port, evaluate **B** later.

## 4. Threading + main loop (SoH playbook)

- pthreads-everywhere: `-pthread -fPIC`, `-sPTHREAD_POOL_SIZE` sized to RoR's
  threadpool + network + physics threads, **fixed heap** (no
  `ALLOW_MEMORY_GROWTH` — growth invalidates worker atomics under SAB).
- Main loop: first cut uses **ASYNCIFY** (per-frame `emscripten_sleep(0)` after
  `renderOneFrame`) so the blocking `while` loop yields to the browser —
  identical to SoH. Later, optionally convert to `emscripten_set_main_loop`
  (cleaner, no ASYNCIFY size/perf tax) since RoR's loop is already isolated in
  main.cpp.
- Serve cross-origin-isolated (COOP `same-origin` + COEP `require-corp`) so
  `SharedArrayBuffer` is available.

## 5. Content delivery

RoR content (terrains, vehicles) is large and freely distributed. Options:
- Preload a small **base content** set into MEMFS at build time (SoH-style),
- Lazy-fetch the rest over HTTP, or use IDBFS for persistence.
- Keep a **bring-your-own-content** convention for anything not redistributable,
  same spirit as SoH's bring-your-own-ROM.

## 6. Phased roadmap (boot-first, like SoH)

- **Phase 0 — Toolchain & inventory.** emsdk + Ninja. Add an `EMSCRIPTEN`
  branch to top-level + `source/main` CMake. Start the wasm sysroot.
- **Phase 1 — Minimal link.** Gate OFF: `ROR_USE_SOCKETW`, `ROR_USE_CURL`,
  Caelum, PagedGeometry, Cg, discord. Replace OIS includes behind stubs. Get
  the RoR objects to **compile and link** against wasm Ogre (even headless).
- **Phase 2 — Ogre on canvas.** Emscripten GLES2/WebGL2 render system; blank
  Ogre render window presenting to the HTML canvas.
- **Phase 3 — Input + audio.** SDL2/HTML5 input replacing OIS; emscripten
  OpenAL for sound.
- **Phase 4 — GUI.** MyGUI rendering → reach the main menu.
- **Phase 5 — Simulation.** Load one terrain + one vehicle; beam physics
  running under pthreads with ASYNCIFY/main-loop yielding.
- **Phase 6 — Content + perf.** MEMFS/IDBFS/fetch content pipeline; tune heap
  size, thread pool, texture/codec memory.
- **Phase 7 — Optional features.** Re-enable where feasible: AngelScript
  scripting, networking via WebSocket proxy, Caelum/PagedGeometry, Cg-shader
  replacements as GLSL ES.

## 7. First concrete steps

1. Stand up emsdk + an empty wasm sysroot dir.
2. Build the leaf deps for wasm first: fmt, rapidjson (header-only), zlib,
   libpng, FreeImage.
3. Decide engine path (A vs B) and fork Ogre to a `wasm-port` branch.
4. Add the `EMSCRIPTEN` CMake branch with everything optional gated OFF.
5. Iterate to a clean **link** before worrying about a clean **boot**.
