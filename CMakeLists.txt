cmake_minimum_required(VERSION 3.24)
project(TtpWaskin VERSION 0.1.0 LANGUAGES CXX RC)
if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "ttp_waskin requires Windows and MSVC")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The shared DLL requires -A Win32 for both player editions")
endif()
# One XP-compatible binary serves both hosts. The C ABI never transfers CRT
# allocations, so the modern host can safely use this DLL's older runtime.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/legacy_windows.cmake")
include(cmake/size_release.cmake)
include(cmake/version.cmake)
add_library(ttp_waskin SHARED src/archive.cpp src/builtin_skin.cpp resources/default_skin.rc src/metadata.cpp src/skin.cpp src/tooltips.cpp src/layout.cpp src/fallback.cpp src/spectrum.cpp src/visual.cpp src/video.cpp src/modern_vis.cpp src/modern.cpp src/plugin.cpp src/ttp_waskin.def "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")
add_dependencies(ttp_waskin ttp_waskin_version)
target_include_directories(ttp_waskin PUBLIC include PRIVATE src resources)
target_compile_definitions(ttp_waskin PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0501)
# XP LoadLibrary does not initialize compiler-generated static TLS guards.
# Nontrivial lazy caches use LazyCache; lookup tables have constant initializers.
target_compile_options(ttp_waskin PRIVATE /W4 /permissive- /EHsc /utf-8 /Zc:threadSafeInit-)
target_link_libraries(ttp_waskin PRIVATE user32 gdi32 gdiplus comctl32 ole32 oleaut32)
set_target_properties(ttp_waskin PROPERTIES PREFIX "" OUTPUT_NAME "ttp_waskin")
install(TARGETS ttp_waskin RUNTIME DESTINATION AddIn COMPONENT Runtime)
install(DIRECTORY DESTINATION Skin/waskin)
install(FILES LICENSE DESTINATION licenses/ttp_waskin)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
install(FILES "${ttplayer_yy_thunks_SOURCE_DIR}/LICENSE"
  DESTINATION licenses/ttp_waskin RENAME YY-Thunks-LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/licenses/VC-LTL-LICENSE.txt"
  DESTINATION licenses/ttp_waskin)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/docs/LEGACY_DEPENDENCIES.md"
  DESTINATION licenses/ttp_waskin)
add_custom_command(TARGET ttp_waskin POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/check_legacy_imports.py"
    "$<TARGET_FILE:ttp_waskin>"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/5.1.2600.txt"
    --exports "${ttplayer_yy_thunks_SOURCE_DIR}/Config/x86/6.1.7600.txt"
    --report "$<TARGET_FILE_DIR:ttp_waskin>/legacy-imports.json"
  VERBATIM)
