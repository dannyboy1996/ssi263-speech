# SSInger VST3 — JUCE 9.0.3 build
cmake_minimum_required(VERSION 3.22)
project(SSInger VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

option(SSINGER_BUILD_TESTS "Build offline C tests (no JUCE needed)" ON)
option(SSINGER_HAVE_M6502 "Enable real 6502 stepping (needs third_party/chips)" OFF)

# --- JUCE 9.0.3, fetched once at configure time ---------------------------
include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 9.0.3
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# --- floooh chips (m6502.h) -------------------------------------------------
# Clone once: git clone --depth 1 https://github.com/floooh/chips.git third_party/chips
if(SSINGER_HAVE_M6502)
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chips/chips/m6502.h")
    message(FATAL_ERROR "SSINGER_HAVE_M6502=ON but third_party/chips/chips/m6502.h missing. See third_party/README.md.")
  endif()
endif()

# --- The SSI-263 engine (this repo's C core, reused as-is) ------------------
set(SSI263_CSRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/csrc" CACHE PATH "ssi263 C core")
add_library(ssi263 STATIC "${SSI263_CSRC_DIR}/ssi263.c" "${SSI263_CSRC_DIR}/ssi263dsp.c")
target_include_directories(ssi263 PUBLIC "${SSI263_CSRC_DIR}")
target_compile_options(ssi263 PRIVATE $<$<C_COMPILER_ID:GNU,Clang>:-ffp-contract=off>)
if(UNIX)
  target_link_libraries(ssi263 PUBLIC m)
endif()

# --- The plugin --------------------------------------------------------------
juce_add_plugin(SSInger
  COMPANY_NAME "ssi263-speech"
  PLUGIN_MANUFACTURER_CODE S263
  PLUGIN_CODE Ssng
  FORMATS VST3 Standalone
  IS_SYNTH TRUE
  PRODUCT_NAME "SSInger"
  NEEDS_MIDI_INPUT TRUE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  VST3_CATEGORIES "Instrument" "Synth" "Vocal"
  DESCRIPTION "SSInger: SC-02 singing voice after the Robovox patent (EP0396141A2)")

target_sources(SSInger PRIVATE
  plugin/SSIngerProcessor.cpp)

target_include_directories(SSInger PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${SSI263_CSRC_DIR}")

if(SSINGER_HAVE_M6502)
  target_include_directories(SSInger PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chips")
  target_compile_definitions(SSInger PRIVATE SSINGER_HAVE_M6502=1)
endif()

target_compile_definitions(SSInger PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SSInger PRIVATE
  ssi263
  juce::juce_audio_utils
  juce::juce_audio_processors)

# --- Offline tests (plain C, no JUCE) -----------------------------------------
if(SSINGER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
