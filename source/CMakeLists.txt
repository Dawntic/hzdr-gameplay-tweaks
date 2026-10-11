#
# Set up the source files and output library
#
set(CURRENT_PROJECT main_output_dll)
set(SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

file(
	GLOB HEADER_FILES
	LIST_DIRECTORIES FALSE
	CONFIGURE_DEPENDS
	"${SOURCE_DIR}/DllProxy/*.h"
	"${SOURCE_DIR}/Hooking/*.h"
	"${SOURCE_DIR}/RenderDoc/*.h"
	"${SOURCE_DIR}/Decima/*.h"
	"${SOURCE_DIR}/Decima/PCore/*.h"
	"${SOURCE_DIR}/Decima/Nx/*.h"
	"${SOURCE_DIR}/*.h"
)

file(
	GLOB SOURCE_FILES
	LIST_DIRECTORIES FALSE
	CONFIGURE_DEPENDS
	"${SOURCE_DIR}/DllProxy/*.cpp"
	"${SOURCE_DIR}/Hooking/*.cpp"
	"${SOURCE_DIR}/RenderDoc/*.cpp"
	"${SOURCE_DIR}/Decima/*.cpp"
	"${SOURCE_DIR}/Decima/Nx/*.cpp"
	"${SOURCE_DIR}/BlackSpace/*.cpp"
	"${SOURCE_DIR}/*.cpp"
)

# Which game the DLL is built for. Everything that hooks Decima internals by signature (the NxDXGIImpl
# Present hook, command list naming) only exists for HZDR. Other games get the engine-agnostic RenderDoc
# setup only: early load, Streamline bypass, diagnostics. Crimson Desert's own engine hooks live in BlackSpace/.
set(MOD_TARGET_GAME "HZDR" CACHE STRING "Game to build for: HZDR or CrimsonDesert")
set_property(CACHE MOD_TARGET_GAME PROPERTY STRINGS HZDR CrimsonDesert)

if(MOD_TARGET_GAME STREQUAL "CrimsonDesert")
	list(FILTER SOURCE_FILES EXCLUDE REGEX "/Decima/.*\\.cpp$")
elseif(MOD_TARGET_GAME STREQUAL "HZDR")
	list(FILTER SOURCE_FILES EXCLUDE REGEX "/BlackSpace/.*\\.cpp$")
else()
	message(FATAL_ERROR "Unknown MOD_TARGET_GAME '${MOD_TARGET_GAME}'")
endif()

source_group(
	TREE "${SOURCE_DIR}/.."
	FILES
		${HEADER_FILES}
		${SOURCE_FILES}
)

add_library(
	${CURRENT_PROJECT}
	SHARED
		${HEADER_FILES}
		${SOURCE_FILES}
)

target_precompile_headers(
	${CURRENT_PROJECT}
    PRIVATE
        PCH.h
)

target_include_directories(
	${CURRENT_PROJECT}
	PRIVATE
		"${SOURCE_DIR}"
)

set_target_properties(
	${CURRENT_PROJECT}
	PROPERTIES
		OUTPUT_NAME "winhttp"
		MSVC_DEBUG_INFORMATION_FORMAT "ProgramDatabase"
)

#
# Compiler-specific options
#
target_compile_features(
	${CURRENT_PROJECT}
	PRIVATE
		cxx_std_23
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
	target_compile_options(
		${CURRENT_PROJECT}
		PRIVATE
			"/utf-8"
			"/sdl"
			"/permissive-"
			"/Zc:preprocessor"
			"/Zc:inline"
			"/EHsc"
			"/GR-"      # Disable runtime type information. This doesn't cover everything.

			"/W4"
			"/wd4100"	# '': unreferenced formal parameter
			"/wd4200"	# nonstandard extension used: zero-sized array in struct/union
			"/wd4201"	# nonstandard extension used: nameless struct/union
			"/wd4324"	# '': structure was padded due to alignment specifier
	)

	target_link_options(
		${CURRENT_PROJECT}
		PRIVATE
			# Force generate release PDBs: https://learn.microsoft.com/en-us/cpp/build/reference/debug-generate-debug-info
			"$<$<CONFIG:RELEASE>:/DEBUG:FULL;/OPT:REF;/OPT:ICF>"
			# Anonymize RSDS PDB paths
			"$<$<CONFIG:RELEASE>:/PDBALTPATH:$<TARGET_FILE_PREFIX:${CURRENT_PROJECT}>$<TARGET_FILE_BASE_NAME:${CURRENT_PROJECT}>.pdb>"
	)
endif()

target_compile_definitions(
	${CURRENT_PROJECT}
	PRIVATE
		NOMINMAX
		VC_EXTRALEAN
		WIN32_LEAN_AND_MEAN
		$<$<STREQUAL:${MOD_TARGET_GAME},CrimsonDesert>:MOD_GAME_CRIMSON_DESERT>
		$<$<STREQUAL:${MOD_TARGET_GAME},CrimsonDesert>:MOD_TARGET_EXE=L"CrimsonDesert.exe">
)

#
# Dependencies
#
# Spdlog
find_package(spdlog CONFIG REQUIRED)
target_link_libraries(${CURRENT_PROJECT} PRIVATE spdlog::spdlog)

# tomlplusplus
find_package(PkgConfig REQUIRED)
pkg_check_modules(tomlplusplus REQUIRED IMPORTED_TARGET tomlplusplus)

# Detours
find_package(detours CONFIG REQUIRED)
target_link_libraries(${CURRENT_PROJECT} PRIVATE detours::detours)

#
# Commands
#
if(NOT $ENV{GAME_ROOT_DIRECTORY} STREQUAL "")
	add_custom_command(
		TARGET ${CURRENT_PROJECT}
		POST_BUILD
		COMMAND
			"${CMAKE_COMMAND}" -E copy_if_different
			"$<TARGET_FILE:${CURRENT_PROJECT}>"
			"$ENV{GAME_ROOT_DIRECTORY}/$<TARGET_FILE_NAME:${CURRENT_PROJECT}>"
	)
endif()
