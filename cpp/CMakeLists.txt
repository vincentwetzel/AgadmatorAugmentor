cmake_minimum_required(VERSION 3.20)
project(AgadmatorAugmentor LANGUAGES CXX)

set(X_VCPKG_APPLOCAL_DEPS_INSTALL ON)
set(CMAKE_X_VCPKG_APPLOCAL_DEPS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force static runtime to avoid api-ms-win-crt-* dependency issues
# This must be set BEFORE project() to take effect
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /MT")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /MTd")

# ── Dependencies from system vcpkg ───────────────────────────────────────────
find_package(OpenCV REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

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
    E:/libchess/dist/include
    ${LIBCHESS_SRC_DIR}
)

# ── Main executable ──────────────────────────────────────────────────────────
add_executable(extract_moves
    src/main.cpp
    src/BoardLocalizer.cpp
    src/UIDetectors.cpp
    src/ChessVideoExtractor.cpp
)

target_include_directories(extract_moves PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

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
        tests/test_chess_video_extractor.cpp
        src/BoardLocalizer.cpp
        src/UIDetectors.cpp
        src/ChessVideoExtractor.cpp
    )

    target_include_directories(test_extract_moves PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

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
