cmake_minimum_required(VERSION 3.19)
project(Tracer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

FetchContent_Declare(omp-sdk
	GIT_REPOSITORY https://github.com/openmultiplayer/open.mp-sdk.git
	GIT_TAG 08cc69a32d96c0eb8fe063d26c78259e54ca853f
)
FetchContent_Declare(pawn
	GIT_REPOSITORY https://github.com/openmultiplayer/compiler.git
	GIT_TAG 29f1a8c7fd2e01929807cd8c50353fbc93bbd651
)
FetchContent_MakeAvailable(omp-sdk pawn)

add_library(subhook STATIC deps/subhook/subhook.c)
target_include_directories(subhook PUBLIC deps/subhook)
target_compile_definitions(subhook PUBLIC SUBHOOK_STATIC)

set(REPORT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/report.html")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${REPORT_SOURCE}")
file(READ "${REPORT_SOURCE}" REPORT_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," REPORT_BYTES "${REPORT_HEX}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/report_template.h"
	"#pragma once\n#include <cstddef>\nstatic const unsigned char reportTemplate[] = {${REPORT_BYTES}};\nstatic const size_t reportTemplateSize = sizeof(reportTemplate);\n")

add_library(Tracer SHARED
	src/backtrace.cpp
	src/config.cpp
	src/crash.cpp
	src/debuginfo.cpp
	src/errors.cpp
	src/hooks.cpp
	src/longcall.cpp
	src/main.cpp
	src/natives.cpp
	src/output.cpp
	src/report.cpp
)
set_target_properties(Tracer PROPERTIES PREFIX "")
target_include_directories(Tracer PRIVATE
	"${pawn_SOURCE_DIR}/source"
	"${pawn_SOURCE_DIR}/source/linux"
	"${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_definitions(Tracer PRIVATE
	HAVE_STDINT_H=1
	PAWN_CELL_SIZE=32
	TRACER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
	TRACER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
	TRACER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
	TRACER_VERSION="${PROJECT_VERSION}"
)
option(TRACER_TEST_NATIVES "Add the natives the tests use to crash the server on purpose" OFF)
if(TRACER_TEST_NATIVES)
	target_compile_definitions(Tracer PRIVATE TRACER_TEST_NATIVES=1)
endif()
if(WIN32)
	target_compile_definitions(Tracer PRIVATE HAVE_ALLOCA_H=0 WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
	target_link_libraries(Tracer PRIVATE dbghelp psapi)
endif()
if(MSVC)
	target_compile_options(Tracer PRIVATE /W3 /utf-8)
endif()
target_link_libraries(Tracer PRIVATE OMP-SDK subhook)
