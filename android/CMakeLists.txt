# The Android app's native side: libmain.so (the runtime, from the rebased copy tools/android/prepare.sh writes)
# and libSDL3.so. Dependencies come from DEPS_PREFIX (tools/android/build_deps.sh: FFmpeg, shaderc, zstd) and SDL_SRC.
cmake_minimum_required(VERSION 3.22)
project(swshrecomp C CXX)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(S ${ROOT}/build/android-src)
if(NOT EXISTS ${S}/GAMELIB_ID)
  message(FATAL_ERROR "run tools/android/prepare.sh first")
endif()
file(READ ${S}/GAMELIB_ID GAMELIB_ID)
string(STRIP "${GAMELIB_ID}" GAMELIB_ID)
set(DEPS_PREFIX "" CACHE PATH "prefix with FFmpeg, shaderc and zstd built for Android arm64")
set(SDL_SRC "" CACHE PATH "SDL3 source tree")

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
add_subdirectory(${SDL_SRC} sdl EXCLUDE_FROM_ALL)

# SMAA's shader as a C string (as the Makefile does)
set(SMAA_INC ${CMAKE_BINARY_DIR}/inc/smaa_hlsl.inc)
add_custom_command(OUTPUT ${SMAA_INC}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/inc
  COMMAND sh -c "LC_ALL=C tr -d '\\r' < ${S}/third_party/smaa/SMAA.hlsl | LC_ALL=C tr '\\200-\\377' '?' | sed -e 's/\\\\/\\\\\\\\/g' -e 's/\"/\\\\\"/g' -e 's/^/\"/' -e 's/$/\\\\n\"/' > ${SMAA_INC}"
  DEPENDS ${S}/third_party/smaa/SMAA.hlsl)

file(GLOB RT_C ${S}/runtime/*.c ${S}/runtime/hle/*.c ${S}/runtime/services/*.c ${S}/runtime/nvn/*.c
     ${S}/runtime/nvn/maxwell/*.c ${S}/runtime/nvn/vulkan/*.c)
file(GLOB RT_CXX ${S}/runtime/nvn/vulkan/*.cpp)
file(GLOB UI_MM ${S}/runtime/ui/*.mm)
set(IMGUI ${S}/third_party/imgui)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)
set_source_files_properties(${UI_MM} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")

add_library(main SHARED ${RT_C} ${RT_CXX} ${UI_MM} ${IMGUI_SRC} ${S}/runtime/platform/android.c ${SMAA_INC})
target_include_directories(main PRIVATE ${S}/runtime ${S}/runtime/nvn ${CMAKE_BINARY_DIR}/inc ${S}/third_party/smaa/Textures
                           ${IMGUI} ${IMGUI}/backends ${DEPS_PREFIX}/include)
target_compile_definitions(main PRIVATE GAMELIB_ID="${GAMELIB_ID}" GAMELIB_CFLAGS="" GAMELIB_SHARED="-shared")
target_compile_options(main PRIVATE $<$<COMPILE_LANGUAGE:C>:-O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Wno-format>
                       $<$<COMPILE_LANGUAGE:CXX>:-O2 -g -std=c++17>)
target_link_directories(main PRIVATE ${DEPS_PREFIX}/lib)
# (the game code library, loaded later, calls into this one: everything stays exported, as -rdynamic on Linux;
#  16 KB pages: Android 15 devices need them, and the guest maps with PLAT_PAGE = 16 KB anyway)
target_link_options(main PRIVATE -Wl,--export-dynamic -Wl,-z,max-page-size=16384)
target_link_libraries(main PRIVATE SDL3::SDL3-shared avformat avcodec avutil swscale swresample shaderc_combined zstd z
                      vulkan android log m dl)
