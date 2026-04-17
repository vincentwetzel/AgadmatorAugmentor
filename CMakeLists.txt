# Extracted from cpp directory
cmake_minimum_required(VERSION 3.20)
project(ChessTubeAnalyzer LANGUAGES CXX)

set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON)
set(CMAKE_X_VCPKG_APPLOCAL_DEPS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_PCH "Use precompiled headers to speed up rebuilds" ON)
option(ENABLE_UNITY_BUILD "Use CMake unity builds for faster full builds" OFF)
option(BUILD_GUI "Build the Qt GUI target" ON)
option(ENABLE_QT_RUNTIME_DEPLOYMENT "Copy Qt/vcpkg runtime files after GUI builds" ON)

if(ENABLE_UNITY_BUILD)
    set(CMAKE_UNITY_BUILD ON)
endif()

# Force static runtime to avoid api-ms-win-crt-* dependency issues
# This must be set BEFORE project() to take effect
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT /FS")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd /FS /Z7")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT /FS")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd /FS /Z7")

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

# Enable Link-Time Optimization (LTO) for Release builds
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    message(STATUS "LTO/IPO enabled for Release builds")
else()
    message(STATUS "LTO/IPO not supported: ${ipo_error}")
endif()

# ── Dependencies from system vcpkg ───────────────────────────────────────────
find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

find_package(Qt6 COMPONENTS Widgets)
if(Qt6_FOUND AND BUILD_GUI)
    message(STATUS "Qt6 found, GUI will be built")
elseif(Qt6_FOUND)
    message(STATUS "Qt6 found, GUI build disabled by BUILD_GUI=OFF")
else()
    message(STATUS "Qt6 not found. Install Qt to build the GUI")
endif()

if(Qt6_FOUND AND BUILD_GUI AND ENABLE_QT_RUNTIME_DEPLOYMENT)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "[Paths]\nPlugins = Qt6/plugins\n")
    function(add_runtime_deployment target_name)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${VCPKG_INSTALLED_DIR}/x64-windows/bin" "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${VCPKG_INSTALLED_DIR}/x64-windows/debug/bin" "$<TARGET_FILE_DIR:${target_name}>"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${VCPKG_INSTALLED_DIR}/x64-windows/Qt6/plugins" "$<TARGET_FILE_DIR:${target_name}>/Qt6/plugins"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory "${VCPKG_INSTALLED_DIR}/x64-windows/debug/Qt6/plugins" "$<TARGET_FILE_DIR:${target_name}>/Qt6/plugins"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/qt.conf" "$<TARGET_FILE_DIR:${target_name}>/qt.conf"
            VERBATIM)
    endfunction()
endif()
# ── Optional CUDA/NPP support (system CUDA SDK, NOT vcpkg) ────────────────────
# Detects system CUDA 13.2 at:
#   C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2
# Links cudart.lib + npp*.lib directly — no need to rebuild OpenCV.
# When found, HAVE_SYSTEM_CUDA is defined and GPU ops use NPP directly.
set(SYSTEM_CUDA_PATH "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2")
set(HAVE_SYSTEM_CUDA FALSE)

if(EXISTS "${SYSTEM_CUDA_PATH}/include/cuda_runtime.h" AND
   EXISTS "${SYSTEM_CUDA_PATH}/lib/x64/cudart.lib" AND
   EXISTS "${SYSTEM_CUDA_PATH}/lib/x64/nppial.lib")
    set(HAVE_SYSTEM_CUDA TRUE)
    message(STATUS "GPU Acceleration: System CUDA 13.2 found — NPP ops enabled")
    message(STATUS "  CUDA path: ${SYSTEM_CUDA_PATH}")
else()
    message(STATUS "GPU Acceleration: System CUDA not found — CPU fallback mode")
endif()

# ── Build libchess from source (ensures matching runtime library) ─────────────
set(LIBCHESS_SRC_DIR "E:/libchess/src")

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
    E:/libchess/dist/include # Restore original path for libchess headers
    ${LIBCHESS_SRC_DIR}
)

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

enable_msvc_parallel_pdb_safety(chess_extraction_core)
enable_msvc_compile_speedups(chess_extraction_core)
enable_project_pch(chess_extraction_core core)

if(HAVE_SYSTEM_CUDA)
    target_compile_definitions(chess_extraction_core PRIVATE HAVE_SYSTEM_CUDA)
    target_include_directories(chess_extraction_core PRIVATE "${SYSTEM_CUDA_PATH}/include")
endif()

# ── Analysis/Postprocess Core (PGN, Stockfish, overlay video) ───────────────
add_library(chess_analysis_core
    src/StockfishAnalyzer.cpp
    src/AnalysisVideoGenerator.cpp
    src/PgnWriter.cpp
    include/FFmpegFilterGraph.cpp
)

target_include_directories(chess_analysis_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chess_analysis_core PUBLIC
    chess_extraction_core
)

enable_msvc_parallel_pdb_safety(chess_analysis_core)
enable_msvc_compile_speedups(chess_analysis_core)
enable_project_pch(chess_analysis_core core)

if(HAVE_SYSTEM_CUDA)
    target_compile_definitions(chess_analysis_core PRIVATE HAVE_SYSTEM_CUDA)
    target_include_directories(chess_analysis_core PRIVATE "${SYSTEM_CUDA_PATH}/include")
endif()

# ── Main executable (CLI) ────────────────────────────────────────────────────
add_executable(extract_moves
    src/main.cpp
)

target_include_directories(extract_moves PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(HAVE_SYSTEM_CUDA)
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

    target_link_options(extract_moves PRIVATE ${CUDA_DELAY_LOADS_LIST})
    target_link_libraries(extract_moves PRIVATE delayimp.lib
        cudart nppc nppial nppicc nppidei nppif nppig nppim nppist nppitc)
endif()

target_link_libraries(extract_moves PRIVATE
    chess_extraction_core
    chess_analysis_core
    CLI11::CLI11
)

enable_msvc_parallel_pdb_safety(extract_moves)
enable_msvc_compile_speedups(extract_moves)
enable_project_pch(extract_moves core)

# Set stack size to 8MB (default is 1MB)
set_target_properties(extract_moves PROPERTIES
    LINK_FLAGS "/STACK:8388608"
)

# ── GUI Executable ───────────────────────────────────────────────────────────
if(Qt6_FOUND AND BUILD_GUI)
    add_executable(analyzer_gui WIN32
        src/main_gui.cpp
        src/MainWindow.cpp
        include/MainWindow.h
        src/VideoProcessorWorker.cpp
        include/VideoProcessorWorker.h
        src/ToggleSwitch.cpp
        include/ToggleSwitch.h
        src/ThemeManager.cpp
        include/ThemeManager.h
    )

    target_include_directories(analyzer_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(analyzer_gui PRIVATE
        chess_extraction_core
        chess_analysis_core
        Qt6::Widgets
        CLI11::CLI11
    )

    enable_msvc_parallel_pdb_safety(analyzer_gui)
    enable_msvc_compile_speedups(analyzer_gui)
    enable_project_pch(analyzer_gui gui)

    if(HAVE_SYSTEM_CUDA)
        target_compile_definitions(analyzer_gui PRIVATE HAVE_SYSTEM_CUDA)
        target_include_directories(analyzer_gui PRIVATE "${SYSTEM_CUDA_PATH}/include")
        target_link_directories(analyzer_gui PRIVATE "${SYSTEM_CUDA_PATH}/lib/x64")
        target_link_options(analyzer_gui PRIVATE ${CUDA_DELAY_LOADS_LIST})
        target_link_libraries(analyzer_gui PRIVATE delayimp.lib
            cudart nppc nppial nppicc nppidei nppif nppig nppim nppist nppitc)
    endif()

    set_target_properties(analyzer_gui PROPERTIES
        OUTPUT_NAME "ChessTube Analyzer"
        LINK_FLAGS "/STACK:8388608"
        AUTOMOC ON
        AUTORCC ON
        AUTOUIC ON
    )

    if(ENABLE_QT_RUNTIME_DEPLOYMENT)
        add_runtime_deployment(analyzer_gui)
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build Google Test unit tests" ON)

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

    if(HAVE_SYSTEM_CUDA)
        target_compile_definitions(test_extract_moves PRIVATE HAVE_SYSTEM_CUDA)
        target_include_directories(test_extract_moves PRIVATE "${SYSTEM_CUDA_PATH}/include")
        target_link_directories(test_extract_moves PRIVATE "${SYSTEM_CUDA_PATH}/lib/x64")
        target_link_options(test_extract_moves PRIVATE ${CUDA_DELAY_LOADS_LIST})
        target_link_libraries(test_extract_moves PRIVATE delayimp.lib
            cudart nppc nppial nppicc nppidei nppif nppig nppim nppist nppitc)
    endif()

    target_link_libraries(test_extract_moves PRIVATE
        chess_extraction_core
        GTest::gtest_main
        GTest::gmock_main
    )

    enable_msvc_parallel_pdb_safety(test_extract_moves)
    enable_msvc_compile_speedups(test_extract_moves)
    enable_project_pch(test_extract_moves test)

    set_target_properties(test_extract_moves PROPERTIES
        LINK_FLAGS "/STACK:8388608"
    )

    include(GoogleTest)
    gtest_discover_tests(test_extract_moves)
endif()
