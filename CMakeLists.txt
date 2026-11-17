# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 The StudioRider authors
#
# StudioRider Receiver: plays a StudioRider listening link inside a DAW (AU + VST3).
cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_OSX_DEPLOYMENT_TARGET) # must be set before project(); ignored outside macOS
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")
endif()

project(StudioRiderReceiver VERSION 1.0.0 LANGUAGES C CXX)

option(STUDIORIDER_RECEIVER_BUILD_PLUGIN "Build the plugin (needs JUCE)" ON)
# The tests run a real StudioRider server: they need StudioRider's sources
# (scripts/fetch_test_server.sh puts them in third_party/StudioRider).
option(STUDIORIDER_RECEIVER_BUILD_TESTS "Build the tests (needs StudioRider's sources)" OFF)
set(STUDIORIDER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/StudioRider" CACHE PATH "StudioRider sources (tests only)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

find_package(Threads REQUIRED)

# --- Decoders: libFLAC (BSD), libopus (BSD) ----------------------------------
if(STUDIORIDER_RECEIVER_BUILD_TESTS)
    # StudioRider's own build provides libFLAC, libopus and its server (studiorider_core).
    set(STUDIORIDER_BUILD_PLUGIN OFF CACHE BOOL "" FORCE)
    add_subdirectory(${STUDIORIDER_SOURCE_DIR} studiorider EXCLUDE_FROM_ALL)
else()
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    foreach(_opt BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS BUILD_UTILS
                 INSTALL_MANPAGES INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG
                 OPUS_BUILD_TESTING OPUS_BUILD_PROGRAMS OPUS_INSTALL_PKG_CONFIG_MODULE
                 OPUS_INSTALL_CMAKE_CONFIG_MODULE OPUS_DRED OPUS_OSCE OPUS_DEEP_PLC)
        set(${_opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
        # Universal build: portable C code in Opus and libFLAC (plenty fast for one stream).
        set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
        foreach(_v FLAC__HAS_X86INTRIN FLAC__HAS_NEONINTRIN FLAC__HAS_A64NEONINTRIN HAVE_CPUID_H)
            set(${_v} 0 CACHE INTERNAL "")
        endforeach()
    elseif(APPLE AND CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
        set(OPUS_PRESUME_NEON ON CACHE BOOL "" FORCE)
        set(OPUS_MAY_HAVE_NEON ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(third_party/flac EXCLUDE_FROM_ALL)
    add_subdirectory(third_party/opus EXCLUDE_FROM_ALL)
    target_compile_definitions(opus PRIVATE OPUS_EXPORT=)
endif()

# --- Receiver core (no JUCE): WebSocket client, decoders, resampler, buffer ---
set(RECEIVER_CORE_SOURCES
    Source/StreamDecoder.cpp
    Source/Resampler.cpp
    Source/ReceiverEngine.cpp
    Source/WebSocketClientPlain.cpp)
if(APPLE)
    list(APPEND RECEIVER_CORE_SOURCES Source/WebSocketClientApple.mm)
    set_source_files_properties(Source/WebSocketClientApple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()
add_library(studiorider_receiver_core STATIC ${RECEIVER_CORE_SOURCES})
target_include_directories(studiorider_receiver_core PUBLIC Source)
target_link_libraries(studiorider_receiver_core PRIVATE FLAC::FLAC opus PUBLIC Threads::Threads)
if(APPLE)
    target_link_libraries(studiorider_receiver_core PUBLIC "-framework Foundation")
endif()

if(STUDIORIDER_RECEIVER_BUILD_TESTS)
    add_executable(studiorider_receiver_test tests/receiver_test.cpp)
    target_link_libraries(studiorider_receiver_test PRIVATE studiorider_receiver_core studiorider_core)
endif()

# --- JUCE plugin ---------------------------------------------------------------
if(STUDIORIDER_RECEIVER_BUILD_PLUGIN)
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE")
    if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "JUCE not found in ${JUCE_DIR} (run build.sh, which downloads it).")
    endif()
    add_subdirectory(${JUCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/JUCE)

    if(APPLE)
        set(RECEIVER_FORMATS AU VST3)
    else()
        set(RECEIVER_FORMATS VST3)
    endif()

    juce_add_plugin(StudioRiderReceiver
        PRODUCT_NAME "StudioRider Receiver"
        COMPANY_NAME "StudioRider"
        BUNDLE_ID "net.studiorider.receiver"
        VERSION ${PROJECT_VERSION}
        PLUGIN_MANUFACTURER_CODE Mxsr   # same manufacturer as StudioRider
        PLUGIN_CODE Mxrv
        FORMATS ${RECEIVER_FORMATS}
        IS_SYNTH FALSE
        # A "MIDI-controlled effect" (AU type aumf): it can also be loaded as the
        # instrument of an instrument track. Some DAWs never run a plain effect
        # (aufx) on an audio or aux track that carries no signal, whatever its
        # tail; they always run an instrument track. The plugin ignores MIDI.
        NEEDS_MIDI_INPUT TRUE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS TRUE
        COPY_PLUGIN_AFTER_BUILD FALSE
        AU_MAIN_TYPE kAudioUnitType_MusicEffect
        VST3_CATEGORIES Fx Generator Tools Network
        DESCRIPTION "Plays a StudioRider stream inside your DAW, to monitor or record it")

    juce_add_binary_data(StudioRiderReceiverAssets
        HEADER_NAME ReceiverBinaryData.h
        NAMESPACE ReceiverBinaryData
        SOURCES assets/logo.png)
    set_target_properties(StudioRiderReceiverAssets PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_sources(StudioRiderReceiver PRIVATE
        Source/ReceiverProcessor.cpp
        Source/ReceiverEditor.cpp)

    target_compile_definitions(StudioRiderReceiver PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_REPORT_APP_USAGE=0
        JUCE_USE_FLAC=0
        JUCE_USE_OGGVORBIS=0)

    target_link_libraries(StudioRiderReceiver
        PRIVATE
            studiorider_receiver_core
            StudioRiderReceiverAssets
            juce::juce_audio_utils
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)

    if(STUDIORIDER_RECEIVER_BUILD_TESTS)
        # State, real stream, "not processed", window screenshots.
        add_executable(studiorider_receiver_plugin_test tests/receiver_plugin_test.cpp)
        target_include_directories(studiorider_receiver_plugin_test PRIVATE Source
                                   $<TARGET_PROPERTY:StudioRiderReceiver,INCLUDE_DIRECTORIES>)
        target_compile_definitions(studiorider_receiver_plugin_test PRIVATE
                                   $<TARGET_PROPERTY:StudioRiderReceiver,COMPILE_DEFINITIONS>)
        target_link_libraries(studiorider_receiver_plugin_test PRIVATE StudioRiderReceiver studiorider_core)
    endif()
endif()
