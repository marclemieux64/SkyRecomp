cmake_minimum_required(VERSION 3.22.1)
project(skyrecomp_quest LANGUAGES C CXX)

find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC
    "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC
    "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(skyrecomp_quest SHARED quest_main.cpp)
target_compile_definitions(skyrecomp_quest PRIVATE
    XR_USE_PLATFORM_ANDROID
    XR_USE_GRAPHICS_API_VULKAN)
target_link_libraries(skyrecomp_quest PRIVATE
    native_app_glue
    OpenXR::openxr_loader
    c++_static
    c++abi
    android
    log
    vulkan)
target_link_options(skyrecomp_quest PRIVATE "-uANativeActivity_onCreate")

# The full game runtime (personal builds): android/engine.
option(SKYRECOMP_ENGINE "Build the Sky Recomp runtime (libmain.so)" OFF)
if(SKYRECOMP_ENGINE)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../../../engine" engine)
endif()
