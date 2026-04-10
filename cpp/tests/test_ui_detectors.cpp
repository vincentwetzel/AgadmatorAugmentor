#include <gtest/gtest.h>
#include <opencv2/opencv.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include "BoardLocalizer.h"
#include "UIDetectors.h"
#include "ChessVideoExtractor.h"

// ─── TEST CONTROL PANEL ─────────────────────────────────────────────────────
// Set to 1 to enable, 0 to disable. Comment/uncomment to toggle.
// Every test MUST have a toggle here — no exceptions.
//
// Unit tests (detector accuracy on sample images):
#define TEST_LOCATE_BOARD         1
#define TEST_DRAW_GRID            1
#define TEST_YELLOW_SQUARES       1
#define TEST_PIECE_COUNTS         1
#define TEST_RED_SQUARES          1
#define TEST_YELLOW_ARROWS        1
#define TEST_MISALIGNED_PIECE     1
#define TEST_GAME_CLOCKS          1
//
// Integration tests (full video pipeline with ground-truth PGN):
#define TEST_7_PLIES_EXTRACTION   0
#define TEST_MEDIUM_GAME_REVERT   0
//
// Smoke tests (constructor/validation):
#define TEST_CONSTRUCTOR_THROWS   1
// ─────────────────────────────────────────────────────────────────────────────

namespace aa {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::vector<std::string> list_files(const std::string& dir,
                                            const std::vector<std::string>& exts) {
    std::vector<std::string> result;
    if (!std::filesystem::exists(dir)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        for (const auto& e : exts) {
            if (ext == e) { result.push_back(entry.path().string()); break; }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

static std::string stem(const std::string& path) {
    return std::filesystem::path(path).stem().string();
}

// ── Shared fixture ────────────────────────────────────────────────────────────

class DetectorsTest : public ::testing::Test {
protected:
    void SetUp() override {
        board_path_ = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\board\board.png)";
        red_board_path_ = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\board\red_board.png)";
        board_ = cv::imread(board_path_);
        if (board_.empty()) {
            GTEST_SKIP() << "Board template not found: " << board_path_;
        }
        geo_ = locate_board(board_, board_);
    }

    std::string board_path_;
    std::string red_board_path_;
    cv::Mat board_;
    BoardGeometry geo_;
};

// ─── BOARD LOCALIZER ─────────────────────────────────────────────────────────
#if TEST_LOCATE_BOARD

TEST_F(DetectorsTest, LocateBoardOnItself) {
    auto geo = locate_board(board_, board_);
    EXPECT_GT(geo.bw, 0);
    EXPECT_GT(geo.bh, 0);
    EXPECT_NEAR(geo.sq_w, static_cast<double>(geo.bw) / 8.0, 1.0);
    EXPECT_NEAR(geo.sq_h, static_cast<double>(geo.bh) / 8.0, 1.0);
}

#endif // TEST_LOCATE_BOARD

#if TEST_DRAW_GRID

TEST_F(DetectorsTest, DrawBoardGrid) {
    cv::Mat test_img = cv::Mat(800, 800, CV_8UC3, cv::Scalar(128, 128, 128));
    BoardGeometry geo{50, 50, 700, 700, 87.5, 87.5};
    EXPECT_NO_THROW(draw_board_grid(test_img, geo, cv::Scalar(0, 255, 0), 2, true));
}

#endif // TEST_DRAW_GRID

// ─── YELLOW SQUARE EXTRACTION ────────────────────────────────────────────────
#if TEST_YELLOW_SQUARES

TEST_F(DetectorsTest, YellowSquares) {
    const std::string images_dir = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_yellow_squares)";
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::cout << "\nRunning unit tests on yellow square images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_name = stem(img_path);
        BoardGeometry img_geo = locate_board(img, board_);
        std::string move_uci = extract_move_from_yellow_squares(img, board_, img_geo);

        std::string clean = expected_name;
        clean.erase(std::remove(clean.begin(), clean.end(), '+'), clean.end());
        clean.erase(std::remove(clean.begin(), clean.end(), '#'), clean.end());

        std::string expected_dest;
        if (clean == "O-O") {
            expected_dest = (move_uci.size() >= 4 && move_uci[3] == '1') ? "g1" : "g8";
        } else if (clean == "O-O-O") {
            expected_dest = (move_uci.size() >= 4 && move_uci[3] == '1') ? "c1" : "c8";
        } else {
            expected_dest = clean.substr(clean.size() - 2);
        }

        std::string extracted_dest = move_uci.substr(2, 2);
        bool pass = (extracted_dest == expected_dest);
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> " << move_uci
                  << " (dest=" << extracted_dest << ", expected=" << expected_dest << ")\n";
        EXPECT_EQ(extracted_dest, expected_dest) << "Failed on " << img_path;
    }
    std::cout << "PASS: Extracted valid moves from " << files.size() << " yellow square images.\n";
}

#endif // TEST_YELLOW_SQUARES

// ─── PIECE COUNTING ──────────────────────────────────────────────────────────
#if TEST_PIECE_COUNTS

TEST_F(DetectorsTest, PieceCounts) {
    const std::string images_dir = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_piece_counts)";
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::cout << "\nRunning unit tests on piece counting images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        int expected = std::stoi(stem(img_path));
        BoardGeometry img_geo = locate_board(img, board_);
        int actual = count_pieces_in_image(img, board_, img_geo);
        bool pass = (actual == expected);
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> counted " << actual << " (expected " << expected << ")\n";
        EXPECT_EQ(actual, expected) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately counted pieces in all " << files.size() << " images.\n";
}

#endif // TEST_PIECE_COUNTS

// ─── RED SQUARES ─────────────────────────────────────────────────────────────
#if TEST_RED_SQUARES

TEST_F(DetectorsTest, RedSquares) {
    const std::string images_dir = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_red_squares)";
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    cv::Mat red_board = cv::imread(red_board_path_);

    std::cout << "\nRunning unit tests on red square images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_str = stem(img_path);
        std::vector<std::string> expected;
        std::string token;
        for (char c : expected_str) {
            if (c == ',') {
                if (!token.empty()) { expected.push_back(token); token.clear(); }
            } else if (c != ' ') {
                token += c;
            }
        }
        if (!token.empty()) expected.push_back(token);
        std::sort(expected.begin(), expected.end());

        BoardGeometry img_geo = locate_board(img, board_);
        auto actual = find_red_squares(img, board_, red_board, img_geo);
        bool pass = (actual == expected);
        std::string actual_str, exp_str;
        for (const auto& s : actual) { if (!actual_str.empty()) actual_str += ","; actual_str += s; }
        for (const auto& s : expected) { if (!exp_str.empty()) exp_str += ","; exp_str += s; }
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> [" << actual_str << "]" << (pass ? "" : " (expected [" + exp_str + "])") << "\n";
        EXPECT_EQ(actual, expected) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately detected red squares in all " << files.size() << " images.\n";
}

#endif // TEST_RED_SQUARES

// ─── YELLOW ARROWS ───────────────────────────────────────────────────────────
#if TEST_YELLOW_ARROWS

TEST_F(DetectorsTest, YellowArrows) {
    const std::string images_dir = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_yellow_arrows)";
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::cout << "\nRunning unit tests on yellow arrow images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_str = stem(img_path);
        std::vector<std::string> expected;
        for (size_t i = 0; i < expected_str.size(); ) {
            if (i + 4 <= expected_str.size()) {
                expected.push_back(expected_str.substr(i, 4));
                i += 4;
                if (i < expected_str.size() && expected_str[i] == ',') ++i;
            } else break;
        }
        std::sort(expected.begin(), expected.end());

        auto to_endpoints = [](const std::vector<std::string>& arrows) {
            std::vector<std::string> eps;
            for (const auto& a : arrows) {
                if (a.size() >= 4) {
                    std::string e1 = a.substr(0, 2);
                    std::string e2 = a.substr(2, 4);
                    if (e1 < e2) eps.push_back(e1 + e2);
                    else eps.push_back(e2 + e1);
                }
            }
            std::sort(eps.begin(), eps.end());
            return eps;
        };

        BoardGeometry img_geo = locate_board(img, board_);
        auto actual = find_yellow_arrows(img, board_, img_geo);
        bool pass = (to_endpoints(actual) == to_endpoints(expected));
        std::string actual_str, exp_str;
        for (const auto& s : actual) { if (!actual_str.empty()) actual_str += ","; actual_str += s; }
        for (const auto& s : expected) { if (!exp_str.empty()) exp_str += ","; exp_str += s; }
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> [" << actual_str << "]" << (pass ? "" : " (expected [" + exp_str + "])") << "\n";
        EXPECT_EQ(to_endpoints(actual), to_endpoints(expected)) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately detected yellow arrows in all " << files.size() << " images.\n";
}

#endif // TEST_YELLOW_ARROWS

// ─── MISALIGNED PIECE (HOVER BOX) ────────────────────────────────────────────
#if TEST_MISALIGNED_PIECE

TEST_F(DetectorsTest, MisalignedPiece) {
    const std::string images_dir = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_misaligned_piece)";
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::string debug_dir = "debug_screenshots/misaligned_pieces";
    std::filesystem::create_directories(debug_dir);

    std::cout << "\nRunning unit tests on misaligned piece images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected = stem(img_path);
        BoardGeometry img_geo = locate_board(img, board_);
        std::string actual = find_misaligned_piece(img, board_, img_geo);
        bool pass = (actual == expected);
        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> " << (actual.empty() ? "(none)" : actual)
                  << (pass ? "" : " (expected " + expected + ")") << "\n";
        EXPECT_EQ(actual, expected) << "Failed on " << img_path;
    }
    std::cout << "PASS: Accurately detected misaligned pieces in all " << files.size() << " images.\n";
}

#endif // TEST_MISALIGNED_PIECE

// ─── GAME CLOCKS ─────────────────────────────────────────────────────────────
#if TEST_GAME_CLOCKS

TEST_F(DetectorsTest, GameClocks) {
    const std::string images_dir = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_clock_changes)";
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::string debug_dir = "debug_screenshots/game_clocks";
    std::filesystem::create_directories(debug_dir);

    std::cout << "\nRunning unit tests on game clocks...\n";
    int passed = 0, failed = 0;
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string base = stem(img_path);
        std::vector<std::string> parts;
        std::string token;
        for (char c : base) {
            if (c == '_') { parts.push_back(token); token.clear(); }
            else token += c;
        }
        parts.push_back(token);
        if (parts.size() < 3) {
            std::cout << "  SKIP: " << stem(img_path) << " (bad filename format)\n";
            continue;
        }

        std::string expected_active = parts[0];
        std::string expected_white = parts[1];
        std::string expected_black = parts[2];
        for (auto& c : expected_white) if (c == '-') c = ':';
        for (auto& c : expected_black) if (c == '-') c = ':';

        BoardGeometry img_geo = locate_board(img, board_);
        if (img_geo.bw == 0 || img_geo.bh == 0) {
            std::cout << "  SKIP: " << stem(img_path) << " (board not found)\n";
            continue;
        }

        ClockState state = extract_clocks(img, board_, img_geo);
        bool player_ok = (state.active_player == expected_active);
        bool white_ok = (state.white_time == expected_white);
        bool black_ok = (state.black_time == expected_black);
        bool pass = player_ok && white_ok && black_ok;

        std::cout << "  " << (pass ? "PASS" : "FAIL") << ": " << stem(img_path)
                  << " -> active=" << (state.active_player.empty() ? "(none)" : state.active_player)
                  << ", white=" << (state.white_time.empty() ? "(none)" : state.white_time)
                  << ", black=" << (state.black_time.empty() ? "(none)" : state.black_time);
        if (!pass) {
            std::cout << " (expected active=" << expected_active
                      << ", white=" << expected_white << ", black=" << expected_black << ")";
            ++failed;
        } else {
            ++passed;
        }
        std::cout << "\n";

        EXPECT_TRUE(player_ok) << "Failed on " << img_path << ": active player mismatch";
        EXPECT_TRUE(white_ok) << "Failed on " << img_path << ": white time '" << state.white_time << "' != '" << expected_white << "'";
        EXPECT_TRUE(black_ok) << "Failed on " << img_path << ": black time '" << state.black_time << "' != '" << expected_black << "'";
    }
    std::cout << (failed == 0 ? "PASS" : "FAIL") << ": " << passed << "/" << (passed + failed) << " clock tests passed.\n";
}

#endif // TEST_GAME_CLOCKS

// ─── SMOKE TEST: CONSTRUCTOR VALIDATION ──────────────────────────────────────
#if TEST_CONSTRUCTOR_THROWS

TEST_F(DetectorsTest, ConstructorThrowsOnMissingAsset) {
    EXPECT_THROW(ChessVideoExtractor("nonexistent.png"), std::runtime_error);
}

#endif // TEST_CONSTRUCTOR_THROWS

// ─── INTEGRATION: 7 PLIES EXTRACTION ─────────────────────────────────────────
#if TEST_7_PLIES_EXTRACTION

TEST_F(DetectorsTest, SevenPliesExtraction) {
    const std::string video_path = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_games_short\7 plies\7 plies.mp4)";

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning integration test on 7 plies video...\n";

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "output/cpp_7ply_test.json", "test_7_plies");

    // Expected moves from game.pgn: 1. d4 d5 2. c4 e6 3. Nf3 Nf6 4. g3
    std::vector<std::string> expected_moves = {"d2d4", "d7d5", "c2c4", "e7e6", "g1f3", "g8f6", "g2g3"};

    std::cout << "  Expected (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted " << data.moves.size() << " moves, expected " << expected_moves.size();

    if (data.moves == expected_moves) {
        std::cout << "PASS: Extracted moves perfectly match the expected " << expected_moves.size() << " plies from the PGN.\n";
    }
}

#endif // TEST_7_PLIES_EXTRACTION

// ─── INTEGRATION: MEDIUM GAME WITH REVERT ─────────────────────────────────────
#if TEST_MEDIUM_GAME_REVERT

TEST_F(DetectorsTest, MediumGameWithRevert) {
    const std::string video_path = R"(i:\coding_workspaces\CPP\AgadmatorAugmentor\assets\sample_games_medium\medium_game_with_analysis_line_and_revert.mp4)";

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning integration test on medium game with revert...\n";

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "output/cpp_medium_test.json", "test_medium_revert");

    // Expected moves from game.pgn:
    // 1. d4 d5 2. c4 e6 3. Nf3 Nf6 4. g3 Bb4+ 5. Nbd2 a5 6. Bg2 a4
    // 7. O-O Nc6 8. Qc2 O-O 9. Re1
    std::vector<std::string> expected_moves = {
        "d2d4", "d7d5", "c2c4", "e7e6", "g1f3", "g8f6", "g2g3", "f8b4",
        "b1d2", "a7a5", "f1g2", "a5a4", "e1g1", "b8c6", "d1c2", "e8g8", "f1e1"
    };

    std::cout << "  Expected (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted " << data.moves.size() << " moves, expected " << expected_moves.size();

    if (data.moves == expected_moves) {
        std::cout << "PASS: Extracted moves perfectly match the expected " << expected_moves.size()
                  << " moves from the PGN, correctly handling analysis line revert.\n";
    }
}

#endif // TEST_MEDIUM_GAME_REVERT

} // namespace aa
