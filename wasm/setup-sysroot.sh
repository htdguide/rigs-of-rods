#!/usr/bin/env bash
# Build the wasm dependency sysroot for the Rigs of Rods emscripten port.
#
# Phase 0 scaffolding: this stands up the Ogre engine fork and the leaf deps.
# Heavier deps (MyGUI, FreeImage, FreeType, AngelScript) are added as later
# phases need them. Run from anywhere; paths are resolved relative to the repo.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSROOT="${ROR_WASM_SYSROOT:-$HERE/sysroot}"
OGRE_FORK="${OGRE_FORK:-https://github.com/htdguide/ogre.git}"
OGRE_BRANCH="${OGRE_BRANCH:-wasm-port}"

# --- toolchain check ---------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
  echo "error: emcc not on PATH. Activate emsdk first: source <emsdk>/emsdk_env.sh" >&2
  exit 1
fi
EMSCRIPTEN_TOOLCHAIN="$(em-config EMSCRIPTEN_ROOT)/cmake/Modules/Platform/Emscripten.cmake"
echo "emsdk:    $(emcc -v 2>&1 | head -1)"
echo "sysroot:  $SYSROOT"

mkdir -p "$SYSROOT"

# --- engine fork -------------------------------------------------------------
if [ ! -d "$HERE/ogre/.git" ]; then
  echo "cloning Ogre fork ($OGRE_BRANCH)…"
  git clone --depth 1 --branch "$OGRE_BRANCH" "$OGRE_FORK" "$HERE/ogre"
fi

# --- Ogre wasm build (the core of Path A) ------------------------------------
# Ogre 1.11.6 already carries an Emscripten EGL/GLES2 backend
# (RenderSystems/GLSupport/src/EGL/Emscripten). Build only what the web target
# needs: GLES2 render system, no D3D/GL, no Cg, no samples/tools.
#
# TODO(phase-2): finalize these options against the Emscripten EGL window and
# install into $SYSROOT.
emcmake cmake -S "$HERE/ogre" -B "$HERE/ogre/build-emscripten" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$EMSCRIPTEN_TOOLCHAIN" \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
  -DCMAKE_BUILD_TYPE=Release \
  -DOGRE_BUILD_RENDERSYSTEM_GLES2=ON \
  -DOGRE_BUILD_RENDERSYSTEM_GL=OFF \
  -DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF \
  -DOGRE_BUILD_PLUGIN_CG=OFF \
  -DOGRE_BUILD_SAMPLES=OFF \
  -DOGRE_BUILD_TOOLS=OFF \
  -DOGRE_BUILD_TESTS=OFF
# cmake --build "$HERE/ogre/build-emscripten" --target install

echo
echo "Phase-0 scaffolding done. Next: get the Ogre wasm build to install,"
echo "then point RoR's CMAKE_PREFIX_PATH at: $SYSROOT"
