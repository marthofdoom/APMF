# APMF native plugin — build structure mirrors MFO's (which copied it from the
# SkyrimDev CommonLibSSE-NG HelloWorld template). Doctrine: adapt a working
# source, never write a build system from memory. Same colorglass CommonLibSSE-NG
# registry + baseline as MFO, so APMF builds against the identical CommonLib.
cmake_minimum_required(VERSION 3.21)

project(APMF VERSION 0.1.0 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)

# GLOB on purpose: the modular channel design means "add a channel = drop ONE
# new .cpp under channels/ and touch NOTHING else" (marth's hard requirement).
# CONFIGURE_DEPENDS re-globs on build so a new file is picked up without editing
# this list. Every .cpp (plugin.cpp + core/* + channels/*) links DIRECTLY into
# the DLL target -- NOT via a static archive -- so each channel's self-
# registration static initializer is retained (see Docs/INVARIANTS.md #9).
file(GLOB_RECURSE APMF_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_commonlibsse_plugin(${PROJECT_NAME} SOURCES ${APMF_SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE PCH.h)

# Emit a PDB even in Release so a field crash symbolizes to a source line, and
# so the tester can [bc]-pin any CTD. /Zi adds debug info; /DEBUG writes the
# .pdb; /OPT:REF,ICF undo the size cost /DEBUG would otherwise leave on.
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zi>)
target_link_options(${PROJECT_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/DEBUG /OPT:REF /OPT:ICF>)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
