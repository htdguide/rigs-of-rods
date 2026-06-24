cmake_minimum_required(VERSION 3.16)
project(ror_wasm_smoke CXX)

# Point at the wasm sysroot built by ../setup-sysroot.sh (pass via CMAKE_PREFIX_PATH).
# Ogre's exported targets reference ZLIB::ZLIB and Freetype::Freetype; provide
# them from the emscripten ports (built into the sysroot via embuilder).
find_package(ZLIB REQUIRED)
find_package(Freetype REQUIRED)
find_package(OGRE 1.11 REQUIRED COMPONENTS Bites Overlay RTShaderSystem)

# sysroot/lib (OGRE_DIR is <sysroot>/lib/OGRE/cmake) so plain -l names from
# Ogre's exported link interface (e.g. -lOgreGLSupportStatic) resolve.
get_filename_component(_ror_sysroot "${OGRE_DIR}/../../.." ABSOLUTE)
link_directories(${_ror_sysroot}/lib ${_ror_sysroot}/lib/OGRE)

add_executable(smoke main.cpp)
set_target_properties(smoke PROPERTIES OUTPUT_NAME "index" SUFFIX ".html")

target_include_directories(smoke PRIVATE
    ${OGRE_INCLUDE_DIRS}
    ${OGRE_PREFIX_DIR}/include/OGRE/RenderSystems/GLES2)

# Static Ogre: link OgreMain + the GLES2 render system + GL support explicitly.
target_link_libraries(smoke PRIVATE
    ${OGRE_PREFIX_DIR}/lib/OGRE/libRenderSystem_GLES2Static.a
    ${OGRE_LIBRARIES}
    ${OGRE_PREFIX_DIR}/lib/libOgreGLSupportStatic.a
    freetype z)

target_link_options(smoke PRIVATE
    -sFULL_ES3 -sMAX_WEBGL_VERSION=2 -sMIN_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_RUNTIME_METHODS=FS,callMain
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/../shell.html)
