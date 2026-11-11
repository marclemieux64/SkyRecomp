# The Sky Recomp runtime as the Quest APK's native library (libmain.so, started by SDL's Android activity).
# It holds no game code: libskygame.so, built on the player's PC from their own game files, is loaded at startup.
cmake_minimum_required(VERSION 3.22.1)

get_filename_component(SKY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(RT "${SKY_ROOT}/runtime")
set(IMGUI "${SKY_ROOT}/third_party/imgui")

find_package(SDL3 REQUIRED CONFIG)
find_package(OpenXR REQUIRED CONFIG)

# ---- shaderc (Maxwell shaders are translated to GLSL and compiled on the headset)
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(sky_spirv_headers
    URL https://github.com/KhronosGroup/SPIRV-Headers/archive/01e0577914a75a2569c846778c2f93aa8e6feddd.tar.gz
    SOURCE_SUBDIR none)
FetchContent_Declare(sky_spirv_tools
    URL https://github.com/KhronosGroup/SPIRV-Tools/archive/19042c8921f35f7bec56b9e5c96c5f5691588ca8.tar.gz
    SOURCE_SUBDIR none)
FetchContent_Declare(sky_glslang
    URL https://github.com/KhronosGroup/glslang/archive/d213562e35573012b6348b2d584457c3704ac09b.tar.gz
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(sky_spirv_headers sky_spirv_tools sky_glslang)
set(SHADERC_SPIRV_HEADERS_DIR "${sky_spirv_headers_SOURCE_DIR}" CACHE STRING "" FORCE)
set(SHADERC_SPIRV_TOOLS_DIR "${sky_spirv_tools_SOURCE_DIR}" CACHE STRING "" FORCE)
set(SHADERC_GLSLANG_DIR "${sky_glslang_SOURCE_DIR}" CACHE STRING "" FORCE)
set(SHADERC_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_EXAMPLES ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SHADERC_SKIP_COPYRIGHT_CHECK ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(sky_shaderc
    URL https://github.com/google/shaderc/archive/refs/tags/v2025.4.tar.gz
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(sky_shaderc)
# shaderc hands glslang a generator expression where it expects ON/OFF, which turns glslang's install rules on.
file(READ "${sky_shaderc_SOURCE_DIR}/third_party/CMakeLists.txt" SHADERC_THIRD_PARTY)
string(REPLACE "set(GLSLANG_ENABLE_INSTALL $<NOT:\${SKIP_GLSLANG_INSTALL}>)" "set(GLSLANG_ENABLE_INSTALL OFF)"
    SHADERC_THIRD_PARTY "${SHADERC_THIRD_PARTY}")
file(WRITE "${sky_shaderc_SOURCE_DIR}/third_party/CMakeLists.txt" "${SHADERC_THIRD_PARTY}")
add_subdirectory("${sky_shaderc_SOURCE_DIR}" shaderc EXCLUDE_FROM_ALL)

# ---- Vulkan Memory Allocator (header only; a system package on desktop)
FetchContent_Declare(sky_vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
    SOURCE_SUBDIR none)
FetchContent_MakeAvailable(sky_vma)

# ---- SMAA's reference shader as a C string (the Makefile does the same with tr / sed)
file(READ "${SKY_ROOT}/third_party/smaa/SMAA.hlsl" SMAA_SRC)
string(REPLACE "\r" "" SMAA_SRC "${SMAA_SRC}")
string(REGEX REPLACE "[^\n\t -~]" "?" SMAA_SRC "${SMAA_SRC}")
string(REPLACE "\\" "\\\\" SMAA_SRC "${SMAA_SRC}")
string(REPLACE "\"" "\\\"" SMAA_SRC "${SMAA_SRC}")
string(REPLACE "\n" "\\n\"\n\"" SMAA_SRC "${SMAA_SRC}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/inc")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/inc/smaa_hlsl.inc" "\"${SMAA_SRC}\"\n")

# ---- sources: what the Makefile builds for Linux, with Android's host layer
file(GLOB RT_C CONFIGURE_DEPENDS
    "${RT}/*.c" "${RT}/vr/*.c" "${RT}/hle/*.c" "${RT}/services/*.c" "${RT}/nvn/*.c" "${RT}/nvn/maxwell/*.c"
    "${RT}/nvn/vulkan/*.c")
file(GLOB RT_CXX CONFIGURE_DEPENDS "${RT}/*.cpp" "${RT}/vr/*.cpp" "${RT}/nvn/vulkan/*.cpp")
file(GLOB RT_UI CONFIGURE_DEPENDS "${RT}/ui/*.mm")
# No FFmpeg on the headset yet: movies and the start screen (its title video) are replaced (android_stubs.c).
list(REMOVE_ITEM RT_C "${RT}/hle/movie.c")
list(REMOVE_ITEM RT_UI "${RT}/ui/launcher.mm")
set_source_files_properties(${RT_UI} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")

add_library(main SHARED
    ${RT_C} ${RT_CXX} ${RT_UI}
    "${RT}/platform/android.c"
    "${RT}/platform/android_stubs.c"
    "${IMGUI}/imgui.cpp" "${IMGUI}/imgui_draw.cpp" "${IMGUI}/imgui_tables.cpp" "${IMGUI}/imgui_widgets.cpp"
    "${IMGUI}/backends/imgui_impl_sdl3.cpp" "${IMGUI}/backends/imgui_impl_vulkan.cpp")

target_include_directories(main PRIVATE
    "${RT}" "${RT}/nvn" "${IMGUI}" "${IMGUI}/backends"
    "${CMAKE_CURRENT_BINARY_DIR}/inc" "${SKY_ROOT}/third_party/smaa/Textures"
    "${sky_shaderc_SOURCE_DIR}/libshaderc/include" "${sky_vma_SOURCE_DIR}/include")
target_compile_definitions(main PRIVATE
    GAMELIB_ID="7d549f7e81e92c48" NDEBUG
    XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_VULKAN VK_USE_PLATFORM_ANDROID_KHR)
target_compile_options(main PRIVATE
    -O2 -fno-strict-aliasing -fvisibility=default -Wno-format -Wno-unused-function)
set_target_properties(main PROPERTIES C_STANDARD 17 CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(main PRIVATE
    SDL3::SDL3 OpenXR::openxr_loader shaderc c++_static c++abi android log vulkan m dl)
# The game code library resolves its runtime calls against this one: keep every symbol exported.
target_link_options(main PRIVATE "-Wl,--export-dynamic" "-Wl,-z,nostart-stop-gc" "-Wl,--error-limit=0")
