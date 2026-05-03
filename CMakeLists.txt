# Extracted from cpp directory
cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

set(CHESSTUBE_DEFAULT_VCPKG_ROOT "")
if(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    set(CHESSTUBE_DEFAULT_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
elseif(WIN32)
    set(CHESSTUBE_DEFAULT_VCPKG_ROOT "E:/vcpkg")
endif()
set(CHESSTUBE_VCPKG_ROOT "${CHESSTUBE_DEFAULT_VCPKG_ROOT}" CACHE PATH
    "Optional vcpkg root used for local default detection")

if(WIN32 AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND CHESSTUBE_VCPKG_ROOT AND
   EXISTS "${CHESSTUBE_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "${CHESSTUBE_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "Path to the vcpkg toolchain file")
endif()

if(WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    if(CHESSTUBE_VCPKG_ROOT AND
       EXISTS "${CHESSTUBE_VCPKG_ROOT}/installed/x64-windows-static/share/opencv4/OpenCVConfig.cmake")
        set(_default_vcpkg_triplet "x64-windows-static")
    elseif(CHESSTUBE_VCPKG_ROOT AND
           EXISTS "${CHESSTUBE_VCPKG_ROOT}/installed/x64-windows/share/opencv4/OpenCVConfig.cmake")
        set(_default_vcpkg_triplet "x64-windows")
    else()
        set(_default_vcpkg_triplet "x64-windows-static")
    endif()

    set(VCPKG_TARGET_TRIPLET "${_default_vcpkg_triplet}"
        CACHE STRING "Default vcpkg triplet for ChessTubeAnalyzer")
endif()

set(ENABLE_VCPKG_APPLOCAL_DEPS OFF CACHE BOOL
    "Let vcpkg copy runtime DLLs beside executables after builds")
set(ENABLE_CMAKE_AUTO_REGEN OFF CACHE BOOL
    "Let generated IDE projects automatically rerun CMake when build inputs change")
set(VCPKG_APPLOCAL_DEPS ${ENABLE_VCPKG_APPLOCAL_DEPS})
set(X_VCPKG_APPLOCAL_DEPS_INSTALL ${ENABLE_VCPKG_APPLOCAL_DEPS})
set(CMAKE_X_VCPKG_APPLOCAL_DEPS ${ENABLE_VCPKG_APPLOCAL_DEPS})

if(NOT ENABLE_CMAKE_AUTO_REGEN)
    set(CMAKE_SUPPRESS_REGENERATION ON)
endif()

project(ChessTubeAnalyzer LANGUAGES CXX)

if(MSVC AND DEFINED VCPKG_TARGET_TRIPLET)
    if(VCPKG_TARGET_TRIPLET MATCHES "(^|-)static($|-)")
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_PCH "Use precompiled headers to speed up rebuilds" ON)
option(ENABLE_UNITY_BUILD "Use CMake unity builds for faster full builds" OFF)
option(BUILD_GUI "Build the Qt GUI target" ON)
option(ENABLE_QT_RUNTIME_DEPLOYMENT "Copy Qt/vcpkg runtime files after GUI builds" OFF)
option(ENABLE_SYSTEM_CUDA "Enable direct system CUDA/NPP integration when available" ON)
option(ENABLE_IPO "Enable interprocedural optimization in Release when supported" OFF)

if(ENABLE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
endif()

if(MSVC)
    # Keep parallel PDB writes enabled while letting the runtime library match the active vcpkg triplet.
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /FS")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /FS /Z7")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /FS")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /FS /Z7")
endif()

function(enable_msvc_parallel_pdb_safety target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /FS)
    endif()
endfunction()

function(enable_msvc_compile_speedups target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /MP)
    endif()
endfunction()

function(enforce_project_msvc_runtime target_name)
    if(MSVC AND DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set_property(TARGET ${target_name} PROPERTY
            MSVC_RUNTIME_LIBRARY "${CMAKE_MSVC_RUNTIME_LIBRARY}")
    endif()
endfunction()

function(enable_project_pch target_name pch_profile)
    if(NOT ENABLE_PCH)
        return()
    endif()

    if(pch_profile STREQUAL "core")
        target_precompile_headers(${target_name} PRIVATE
            <algorithm>
            <array>
            <atomic>
            <chrono>
            <cmath>
            <filesystem>
            <functional>
            <map>
            <memory>
            <optional>
            <string>
            <string_view>
            <thread>
            <tuple>
            <utility>
            <vector>
            <opencv2/opencv.hpp>
        )
    elseif(pch_profile STREQUAL "gui")
        target_precompile_headers(${target_name} PRIVATE
            <algorithm>
            <atomic>
            <functional>
            <map>
            <memory>
            <optional>
            <string>
            <vector>
            <QApplication>
            <QComboBox>
            <QDir>
            <QFileInfo>
            <QGroupBox>
            <QLabel>
            <QLineEdit>
            <QMainWindow>
            <QObject>
            <QProgressBar>
            <QPushButton>
            <QSettings>
            <QString>
            <QTextEdit>
            <QThread>
            <QWidget>
            <opencv2/opencv.hpp>
        )
    elseif(pch_profile STREQUAL "test")
        target_precompile_headers(${target_name} PRIVATE
            <algorithm>
            <chrono>
            <filesystem>
            <iomanip>
            <iostream>
            <string>
            <vector>
            <gtest/gtest.h>
            <opencv2/opencv.hpp>
        )
    endif()
endfunction()

function(configure_project_stack_size target_name)
    if(MSVC)
        set_target_properties(${target_name} PROPERTIES
            LINK_FLAGS "/STACK:8388608")
    endif()
endfunction()

function(configure_cuda_target target_name)
    if(NOT HAVE_SYSTEM_CUDA)
        return()
    endif()

    target_compile_definitions(${target_name} PRIVATE HAVE_SYSTEM_CUDA)
    target_include_directories(${target_name} PRIVATE "${SYSTEM_CUDA_PATH}/include")

    if(WIN32)
        target_link_directories(${target_name} PRIVATE "${SYSTEM_CUDA_PATH}/lib/x64")
        target_link_options(${target_name} PRIVATE ${CUDA_DELAY_LOADS_LIST})
        target_link_libraries(${target_name} PRIVATE delayimp.lib
            cudart nppc nppial nppicc nppidei nppif nppig nppim nppist nppitc)
    endif()
endfunction()

# Enable Link-Time Optimization (LTO) for Release builds
if(ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        message(STATUS "LTO/IPO enabled for Release builds")
    else()
        message(STATUS "LTO/IPO not supported: ${ipo_error}")
    endif()
endif()

# ── Dependencies from system vcpkg ───────────────────────────────────────────
find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

if(BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    message(STATUS "Qt6 found, GUI will be built")
endif()

if(BUILD_GUI AND ENABLE_QT_RUNTIME_DEPLOYMENT AND WIN32 AND
   DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "[Paths]\nPlugins = Qt6/plugins\n")
    function(add_runtime_deployment target_name)
        set(_qt_runtime_triplet "${VCPKG_TARGET_TRIPLET}")
        if(_qt_runtime_triplet MATCHES "-static")
            message(STATUS "Skipping Qt runtime deployment for ${target_name}: static vcpkg triplet '${VCPKG_TARGET_TRIPLET}' does not need copied Qt runtime DLLs")
            return()
        endif()

        set(_qt_runtime_root "${VCPKG_INSTALLED_DIR}/${_qt_runtime_triplet}")
        if(NOT EXISTS "${_qt_runtime_root}/bin")
            message(STATUS "Skipping Qt runtime deployment for ${target_name}: '${_qt_runtime_root}' does not contain runtime binaries")
            return()
        endif()

        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_qt_runtime_root}/bin" "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_qt_runtime_root}/debug/bin" "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_qt_runtime_root}/Qt6/plugins" "$<TARGET_FILE_DIR:${target_name}>/Qt6/plugins"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_qt_runtime_root}/debug/Qt6/plugins" "$<TARGET_FILE_DIR:${target_name}>/Qt6/plugins"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "$<TARGET_FILE_DIR:${target_name}>/qt.conf"
            VERBATIM)
    endfunction()
endif()
# ── Optional CUDA/NPP support (system CUDA SDK, NOT vcpkg) ────────────────────
set(CHESSTUBE_DEFAULT_CUDA_PATH "")
if(DEFINED ENV{CUDA_PATH} AND NOT "$ENV{CUDA_PATH}" STREQUAL "")
    set(CHESSTUBE_DEFAULT_CUDA_PATH "$ENV{CUDA_PATH}")
elseif(WIN32)
    set(CHESSTUBE_DEFAULT_CUDA_PATH "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2")
endif()
set(SYSTEM_CUDA_PATH "${CHESSTUBE_DEFAULT_CUDA_PATH}" CACHE PATH
    "Optional system CUDA toolkit root for direct NPP integration")
set(HAVE_SYSTEM_CUDA FALSE)

if(ENABLE_SYSTEM_CUDA AND WIN32 AND SYSTEM_CUDA_PATH AND
   EXISTS "${SYSTEM_CUDA_PATH}/include/cuda_runtime.h" AND
   EXISTS "${SYSTEM_CUDA_PATH}/lib/x64/cudart.lib" AND
   EXISTS "${SYSTEM_CUDA_PATH}/lib/x64/nppial.lib")
    set(HAVE_SYSTEM_CUDA TRUE)
    message(STATUS "GPU Acceleration: system CUDA toolkit found - NPP ops enabled")
    message(STATUS "  CUDA path: ${SYSTEM_CUDA_PATH}")
else()
    message(STATUS "GPU Acceleration: system CUDA not found or disabled - CPU fallback mode")
endif()

# ── Build libchess from source (ensures matching runtime library) ─────────────
set(CHESSTUBE_DEFAULT_LIBCHESS_ROOT "")
if(DEFINED ENV{LIBCHESS_ROOT} AND NOT "$ENV{LIBCHESS_ROOT}" STREQUAL "")
    set(CHESSTUBE_DEFAULT_LIBCHESS_ROOT "$ENV{LIBCHESS_ROOT}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchess/src")
    set(CHESSTUBE_DEFAULT_LIBCHESS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchess")
elseif(WIN32)
    set(CHESSTUBE_DEFAULT_LIBCHESS_ROOT "E:/libchess")
endif()

set(LIBCHESS_ROOT "${CHESSTUBE_DEFAULT_LIBCHESS_ROOT}" CACHE PATH
    "Path to the libchess source tree root")
set(LIBCHESS_SRC_DIR "${LIBCHESS_ROOT}/src" CACHE PATH
    "Path to libchess source files")
set(LIBCHESS_INCLUDE_DIR "${LIBCHESS_ROOT}/dist/include" CACHE PATH
    "Path to libchess public headers")

if(NOT EXISTS "${LIBCHESS_SRC_DIR}/set_fen.cpp")
    message(FATAL_ERROR
        "libchess sources were not found. Set LIBCHESS_ROOT or LIBCHESS_SRC_DIR to a valid libchess checkout.")
endif()

if(NOT EXISTS "${LIBCHESS_INCLUDE_DIR}")
    message(FATAL_ERROR
        "libchess headers were not found. Set LIBCHESS_INCLUDE_DIR to the libchess include directory.")
endif()

add_library(chess STATIC
    ${LIBCHESS_SRC_DIR}/set_fen.cpp
    ${LIBCHESS_SRC_DIR}/get_fen.cpp
    ${LIBCHESS_SRC_DIR}/legal_moves.cpp
    ${LIBCHESS_SRC_DIR}/legal_captures.cpp
    ${LIBCHESS_SRC_DIR}/legal_noncaptures.cpp
    ${LIBCHESS_SRC_DIR}/makemove.cpp
    ${LIBCHESS_SRC_DIR}/undomove.cpp
    ${LIBCHESS_SRC_DIR}/movegen.cpp
    ${LIBCHESS_SRC_DIR}/king_allowed.cpp
    ${LIBCHESS_SRC_DIR}/checkers.cpp
    ${LIBCHESS_SRC_DIR}/check_evasions.cpp
    ${LIBCHESS_SRC_DIR}/squares_attacked.cpp
    ${LIBCHESS_SRC_DIR}/square_attacked.cpp
    ${LIBCHESS_SRC_DIR}/pinned.cpp
    ${LIBCHESS_SRC_DIR}/attackers.cpp
    ${LIBCHESS_SRC_DIR}/is_legal.cpp
    ${LIBCHESS_SRC_DIR}/valid.cpp
    ${LIBCHESS_SRC_DIR}/count_moves.cpp
    ${LIBCHESS_SRC_DIR}/perft.cpp
    ${LIBCHESS_SRC_DIR}/zobrist.cpp
    ${LIBCHESS_SRC_DIR}/predict_hash.cpp
)

target_include_directories(chess PUBLIC
    ${LIBCHESS_INCLUDE_DIR}
    ${LIBCHESS_SRC_DIR}
)

enforce_project_msvc_runtime(chess)

# ── Extraction Core (video decoding, CV, move verification) ─────────────────
add_library(chess_extraction_core
    src/BoardLocalizer.cpp
    src/BoardAnalysis.cpp
    src/ArrowDetector.cpp
    src/ClockRecognizer.cpp
    src/ChessVideoExtractor.cpp
    src/ExtractorUtils.cpp
    src/MoveValidations.cpp
    src/GPUAccelerator.cpp
    src/FramePrefetcher.cpp
)

target_include_directories(chess_extraction_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chess_extraction_core PUBLIC
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    chess
)

enforce_project_msvc_runtime(chess_extraction_core)
enable_msvc_parallel_pdb_safety(chess_extraction_core)
enable_msvc_compile_speedups(chess_extraction_core)
enable_project_pch(chess_extraction_core core)

configure_cuda_target(chess_extraction_core)

# ── Analysis/Postprocess Core (PGN, Stockfish, overlay video) ───────────────
add_library(chess_analysis_core
    src/StockfishAnalyzer.cpp
    src/AnalysisVideoGenerator.cpp
    src/PgnWriter.cpp
    src/ChessFenUtils.cpp
    src/ImageWriteUtils.cpp
    src/AnalysisVideoRenderUtils.cpp
    include/FFmpegFilterGraph.cpp
)

target_include_directories(chess_analysis_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chess_analysis_core PUBLIC
    chess_extraction_core
)

enforce_project_msvc_runtime(chess_analysis_core)
enable_msvc_parallel_pdb_safety(chess_analysis_core)
enable_msvc_compile_speedups(chess_analysis_core)
enable_project_pch(chess_analysis_core core)

configure_cuda_target(chess_analysis_core)

# ── Main executable (CLI) ────────────────────────────────────────────────────
add_executable(extract_moves
    src/main.cpp
)

target_include_directories(extract_moves PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(HAVE_SYSTEM_CUDA AND WIN32)
    target_link_directories(extract_moves PRIVATE "${SYSTEM_CUDA_PATH}/lib/x64")
    
    # Dynamically find the exact names of the CUDA runtime DLLs to delay-load.
    # This prevents 0xc0000135 crashes caused by version suffix mismatches 
    # (e.g., if the .lib imports nppc64_132.dll instead of nppc64_13.dll).
    file(GLOB CUDA_DLL_PATHS 
        "${SYSTEM_CUDA_PATH}/bin/cudart64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppc64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppial64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppicc64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppidei64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppif64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppig64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppim64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppist64_*.dll"
        "${SYSTEM_CUDA_PATH}/bin/nppitc64_*.dll"
    )
    
    set(CUDA_DELAY_LOADS_LIST "")
    if(CUDA_DLL_PATHS)
        foreach(DLL_PATH IN LISTS CUDA_DLL_PATHS)
            get_filename_component(DLL_NAME "${DLL_PATH}" NAME)
            list(APPEND CUDA_DELAY_LOADS_LIST "/DELAYLOAD:${DLL_NAME}")
        endforeach()
    else()
        # Fallback: if GLOB fails (e.g., due to permissions or path formats), 
        # guarantee the delay-load flags are populated for CUDA 13.
        set(CUDA_DELAY_LOADS_LIST 
            "/DELAYLOAD:cudart64_13.dll" "/DELAYLOAD:nppc64_13.dll"
            "/DELAYLOAD:nppial64_13.dll" "/DELAYLOAD:nppicc64_13.dll"
            "/DELAYLOAD:nppidei64_13.dll" "/DELAYLOAD:nppif64_13.dll"
            "/DELAYLOAD:nppig64_13.dll" "/DELAYLOAD:nppim64_13.dll"
            "/DELAYLOAD:nppist64_13.dll" "/DELAYLOAD:nppitc64_13.dll")
    endif()

    # Suppress LNK4199 warning when a DLL is delay-loaded but no symbols are imported from it
    list(APPEND CUDA_DELAY_LOADS_LIST "/IGNORE:4199")

endif()

configure_cuda_target(extract_moves)

target_link_libraries(extract_moves PRIVATE
    chess_extraction_core
    chess_analysis_core
    CLI11::CLI11
)

enforce_project_msvc_runtime(extract_moves)
enable_msvc_parallel_pdb_safety(extract_moves)
enable_msvc_compile_speedups(extract_moves)
enable_project_pch(extract_moves core)

configure_project_stack_size(extract_moves)

# ── GUI Executable ───────────────────────────────────────────────────────────
if(Qt6_FOUND AND BUILD_GUI)
    set(CHESSTUBE_GUI_MOC_HEADERS
        include/MainWindow.h
        include/SettingsDialog.h
        include/VideoProcessorWorker.h
        include/ToggleSwitch.h
        include/OverlayEditorDialog.h
    )

    add_executable(analyzer_gui
        src/main_gui.cpp
        src/MainWindow.cpp
        src/MainWindow_UI.cpp
        src/MainWindow_Settings.cpp
        include/MainWindow.h
        src/SettingsDialog.cpp
        include/SettingsDialog.h
        src/VideoProcessorWorker.cpp
        include/VideoProcessorWorker.h
        src/ToggleSwitch.cpp
        include/ToggleSwitch.h
        src/ThemeManager.cpp
        include/ThemeManager.h
        src/OverlayEditorDialog.cpp
        include/OverlayEditorDialog.h
        src/TemplateManager.cpp
        include/TemplateManager.h
    )

    qt6_wrap_cpp(analyzer_gui_moc_sources TARGET analyzer_gui ${CHESSTUBE_GUI_MOC_HEADERS})
    target_sources(analyzer_gui PRIVATE ${analyzer_gui_moc_sources})

    target_include_directories(analyzer_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(analyzer_gui PRIVATE
        chess_extraction_core
        chess_analysis_core
        Qt6::Widgets
    )

    enforce_project_msvc_runtime(analyzer_gui)
    enable_msvc_parallel_pdb_safety(analyzer_gui)
    enable_msvc_compile_speedups(analyzer_gui)
    enable_project_pch(analyzer_gui gui)

    configure_cuda_target(analyzer_gui)

    set_target_properties(analyzer_gui PROPERTIES
        OUTPUT_NAME "ChessTube Analyzer"
        AUTOMOC OFF
        AUTORCC OFF
        AUTOUIC OFF
    )
    if(WIN32)
        # set_target_properties(analyzer_gui PROPERTIES WIN32_EXECUTABLE TRUE) # Temporarily disabled to debug startup crashes
    endif()
    configure_project_stack_size(analyzer_gui)

    if(ENABLE_QT_RUNTIME_DEPLOYMENT)
        add_runtime_deployment(analyzer_gui)
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build Google Test unit tests" OFF)

if(BUILD_TESTS)
    # FetchContent sub-builds cache the generator they were created with.
    # If the active generator changes, CMake will refuse to reuse that stale
    # nested build tree, so clear it automatically and let FetchContent rebuild.
    set(GOOGLETEST_SUBBUILD_DIR "${CMAKE_BINARY_DIR}/_deps/googletest-subbuild")
    set(GOOGLETEST_SUBBUILD_CACHE "${GOOGLETEST_SUBBUILD_DIR}/CMakeCache.txt")
    if(EXISTS "${GOOGLETEST_SUBBUILD_CACHE}")
        file(READ "${GOOGLETEST_SUBBUILD_CACHE}" GOOGLETEST_SUBBUILD_CACHE_TEXT)
        string(REGEX MATCH "CMAKE_GENERATOR:INTERNAL=([^\r\n]+)" _gtest_generator_match "${GOOGLETEST_SUBBUILD_CACHE_TEXT}")
        set(GOOGLETEST_SUBBUILD_GENERATOR "${CMAKE_MATCH_1}")
        if(NOT GOOGLETEST_SUBBUILD_GENERATOR STREQUAL CMAKE_GENERATOR)
            message(STATUS "Removing stale googletest sub-build generated by '${GOOGLETEST_SUBBUILD_GENERATOR}' (current generator: '${CMAKE_GENERATOR}')")
            file(REMOVE_RECURSE "${GOOGLETEST_SUBBUILD_DIR}")
        endif()
    endif()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(test_extract_moves
        tests/test_ui_detectors.cpp
    )

    target_include_directories(test_extract_moves PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    configure_cuda_target(test_extract_moves)

    target_link_libraries(test_extract_moves PRIVATE
        chess_extraction_core
        GTest::gtest_main
        GTest::gmock_main
    )

    enforce_project_msvc_runtime(test_extract_moves)
    enable_msvc_parallel_pdb_safety(test_extract_moves)
    enable_msvc_compile_speedups(test_extract_moves)
    enable_project_pch(test_extract_moves test)

    configure_project_stack_size(test_extract_moves)

    include(GoogleTest)
    gtest_discover_tests(test_extract_moves)
endif()
