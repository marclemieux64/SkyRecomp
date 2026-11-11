cmake_minimum_required(VERSION 3.22.1)
project(skyrecomp_android_game C)
set(SKYRECOMP_GAME_OPT "-O1" CACHE STRING "Optimization flag for generated game code")

if(NOT DEFINED SKYRECOMP_ROOT OR NOT DEFINED SKYRECOMP_GENERATED_DIR)
    message(FATAL_ERROR "Set SKYRECOMP_ROOT and SKYRECOMP_GENERATED_DIR")
endif()
file(TO_CMAKE_PATH "${SKYRECOMP_ROOT}" SKYRECOMP_ROOT)
file(TO_CMAKE_PATH "${SKYRECOMP_GENERATED_DIR}" SKYRECOMP_GENERATED_DIR)

file(GLOB MAIN_SOURCES CONFIGURE_DEPENDS "${SKYRECOMP_GENERATED_DIR}/main/*.c")
file(GLOB SDK_SOURCES CONFIGURE_DEPENDS "${SKYRECOMP_GENERATED_DIR}/sdk/*.c")
file(GLOB SUBSDK0_SOURCES CONFIGURE_DEPENDS "${SKYRECOMP_GENERATED_DIR}/subsdk0/*.c")
file(GLOB SUBSDK1_SOURCES CONFIGURE_DEPENDS "${SKYRECOMP_GENERATED_DIR}/subsdk1/*.c")

foreach(table
        "${SKYRECOMP_GENERATED_DIR}/main/main_table.c"
        "${SKYRECOMP_GENERATED_DIR}/sdk/sdk_table.c"
        "${SKYRECOMP_GENERATED_DIR}/subsdk0/subsdk0_table.c"
        "${SKYRECOMP_GENERATED_DIR}/subsdk1/subsdk1_table.c")
    if(NOT EXISTS "${table}")
        message(FATAL_ERROR "Generated game table is missing: ${table}")
    endif()
endforeach()

add_library(skygame SHARED
    ${MAIN_SOURCES}
    ${SDK_SOURCES}
    ${SUBSDK0_SOURCES}
    ${SUBSDK1_SOURCES}
    "${SKYRECOMP_ROOT}/tools/gamelib_entry.c"
    "${SKYRECOMP_ROOT}/tools/subsdk8_empty.c")

target_include_directories(skygame PRIVATE "${SKYRECOMP_ROOT}/runtime")
target_compile_definitions(skygame PRIVATE GAMELIB_ID="7d549f7e81e92c48")
target_compile_options(skygame PRIVATE
    ${SKYRECOMP_GAME_OPT} -fno-strict-aliasing -fvisibility=hidden
    -Wno-return-type -Wno-unused-label)

# Generated calls resolve against the Quest runtime library (libmain.so, built by
# Gradle from android/engine) when Android loads the APK. Keeping the game in a
# separate .so prevents private generated code from becoming part of the public
# runtime APK. Android only binds symbols to libraries a module names, so an
# empty stand-in with the runtime's name records that dependency here.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/runtime_stub.c" "void skyrecomp_runtime_stub(void) {}
")
add_library(runtime_stub SHARED "${CMAKE_CURRENT_BINARY_DIR}/runtime_stub.c")
set_target_properties(runtime_stub PROPERTIES OUTPUT_NAME main
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/runtime_stub")
target_link_options(runtime_stub PRIVATE "-Wl,-soname,libmain.so")
add_dependencies(skygame runtime_stub)
target_link_options(skygame PRIVATE
    "-Wl,--unresolved-symbols=ignore-all"
    "-Wl,--exclude-libs,ALL"
    "-Wl,--no-as-needed" "-L${CMAKE_CURRENT_BINARY_DIR}/runtime_stub" "-lmain" "-Wl,--as-needed")
