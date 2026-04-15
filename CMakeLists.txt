# Extracted from cpp directory
cmake_minimum_required(VERSION 3.20)
project(ChessTubeAnalyzer LANGUAGES CXX)

set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON)
set(CMAKE_X_VCPKG_APPLOCAL_DEPS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Force static runtime to avoid api-ms-win-crt-* dependency issues
# This must be set BEFORE project() to take effect
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")

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
if(Qt6_FOUND)
    message(STATUS "Qt6 found, GUI will be built")
else()
    message(STATUS "Qt6 not found. Install Qt to build the GUI")
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

# ── Main executable ──────────────────────────────────────────────────────────
add_executable(extract_moves
    src/main.cpp
    src/BoardLocalizer.cpp
    src/BoardAnalysis.cpp
    src/ArrowDetector.cpp
    src/ClockRecognizer.cpp
    src/ChessVideoExtractor.cpp
    src/FramePrefetcher.cpp
    src/GPUAccelerator.cpp
    src/StockfishAnalyzer.cpp
)

target_include_directories(extract_moves PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(HAVE_SYSTEM_CUDA)
    target_compile_definitions(extract_moves PRIVATE HAVE_SYSTEM_CUDA)
    target_include_directories(extract_moves PRIVATE "${SYSTEM_CUDA_PATH}/include")
    target_link_directories(extract_moves PRIVATE "${SYSTEM_CUDA_PATH}/lib/x64")
    # Use delay-load so the binary runs even without CUDA runtime DLLs installed.
    # CUDA 13.2 uses 64_13 suffix for runtime DLLs (different from .lib names).
    set(CUDA_DELAY_LOADS
        /DELAYLOAD:nppial64_13.dll
        /DELAYLOAD:nppicc64_13.dll
        /DELAYLOAD:nppig64_13.dll
        /DELAYLOAD:nppist64_13.dll
    )
    target_link_options(extract_moves PRIVATE ${CUDA_DELAY_LOADS})
    target_link_libraries(extract_moves PRIVATE delayimp.lib
        cudart nppc nppial nppicc nppidei nppif nppig nppim nppist nppitc)
endif()

target_link_libraries(extract_moves PRIVATE
    ${OpenCV_LIBS}
    nlohmann_json::nlohmann_json
    CLI11::CLI11
    chess
)

# Set stack size to 8MB (default is 1MB)
set_target_properties(extract_moves PROPERTIES
    LINK_FLAGS "/STACK:8388608"
)

# ── GUI Executable ───────────────────────────────────────────────────────────
if(Qt6_FOUND)
    add_executable(augmentor_gui WIN32
        src/main_gui.cpp
        src/MainWindow.cpp
        include/MainWindow.h
        src/VideoProcessorWorker.cpp
        include/VideoProcessorWorker.h
        src/PgnWriter.cpp
        src/StockfishAnalyzer.cpp
        src/BoardLocalizer.cpp
        src/BoardAnalysis.cpp
        src/ArrowDetector.cpp
        src/ClockRecognizer.cpp
        src/ChessVideoExtractor.cpp
        src/FramePrefetcher.cpp
        src/GPUAccelerator.cpp
        src/ToggleSwitch.cpp
        include/ToggleSwitch.h
        src/ThemeManager.cpp
        include/ThemeManager.h
    )

    target_include_directories(augmentor_gui PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        E:/libchess/dist/include
        ${LIBCHESS_SRC_DIR}
    )

    target_link_libraries(augmentor_gui PRIVATE
        Qt6::Widgets
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
        CLI11::CLI11
        chess
    )

    if(HAVE_SYSTEM_CUDA)
        target_compile_definitions(augmentor_gui PRIVATE HAVE_SYSTEM_CUDA)
        target_include_directories(augmentor_gui PRIVATE "${SYSTEM_CUDA_PATH}/include")
        target_link_directories(augmentor_gui PRIVATE "${SYSTEM_CUDA_PATH}/lib/x64")
        target_link_options(augmentor_gui PRIVATE ${CUDA_DELAY_LOADS})
        target_link_libraries(augmentor_gui PRIVATE delayimp.lib
            cudart nppc nppial nppicc nppidei nppif nppig nppim nppist nppitc)
    endif()

    set_target_properties(augmentor_gui PROPERTIES
        LINK_FLAGS "/STACK:8388608"
        AUTOMOC ON
    )
    # Temporarily disable LTO for augmentor_gui to diagnose potential hangs
    if(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE)
        set_property(TARGET augmentor_gui PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
        message(STATUS "LTO/IPO disabled for augmentor_gui to diagnose hang.")
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build Google Test unit tests" ON)

if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()

    add_executable(test_extract_moves
        tests/test_ui_detectors.cpp
        src/BoardLocalizer.cpp
        src/BoardAnalysis.cpp
        src/ArrowDetector.cpp
        src/ClockRecognizer.cpp
        src/ChessVideoExtractor.cpp
        src/FramePrefetcher.cpp
        src/GPUAccelerator.cpp
    )

    target_include_directories(test_extract_moves PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    if(HAVE_SYSTEM_CUDA)
        target_compile_definitions(test_extract_moves PRIVATE HAVE_SYSTEM_CUDA)
        target_include_directories(test_extract_moves PRIVATE "${SYSTEM_CUDA_PATH}/include")
        target_link_directories(test_extract_moves PRIVATE "${SYSTEM_CUDA_PATH}/lib/x64")
        target_link_options(test_extract_moves PRIVATE ${CUDA_DELAY_LOADS})
        target_link_libraries(test_extract_moves PRIVATE delayimp.lib
            cudart nppc nppial nppicc nppidei nppif nppig nppim nppist nppitc)
    endif()

    target_link_libraries(test_extract_moves PRIVATE
        ${OpenCV_LIBS}
        nlohmann_json::nlohmann_json
        CLI11::CLI11
        chess
        GTest::gtest_main
        GTest::gmock_main
    )

    set_target_properties(test_extract_moves PROPERTIES
        LINK_FLAGS "/STACK:8388608"
    )

    include(GoogleTest)
    gtest_discover_tests(test_extract_moves)
endif()
