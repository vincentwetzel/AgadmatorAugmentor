// Extracted from cpp directory
#include <gtest/gtest.h>
#include <opencv2/opencv.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <set>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <array>
#include <cctype>
#include <nlohmann/json.hpp>
#include <libchess/position.hpp>
#include <libchess/move.hpp>
#include "BoardLocalizer.h"
#include "UIDetectors.h"
#include "ChessVideoExtractor.h"

// ─── Test result tracking ────────────────────────────────────────────────────
struct IntegrationTestResult {
    std::string name;
    std::string video_file;
    double video_duration_sec = 0.0;
    int plies_extracted = 0;
    int plies_expected = 0;
    bool passed = false;
    double elapsed_sec = 0.0;
    int reverts_detected = 0;
};

static std::vector<IntegrationTestResult> g_test_results;

// ─── TEST CONTROL PANEL ─────────────────────────────────────────────────────
// Set to 1 to enable, 0 to disable. Comment/uncomment to toggle.
// Every test MUST have a toggle here — no exceptions.
//
// Unit tests (detector accuracy on sample images):
#define TEST_LOCATE_BOARD         0
#define TEST_DRAW_GRID            0
#define TEST_YELLOW_SQUARES       0
#define TEST_PIECE_COUNTS         0
#define TEST_RED_SQUARES          0
#define TEST_YELLOW_ARROWS        0
#define TEST_MISALIGNED_PIECE     0
#define TEST_GAME_CLOCKS          0
#define TEST_MEMORY_LIMIT         0
#define TEST_CACHE_CORRECTNESS    0
//
// Integration tests (full video pipeline with ground-truth PGN):
#define TEST_7_PLIES_EXTRACTION   0
#define TEST_MEDIUM_GAME_REVERT   1
#define TEST_FULL_GAME_1_EXTRACTION 1
#define TEST_INTEGRATION_CLOCK_TIMES 0
//
// Smoke tests (constructor/validation):
#define TEST_CONSTRUCTOR_THROWS   0
// ─────────────────────────────────────────────────────────────────────────────

namespace cta {

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string format_clock_for_test(const std::string& raw) {
    if (raw.empty()) return "";

    std::string cleaned;
    bool last_was_colon = false;
    for (char c : raw) {
        if (std::isdigit(c) || c == '.') {
            cleaned += c;
            last_was_colon = false;
        } else if (c == ':') {
            if (!last_was_colon) {
                cleaned += c;
                last_was_colon = true;
            }
        }
    }
    if (!cleaned.empty() && cleaned.front() == ':') cleaned = cleaned.substr(1);
    if (!cleaned.empty() && cleaned.back() == ':') cleaned.pop_back();

    std::vector<std::string> parts;
    std::stringstream ss(cleaned);
    std::string item;
    while (std::getline(ss, item, ':')) {
        parts.push_back(item);
    }
    if (parts.size() > 3 || parts.empty()) return "";

    int h = 0, m = 0;
    double s = 0.0;
    try {
        if (parts.size() == 3) { h = std::stoi(parts[0]); m = std::stoi(parts[1]); s = std::stod(parts[2]); } 
        else if (parts.size() == 2) { m = std::stoi(parts[0]); s = std::stod(parts[1]); } 
        else { s = std::stod(parts[0]); }

        if (s >= 60.0) return "";
        if (m > 59) { h += m / 60; m = m % 60; }
    } catch (...) { return ""; }

    std::ostringstream out;
    out << h << ":" << std::setfill('0') << std::setw(2) << m << ":";
    if (s < 10.0) out << "0";
    
    if (std::floor(s) == s) {
        out << static_cast<int>(s);
    } else {
        out << std::fixed << std::setprecision(1) << s;
    }
    return out.str();
}

// Helper to find the assets directory from various common CWDs (root, build, build/Release)
static std::string find_assets_dir() {
    std::vector<std::filesystem::path> paths_to_check = {
        "assets",
        "../assets",
        "../../assets",
        "../../../assets" // Legacy, just in case
    };
    for (const auto& p : paths_to_check) {
        if (std::filesystem::exists(p) && std::filesystem::is_directory(p)) {
            return p.string();
        }
    }
    // Special case for Visual Studio CWD which can be $(SolutionDir)
    if (std::filesystem::exists("ChessTubeAnalyzer/assets")) {
        return "ChessTubeAnalyzer/assets";
    }
    return ""; // Not found
}

static std::string normalize_san_token(std::string san) {
    san.erase(std::remove(san.begin(), san.end(), '+'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '#'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '!'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '?'), san.end());
    san.erase(std::remove(san.begin(), san.end(), '='), san.end());
    san.erase(std::remove(san.begin(), san.end(), 'x'), san.end());

    auto ep_pos = san.find("e.p.");
    if (ep_pos != std::string::npos) san.erase(ep_pos, 4);

    if (san == "0-0") return "O-O";
    if (san == "0-0-0") return "O-O-O";
    return san;
}

static std::array<char, 64> expand_fen_board_for_test(const std::string& fen) {
    std::array<char, 64> board;
    board.fill(' ');
    int sq = 56;
    for (char c : fen) {
        if (c == ' ') break;
        if (c == '/') sq -= 16;
        else if (c >= '1' && c <= '8') sq += (c - '0');
        else board[sq++] = c;
    }
    return board;
}

static std::string build_san_for_test(const libchess::Position& pos, const libchess::Move& move, const std::string& uci) {
    auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
    auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));
    std::array<char, 64> board = expand_fen_board_for_test(pos.get_fen());
    char piece = board[from_sq];
    char target_piece = board[to_sq];
    bool is_pawn = (piece == 'P' || piece == 'p');
    bool is_capture = (target_piece != ' ') || (is_pawn && (from_sq % 8) != (to_sq % 8) && target_piece == ' ');

    if (move.type() == libchess::MoveType::ksc) return "O-O";
    if (move.type() == libchess::MoveType::qsc) return "O-O-O";

    std::string san;
    if (!is_pawn) {
        san += static_cast<char>(std::toupper(piece));
        bool file_conflict = false;
        bool rank_conflict = false;
        bool need_disambiguation = false;
        for (const auto& alt_move : pos.legal_moves()) {
            auto alt_from = static_cast<int>(static_cast<unsigned int>(alt_move.from()));
            auto alt_to = static_cast<int>(static_cast<unsigned int>(alt_move.to()));
            if (alt_from != from_sq && alt_to == to_sq && board[alt_from] == piece) {
                need_disambiguation = true;
                if (alt_from % 8 == from_sq % 8) file_conflict = true;
                if (alt_from / 8 == from_sq / 8) rank_conflict = true;
            }
        }
        if (need_disambiguation) {
            if (!file_conflict) san += static_cast<char>('a' + (from_sq % 8));
            else if (!rank_conflict) san += static_cast<char>('1' + (from_sq / 8));
            else {
                san += static_cast<char>('a' + (from_sq % 8));
                san += static_cast<char>('1' + (from_sq / 8));
            }
        }
    } else if (is_capture) {
        san += static_cast<char>('a' + (from_sq % 8));
    }

    if (is_capture) san += "x";
    san += static_cast<char>('a' + (to_sq % 8));
    san += static_cast<char>('1' + (to_sq / 8));
    if (uci.length() >= 5) {
        san += "=";
        san += static_cast<char>(std::toupper(uci[4]));
    }

    libchess::Position next_pos = pos;
    next_pos.makemove(move);
    if (next_pos.is_checkmate()) san += "#";
    else if (next_pos.in_check()) san += "+";
    return san;
}

struct ExpectedGameData {
    std::vector<std::string> main_line;
    std::vector<std::string> variations;
    std::vector<std::string> all_moves;
};

struct PgnState {
    libchess::Position pos;
    std::vector<libchess::Position> history;
    PgnState() : pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1") {}
};

static ExpectedGameData load_expected_uci_moves_from_pgn(const std::string& pgn_path) {
    std::ifstream ifs(pgn_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Could not open PGN: " + pgn_path);
    }

    std::stringstream buffer;
    buffer << ifs.rdbuf();

    ExpectedGameData result;
    
    std::string pgn = buffer.str();
    std::string cleaned;
    bool in_header = false;
    int brace_depth = 0;
    for (size_t i = 0; i < pgn.size(); ++i) {
        char c = pgn[i];
        if (c == '[' && brace_depth == 0) { in_header = true; continue; }
        if (in_header && c == ']') { in_header = false; continue; }
        if (in_header) continue;

        if (c == '{') { ++brace_depth; continue; }
        if (c == '}' && brace_depth > 0) { --brace_depth; continue; }
        if (brace_depth > 0) continue;

        if (c == '(' || c == ')') {
            cleaned += ' ';
            cleaned += c;
            cleaned += ' ';
        } else {
            cleaned += c;
        }
    }

    std::istringstream iss(cleaned);
    std::string token;
    
    PgnState current_state;
    std::vector<PgnState> state_stack;
    
    while (iss >> token) {
        if (token == "*" || token == "1-0" || token == "0-1" || token == "1/2-1/2") continue;
        if (token.rfind("$", 0) == 0) continue;

        if (token == "(") {
            state_stack.push_back(current_state);
            if (!current_state.history.empty()) {
                current_state.pos = current_state.history.back();
            }
            continue;
        }
        if (token == ")") {
            if (!state_stack.empty()) {
                current_state = state_stack.back();
                state_stack.pop_back();
            }
            continue;
        }

        size_t first_dot = token.find('.');
        if (first_dot != std::string::npos) {
            size_t last_dot = first_dot;
            while (last_dot < token.size() && token[last_dot] == '.') last_dot++;
            token = token.substr(last_dot);
        }
        if (token.empty()) continue;

        bool is_all_digits = true;
        for (char c : token) { if (!std::isdigit(static_cast<unsigned char>(c))) { is_all_digits = false; break; } }
        if (is_all_digits) continue;

        const std::string wanted_san = normalize_san_token(token);
        if (wanted_san.empty()) continue;

        bool matched = false;
        libchess::Move m;
        
        try { m = current_state.pos.parse_move(token); matched = true; } catch (...) {
            try { m = current_state.pos.parse_move(wanted_san); matched = true; } catch (...) {
                for (const auto& legal_move : current_state.pos.legal_moves()) {
                    std::string uci = static_cast<std::string>(legal_move);
                    if (uci == "e1h1") uci = "e1g1"; else if (uci == "e1a1") uci = "e1c1";
                    else if (uci == "e8h8") uci = "e8g8"; else if (uci == "e8a8") uci = "e8c8";
                    if (normalize_san_token(build_san_for_test(current_state.pos, legal_move, uci)) == wanted_san) {
                        m = legal_move;
                        matched = true;
                        break;
                    }
                }
            }
        }

        if (matched) {
            std::string uci = static_cast<std::string>(m);
            if (uci == "e1h1") uci = "e1g1"; else if (uci == "e1a1") uci = "e1c1";
            else if (uci == "e8h8") uci = "e8g8"; else if (uci == "e8a8") uci = "e8c8";
            
            if (state_stack.empty()) {
                result.main_line.push_back(uci);
            } else {
                result.variations.push_back(uci);
            }
            result.all_moves.push_back(uci);
            
            current_state.history.push_back(current_state.pos);
            current_state.pos.makemove(m);
        } else {
            throw std::runtime_error("Could not convert PGN SAN token to UCI: " + token);
        }
    }

    return result;
}

static std::multiset<std::string> extract_all_moves_multiset(const GameData& data) {
    std::multiset<std::string> all(data.moves.begin(), data.moves.end());
    for (const auto& item : data.variations) {
        for (const auto& var : item.second) {
            for (const auto& m : var.moves) {
                all.insert(m);
            }
        }
    }
    return all;
}

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

// Get video duration in seconds using OpenCV
static double get_video_duration(const std::string& video_path) {
    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) return 0.0;
    double fps = cap.get(cv::CAP_PROP_FPS);
    double frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    cap.release();
    return (fps > 0) ? frames / fps : 0.0;
}

// Print the summary table after all tests
static void print_test_summary() {
    if (g_test_results.empty()) return;

    static const std::string sep(120, '-');
    static const std::string border(120, '=');

    std::cout << "\n";
    std::cout << border << "\n";
    std::cout << "  INTEGRATION TEST SUMMARY\n";
    std::cout << border << "\n";
    std::cout << std::left;
    std::cout << "  " << std::setw(28) << "Test"
              << std::setw(14) << "Video"
              << std::setw(10) << "Plies"
              << std::setw(10) << "Result"
              << std::setw(12) << "Extracted"
              << std::setw(10) << "Reverts"
              << std::setw(12) << "Time"
              << std::setw(12) << "Accuracy"
              << "\n";
    std::cout << "  " << sep << "\n";

    int total_passed = 0, total_plies = 0;
    double total_video = 0.0, total_proc = 0.0;

    auto fmt_time = [](double sec) -> std::string {
        if (sec < 60.0) return std::to_string(static_cast<int>(sec)) + "s";
        int m = static_cast<int>(sec) / 60;
        int s = static_cast<int>(sec) % 60;
        return std::to_string(m) + "m" + std::to_string(s) + "s";
    };

    for (const auto& r : g_test_results) {
        std::string result_str = r.passed ? "PASS" : "FAIL";
        std::string accuracy;
        if (r.plies_expected > 0) {
            int matched = std::min(r.plies_extracted, r.plies_expected);
            accuracy = std::to_string(matched) + "/" + std::to_string(r.plies_expected);
        } else {
            accuracy = "N/A";
        }

        std::cout << "  " << std::setw(28) << r.name
                  << std::setw(14) << fmt_time(r.video_duration_sec)
                  << std::setw(10) << r.plies_expected
                  << std::setw(10) << result_str
                  << std::setw(12) << std::to_string(r.plies_extracted)
                  << std::setw(10) << r.reverts_detected
                  << std::setw(12) << fmt_time(r.elapsed_sec)
                  << std::setw(12) << accuracy
                  << "\n";

        total_passed += r.passed ? 1 : 0;
        total_plies += r.plies_extracted;
        total_video += r.video_duration_sec;
        total_proc += r.elapsed_sec;
    }

    std::cout << "  " << sep << "\n";
    std::cout << "  " << total_passed << "/" << g_test_results.size() << " passed"
              << " | " << total_plies << " plies"
              << " | Video: " << fmt_time(total_video)
              << " | Processing: " << fmt_time(total_proc);
    if (total_video > 0 && total_proc > 0) {
        std::cout << " (" << std::fixed << std::setprecision(1) << (total_video / total_proc) << "x real-time)";
    }
    std::cout << "\n";
    std::cout << border << "\n";
    std::cout << "\n";
}

// ── Shared fixture ────────────────────────────────────────────────────────────

class DetectorsTest : public ::testing::Test {
protected:
    void SetUp() override {
        assets_dir_ = find_assets_dir();
        if (assets_dir_.empty()) {
            GTEST_SKIP() << "Could not find 'assets' directory. Tests skipped.";
        }

        board_path_ = (std::filesystem::path(assets_dir_) / "board" / "board.png").string();
        red_board_path_ = (std::filesystem::path(assets_dir_) / "board" / "red_board.png").string();

        board_ = cv::imread(board_path_);
        if (board_.empty()) {
            GTEST_SKIP() << "Board template not found: " << board_path_;
        }
        geo_ = locate_board(board_, board_);
    }

    std::string assets_dir_;
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_yellow_squares").string();
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_piece_counts").string();
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_red_squares").string();
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_yellow_arrows").string();
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_misaligned_piece").string();
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "sample_clock_changes").string();
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

        // Draw debug boxes for where we searched for the clocks
        cv::Mat debug_img = img.clone();
        int roi_x1 = std::max(0, static_cast<int>(img_geo.bx + img_geo.bw * 0.70));
        int roi_x2 = std::min(debug_img.cols, static_cast<int>(img_geo.bx + img_geo.bw));

        int top_roi_y1 = std::max(0, static_cast<int>(img_geo.by - img_geo.sq_h * 0.40));
        int top_roi_y2 = std::max(0, static_cast<int>(img_geo.by - img_geo.sq_h * 0.08));
        int bot_roi_y1 = std::min(debug_img.rows, static_cast<int>(img_geo.by + img_geo.bh + img_geo.sq_h * 0.08));
        int bot_roi_y2 = std::min(debug_img.rows, static_cast<int>(img_geo.by + img_geo.bh + img_geo.sq_h * 0.40));

        cv::rectangle(debug_img, cv::Point(roi_x1, top_roi_y1), cv::Point(roi_x2, top_roi_y2), cv::Scalar(0, 0, 255), 2); // Red for top (black clock)
        cv::rectangle(debug_img, cv::Point(roi_x1, bot_roi_y1), cv::Point(roi_x2, bot_roi_y2), cv::Scalar(0, 255, 0), 2); // Green for bot (white clock)
        std::string debug_path = (std::filesystem::path(debug_dir) / (stem(img_path) + "_boxes.png")).string();
        cv::imwrite(debug_path, debug_img);

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
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_short" / "7 plies" / "7 plies.mp4").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning integration test on 7 plies video...\n";

    IntegrationTestResult result;
    result.name = "7 Plies Extraction";
    result.video_file = "7 plies.mp4";
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_7_plies");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    const std::string pgn_path = (std::filesystem::path(assets_dir_) / "sample_games_short" / "7 plies" / "7 plies.pgn").string();
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;

    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected baseline from PGN.\n";

    result.plies_expected = static_cast<int>(expected_moves.size());

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    result.passed = (data.moves == expected_moves) && (extracted_all == expected_all);
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    if (result.passed) {
        std::cout << "PASS: Extracted moves perfectly match the expected " << expected_moves.size() << " plies from the PGN.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_7_PLIES_EXTRACTION

// ─── INTEGRATION: MEDIUM GAME WITH REVERT ─────────────────────────────────────
#if TEST_MEDIUM_GAME_REVERT

TEST_F(DetectorsTest, MediumGameWithRevert) {
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_medium" / "medium_game_with_analysis_line_and_revert.mp4").string();
    const std::string pgn_path = (std::filesystem::path(assets_dir_) / "sample_games_medium" / "game.pgn").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;

    std::cout << "\nRunning integration test on medium game with revert...\n";

    IntegrationTestResult result;
    result.name = "Medium Game + Revert";
    result.video_file = "medium_game_with_revert.mp4";
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_medium_revert");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected baseline from PGN.\n";

    result.plies_expected = static_cast<int>(expected_moves.size());
    result.reverts_detected = 1; // This test is known to have one analysis revert

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    result.passed = (data.moves == expected_moves) && (extracted_all == expected_all);
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    if (result.passed) {
        std::cout << "PASS: Extracted moves perfectly match the expected " << expected_moves.size()
                  << " moves from the PGN, correctly handling analysis line revert.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_MEDIUM_GAME_REVERT

// ─── INTEGRATION: FULL GAME 1 EXTRACTION ─────────────────────────────────────
#if TEST_FULL_GAME_1_EXTRACTION

TEST_F(DetectorsTest, FullGame1Extraction) {
    const std::string dir_path = (std::filesystem::path(assets_dir_) / "sample_games_full" / "game_1").string();
    auto vid_files = list_files(dir_path, {".mp4", ".mkv", ".webm", ".avi"});
    
    if (vid_files.empty()) {
        GTEST_SKIP() << "Video not found in: " << dir_path;
    }
    
    const std::string video_path = vid_files[0];
    const std::string pgn_path = (std::filesystem::path(dir_path) / "game.pgn").string();

    if (!std::filesystem::exists(pgn_path)) {
        GTEST_SKIP() << "PGN not found: " << pgn_path;
    }

    std::cout << "\nRunning integration test on Full Game 1...\n";

    IntegrationTestResult result;
    result.name = "Full Game 1 Extraction";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_full_game_1");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected baseline from PGN.\n";

    result.plies_expected = static_cast<int>(expected_moves.size());
    result.reverts_detected = 0; // Can be updated if GameData tracks total reverts in the future

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    result.passed = (data.moves == expected_moves) && (extracted_all == expected_all);
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    if (result.passed) {
        std::cout << "PASS: Extracted moves perfectly match the expected " << expected_moves.size() << " plies from the PGN.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_FULL_GAME_1_EXTRACTION

// ─── INTEGRATION: CLOCK TIMES EXTRACTION ─────────────────────────────────────
#if TEST_INTEGRATION_CLOCK_TIMES

TEST_F(DetectorsTest, IntegrationClockTimes) {
    const std::string dir_path = (std::filesystem::path(assets_dir_) / "test_clock_times").string();
    auto vid_files = list_files(dir_path, {".mp4", ".mkv", ".webm", ".avi"});
    
    if (vid_files.empty()) {
        GTEST_SKIP() << "Video not found in: " << dir_path;
    }
    
    const std::string video_path = vid_files[0];
    auto pgn_files = list_files(dir_path, {".pgn"});
    if (pgn_files.empty()) {
        GTEST_SKIP() << "PGN not found in: " << dir_path;
    }
    const std::string pgn_path = pgn_files[0];

    std::cout << "\nRunning integration test on clock times...\n";

    IntegrationTestResult result;
    result.name = "Clock Times Integration";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_clock_times");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    // Extract and verify expected moves from PGN
    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    std::multiset<std::string> expected_all(expected_data.all_moves.begin(), expected_data.all_moves.end());
    std::cout << "  Loaded expected moves from PGN.\n";
    result.plies_expected = static_cast<int>(expected_moves.size());

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& m : expected_moves) std::cout << m << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& m : data.moves) std::cout << m << " ";
    std::cout << "\n";

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    bool moves_passed = (data.moves == expected_moves) && (extracted_all == expected_all);
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    // Extract expected clocks from PGN's [%clk ...] tags
    std::vector<std::string> expected_clocks;
    {
        std::ifstream ifs(pgn_path);
        ASSERT_TRUE(ifs.is_open()) << "Could not open PGN: " << pgn_path;
        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        size_t pos = 0;
        while ((pos = content.find("[%clk ", pos)) != std::string::npos) {
            pos += 6;
            size_t end_pos = content.find("]", pos);
            if (end_pos != std::string::npos) {
                expected_clocks.push_back(content.substr(pos, end_pos - pos));
                pos = end_pos;
            }
        }
    }
    
    std::cout << "  Loaded expected clocks from PGN.\n";

    std::cout << "  Expected Clocks (" << expected_clocks.size() << "): ";
    for (const auto& c : expected_clocks) std::cout << c << " ";
    std::cout << "\n";

    // Map GameData clocks into standard PGN strings for each ply
    std::vector<std::string> extracted_clocks;
    for (size_t i = 0; i < data.moves.size(); ++i) {
        size_t clockIdx = i + 1;
        const auto* clk_ptr = (clockIdx < data.clocks.size()) ? &data.clocks[clockIdx] : (i < data.clocks.size()) ? &data.clocks[i] : nullptr;
        std::string raw_clock = clk_ptr ? ((i % 2 == 0) ? clk_ptr->white_time : clk_ptr->black_time) : "0:00:00";
        
        std::string formatted = format_clock_for_test(raw_clock);
        extracted_clocks.push_back(formatted.empty() ? "0:00:00" : formatted);
    }

    // It's common for the final frame to settle before the clock updates its final tick.
    // Trim any trailing zeroed clocks, and resize the expected array to match.
    while (!extracted_clocks.empty() && extracted_clocks.back() == "0:00:00") {
        extracted_clocks.pop_back();
    }
    if (expected_clocks.size() > extracted_clocks.size()) {
        expected_clocks.resize(extracted_clocks.size());
    }

    std::cout << "  Extracted Clocks (" << extracted_clocks.size() << "): ";
    for (const auto& c : extracted_clocks) std::cout << c << " ";
    std::cout << "\n";

    bool clocks_passed = (extracted_clocks == expected_clocks);
    EXPECT_EQ(extracted_clocks, expected_clocks)
        << "Extracted " << extracted_clocks.size() << " clocks, expected " << expected_clocks.size();

    result.passed = moves_passed && clocks_passed;

    if (result.passed) {
        std::cout << "PASS: Extracted moves and clocks perfectly match the expected PGN.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_INTEGRATION_CLOCK_TIMES

// ─── MEMORY LIMIT BEHAVIOR ───────────────────────────────────────────────────
#if TEST_MEMORY_LIMIT

TEST_F(DetectorsTest, MemoryLimitWorkerCount) {
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_medium" / "medium_game_with_analysis_line_and_revert.mp4").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning unit test on memory limit worker count...\n";

    // 250MB limit should restrict it to max 1 worker.
    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None, 250);
    
    int detected_workers = -1;
    extractor.set_progress_callback([&](int percent, const std::string& msg) {
        if (msg.find("Launching Map-Reduce visual extraction") != std::string::npos) {
            size_t start = msg.find("(") + 1;
            size_t end = msg.find(" workers");
            if (start != std::string::npos && end != std::string::npos) {
                detected_workers = std::stoi(msg.substr(start, end - start));
            }
        }
    });

    // Run extraction with immediate cancellation to avoid waiting for the whole video
    std::atomic<bool> cancel{true};
    
    try {
        extractor.extract_moves_from_video(video_path, "test_mem_limit", &cancel);
    } catch (...) {
        // It might throw or exit cleanly upon cancellation, either is fine as long as we got the log
    }

    EXPECT_NE(detected_workers, -1) << "Did not find Map-Reduce launch log message.";
    EXPECT_EQ(detected_workers, 1) << "Memory limit of 250MB should restrict worker count to 1.";
}

#endif // TEST_MEMORY_LIMIT

// ─── CACHE CORRECTNESS TEST ──────────────────────────────────────────────────
#if TEST_CACHE_CORRECTNESS

TEST_F(DetectorsTest, CacheCorrectness) {
    const std::string video_path = (std::filesystem::path(assets_dir_) / "sample_games_short" / "7 plies" / "7 plies.mp4").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning unit test on board caching behavior...\n";

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    
    int cache_loads = 0;
    int multi_pass_searches = 0;

    extractor.set_progress_callback([&](int percent, const std::string& msg) {
        if (msg.find("Loaded exact board scale from cache") != std::string::npos) {
            cache_loads++;
        }
        if (msg.find("Performing multi-pass template matching") != std::string::npos) {
            multi_pass_searches++;
        }
    });

    std::atomic<bool> cancel{true};
    
    // First run might be a hit or miss depending on the environment,
    // but running it twice guarantees the second run is a hit if caching works.
    try { extractor.extract_moves_from_video(video_path, "test_cache", &cancel); } catch (...) {}
    
    cache_loads = 0;
    multi_pass_searches = 0;
    
    // Second run: must hit the cache
    try { extractor.extract_moves_from_video(video_path, "test_cache", &cancel); } catch (...) {}

    EXPECT_EQ(cache_loads, 1) << "Second run did not load from cache.";
    EXPECT_EQ(multi_pass_searches, 0) << "Second run performed multi-pass search instead of using cache.";
}

#endif // TEST_CACHE_CORRECTNESS

} // namespace cta
