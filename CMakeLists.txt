cmake_minimum_required(VERSION 3.20)
project(CombatMod LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
# Static CRT (/MT, /MTd): the ASI must not depend on a VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(MH ${CMAKE_SOURCE_DIR}/external/minhook)
add_library(CombatMod SHARED
  src/dllmain.cpp src/log.cpp src/sigscan.cpp src/bridge.cpp src/timecurve.cpp
  src/util.cpp src/memwrite.cpp src/trace.cpp src/trace_stub.asm src/watch.cpp src/input.cpp src/call.cpp src/selftest.cpp src/timeamp.cpp src/skillctx.cpp src/etrace.cpp src/playertrack.cpp src/settings.cpp src/launchboost.cpp src/features.cpp src/uimodel.cpp src/ui.cpp src/pause.cpp src/hookretry.cpp src/telemetry.cpp src/triggers.cpp src/launchscan.cpp src/layoutcheck.cpp src/version.cpp
  ${MH}/src/buffer.c ${MH}/src/hook.c ${MH}/src/trampoline.c ${MH}/src/hde/hde64.c)

target_include_directories(CombatMod PRIVATE ${MH}/include ${MH}/src)
target_compile_definitions(CombatMod PRIVATE
  _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(CombatMod PRIVATE $<$<COMPILE_LANGUAGE:C,CXX>:/W3> $<$<COMPILE_LANGUAGE:CXX>:/EHsc> $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
target_link_libraries(CombatMod PRIVATE ws2_32 dbghelp psapi comctl32)

# Output: CombatMod\build\CombatMod.asi (all configs), plus a copy into dist\.
set(OUT ${CMAKE_SOURCE_DIR}/build)
set_target_properties(CombatMod PROPERTIES
  OUTPUT_NAME CombatMod PREFIX "" SUFFIX ".asi"
  RUNTIME_OUTPUT_DIRECTORY ${OUT} LIBRARY_OUTPUT_DIRECTORY ${OUT}
  RUNTIME_OUTPUT_DIRECTORY_DEBUG ${OUT} RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUT}
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO ${OUT} RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL ${OUT}
  LIBRARY_OUTPUT_DIRECTORY_DEBUG ${OUT} LIBRARY_OUTPUT_DIRECTORY_RELEASE ${OUT}
  LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO ${OUT} LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL ${OUT})
add_custom_command(TARGET CombatMod POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/dist
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CombatMod> ${CMAKE_SOURCE_DIR}/dist/)
