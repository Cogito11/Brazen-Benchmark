cmake_minimum_required(VERSION 3.16)
project(Brazen CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_SOURCE_DIR}/cmake/BrazenVersion.cmake)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Version string baked into the binary (shown on the App Info screen).
# The tracked default lives in cmake/BrazenVersion.cmake. CI can override it
# with -DBRAZEN_VERSION: stable releases use the pushed tag, while rolling
# builds use a rolling identifier.
if(NOT DEFINED BRAZEN_VERSION)
    set(BRAZEN_VERSION "${BRAZEN_DEFAULT_VERSION}")
endif()

# Statically link the MSVC runtime on Windows. Without this, CMake/MSVC
# defaults to the dynamic runtime (/MD), which makes Brazen.exe depend on
# VCRUNTIME140.dll / VCRUNTIME140_1.dll / MSVCP140.dll being present on
# the machine that runs it -- fine on the CI runner (Visual Studio is
# preinstalled there) but missing on most end users' machines, causing
# a "missing DLL" error on launch. Same reasoning as the GLFW static-link
# fallback above: don't depend on something that only exists on the
# machine that built it.
if(WIN32)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Same reasoning as above, but for MinGW GCC: CI's windows-latest runner
# has a MinGW toolchain on PATH (bundled with Strawberry Perl) that
# CMake's compiler auto-detection picks up ahead of MSVC's cl.exe, since
# nothing in the workflow explicitly sets up the MSVC dev environment.
# So the actual shipped binary has so far been built with GCC, not MSVC,
# meaning the CMAKE_MSVC_RUNTIME_LIBRARY setting above was a silent
# no-op -- it only affects real MSVC builds. Without this, a MinGW build
# dynamically depends on libwinpthread-1.dll, libgcc_s_seh-1.dll, and
# libstdc++-6.dll being present on the machine that runs it, none of
# which ship with stock Windows.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libgcc -static-libstdc++ -static)
endif()

find_package(OpenGL REQUIRED)

# Prefer system GLFW (installed via package manager); fall back to
# fetching source if it isn't found.
find_package(glfw3 3.3 QUIET)
if(NOT glfw3_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # X11 only: this app has no Wayland-specific code, and GLFW's
    # Wayland backend is ON by default as of 3.4, which requires
    # wayland-scanner/libxkbcommon-dev/libwayland-dev to even configure.
    # Without this, the FetchContent fallback fails outright on any
    # Linux box that doesn't already have system GLFW (and therefore
    # those extra packages) installed. X11 apps still run fine on a
    # Wayland desktop via XWayland.
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

# Prefer an already-present vendored copy (kept out of git, see
# .gitignore); fall back to fetching it into that same path if it's
# missing, e.g. on a fresh clone or a CI runner. Same "use what's
# already there, fetch if not" pattern as GLFW above, and pointing
# SOURCE_DIR at IMGUI_DIR (rather than the FetchContent default of
# build/_deps/imgui-src) means nothing below this block needs to know
# or care which path it came from.
if(NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90.9
        SOURCE_DIR     ${IMGUI_DIR}
    )
    FetchContent_MakeAvailable(imgui)
endif()

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ---- Main GUI application ----
add_executable(Brazen
    src/main.cpp
    src/ui/BrazenApp.cpp
    src/gpu/GLLoader.cpp
    src/gpu/GpuComputeTest.cpp
    src/gpu/GpuTestRunner.cpp
)
target_include_directories(Brazen PRIVATE src)
target_link_libraries(Brazen PRIVATE imgui glfw OpenGL::GL)
target_compile_definitions(Brazen PRIVATE BRAZEN_VERSION="${BRAZEN_VERSION}")
find_package(Threads REQUIRED)
target_link_libraries(Brazen PRIVATE Threads::Threads)

if(WIN32)
    target_link_libraries(Brazen PRIVATE opengl32)
elseif(APPLE)
    target_link_libraries(Brazen PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
endif()

# ---- Headless console smoke-test of the core benchmarking logic ----
# Useful in CI or environments without a display/GPU.
option(BRAZEN_BUILD_CORE_TEST "Build the headless core-only console test" ON)
if(BRAZEN_BUILD_CORE_TEST)
    add_executable(BrazenCoreTest src/core_test_main.cpp)
    target_include_directories(BrazenCoreTest PRIVATE src)
    target_link_libraries(BrazenCoreTest PRIVATE Threads::Threads)
endif()
