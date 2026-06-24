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

# --- leaf deps via emscripten ports ------------------------------------------
# zlib + freetype ship as emscripten ports; pre-build them into the emscripten
# sysroot so Ogre's find_package() locates them (Ogre's own bundled-dependency
# downloader points at dead URLs, so we disable it below).
embuilder build zlib freetype

# --- header/lib leaf deps (fmt, rapidjson) -----------------------------------
build_dep() { # name git tag extra_cmake_args...
  local name="$1" url="$2" tag="$3"; shift 3
  [ -d "$HERE/$name" ] || git clone --depth 1 ${tag:+--branch "$tag"} "$url" "$HERE/$name"
  emcmake cmake -S "$HERE/$name" -B "$HERE/$name/build-emscripten" -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@"
  cmake --build "$HERE/$name/build-emscripten" --target install
}
build_dep fmt       https://github.com/fmtlib/fmt.git       10.1.1 -DFMT_TEST=OFF -DFMT_DOC=OFF
build_dep rapidjson https://github.com/Tencent/rapidjson.git ""    -DRAPIDJSON_BUILD_TESTS=OFF -DRAPIDJSON_BUILD_EXAMPLES=OFF -DRAPIDJSON_BUILD_DOC=OFF

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
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DOGRE_BUILD_DEPENDENCIES=OFF \
  -DOGRE_BUILD_LIBS_AS_FRAMEWORKS=OFF \
  -DOGRE_BUILD_RENDERSYSTEM_GLES2=ON \
  -DOGRE_BUILD_RENDERSYSTEM_GL=OFF \
  -DOGRE_BUILD_RENDERSYSTEM_GL3PLUS=OFF \
  -DOGRE_BUILD_PLUGIN_CG=OFF \
  -DOGRE_BUILD_SAMPLES=OFF \
  -DOGRE_BUILD_TOOLS=OFF \
  -DOGRE_BUILD_TESTS=OFF \
  -DOGRE_BUILD_COMPONENT_TERRAIN=ON \
  -DOGRE_BUILD_COMPONENT_PAGING=ON \
  -DOGRE_BUILD_COMPONENT_OVERLAY=ON \
  -DOGRE_BUILD_COMPONENT_RTSHADERSYSTEM=ON \
  -DOGRE_BUILD_COMPONENT_MESHLODGENERATOR=ON \
  -DOGRE_BUILD_COMPONENT_BITES=ON
cmake --build "$HERE/ogre/build-emscripten" --target install

# --- MyGUI (Ogre-based GUI) --------------------------------------------------
[ -d "$HERE/mygui" ] || git clone --depth 1 --branch wasm-port https://github.com/htdguide/mygui.git "$HERE/mygui"
emcmake cmake -S "$HERE/mygui" -B "$HERE/mygui/build-emscripten" -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOGRE_DIR="$SYSROOT/lib/OGRE/cmake" \
  -DMYGUI_STATIC=ON -DMYGUI_RENDERSYSTEM=3 -DMYGUI_USE_FREETYPE=ON \
  -DMYGUI_BUILD_DEMOS=OFF -DMYGUI_BUILD_PLUGINS=OFF -DMYGUI_BUILD_TOOLS=OFF \
  -DMYGUI_BUILD_TEST_APP=OFF -DMYGUI_BUILD_WRAPPER=OFF -DMYGUI_BUILD_UNITTESTS=OFF \
  -DMYGUI_DISABLE_PLUGINS=ON
cmake --build "$HERE/mygui/build-emscripten" --target install

# --- OIS (null input backend for wasm) ---------------------------------------
[ -d "$HERE/ois" ] || git clone --depth 1 --branch wasm-port https://github.com/htdguide/OIS.git "$HERE/ois"
emcmake cmake -S "$HERE/ois" -B "$HERE/ois/build-emscripten" -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DOIS_BUILD_SHARED_LIBS=OFF -DOIS_BUILD_DEMOS=OFF
cmake --build "$HERE/ois/build-emscripten" --target install

# --- AngelScript (scripting VM — portable interpreter, builds for wasm) -------
if [ ! -d "$HERE/angelscript_sdk" ]; then
  curl -sL -o "$HERE/as.zip" "https://www.angelcode.com/angelscript/sdk/files/angelscript_2.35.1.zip"
  unzip -q "$HERE/as.zip" -d "$HERE/angelscript_sdk" && rm -f "$HERE/as.zip"
fi
AS_CMAKE="$HERE/angelscript_sdk/sdk/angelscript/projects/cmake"
emcmake cmake -S "$AS_CMAKE" -B "$AS_CMAKE/build-emscripten" -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$AS_CMAKE/build-emscripten" --target install

echo
echo "wasm sysroot complete: $SYSROOT"
echo "  Ogre + components, fmt, rapidjson, MyGUI, OIS (null input), AngelScript."
echo "Next: configure Rigs of Rods with -DCMAKE_PREFIX_PATH=$SYSROOT (emcmake)."
