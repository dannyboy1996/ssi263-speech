# Offline C tests: no JUCE, no network. Usable standalone:
#   gcc -std=c99 -I <robovox> -I <csrc> test_6850_midi.c <csrc>/ssi263.c -lm
cmake_minimum_required(VERSION 3.22)
project(RobovoxTests LANGUAGES C CXX)
enable_testing()

if(NOT DEFINED SSI263_CSRC_DIR)
  set(SSI263_CSRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../src/csrc")
endif()

add_executable(test_6850_midi test_6850_midi.c
  "${SSI263_CSRC_DIR}/ssi263.c" "${SSI263_CSRC_DIR}/ssi263dsp.c")
target_include_directories(test_6850_midi PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${SSI263_CSRC_DIR}")
target_compile_options(test_6850_midi PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang>:-std=c99 -Wall -Wextra -ffp-contract=off>)
if(UNIX)
  target_link_libraries(test_6850_midi PRIVATE m)
endif()

add_test(NAME acia6850_translator_bus_engine COMMAND test_6850_midi)

# The emulator headers ship to C++ (JUCE plugin). Compile-only check.
add_library(robovox_cxxcheck STATIC cxxcheck.cpp)
target_include_directories(robovox_cxxcheck PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${SSI263_CSRC_DIR}")
target_compile_features(robovox_cxxcheck PRIVATE cxx_std_20)
