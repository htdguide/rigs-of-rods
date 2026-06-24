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

# pthreads-everywhere: every dependency archive must be compiled with -pthread
# (atomics / shared-memory ABI) and -fPIC so it links against the pthread-enabled
# RoR build. Propagate via the standard CXXFLAGS/CFLAGS env that cmake picks up.
export CXXFLAGS="-pthread -fPIC ${CXXFLAGS:-}"
export CFLAGS="-pthread -fPIC ${CFLAGS:-}"

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

# --- SocketW (TCP wrapper) ---------------------------------------------------
# Compiles for wasm via emscripten's BSD-socket emulation. Non-functional at
# runtime (browsers have no raw TCP) but keeps RoR's networking code paths
# compiling so the Network class is a complete type.
[ -d "$HERE/socketw" ] || git clone --depth 1 https://github.com/RigsOfRods/socketw.git "$HERE/socketw"
emcmake cmake -S "$HERE/socketw" -B "$HERE/socketw/build-emscripten" -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF
cmake --build "$HERE/socketw/build-emscripten" --target install

# --- libcurl (HTTP) ----------------------------------------------------------
# RoR's Network header hard-references CURLcode, so curl must be present to
# compile even with networking non-functional. Build a minimal static libcurl
# (no TLS / extra protocols).
[ -d "$HERE/curl" ] || git clone --depth 1 --branch curl-8_2_1 https://github.com/curl/curl.git "$HERE/curl"
emcmake cmake -S "$HERE/curl" -B "$HERE/curl/build-emscripten" -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_CURL_EXE=OFF \
  -DBUILD_TESTING=OFF -DCURL_USE_OPENSSL=OFF -DCURL_USE_LIBPSL=OFF -DCURL_ENABLE_SSL=OFF \
  -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_ZLIB=OFF -DCURL_DISABLE_LDAP=ON
cmake --build "$HERE/curl/build-emscripten" --target install

# --- AngelScript (scripting VM — portable interpreter, builds for wasm) -------
if [ ! -d "$HERE/angelscript_sdk" ]; then
  curl -sL -o "$HERE/as.zip" "https://www.angelcode.com/angelscript/sdk/files/angelscript_2.35.1.zip"
  unzip -q "$HERE/as.zip" -d "$HERE/angelscript_sdk" && rm -f "$HERE/as.zip"
fi
AS_CMAKE="$HERE/angelscript_sdk/sdk/angelscript/projects/cmake"
emcmake cmake -S "$AS_CMAKE" -B "$AS_CMAKE/build-emscripten" -G Ninja \
  -DCMAKE_INSTALL_PREFIX="$SYSROOT" -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$AS_CMAKE/build-emscripten" --target install

# --- OpenAL EFX headers ------------------------------------------------------
# emscripten's OpenAL ships al.h/alc.h/alext.h but not the EFX extension headers
# that RoR's audio includes. Vendor them from openal-soft so RoR compiles (EFX
# is a runtime no-op on the browser backend).
mkdir -p "$SYSROOT/include/AL"
for h in efx-presets.h efx-creative.h; do
  [ -f "$SYSROOT/include/AL/$h" ] || \
    curl -sL -o "$SYSROOT/include/AL/$h" "https://raw.githubusercontent.com/kcat/openal-soft/master/include/AL/$h"
done
# efx.h needs two fixes for emscripten's OpenAL: redirect its relative
# "alc.h"/"al.h" includes to <AL/...>, and define the AL_APIENTRY /
# AL_API_NOEXCEPT17 calling-convention macros it expects (emscripten's al.h
# does not provide them).
if [ ! -f "$SYSROOT/include/AL/efx.h" ]; then
  curl -sL "https://raw.githubusercontent.com/kcat/openal-soft/master/include/AL/efx.h" -o "$HERE/efx_orig.h"
  SYSROOT="$SYSROOT" HERE="$HERE" python3 - <<'PY'
import os
src = open(os.environ['HERE'] + '/efx_orig.h').read()
src = src.replace('#include "alc.h"', '#include <AL/alc.h>').replace('#include "al.h"', '#include <AL/al.h>')
shim = ('#ifndef AL_APIENTRY\n#define AL_APIENTRY\n#endif\n'
        '#ifndef AL_API_NOEXCEPT17\n#define AL_API_NOEXCEPT17\n#endif\n')
open(os.environ['SYSROOT'] + '/include/AL/efx.h', 'w').write(shim + src)
PY
  rm -f "$HERE/efx_orig.h"
fi

echo
echo "wasm sysroot complete: $SYSROOT"
echo "  Ogre + components, fmt, rapidjson, MyGUI, OIS (null input), AngelScript, EFX headers."
echo "Next: configure Rigs of Rods with -DCMAKE_PREFIX_PATH=$SYSROOT (emcmake)."
