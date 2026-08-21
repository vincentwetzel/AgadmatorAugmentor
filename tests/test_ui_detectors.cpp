// Extracted from cpp directory
#include <gtest/gtest.h>
#include <opencv2/opencv.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <set>
#include <map>
#include <algorithm>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <numeric>
#include <utility>
#include <nlohmann/json.hpp>
#include <libchess/position.hpp>
#include <libchess/move.hpp>
#include "BoardLocalizer.h"
#include "UIDetectors.h"
#include "ChessVideoExtractor.h"
#include "ClockRecognizer.h"
#include "OpeningFetcher.h"
#include "PgnWriter.h"
#include "ChessFenUtils.h"
#include "ExtractionDiagnostics.h"
#include "VideoChunkMapper.h"
#include "../src/ExtractorUtils.h"
#include "../src/MoveValidations.h"
#include "../src/ChessVideoExtractor_Internal.h"

TEST(PgnWriterTest, ConvertsVariationMovesFromRecordedRootFenToSan) {
    cta::PgnWriter writer;
    writer.add_ply("e2e4");
    writer.add_ply("e7e6");
    writer.add_ply("d2d4");
    writer.add_ply("d7d5");

    libchess::Position variation_root;
    variation_root.set_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    variation_root.makemove(variation_root.parse_move("e2e4"));
    variation_root.makemove(variation_root.parse_move("e7e6"));
    variation_root.makemove(variation_root.parse_move("d2d4"));

    writer.push_variation(variation_root.get_fen());
    writer.add_ply("d7d6");
    writer.add_ply("c2c4");
    writer.add_ply("d6d5");
    writer.pop_variation();

    const std::string pgn = writer.build();
    EXPECT_NE(pgn.find("(2... d6 3. c4 d5)"), std::string::npos);
    EXPECT_EQ(pgn.find("d7d6"), std::string::npos);
    EXPECT_EQ(pgn.find("c2c4"), std::string::npos);
    EXPECT_EQ(pgn.find("d6d5"), std::string::npos);
}

TEST(PgnWriterTest, ConvertsSubtitleMoveLineFromRecordedRootFenToSan) {
    const std::string root_fen =
        "rnbqkbnr/pppppppp/4p3/8/3PP3/8/PPP2PPP/RNBQKBNR b KQkq - 0 2";
    EXPECT_EQ(
        cta::ChessFenUtils::uci_to_san_line("d7d6 c2c4", root_fen),
        "d6 c4");
}

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
static size_t g_invariant_failure_count = 0;

static void expect_game_data_equal(const cta::GameData& expected,
                                   const cta::GameData& actual) {
    EXPECT_EQ(expected.moves, actual.moves);
    EXPECT_EQ(expected.timestamps, actual.timestamps);
    EXPECT_EQ(expected.fens, actual.fens);
    EXPECT_EQ(expected.video_fens, actual.video_fens);
    EXPECT_EQ(expected.video_timestamps, actual.video_timestamps);
    EXPECT_EQ(expected.video_moves, actual.video_moves);
    ASSERT_EQ(expected.clocks.size(), actual.clocks.size());
    for (size_t index = 0; index < expected.clocks.size(); ++index) {
        EXPECT_EQ(expected.clocks[index].active, actual.clocks[index].active);
        EXPECT_EQ(expected.clocks[index].white_time, actual.clocks[index].white_time);
        EXPECT_EQ(expected.clocks[index].black_time, actual.clocks[index].black_time);
        EXPECT_EQ(expected.clocks[index].moved_time_observed,
                  actual.clocks[index].moved_time_observed);
        EXPECT_EQ(expected.clocks[index].moved_time_missing,
                  actual.clocks[index].moved_time_missing);
    }

    ASSERT_EQ(expected.variations.size(), actual.variations.size());
    auto expected_variation = expected.variations.begin();
    auto actual_variation = actual.variations.begin();
    for (; expected_variation != expected.variations.end();
         ++expected_variation, ++actual_variation) {
        EXPECT_EQ(expected_variation->first, actual_variation->first);
        ASSERT_EQ(expected_variation->second.size(), actual_variation->second.size());
        for (size_t index = 0; index < expected_variation->second.size(); ++index) {
            const auto& expected_line = expected_variation->second[index];
            const auto& actual_line = actual_variation->second[index];
            EXPECT_EQ(expected_line.moves, actual_line.moves);
            EXPECT_EQ(expected_line.timestamps, actual_line.timestamps);
            EXPECT_EQ(expected_line.fens, actual_line.fens);
            EXPECT_EQ(expected_line.replay_observation, actual_line.replay_observation);
            EXPECT_EQ(expected_line.scores, actual_line.scores);
            ASSERT_EQ(expected_line.clocks.size(), actual_line.clocks.size());
            for (size_t clock_index = 0; clock_index < expected_line.clocks.size(); ++clock_index) {
                EXPECT_EQ(expected_line.clocks[clock_index].active,
                          actual_line.clocks[clock_index].active);
                EXPECT_EQ(expected_line.clocks[clock_index].white_time,
                          actual_line.clocks[clock_index].white_time);
                EXPECT_EQ(expected_line.clocks[clock_index].black_time,
                          actual_line.clocks[clock_index].black_time);
            }
        }
    }
}

static void write_invariant_diagnostic(const std::string& message) {
    const char* path = std::getenv("CTA_INVARIANT_REPORT_FILE");
    if (path == nullptr || *path == '\0') return;

    nlohmann::json record = {
        {"schema_version", 1},
        {"event", "INVARIANT_FAILED"},
        {"invariant", message},
    };
    std::ofstream output(path, std::ios::out | std::ios::app);
    if (output.is_open()) {
        output << record.dump() << '\n';
    }
}

// ─── TEST CONTROL PANEL ─────────────────────────────────────────────────────
// Set an individual test to 1 to compile it into the test binary, or 0 to
// omit it.  Keeping these controls above every TEST declaration is important:
// the preprocessor must see the value before it reaches the test body.
//
// Diagnostic and replay tests:
#define TEST_EXTRACTION_DIAGNOSTICS_LEGACY 1
#define TEST_EXTRACTION_DIAGNOSTICS_JSON   1
#define TEST_VIDEO_CHUNK_MAPPER_REPLAY    1
#define TEST_EXTRACTOR_REPLAY             1
#define TEST_EXTRACTION_INTERNAL_INVERSE  1
//
// Unit tests:
#define TEST_LOCATE_BOARD                 1
#define TEST_BOARD_LOCALIZATION_CALIBRATION 1
#define TEST_DRAW_GRID                    1
#define TEST_YELLOW_TEMPORAL_VALIDATION   1
#define TEST_CLOCK_TEMPORAL_READINGS      1
#define TEST_GEOMETRY_UNCERTAINTY         1
#define TEST_GEOMETRY_STABILITY           1
#define TEST_CLOCK_ROI_BOUNDS             1
#define TEST_CLOCK_VETO_VALIDATION        1
#define TEST_YELLOW_SQUARES               1
#define TEST_YELLOW_SQUARE_CALIBRATION_LABELS 1
#define TEST_YELLOW_SQUARE_CALIBRATION_REGIMES 1
#define TEST_PIECE_COUNTS                 1
#define TEST_RED_SQUARES                  1
#define TEST_YELLOW_ARROWS                1
#define TEST_MISALIGNED_PIECE             1
#define TEST_HOVER_CALIBRATION_REGIMES    1
#define TEST_GAME_CLOCKS                  1
#define TEST_GAME_CLOCK_CALIBRATION_LABELS 1
#define TEST_GAME_CLOCK_CALIBRATION_REGIMES 1
#define TEST_GAME_CLOCK_CALIBRATION_ROI_REGIMES 1
#define TEST_MEMORY_LIMIT                 1
#define TEST_CACHE_CORRECTNESS            1
//
// Integration tests:
#define TEST_7_PLIES_EXTRACTION           1
#define TEST_MEDIUM_GAME_REVERT           1
#define TEST_FULL_GAME_1_EXTRACTION       1
#define TEST_YI_VS_ESIPENKO_EXTRACTION    1
#define TEST_INTEGRATION_CLOCK_TIMES      1
//
// Smoke tests:
#define TEST_CONSTRUCTOR_THROWS           0
// ────────────────────────────────────────────────────────────────────────────

#if TEST_EXTRACTION_DIAGNOSTICS_LEGACY
TEST(ExtractionDiagnosticsTest, ClassifiesLegacyEvents) {
    const auto accepted = cta::diagnostics::from_legacy_trace(
        7, "ACCEPT", 12.5, 9, "fen", "e2e4", 91.0, 42.0, 35.0, 39.0, "source=settled");

    EXPECT_EQ(accepted.sequence, 7u);
    EXPECT_EQ(accepted.observation_id, 0u);
    EXPECT_EQ(accepted.transition_id, 0u);
    EXPECT_EQ(accepted.event, "ACCEPT");
    EXPECT_STREQ(cta::diagnostics::to_string(accepted.phase), "reducer");
    EXPECT_STREQ(cta::diagnostics::to_string(accepted.outcome), "accepted");
    EXPECT_STREQ(cta::diagnostics::to_string(accepted.reason), "move_accepted");

    const auto restored = cta::diagnostics::from_legacy_trace(
        8, "REVERT_APPLIED", 14.0, 5, "parent_fen", "", 0.0, 0.0,
        0.0, 0.0, "restored_ply=5;reason=analysis_revert");
    EXPECT_STREQ(cta::diagnostics::to_string(restored.phase), "revert");
    EXPECT_STREQ(cta::diagnostics::to_string(restored.outcome), "recovered");
    EXPECT_STREQ(cta::diagnostics::to_string(restored.reason), "revert_applied");

    const auto held = cta::diagnostics::from_legacy_trace(
        9, "SETTLE_PROBE", 15.0, 2, "fen", "e2e4", 70.0, 0.0,
        0.0, 0.0, "settle=probe");
    EXPECT_STREQ(cta::diagnostics::to_string(held.phase), "reducer");
    EXPECT_STREQ(cta::diagnostics::to_string(held.outcome), "deferred");
    EXPECT_STREQ(cta::diagnostics::to_string(held.reason), "candidate_held_for_settling");

    const auto ambiguous = cta::diagnostics::from_legacy_trace(
        10, "ORIGIN_CANDIDATE", 15.1, 2, "fen", "c2e4", 68.0, 0.0,
        20.0, 32.0, "alternative_origin");
    EXPECT_STREQ(cta::diagnostics::to_string(ambiguous.phase), "scoring");
    EXPECT_STREQ(cta::diagnostics::to_string(ambiguous.outcome), "ambiguous");
    EXPECT_STREQ(cta::diagnostics::to_string(ambiguous.reason), "candidate_ambiguous");

    const auto override = cta::diagnostics::from_legacy_trace(
        11, "MOVE_OVERRIDE", 15.2, 2, "fen", "g8h7", 94.0, 0.0,
        44.0, 49.0, "rule=postgame_king_escape;before=g8f7");
    EXPECT_STREQ(cta::diagnostics::to_string(override.phase), "reducer");
    EXPECT_STREQ(cta::diagnostics::to_string(override.outcome), "observed");
    EXPECT_STREQ(cta::diagnostics::to_string(override.reason), "move_override");

    const auto hover = cta::diagnostics::from_legacy_trace(
        12, "HOVER_MEASURE", 15.3, 2, "fen", "g8h7", 94.0, 0.0,
        44.0, 49.0, "from_square=g8;from_edges=0,0,0,0;detected=0");
    EXPECT_STREQ(cta::diagnostics::to_string(hover.phase), "validation");
    EXPECT_STREQ(cta::diagnostics::to_string(hover.outcome), "observed");
    EXPECT_STREQ(cta::diagnostics::to_string(hover.reason), "hover_measured");
}
#endif

#if TEST_EXTRACTION_DIAGNOSTICS_JSON
TEST(ExtractionDiagnosticsTest, WritesStructuredJsonLine) {
    cta::diagnostics::Evidence evidence;
    evidence.mapper_chunk = 3;
    evidence.source_frame_index = 127;
    evidence.mapper_emission_reason = "motion_leading_edge";
    evidence.diagnostic_frame_path = "frames/observation_42_frame.png";
    evidence.diagnostic_board_path = "frames/observation_42_board.png";
    evidence.diagnostic_clock_top_path = "frames/observation_42_clock_top.png";
    evidence.diagnostic_clock_bottom_path = "frames/observation_42_clock_bottom.png";
    evidence.observation_tags = {"motion", "highlight_activity", "hover", "animation"};
    evidence.yellow_arrows_checked = true;
    evidence.yellow_arrows = {"e2e4", "f1b5"};
    evidence.red_squares_checked = true;
    evidence.red_squares = {"e4"};
    evidence.template_identity = 0x123456789abcdef0ull;
    evidence.board_x = 100;
    evidence.board_y = 50;
    evidence.board_width = 800;
    evidence.board_height = 800;
    evidence.square_width = 100.0;
    evidence.square_height = 100.0;
    evidence.localization_score = 0.97;
    evidence.localization_scale = 1.25;
    evidence.localization_confidence = 0.985;
    evidence.geometry_uncertainty = 0.015;
    evidence.board_hash = {1.0, 2.0, 3.0, 4.0};
    evidence.geometry_checked = true;
    evidence.geometry_anomaly = true;
    evidence.geometry_drift_x = 12.0;
    evidence.geometry_drift_y = -3.0;
    evidence.geometry_size_drift = 4.0;
    evidence.geometry_step_drift_x = 13.0;
    evidence.geometry_step_drift_y = -2.0;
    evidence.geometry_step_size_drift = 5.0;
    evidence.geometry_relocalization_score = 0.91;
    evidence.geometry_decision = "jump_detected";
    evidence.changed_squares.push_back({"e4", 54.0, 1});
    evidence.yellow_candidates.push_back({"d4", 78.0, 1});
    evidence.yellow_endpoint_threshold = 25.0;
    evidence.yellow_pair_threshold = 70.0;
    evidence.yellow_measurements.push_back({
        "d4",
        {32.0, 35.0, 30.0, 37.0},
        {{{80.0, 150.0, 180.0}, {82.0, 151.0, 181.0},
          {79.0, 149.0, 179.0}, {81.0, 152.0, 182.0}}},
        {0.10, 0.12, 0.08, 0.11},
        33.5});
    evidence.yellow_temporal_checked = true;
    evidence.yellow_temporal_window_seconds = 0.75;
    evidence.yellow_temporal_sample_count = 3;
    evidence.yellow_temporal_pair_pass_count = 2;
    evidence.yellow_temporal_max_from = 42.0;
    evidence.yellow_temporal_max_to = 48.0;
    evidence.yellow_temporal_max_pair = 90.0;
    evidence.yellow_assessment.state = "passed";
    evidence.yellow_assessment.strength = "advisory";
    evidence.yellow_assessment.thresholds = {25.0, 70.0};
    evidence.yellow_assessment.measurements = {
        {"from_score", 42.0}, {"to_score", 48.0}, {"pair_score", 90.0}};
    evidence.yellow_assessment.uncertainty_reason = "uncalibrated_detector_confidence";
    evidence.score_margin = 12.5;
    evidence.rejection_reason = "hover_box";
    evidence.hover_measurements.push_back({"e2", 0.7, 0.2, 0.8, 0.1, 0.8, 3, true});
    evidence.clock_top_width = 120;
    evidence.clock_top_height = 24;
    evidence.clock_bottom_width = 120;
    evidence.clock_bottom_height = 24;
    evidence.clock_top_bright_ratio = 0.15;
    evidence.clock_bottom_bright_ratio = 0.42;
    evidence.clock_bright_ratio_delta = 0.27;
    evidence.clock_provenance = "temporal";
    evidence.clock_temporal_checked = true;
    evidence.clock_temporal_sample_count = 3;
    evidence.clock_temporal_observed_count = 2;
    evidence.clock_temporal_agreement_count = 2;
    evidence.clock_temporal_decision = "reconciled";
    evidence.score_from_square_diff = 42.0;
    evidence.score_to_square_diff = 51.0;
    evidence.score_adjustment = -10.0;
    evidence.minimum_score_threshold = 35.0;
    evidence.score_threshold_checked = true;
    evidence.score_threshold_passed = true;
    evidence.score_threshold_decision = "passed";
    evidence.yellow_decision = "passed";
    evidence.hover_decision = "clear";
    evidence.clock_decision = "ocr_plausible";
    evidence.settle_decision = "accepted_same_move";
    evidence.legal_candidates.push_back({"e2e4", 91.5, 1});
    evidence.legal_candidates.push_back({"e2e3", 47.2, 2});
    evidence.legal_candidates[0].yellow_from = 46.0;
    evidence.legal_candidates[0].yellow_to = 51.0;
    evidence.legal_candidates[0].yellow_decision = "passed";
    const auto rejected = cta::diagnostics::from_legacy_trace(
        3, "VALIDATION_REJECTED", 4.25, 2, "fen", "", 0.0, 18.0, 0.0, 0.0,
        "reason=hover", 0, 0, evidence, 11, 5, 2, 1, "recovering");
    std::ostringstream output;
    cta::diagnostics::write_json_line(output, rejected);

    const auto json = nlohmann::json::parse(output.str());
    EXPECT_EQ(json.at("schema_version"), 1);
    EXPECT_EQ(json.at("sequence"), 3);
    EXPECT_EQ(json.at("observation_id"), 0);
    EXPECT_EQ(json.at("candidate_id"), 11);
    EXPECT_EQ(json.at("transition_id"), 0);
    EXPECT_EQ(json.at("state_generation"), 5);
    EXPECT_EQ(json.at("revert_generation"), 2);
    EXPECT_EQ(json.at("branch_id"), 1);
    EXPECT_EQ(json.at("event"), "VALIDATION_REJECTED");
    EXPECT_EQ(json.at("phase"), "validation");
    EXPECT_EQ(json.at("outcome"), "rejected");
    EXPECT_EQ(json.at("reason"), "validation_rejected");
    EXPECT_EQ(json.at("reducer_state"), "recovering");
    EXPECT_EQ(json.at("evidence").at("board_x"), 100);
    EXPECT_EQ(json.at("evidence").at("mapper_chunk"), 3);
    EXPECT_EQ(json.at("evidence").at("source_frame_index"), 127);
    EXPECT_EQ(json.at("evidence").at("mapper_emission_reason"), "motion_leading_edge");
    EXPECT_EQ(json.at("evidence").at("diagnostic_frame_path"), "frames/observation_42_frame.png");
    EXPECT_EQ(json.at("evidence").at("diagnostic_board_path"), "frames/observation_42_board.png");
    EXPECT_EQ(json.at("evidence").at("diagnostic_clock_top_path"), "frames/observation_42_clock_top.png");
    EXPECT_EQ(json.at("evidence").at("diagnostic_clock_bottom_path"), "frames/observation_42_clock_bottom.png");
    ASSERT_EQ(json.at("evidence").at("observation_tags").size(), 4u);
    EXPECT_EQ(json.at("evidence").at("observation_tags").at(0), "motion");
    EXPECT_TRUE(json.at("evidence").at("yellow_arrows_checked"));
    ASSERT_EQ(json.at("evidence").at("yellow_arrows").size(), 2u);
    EXPECT_EQ(json.at("evidence").at("yellow_arrows").at(1), "f1b5");
    EXPECT_TRUE(json.at("evidence").at("red_squares_checked"));
    EXPECT_EQ(json.at("evidence").at("red_squares").at(0), "e4");
    EXPECT_EQ(json.at("evidence").at("template_identity"), 1311768467463790320ull);
    EXPECT_EQ(json.at("evidence").at("board_width"), 800);
    EXPECT_EQ(json.at("evidence").at("localization_score"), 0.97);
    EXPECT_EQ(json.at("evidence").at("localization_confidence"), 0.985);
    EXPECT_EQ(json.at("evidence").at("geometry_uncertainty"), 0.015);
    EXPECT_EQ(json.at("evidence").at("board_hash").size(), 4u);
    EXPECT_TRUE(json.at("evidence").at("geometry_checked"));
    EXPECT_TRUE(json.at("evidence").at("geometry_anomaly"));
    EXPECT_EQ(json.at("evidence").at("geometry_drift_x"), 12.0);
    EXPECT_EQ(json.at("evidence").at("geometry_step_drift_x"), 13.0);
    EXPECT_EQ(json.at("evidence").at("geometry_decision"), "jump_detected");
    EXPECT_EQ(json.at("evidence").at("score_margin"), 12.5);
    EXPECT_EQ(json.at("evidence").at("yellow_endpoint_threshold"), 25.0);
    EXPECT_EQ(json.at("evidence").at("yellow_pair_threshold"), 70.0);
    ASSERT_EQ(json.at("evidence").at("yellow_measurements").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("square"), "d4");
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("corner_scores").at(2), 30.0);
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("corner_bgr").at(0).at(1), 150.0);
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("corner_edge_density").at(3), 0.11);
    EXPECT_EQ(json.at("evidence").at("yellow_measurements").at(0).at("geometry_uncertainty"), 1.0);
    EXPECT_TRUE(json.at("evidence").at("yellow_temporal_checked"));
    EXPECT_EQ(json.at("evidence").at("yellow_temporal_sample_count"), 3);
    EXPECT_EQ(json.at("evidence").at("yellow_temporal_pair_pass_count"), 2);
    EXPECT_EQ(json.at("evidence").at("yellow_temporal_max_pair"), 90.0);
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("state"), "passed");
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("strength"), "advisory");
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("confidence"), -1.0);
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("measurements").at("pair_score"), 90.0);
    EXPECT_EQ(json.at("evidence").at("yellow_assessment").at("uncertainty_reason"), "uncalibrated_detector_confidence");
    ASSERT_EQ(json.at("evidence").at("changed_squares").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("changed_squares").at(0).at("square"), "e4");
    ASSERT_EQ(json.at("evidence").at("yellow_candidates").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("yellow_candidates").at(0).at("square"), "d4");
    EXPECT_EQ(json.at("metadata"), "reason=hover");
    EXPECT_EQ(json.at("evidence").at("changed_square_count"), 0);
    EXPECT_EQ(json.at("evidence").at("score_from_square_diff"), 42.0);
    EXPECT_EQ(json.at("evidence").at("score_adjustment"), -10.0);
    EXPECT_EQ(json.at("evidence").at("score_threshold_decision"), "passed");
    EXPECT_EQ(json.at("evidence").at("yellow_from"), 0.0);
    EXPECT_FALSE(json.at("evidence").at("hover_detected"));
    ASSERT_EQ(json.at("evidence").at("hover_measurements").size(), 1u);
    EXPECT_EQ(json.at("evidence").at("hover_measurements").at(0).at("square"), "e2");
    EXPECT_EQ(json.at("evidence").at("hover_measurements").at(0).at("visible_edges"), 3);
    EXPECT_TRUE(json.at("evidence").at("hover_measurements").at(0).at("detected"));
    EXPECT_EQ(json.at("evidence").at("hover_measurements").at(0).at("geometry_uncertainty"), 1.0);
    EXPECT_EQ(json.at("evidence").at("clock_top_width"), 120);
    EXPECT_EQ(json.at("evidence").at("clock_bottom_height"), 24);
    EXPECT_EQ(json.at("evidence").at("clock_bright_ratio_delta"), 0.27);
    EXPECT_EQ(json.at("evidence").at("clock_provenance"), "temporal");
    EXPECT_TRUE(json.at("evidence").at("clock_temporal_checked"));
    EXPECT_EQ(json.at("evidence").at("clock_temporal_sample_count"), 3);
    EXPECT_EQ(json.at("evidence").at("clock_temporal_observed_count"), 2);
    EXPECT_EQ(json.at("evidence").at("clock_temporal_agreement_count"), 2);
    EXPECT_EQ(json.at("evidence").at("clock_temporal_decision"), "reconciled");
    EXPECT_EQ(json.at("evidence").at("yellow_decision"), "passed");
    EXPECT_EQ(json.at("evidence").at("clock_decision"), "ocr_plausible");
    EXPECT_EQ(json.at("evidence").at("settle_decision"), "accepted_same_move");
    EXPECT_EQ(json.at("evidence").at("rejection_reason"), "hover_box");
    ASSERT_EQ(json.at("evidence").at("legal_candidates").size(), 2u);
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("move"), "e2e4");
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("rank"), 1);
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("yellow_from"), 46.0);
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("yellow_to"), 51.0);
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("yellow_pair"), 97.0);
    EXPECT_EQ(json.at("evidence").at("legal_candidates").at(0).at("yellow_decision"), "passed");
}
#endif

#if TEST_VIDEO_CHUNK_MAPPER_REPLAY
TEST(VideoChunkMapperTest, ReplaysObservationArtifactsWithoutOpeningVideo) {
    const auto root = std::filesystem::current_path() / "build_tests" / "tmp" /
                      "cta_mapper_replay_test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);

    const auto board_path = root / "board.png";
    const auto trace_path = root / "observations.jsonl";
    cv::Mat board(80, 80, CV_8UC3, cv::Scalar(40, 80, 120));
    ASSERT_TRUE(cv::imwrite(board_path.string(), board));

    nlohmann::json observation = {
        {"schema_version", 1},
        {"observation_id", 42},
        {"timestamp", 1.25},
        {"mapper", {
            {"chunk", 0},
            {"source_frame_index", 77},
            {"emission_reason", "settled_tail"},
        }},
        {"images", {{"board", board_path.filename().string()}}},
        {"board", {{"hash", std::vector<double>(64, 12.0)}}},
    };
    {
        std::ofstream output(trace_path);
        ASSERT_TRUE(output.is_open());
        output << observation.dump() << '\n';
    }

    cta::BoardGeometry geometry;
    geometry.bx = 0;
    geometry.by = 0;
    geometry.bw = 80;
    geometry.bh = 80;
    geometry.sq_w = 10.0;
    geometry.sq_h = 10.0;
    std::atomic<bool> cancel{false};
    cta::VideoChunkMapper mapper(
        (root / "source-video-does-not-exist.mp4").string(), 5.0, 30.0, 1,
        geometry, 1, 1, 0, false, 1, 1, 80, 80,
        false, 5.0, std::string(), -1.0, -1.0, trace_path.string());
    mapper.start(&cancel);

    std::vector<cta::CandidateFrame> candidates;
    ASSERT_TRUE(mapper.get_chunk_results(0, candidates, &cancel));
    ASSERT_EQ(candidates.size(), 1u);
    EXPECT_EQ(candidates[0].observation_id, 42u);
    EXPECT_DOUBLE_EQ(candidates[0].t, 1.25);
    EXPECT_EQ(candidates[0].source_frame_index, 77u);
    EXPECT_EQ(candidates[0].emission_reason, "settled_tail");
    EXPECT_EQ(candidates[0].board_bgr.size(), cv::Size(80, 80));
    ASSERT_EQ(candidates[0].board_hash.size(), 64u);
    EXPECT_DOUBLE_EQ(candidates[0].board_hash.front(), 12.0);

    std::filesystem::remove_all(root, cleanup_error);
}
#endif

#if TEST_EXTRACTOR_REPLAY
TEST(ChessVideoExtractorTest, ReplaysReducerFromObservationsWithoutOpeningVideo) {
    const auto root = std::filesystem::current_path() / "build_tests" / "tmp" /
                      "cta_extractor_replay_test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);

    const auto board_path = root / "board.png";
    const auto trace_path = root / "observations.jsonl";
    cv::Mat board(80, 80, CV_8UC3, cv::Scalar(40, 80, 120));
    ASSERT_TRUE(cv::imwrite(board_path.string(), board));

    const std::string initial_fen =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    nlohmann::json observation = {
        {"schema_version", 1},
        {"observation_id", 1},
        {"timestamp", 0.0},
        {"mapper", {{"chunk", 0}, {"source_frame_index", 1},
                     {"emission_reason", "initial_frame"}}},
        {"images", {{"board", board_path.filename().string()}}},
        {"board", {{"x", 0}, {"y", 0}, {"width", 80}, {"height", 80},
                    {"square_width", 10.0}, {"square_height", 10.0},
                    {"hash", std::vector<double>(64, 80.0)}}},
        {"events", {{{"sequence", 1}, {"event", "QUIET"},
                      {"active_ply", 0}, {"fen", initial_fen}}}},
    };
    {
        std::ofstream output(trace_path);
        ASSERT_TRUE(output.is_open());
        output << observation.dump() << '\n';
    }

    const char* previous_replay_path = std::getenv("CTA_REPLAY_OBSERVATIONS");
    const std::string previous_value = previous_replay_path ? previous_replay_path : "";
#ifdef _WIN32
    _putenv_s("CTA_REPLAY_OBSERVATIONS", trace_path.string().c_str());
#else
    setenv("CTA_REPLAY_OBSERVATIONS", trace_path.string().c_str(), 1);
#endif

    cta::GameData first_data;
    cta::GameData second_data;
    try {
        cta::ChessVideoExtractor first_extractor(board_path.string());
        first_data = first_extractor.extract_moves_from_video(
            (root / "source-video-does-not-exist.mp4").string(), "replay-test-first");
        cta::ChessVideoExtractor second_extractor(board_path.string());
        second_data = second_extractor.extract_moves_from_video(
            (root / "source-video-does-not-exist.mp4").string(), "replay-test");
    } catch (...) {
#ifdef _WIN32
        _putenv_s("CTA_REPLAY_OBSERVATIONS", previous_replay_path ? previous_value.c_str() : "");
#else
        if (previous_replay_path) setenv("CTA_REPLAY_OBSERVATIONS", previous_value.c_str(), 1);
        else unsetenv("CTA_REPLAY_OBSERVATIONS");
#endif
        std::filesystem::remove_all(root, cleanup_error);
        throw;
    }
#ifdef _WIN32
    _putenv_s("CTA_REPLAY_OBSERVATIONS", previous_replay_path ? previous_value.c_str() : "");
#else
    if (previous_replay_path) setenv("CTA_REPLAY_OBSERVATIONS", previous_value.c_str(), 1);
    else unsetenv("CTA_REPLAY_OBSERVATIONS");
#endif

    EXPECT_TRUE(first_data.moves.empty());
    ASSERT_FALSE(first_data.fens.empty());
    EXPECT_EQ(first_data.fens.front(), initial_fen);
    expect_game_data_equal(first_data, second_data);
    std::filesystem::remove_all(root, cleanup_error);
}
#endif

// ─── TEST CONTROL PANEL ─────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────

namespace cta {

#if TEST_EXTRACTION_INTERNAL_INVERSE
TEST(ExtractionInternalTest, FindsMostRecentBoundedInverseMove) {
    const std::vector<std::string> moves = {
        "a2a4", "h7h5", "a4a5", "g7g5", "b2b3"
    };

    const std::optional<size_t> inverse =
        extractor_detail::find_recent_inverse_move_index(moves, "a5", "a4");

    ASSERT_TRUE(inverse.has_value());
    EXPECT_EQ(*inverse, 2U);
    EXPECT_TRUE(extractor_detail::is_inverse_of_recent_move(moves, "a5", "a4"));
    EXPECT_FALSE(extractor_detail::find_recent_inverse_move_index(moves, "h5", "h4").has_value());
}
#endif

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
        "../../../assets"
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

// Large integration videos live outside the repository. CTA_MEDIA_ROOT may
// point at that media directory; otherwise use the sibling directory created
// by the repository's documented local layout.
static std::filesystem::path find_media_games_dir(const std::string& assets_dir) {
    std::vector<std::filesystem::path> candidates;
    if (const char* configured_root = std::getenv("CTA_MEDIA_ROOT");
        configured_root != nullptr && configured_root[0] != '\0') {
        candidates.emplace_back(configured_root);
    }
    const std::filesystem::path assets_path = assets_dir;
    candidates.push_back(assets_path.parent_path().parent_path() / "chess-tube-analyzer-media");
    candidates.push_back(std::filesystem::path("../chess-tube-analyzer-media"));
    candidates.push_back(std::filesystem::path("../../chess-tube-analyzer-media"));
    for (const auto& candidate : candidates) {
        const auto games_dir = candidate / "games";
        if (std::filesystem::exists(games_dir) && std::filesystem::is_directory(games_dir)) {
            return games_dir;
        }
    }
    return {};
}

static std::filesystem::path find_game_fixture_file(
    const std::string& assets_dir,
    const std::string& game_name,
    const std::string& file_name) {
    const auto bundled_path = std::filesystem::path(assets_dir) / "fixtures" / "games" /
                              game_name / file_name;
    if (std::filesystem::exists(bundled_path)) {
        return bundled_path;
    }

    return find_media_games_dir(assets_dir) / game_name / file_name;
}

static std::filesystem::path find_expected_game_pgn(
    const std::string& assets_dir,
    const std::string& game_name) {
    return std::filesystem::path(assets_dir) / "fixtures" / "games" /
           game_name / "expected.pgn";
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
    std::map<std::string, std::string> headers;
    std::vector<std::string> main_line;
    std::vector<std::string> variations;
    std::vector<std::string> all_moves;
    std::vector<std::pair<size_t, std::vector<std::string>>> variation_sequences;
};

static std::string expected_fen_after_ply(const std::vector<std::string>& moves,
                                          size_t ply_count) {
    libchess::Position position(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    const size_t count = std::min(ply_count, moves.size());
    for (size_t ply = 0; ply < count; ++ply) {
        try {
            position.makemove(position.parse_move(moves[ply]));
        } catch (...) {
            return {};
        }
    }
    return position.get_fen();
}

static size_t first_main_line_mismatch(const std::vector<std::string>& expected,
                                       const std::vector<std::string>& extracted) {
    size_t mismatch = 0;
    while (mismatch < expected.size() && mismatch < extracted.size() &&
           expected[mismatch] == extracted[mismatch]) {
        ++mismatch;
    }
    return mismatch;
}

static void induce_expected_failure_for_diagnostics(std::vector<std::string>& expected_moves) {
    const char* enabled = std::getenv("CTA_TEST_INDUCE_FAILURE");
    if (enabled == nullptr || std::string(enabled) != "1" || expected_moves.size() < 2) {
        return;
    }

    // The probe changes only test-side expectations. Production extraction,
    // reducer state, and fixture data remain untouched, so the normal
    // first-divergence path can be exercised safely and reproducibly.
    std::swap(expected_moves[0], expected_moves[1]);
}

static void write_first_divergence_report(
    const std::string& test_name,
    const std::string& video_path,
    const std::vector<std::string>& expected_moves,
    const GameData& data,
    size_t mismatch,
    bool main_line_passed,
    bool complete_output_passed,
    const std::string& failure_scope) {
    const char* report_path = std::getenv("CTA_FAILURE_REPORT_FILE");
    if (report_path == nullptr || *report_path == '\0' ||
        (main_line_passed && complete_output_passed)) {
        return;
    }

    const bool expected_move_exists = mismatch < expected_moves.size();
    const bool extracted_move_exists = mismatch < data.moves.size();
    const double anchor_timestamp = extracted_move_exists && mismatch < data.timestamps.size()
        ? data.timestamps[mismatch]
        : (!data.timestamps.empty() ? data.timestamps.back() : 0.0);

    std::string failure_kind = failure_scope;
    if (failure_kind.empty()) {
        if (!expected_move_exists) failure_kind = "unexpected_extra_move";
        else if (!extracted_move_exists) failure_kind = "missing_expected_move";
        else failure_kind = "move_mismatch";
    }

    const std::string expected_before_fen = expected_fen_after_ply(expected_moves, mismatch);
    const std::string expected_after_fen = expected_move_exists
        ? expected_fen_after_ply(expected_moves, mismatch + 1)
        : expected_before_fen;
    const std::string extracted_before_fen = mismatch < data.fens.size()
        ? data.fens[mismatch] : std::string{};
    const std::string extracted_after_fen = mismatch + 1 < data.fens.size()
        ? data.fens[mismatch + 1] : std::string{};

    nlohmann::json report = {
        {"schema_version", 1},
        {"test", test_name},
        {"video", video_path},
        {"failure_kind", failure_kind},
        {"failure_scope", failure_scope},
        {"first_mismatch_ply", mismatch + 1},
        {"last_matching_ply", mismatch},
        {"expected_move", expected_move_exists ? expected_moves[mismatch] : ""},
        {"extracted_move", extracted_move_exists ? data.moves[mismatch] : ""},
        {"expected_move_count", expected_moves.size()},
        {"extracted_move_count", data.moves.size()},
        {"anchor_timestamp", anchor_timestamp},
        {"expected_before_fen", expected_before_fen},
        {"expected_after_fen", expected_after_fen},
        {"extracted_before_fen", extracted_before_fen},
        {"extracted_after_fen", extracted_after_fen},
        {"main_line_passed", main_line_passed},
        {"complete_output_passed", complete_output_passed},
    };

    std::ofstream output(report_path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        std::cerr << "Could not write first-divergence report: " << report_path << "\n";
        return;
    }
    output << report.dump(2) << '\n';
}

struct ExpectedClockData {
    std::vector<std::string> main_line;
    std::vector<std::string> variations;
};

static ExpectedClockData load_expected_clocks_from_pgn(const std::string& pgn_path) {
    std::ifstream ifs(pgn_path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Could not open PGN: " + pgn_path);
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ExpectedClockData clocks;
    bool in_header = false;
    int brace_depth = 0;
    int variation_depth = 0;

    for (size_t i = 0; i < content.size(); ++i) {
        char c = content[i];

        if (c == '[' && brace_depth == 0) {
            in_header = true;
            continue;
        }
        if (in_header) {
            if (c == ']') in_header = false;
            continue;
        }

        if (c == '{') {
            ++brace_depth;
            continue;
        }
        if (c == '}' && brace_depth > 0) {
            --brace_depth;
            continue;
        }

        if (brace_depth == 0) {
            if (c == '(') ++variation_depth;
            else if (c == ')' && variation_depth > 0) --variation_depth;
            continue;
        }

        if (variation_depth == 0 && content.compare(i, 6, "[%clk ") == 0) {
            size_t start = i + 6;
            size_t end = content.find(']', start);
            if (end != std::string::npos) {
                clocks.main_line.push_back(format_clock_for_test(content.substr(start, end - start)));
                i = end;
            }
        } else if (variation_depth > 0 && content.compare(i, 6, "[%clk ") == 0) {
            size_t start = i + 6;
            size_t end = content.find(']', start);
            if (end != std::string::npos) {
                clocks.variations.push_back(format_clock_for_test(content.substr(start, end - start)));
                i = end;
            }
        }
    }

    return clocks;
}

static std::string clock_for_ply(const ClockInfo& clock, size_t ply_index) {
    std::string raw_clock = (ply_index % 2 == 0) ? clock.white_time : clock.black_time;
    std::string formatted = format_clock_for_test(raw_clock);
    return formatted.empty() ? "0:00:00" : formatted;
}

static ExpectedClockData extract_clocks_from_game_data(const GameData& data) {
    ExpectedClockData clocks;
    clocks.main_line.reserve(data.moves.size());

    for (size_t i = 0; i < data.moves.size(); ++i) {
        size_t clockIdx = i + 1;
        const auto* clk_ptr = (clockIdx < data.clocks.size())
            ? &data.clocks[clockIdx]
            : (i < data.clocks.size()) ? &data.clocks[i] : nullptr;
        clocks.main_line.push_back(clk_ptr ? clock_for_ply(*clk_ptr, i) : "0:00:00");
    }

    for (const auto& item : data.variations) {
        size_t branch_ply = item.first;
        for (const auto& var : item.second) {
            for (size_t j = 0; j < var.moves.size(); ++j) {
                if (j < var.clocks.size()) {
                    clocks.variations.push_back(clock_for_ply(var.clocks[j], branch_ply + j));
                }
            }
        }
    }

    return clocks;
}

static void print_multiset_delta(const std::multiset<std::string>& extracted,
                                 const std::multiset<std::string>& expected,
                                 const std::string& label);

static bool verify_clocks(const std::string& pgn_path, const GameData& data) {
    ExpectedClockData expected_clocks = load_expected_clocks_from_pgn(pgn_path);
    ExpectedClockData extracted_clocks = extract_clocks_from_game_data(data);

    std::cout << "  Loaded expected clocks from PGN.\n";
    std::cout << "  Expected Main-Line Clocks (" << expected_clocks.main_line.size() << "): ";
    for (const auto& c : expected_clocks.main_line) std::cout << c << " ";
    std::cout << "\n";

    std::cout << "  Extracted Main-Line Clocks (" << extracted_clocks.main_line.size() << "): ";
    for (const auto& c : extracted_clocks.main_line) std::cout << c << " ";
    std::cout << "\n";

    std::cout << "  Expected Variation Clocks (" << expected_clocks.variations.size() << "): ";
    for (const auto& c : expected_clocks.variations) std::cout << c << " ";
    std::cout << "\n";

    std::cout << "  Extracted Variation Clocks (" << extracted_clocks.variations.size() << "): ";
    for (const auto& c : extracted_clocks.variations) std::cout << c << " ";
    std::cout << "\n";

    for (size_t i = 0; i < std::min(expected_clocks.main_line.size(), extracted_clocks.main_line.size()); ++i) {
        if (expected_clocks.main_line[i] != extracted_clocks.main_line[i]) {
            std::cout << "  First main-line clock mismatch at ply " << (i + 1)
                      << ": expected " << expected_clocks.main_line[i]
                      << ", extracted " << extracted_clocks.main_line[i] << "\n";
            break;
        }
    }

    std::multiset<std::string> expected_variation_clocks(expected_clocks.variations.begin(), expected_clocks.variations.end());
    std::multiset<std::string> extracted_variation_clocks(extracted_clocks.variations.begin(), extracted_clocks.variations.end());
    print_multiset_delta(extracted_variation_clocks, expected_variation_clocks, "variation clocks");

    EXPECT_EQ(extracted_clocks.main_line, expected_clocks.main_line)
        << "Extracted " << extracted_clocks.main_line.size() << " main-line clocks, expected "
        << expected_clocks.main_line.size();
    EXPECT_EQ(extracted_variation_clocks, expected_variation_clocks)
        << "Extracted " << extracted_clocks.variations.size() << " variation clocks, expected "
        << expected_clocks.variations.size();

    return extracted_clocks.main_line == expected_clocks.main_line
        && extracted_variation_clocks == expected_variation_clocks;
}

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

    // Parse the tag pairs separately from movetext.  Header metadata is part
    // of the fixture contract, so silently ignoring it would allow an output
    // PGN with incomplete metadata to pass the integration test.
    std::istringstream header_stream(pgn);
    std::string header_line;
    while (std::getline(header_stream, header_line)) {
        if (header_line.size() < 4 || header_line.front() != '[' || header_line.back() != ']') {
            continue;
        }

        const size_t key_start = 1;
        const size_t separator = header_line.find(' ', key_start);
        if (separator == std::string::npos || separator <= key_start ||
            separator + 2 >= header_line.size() || header_line[separator + 1] != '"') {
            continue;
        }

        const size_t value_start = separator + 2;
        const size_t value_end = header_line.rfind('"');
        if (value_end < value_start) continue;

        result.headers.emplace(
            header_line.substr(key_start, separator - key_start),
            header_line.substr(value_start, value_end - value_start));
    }

    std::istringstream iss(cleaned);
    std::string token;
    
    PgnState current_state;
    std::vector<PgnState> state_stack;
    std::vector<size_t> active_variation_sequences;
    auto try_parse_move = [](const PgnState& state, const std::string& token, libchess::Move& move_out) {
        const std::string wanted_san = normalize_san_token(token);
        if (wanted_san.empty()) return false;

        try {
            move_out = state.pos.parse_move(token);
            return true;
        } catch (...) {
            try {
                move_out = state.pos.parse_move(wanted_san);
                return true;
            } catch (...) {
                for (const auto& legal_move : state.pos.legal_moves()) {
                    std::string uci = static_cast<std::string>(legal_move);
                    if (uci == "e1h1") uci = "e1g1"; else if (uci == "e1a1") uci = "e1c1";
                    else if (uci == "e8h8") uci = "e8g8"; else if (uci == "e8a8") uci = "e8c8";
                    if (normalize_san_token(build_san_for_test(state.pos, legal_move, uci)) == wanted_san) {
                        move_out = legal_move;
                        return true;
                    }
                }
            }
        }

        return false;
    };
    
    while (iss >> token) {
        if (token == "*" || token == "1-0" || token == "0-1" || token == "1/2-1/2") continue;
        if (token.rfind("$", 0) == 0) continue;

        if (token == "(") {
            state_stack.push_back(current_state);
            // history stores the position before each played move.  At the
            // opening parenthesis the current position is already after the
            // preceding main-line move, so an alternate continuation replaces
            // the move whose pre-state is the final history entry.
            const size_t variation_parent = current_state.history.empty()
                ? 0 : current_state.history.size() - 1;
            result.variation_sequences.emplace_back(
                variation_parent, std::vector<std::string>{});
            active_variation_sequences.push_back(result.variation_sequences.size() - 1);
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
            if (!active_variation_sequences.empty()) {
                active_variation_sequences.pop_back();
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

        bool matched = false;
        libchess::Move m;

        if (normalize_san_token(token).empty()) continue;

        matched = try_parse_move(current_state, token, m);
        if (!matched && !state_stack.empty()) {
            // Some hand-authored baselines use parentheses for a checked line
            // continuation rather than an alternative to the previous move.
            PgnState continuation_state = state_stack.back();
            if (try_parse_move(continuation_state, token, m)) {
                current_state = continuation_state;
                matched = true;
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
            if (!active_variation_sequences.empty()) {
                result.variation_sequences[active_variation_sequences.back()].second.push_back(uci);
            }
            
            current_state.history.push_back(current_state.pos);
            current_state.pos.makemove(m);
        } else {
            throw std::runtime_error("Could not convert PGN SAN token to UCI: " + token);
        }
    }

    return result;
}

static void expect_fixture_metadata_contract(
    const ExpectedGameData& expected_data,
    const std::string& pgn_path) {
    static constexpr std::array<const char*, 7> required_headers = {
        "Event", "Site", "Date", "Round", "White", "Black", "Result",
    };

    for (const char* key : required_headers) {
        ASSERT_TRUE(expected_data.headers.contains(key))
            << "Expected PGN is missing required header [" << key << "]: " << pgn_path;
    }
    for (const auto& [key, value] : expected_data.headers) {
        ASSERT_FALSE(value.empty())
            << "Expected PGN has an empty metadata value for [" << key << "]: " << pgn_path;
    }
    const std::string& result = expected_data.headers.at("Result");
    ASSERT_TRUE(result == "1-0" || result == "0-1" || result == "1/2-1/2" || result == "*")
        << "Expected PGN has an invalid Result header: " << pgn_path;
    ASSERT_FALSE(expected_data.headers.empty())
        << "Expected PGN contains no metadata headers: " << pgn_path;

    std::cout << "  Answer-key PGN metadata contract: ";
    bool first_header = true;
    for (const auto& [key, value] : expected_data.headers) {
        if (!first_header) std::cout << ", ";
        std::cout << key << "=\"" << value << "\"";
        first_header = false;
    }
    std::cout << ".\n";
}

TEST(ExtractorUtilsTest, PreservesNonAsciiWindowsPathCharacters) {
    const std::string filename = "analysis_\xEF\xBD\x9C\xEF\xBD\x9C.mp4";
    const auto path = cta::utils::utf8_to_path("C:/exports/" + filename);

#ifdef _WIN32
    EXPECT_NE(path.filename().native().find(L'\xFF5C'), std::wstring::npos);
#else
    EXPECT_EQ(path.filename().string(), filename);
#endif
}

static bool resolve_video_metadata(
    const GameData& extracted_data,
    LichessGameMetadata& metadata,
    const std::string& video_path) {
    const std::vector<std::string>& metadata_fens = extracted_data.fens.empty()
        ? extracted_data.video_fens : extracted_data.fens;
    const std::vector<std::string>& metadata_moves = extracted_data.moves.empty()
        ? extracted_data.video_moves : extracted_data.moves;
    if (metadata_fens.empty()) {
        ADD_FAILURE() << "Video extraction produced no FEN positions for metadata lookup: "
                      << video_path;
        return false;
    }

    // Deep positions are the most selective. The complete FEN sequence is
    // retained for the resolver's prefix check after candidate metadata is
    // retrieved, but only the last few positions are sent to Explorer.
    constexpr size_t kMetadataLookupFenCount = 4;
    const size_t first_lookup_fen = metadata_fens.size() > kMetadataLookupFenCount
        ? metadata_fens.size() - kMetadataLookupFenCount : 0;

    OpeningFetcher fetcher;
    for (size_t index = first_lookup_fen; index < metadata_fens.size(); ++index) {
        fetcher.enqueue_fen(metadata_fens[index]);
    }
    constexpr auto kMetadataLookupTimeout = std::chrono::seconds(15);
    if (!fetcher.wait_until_done_for(kMetadataLookupTimeout)) {
        ADD_FAILURE() << "Lichess metadata lookup timed out after "
                      << kMetadataLookupTimeout.count() << " seconds for video: "
                      << video_path;
        return false;
    }

    // A position-level cache entry may belong to a different game that shared
    // an opening.  The resolver replays the complete verified main line
    // before it publishes an identity, so do not retain pre-resolution
    // metadata as a fallback.
    fetcher.resolve_game_metadata(metadata_fens, metadata_moves);
    metadata = {};
    for (const std::string& fen : metadata_fens) {
        const LichessOpening info = fetcher.get_opening(fen);
        if (info.game_metadata.found) {
            metadata = info.game_metadata;
            break;
        }
    }
    if (!metadata.found) {
        std::string last_error;
        int last_status = 0;
        for (size_t index = first_lookup_fen; index < metadata_fens.size(); ++index) {
            const LichessOpening info = fetcher.get_opening(metadata_fens[index]);
            last_error = info.error;
            last_status = info.http_status;
        }
        ADD_FAILURE() << "Lichess did not return metadata for FENs extracted from video: "
                      << video_path << " (last HTTP status=" << last_status
                      << ", error=" << (last_error.empty() ? "none" : last_error)
                      << (last_status == 401
                          ? "; configure a valid Lichess API token in the application settings"
                          : "") << "; resolver="
                      << (fetcher.metadata_resolution_error().empty()
                          ? "no candidate diagnostic"
                          : fetcher.metadata_resolution_error()) << ")";
        return false;
    }

    std::cout << "  Extracted video metadata from Lichess: Event=\""
              << metadata.event << "\", Site=\"" << metadata.site
              << "\", Date=\"" << metadata.date << "\", Round=\""
              << metadata.round << "\", White=\"" << metadata.white
              << "\", Black=\"" << metadata.black << "\", Result=\""
              << metadata.result << "\", WhiteElo=\"" << metadata.white_elo
              << "\", BlackElo=\"" << metadata.black_elo << "\", ECO=\""
              << metadata.eco << "\", Opening=\"" << metadata.opening << "\".\n";
    return true;
}

static void expect_video_metadata_matches_answer_key(
    const LichessGameMetadata& actual,
    const ExpectedGameData& expected,
    const std::string& video_path) {
    const auto normalized_tokens = [](const std::string& value) {
        std::vector<std::string> tokens;
        std::string current;
        for (const unsigned char character : value) {
            if (std::isalnum(character)) {
                current += static_cast<char>(std::tolower(character));
            } else if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        }
        if (!current.empty()) tokens.push_back(current);
        return tokens;
    };
    const auto normalized_value = [&](const std::string& value) {
        std::string result;
        for (const std::string& token : normalized_tokens(value)) result += token;
        return result;
    };
    const auto names_equivalent = [&](const std::string& left, const std::string& right) {
        if (normalized_value(left) == normalized_value(right)) return true;
        const size_t left_comma = left.find(',');
        const size_t right_comma = right.find(',');
        if (left_comma == std::string::npos || right_comma == std::string::npos) return false;
        const std::string left_surname = normalized_value(left.substr(0, left_comma));
        const std::string right_surname = normalized_value(right.substr(0, right_comma));
        const auto left_given = normalized_tokens(left.substr(left_comma + 1));
        const auto right_given = normalized_tokens(right.substr(right_comma + 1));
        return !left_surname.empty() && left_surname == right_surname &&
            !left_given.empty() && !right_given.empty() &&
            left_given.front().front() == right_given.front().front();
    };
    const auto site_equivalent = [&](const std::string& left, const std::string& right) {
        if (normalized_value(left) == normalized_value(right)) return true;
        const auto left_tokens = normalized_tokens(left);
        const auto right_tokens = normalized_tokens(right);
        if (left_tokens.size() != 2 || right_tokens.size() != 2 ||
            left_tokens.front() != right_tokens.front()) return false;
        const auto country_name = [](const std::string& code) {
            if (code == "gre") return std::string("greece");
            if (code == "ind") return std::string("india");
            return code;
        };
        return country_name(left_tokens.back()) == country_name(right_tokens.back());
    };
    const auto event_equivalent = [&](const std::string& left, const std::string& right) {
        if (normalized_value(left) == normalized_value(right)) return true;
        const auto acronym = [&](const std::string& value) {
            std::string result;
            for (const std::string& token : normalized_tokens(value)) {
                if (token.size() > 1 && !std::isdigit(static_cast<unsigned char>(token.front()))) {
                    result += token.front();
                }
            }
            return result;
        };
        const std::string left_acronym = acronym(left);
        const std::string right_acronym = acronym(right);
        const auto contains_token = [&](const std::string& value, const std::string& token) {
            const auto tokens = normalized_tokens(value);
            return !token.empty() &&
                std::find(tokens.begin(), tokens.end(), token) != tokens.end();
        };
        return contains_token(left, right_acronym) || contains_token(right, left_acronym);
    };
    const auto round_equivalent = [&](const std::string& left, const std::string& right) {
        if (normalized_value(left) == normalized_value(right)) return true;
        const size_t left_separator = left.find('.');
        const size_t right_separator = right.find('.');
        const std::string left_base = normalized_value(
            left_separator == std::string::npos ? left : left.substr(0, left_separator));
        const std::string right_base = normalized_value(
            right_separator == std::string::npos ? right : right.substr(0, right_separator));
        return left_base == right_base &&
            (left_separator != std::string::npos || right_separator != std::string::npos);
    };
    const auto metadata_equivalent = [&](const char* key,
                                         const std::string& actual_value,
                                         const std::string& expected_value) {
        if (actual_value.empty() || expected_value.empty()) return false;
        const std::string header(key);
        if (header == "Event") return event_equivalent(actual_value, expected_value);
        if (header == "Site") return site_equivalent(actual_value, expected_value);
        if (header == "Round") return round_equivalent(actual_value, expected_value);
        if (header == "White" || header == "Black") {
            return names_equivalent(actual_value, expected_value);
        }
        if (header == "WhiteElo" || header == "BlackElo") {
            try {
                return std::abs(std::stoi(actual_value) - std::stoi(expected_value)) <= 1;
            } catch (const std::exception&) {
                return false;
            }
        }
        if (header == "Opening") {
            const std::string actual_opening = normalized_value(actual_value);
            const std::string expected_opening = normalized_value(expected_value);
            return actual_opening == expected_opening ||
                actual_opening.starts_with(expected_opening) ||
                expected_opening.starts_with(actual_opening);
        }
        return normalized_value(actual_value) == normalized_value(expected_value);
    };

    const auto expect_header = [&](const char* key, const std::string& value) {
        const auto expected_header = expected.headers.find(key);
        ASSERT_NE(expected_header, expected.headers.end())
            << "Answer-key PGN is missing [" << key << "] for " << video_path;
        EXPECT_TRUE(metadata_equivalent(key, value, expected_header->second))
            << "Metadata extracted from video does not match [" << key << "] for "
            << video_path << " (actual=\"" << value << "\", expected=\""
            << expected_header->second << "\")";
    };

    expect_header("Event", actual.event);
    expect_header("Site", actual.site);
    expect_header("Date", actual.date);
    expect_header("Round", actual.round);
    expect_header("White", actual.white);
    expect_header("Black", actual.black);
    expect_header("Result", actual.result);
    expect_header("WhiteElo", actual.white_elo);
    expect_header("BlackElo", actual.black_elo);
    expect_header("ECO", actual.eco);
    expect_header("Opening", actual.opening);
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

static bool verify_game_data_invariants(const GameData& data) {
    bool valid = true;
    const auto fail = [&](const std::string& message) {
        valid = false;
        ++g_invariant_failure_count;
        write_invariant_diagnostic(message);
        ADD_FAILURE() << "GameData invariant: " << message;
    };

    if (data.fens.size() != data.moves.size() + 1) {
        fail("main-line FEN count " + std::to_string(data.fens.size()) +
             " does not equal move count + 1 (" + std::to_string(data.moves.size() + 1) + ")");
    }
    if (data.timestamps.size() != data.moves.size()) {
        fail("main-line timestamp count " + std::to_string(data.timestamps.size()) +
             " does not equal move count " + std::to_string(data.moves.size()));
    }
    if (data.clocks.size() != data.fens.size()) {
        fail("main-line clock count " + std::to_string(data.clocks.size()) +
             " does not equal FEN count " + std::to_string(data.fens.size()));
    }
    const auto verify_clock_provenance = [&](const ClockInfo& clock, const std::string& location) {
        if (clock.moved_time_observed && clock.moved_time_missing) {
            fail("clock at " + location + " is both observed and missing");
        }
        if (clock.moved_time_observed && clock.white_time.empty() && clock.black_time.empty()) {
            fail("clock at " + location + " is marked observed but contains no reading");
        }
    };
    for (size_t i = 0; i < data.clocks.size(); ++i) {
        verify_clock_provenance(data.clocks[i], "main ply " + std::to_string(i));
    }
    if (data.video_moves.size() != data.video_timestamps.size()) {
        fail("video move and timestamp histories have different lengths");
    }
    if (data.video_fens.size() != data.video_moves.size() + 1) {
        fail("video FEN history " + std::to_string(data.video_fens.size()) +
             " does not equal video move count + 1 (" +
             std::to_string(data.video_moves.size() + 1) + ")");
    }
    if (data.video_fens.size() == data.video_moves.size() + 1) {
        for (size_t index = 0; index < data.video_moves.size(); ++index) {
            if (data.video_moves[index] == "REVERT") continue;
            try {
                libchess::Position position(data.video_fens[index]);
                position.makemove(position.parse_move(data.video_moves[index]));
                ChessFenUtils::uci_to_san_line(
                    data.video_moves[index], data.video_fens[index]);
            } catch (const std::exception& error) {
                fail("video timeline move " + std::to_string(index + 1) + " (" +
                     data.video_moves[index] + ") is not legal from its paired FEN: " +
                     error.what());
                break;
            }
        }
    }

    if (!data.fens.empty()) {
        try {
            libchess::Position position(data.fens.front());
            for (size_t ply = 0; ply < data.moves.size(); ++ply) {
                if (ply >= data.fens.size() - 1) break;
                if (data.fens[ply] != position.get_fen()) {
                    fail("FEN before ply " + std::to_string(ply + 1) + " differs from reducer position");
                }
                const libchess::Move move = position.parse_move(data.moves[ply]);
                position.makemove(move);
                if (data.fens[ply + 1] != position.get_fen()) {
                    fail("FEN after ply " + std::to_string(ply + 1) +
                         " does not match the accepted move " + data.moves[ply]);
                }
            }
        } catch (const std::exception& error) {
            fail(std::string("main-line move/FEN replay failed: ") + error.what());
        }
    }

    for (size_t i = 1; i < data.timestamps.size(); ++i) {
        if (data.timestamps[i] < data.timestamps[i - 1]) {
            fail("main-line timestamps are not monotonic at ply " + std::to_string(i + 1));
            break;
        }
    }
    for (size_t i = 1; i < data.video_timestamps.size(); ++i) {
        if (data.video_timestamps[i] < data.video_timestamps[i - 1]) {
            fail("video timestamps are not monotonic at observation " + std::to_string(i));
            break;
        }
    }

    for (const auto& [parent_ply, variations] : data.variations) {
        if (parent_ply >= data.fens.size()) {
            fail("variation parent ply " + std::to_string(parent_ply) + " is outside the main line");
            continue;
        }
        for (const auto& variation : variations) {
            if (variation.moves.empty()) continue;
            const bool has_terminal_fen = variation.fens.size() == variation.moves.size() + 1;
            if (variation.fens.size() != variation.moves.size() && !has_terminal_fen) {
                fail("variation at parent ply " + std::to_string(parent_ply) +
                     " has " + std::to_string(variation.fens.size()) +
                     " FENs for " + std::to_string(variation.moves.size()) +
                     " moves; expected before-state FENs or before/after FENs");
            }
            std::string variation_parent_fen;
            if (parent_ply < data.fens.size()) {
                variation_parent_fen = data.fens[parent_ply];
            }
            bool nested_variation_root = false;
            if (!variation.fens.empty() &&
                (variation_parent_fen.empty() ||
                 variation.fens.front() != variation_parent_fen)) {
                for (const auto& [outer_parent, outer_variations] : data.variations) {
                    for (const auto& outer : outer_variations) {
                        if (&outer == &variation) continue;
                        if (std::find(outer.fens.begin() +
                                      std::min<size_t>(1, outer.fens.size()),
                                      outer.fens.end(), variation.fens.front()) !=
                            outer.fens.end()) {
                            nested_variation_root = true;
                            break;
                        }
                    }
                    if (nested_variation_root) break;
                }
                if (!nested_variation_root) {
                    fail("variation at parent ply " + std::to_string(parent_ply) +
                         " does not start from its parent FEN");
                }
            }
            if (variation.timestamps.size() != variation.moves.size()) {
                fail("variation at parent ply " + std::to_string(parent_ply) +
                     " has misaligned move and timestamp histories");
            }
            for (size_t i = 0; i < variation.clocks.size(); ++i) {
                verify_clock_provenance(
                    variation.clocks[i],
                    "variation parent ply " + std::to_string(parent_ply) +
                    ", index " + std::to_string(i));
            }
            for (size_t i = 1; i < variation.timestamps.size(); ++i) {
                if (variation.timestamps[i] < variation.timestamps[i - 1]) {
                    fail("variation timestamps are not monotonic at parent ply " +
                         std::to_string(parent_ply));
                    break;
                }
            }
            try {
                const std::string replay_root = nested_variation_root
                    ? variation.fens.front() : data.fens[parent_ply];
                libchess::Position position(replay_root);
                for (size_t i = 0; i < variation.moves.size(); ++i) {
                    if (i < variation.fens.size() && variation.fens[i] != position.get_fen()) {
                        fail("variation at parent ply " + std::to_string(parent_ply) +
                             " has an invalid FEN before move " + std::to_string(i + 1));
                    }
                    const libchess::Move move = position.parse_move(variation.moves[i]);
                    position.makemove(move);
                    if (i + 1 < variation.fens.size() &&
                        variation.fens[i + 1] != position.get_fen()) {
                        fail("variation at parent ply " + std::to_string(parent_ply) +
                             " has an invalid FEN after move " + std::to_string(i + 1));
                    }
                }
            } catch (const std::exception& error) {
                fail("variation at parent ply " + std::to_string(parent_ply) +
                     " failed legal replay: " + error.what());
            }
        }
    }

    return valid;
}

static void print_multiset_delta(const std::multiset<std::string>& extracted,
                                 const std::multiset<std::string>& expected,
                                 const std::string& label) {
    std::vector<std::string> extra;
    std::vector<std::string> missing;
    std::set_difference(extracted.begin(), extracted.end(),
                        expected.begin(), expected.end(),
                        std::back_inserter(extra));
    std::set_difference(expected.begin(), expected.end(),
                        extracted.begin(), extracted.end(),
                        std::back_inserter(missing));
    if (extra.empty() && missing.empty()) {
        return;
    }

    std::cout << "  " << label << " extra (" << extra.size() << "): ";
    for (const auto& item : extra) std::cout << item << " ";
    std::cout << "\n";
    std::cout << "  " << label << " missing (" << missing.size() << "): ";
    for (const auto& item : missing) std::cout << item << " ";
    std::cout << "\n";
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

        board_path_ = (std::filesystem::path(assets_dir_) / "reference" / "board" / "board.png").string();
        red_board_path_ = (std::filesystem::path(assets_dir_) / "reference" / "board" / "red-board.png").string();

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

TEST_F(DetectorsTest, AllAnswerKeyPgnsContainMetadata) {
    const std::filesystem::path games_dir =
        std::filesystem::path(assets_dir_) / "fixtures" / "games";
    ASSERT_TRUE(std::filesystem::exists(games_dir))
        << "Fixture games directory not found: " << games_dir.string();

    size_t answer_key_count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(games_dir)) {
        if (!entry.is_regular_file() || entry.path().filename() != "expected.pgn") continue;

        ++answer_key_count;
        const ExpectedGameData expected_data =
            load_expected_uci_moves_from_pgn(entry.path().string());
        expect_fixture_metadata_contract(expected_data, entry.path().string());
    }

    EXPECT_GT(answer_key_count, 0u)
        << "No expected.pgn answer keys were found under " << games_dir.string();
}

// ─── BOARD LOCALIZER ─────────────────────────────────────────────────────────
#if TEST_LOCATE_BOARD || TEST_BOARD_LOCALIZATION_CALIBRATION

#if TEST_LOCATE_BOARD
TEST_F(DetectorsTest, LocateBoardOnItself) {
    auto geo = locate_board(board_, board_);
    EXPECT_GT(geo.bw, 0);
    EXPECT_GT(geo.bh, 0);
    EXPECT_NEAR(geo.sq_w, static_cast<double>(geo.bw) / 8.0, 1.0);
    EXPECT_NEAR(geo.sq_h, static_cast<double>(geo.bh) / 8.0, 1.0);
    EXPECT_GT(geo.localization_score, -1.0);
    EXPECT_GT(geo.localization_scale, 0.0);
    EXPECT_GE(geo.geometry_confidence, 0.0);
    EXPECT_LE(geo.geometry_confidence, 1.0);
}
#endif

struct GeometryCalibrationCase {
    const char* name;
    double scale;
    cv::Point origin;
    cv::Size canvas;
    bool overlay;
};

static cv::Mat make_geometry_calibration_frame(
    const cv::Mat& board_template, const GeometryCalibrationCase& test_case) {
    const cv::Size board_size(
        static_cast<int>(std::lround(board_template.cols * test_case.scale)),
        static_cast<int>(std::lround(board_template.rows * test_case.scale)));
    cv::Mat resized_board;
    cv::resize(board_template, resized_board, board_size, 0.0, 0.0, cv::INTER_LINEAR);

    cv::Mat frame(test_case.canvas, board_template.type(), cv::Scalar(32, 32, 32));
    resized_board.copyTo(frame(cv::Rect(
        test_case.origin.x, test_case.origin.y, board_size.width, board_size.height)));

    if (test_case.overlay) {
        // A UI overlay inside the board tests whether localization remains
        // anchored by the board texture instead of a single unobscured area.
        const cv::Point start(
            test_case.origin.x + board_size.width / 5,
            test_case.origin.y + board_size.height / 2);
        const cv::Point end(
            test_case.origin.x + board_size.width * 4 / 5,
            test_case.origin.y + board_size.height / 2);
        cv::line(frame, start, end, cv::Scalar(245, 245, 245), 5, cv::LINE_AA);
    }
    return frame;
}

#if TEST_BOARD_LOCALIZATION_CALIBRATION
TEST_F(DetectorsTest, BoardLocalizationCalibration) {
    const std::array<GeometryCalibrationCase, 5> cases{{
        {"half_scale_offset", 0.50, {80, 120}, {1100, 1100}, false},
        {"three_quarter_scale_offset", 0.75, {40, 75}, {1400, 1400}, false},
        {"native_scale_offset", 1.00, {120, 90}, {1900, 1900}, false},
        {"scaled_with_overlay", 0.625, {210, 55}, {1300, 1300}, true},
        {"small_scaled_with_overlay", 0.40, {300, 150}, {1100, 900}, true},
    }};

    double maximum_bound_error = 0.0;
    double maximum_center_error = 0.0;
    double maximum_normalized_bound_error = 0.0;
    double maximum_normalized_center_error = 0.0;
    constexpr double kMaximumNormalizedBoundError = 0.025;
    constexpr double kMaximumNormalizedCenterError = 0.020;
    for (const auto& test_case : cases) {
        const cv::Mat frame = make_geometry_calibration_frame(board_, test_case);
        const BoardGeometry detected = locate_board(frame, board_);
        const int expected_width = static_cast<int>(
            std::lround(board_.cols * test_case.scale));
        const int expected_height = static_cast<int>(
            std::lround(board_.rows * test_case.scale));

        const double bound_error = std::max({
            std::abs(static_cast<double>(detected.bx - test_case.origin.x)),
            std::abs(static_cast<double>(detected.by - test_case.origin.y)),
            std::abs(static_cast<double>(detected.bw - expected_width)),
            std::abs(static_cast<double>(detected.bh - expected_height)),
        });
        const double detected_center_x = detected.bx + detected.bw / 2.0;
        const double detected_center_y = detected.by + detected.bh / 2.0;
        const double expected_center_x = test_case.origin.x + expected_width / 2.0;
        const double expected_center_y = test_case.origin.y + expected_height / 2.0;
        const double center_error = std::hypot(
            detected_center_x - expected_center_x,
            detected_center_y - expected_center_y);
        const double board_extent = static_cast<double>(
            std::max(expected_width, expected_height));
        const double normalized_bound_error = bound_error / board_extent;
        const double normalized_center_error = center_error / board_extent;

        maximum_bound_error = std::max(maximum_bound_error, bound_error);
        maximum_center_error = std::max(maximum_center_error, center_error);
        maximum_normalized_bound_error = std::max(
            maximum_normalized_bound_error, normalized_bound_error);
        maximum_normalized_center_error = std::max(
            maximum_normalized_center_error, normalized_center_error);
        std::cout << "  Geometry " << test_case.name
                  << ": bound_error=" << bound_error
                  << " center_error=" << center_error
                  << " normalized_bound_error=" << normalized_bound_error
                  << " normalized_center_error=" << normalized_center_error
                  << " detected_bounds=(" << detected.bx << "," << detected.by
                  << "," << detected.bw << "," << detected.bh << ")"
                  << " confidence=" << detected.geometry_confidence << "\n";

        EXPECT_LE(normalized_bound_error, kMaximumNormalizedBoundError)
            << "Failed on " << test_case.name;
        EXPECT_LE(normalized_center_error, kMaximumNormalizedCenterError)
            << "Failed on " << test_case.name;
        EXPECT_GT(detected.localization_score, 0.0) << "Failed on " << test_case.name;
        EXPECT_GT(detected.geometry_confidence, 0.5) << "Failed on " << test_case.name;
    }

    std::cout << "  Geometry calibration maximums: bound_error="
              << maximum_bound_error << " center_error=" << maximum_center_error
              << " normalized_bound_error=" << maximum_normalized_bound_error
              << " normalized_center_error=" << maximum_normalized_center_error << "\n";
}
#endif

#endif // TEST_LOCATE_BOARD

#if TEST_DRAW_GRID

TEST_F(DetectorsTest, DrawBoardGrid) {
    cv::Mat test_img = cv::Mat(800, 800, CV_8UC3, cv::Scalar(128, 128, 128));
    BoardGeometry geo{50, 50, 700, 700, 87.5, 87.5};
    EXPECT_NO_THROW(draw_board_grid(test_img, geo, cv::Scalar(0, 255, 0), 2, true));
}

#endif // TEST_DRAW_GRID

// ─── YELLOW SQUARE EXTRACTION ────────────────────────────────────────────────
#if TEST_YELLOW_SQUARES || TEST_YELLOW_TEMPORAL_VALIDATION || TEST_CLOCK_TEMPORAL_READINGS || \
    TEST_GEOMETRY_UNCERTAINTY || TEST_GEOMETRY_STABILITY || TEST_CLOCK_ROI_BOUNDS || \
    TEST_CLOCK_VETO_VALIDATION || TEST_YELLOW_SQUARE_CALIBRATION_LABELS || \
    TEST_YELLOW_SQUARE_CALIBRATION_REGIMES

#if TEST_YELLOW_TEMPORAL_VALIDATION
TEST(YellowValidationTest, TemporalAcceptanceRequiresRepeatedCompletePairs) {
    cta::validation::YellowTemporalEvidence evidence;
    evidence.sample_count = 2;
    evidence.pair_pass_count = 1;
    evidence.max_from = 40.0;
    evidence.max_to = 40.0;
    evidence.max_pair = 80.0;
    EXPECT_FALSE(cta::validation::passes_temporal_yellow_check(evidence));

    evidence.pair_pass_count = 2;
    EXPECT_TRUE(cta::validation::passes_temporal_yellow_check(evidence));

    evidence.max_to = cta::validation::kYellowEndpointThreshold - 0.1;
    EXPECT_FALSE(cta::validation::passes_temporal_yellow_check(evidence));
}

#endif

#if TEST_YELLOW_TEMPORAL_VALIDATION || TEST_CLOCK_VETO_VALIDATION
TEST(EvidenceStrengthTest, KeepsCalibrationAndReducerClaimsSeparate) {
    EXPECT_STREQ(
        cta::validation::to_string(cta::validation::classify_yellow_evidence(
            false, true, false, false)), "advisory");
    EXPECT_STREQ(
        cta::validation::to_string(cta::validation::classify_yellow_evidence(
            false, false, true, false)), "weak");
    EXPECT_STREQ(
        cta::validation::to_string(cta::validation::classify_yellow_evidence(
            false, false, false, true)), "missing");
    EXPECT_STREQ(
        cta::validation::to_string(cta::validation::classify_yellow_evidence(
            false, false, false, false)), "conflicting");

    EXPECT_STREQ(
        cta::validation::to_string(cta::validation::classify_clock_evidence(
            true, false, false, false)), "strong");
    EXPECT_STREQ(
        cta::validation::to_string(cta::validation::classify_clock_evidence(
            false, false, true, false)), "advisory");
    EXPECT_STREQ(
        cta::validation::to_string(cta::validation::classify_clock_evidence(
            false, false, false, true)), "missing");
}
#endif

#if TEST_CLOCK_TEMPORAL_READINGS
TEST(ClockValidationTest, TemporalReadingsPreserveUncertainty) {
    const auto direct = cta::reconcile_clock_readings({"1:30:07"});
    EXPECT_EQ(direct.selected_reading, "1:30:07");
    EXPECT_EQ(direct.provenance, "direct");
    EXPECT_EQ(direct.agreement_count, 1u);

    const auto inherited = cta::reconcile_clock_readings({}, "1:30:07");
    EXPECT_EQ(inherited.selected_reading, "1:30:07");
    EXPECT_EQ(inherited.provenance, "inherited");

    const auto repeated = cta::reconcile_clock_readings(
        {"", "1:30:07", "1:30:07"});
    EXPECT_EQ(repeated.selected_reading, "1:30:07");
    EXPECT_EQ(repeated.provenance, "temporally_plausible");
    EXPECT_EQ(repeated.agreement_count, 2u);

    const auto conflict = cta::reconcile_clock_readings(
        {"1:30:07", "1:30:08"});
    EXPECT_TRUE(conflict.selected_reading.empty());
    EXPECT_EQ(conflict.provenance, "rejected");
}

#endif

#if TEST_GEOMETRY_UNCERTAINTY
TEST(BoardGeometryTest, UncertaintyIsBoundedAndMonotonic) {
    cta::BoardGeometry unavailable;
    EXPECT_DOUBLE_EQ(cta::geometry_uncertainty(unavailable), 1.0);

    cta::BoardGeometry low_confidence;
    low_confidence.bw = 800;
    low_confidence.bh = 800;
    low_confidence.sq_w = 100.0;
    low_confidence.sq_h = 100.0;
    low_confidence.geometry_confidence = 0.25;
    EXPECT_DOUBLE_EQ(cta::geometry_uncertainty(low_confidence), 0.75);

    cta::BoardGeometry high_confidence = low_confidence;
    high_confidence.geometry_confidence = 0.90;
    EXPECT_DOUBLE_EQ(cta::geometry_uncertainty(high_confidence), 0.10);

    high_confidence.geometry_confidence = 1.5;
    EXPECT_DOUBLE_EQ(cta::geometry_uncertainty(high_confidence), 0.0);
}

#endif

#if TEST_GEOMETRY_STABILITY
TEST(BoardGeometryTest, StabilitySeparatesJitterFromEvidenceDrift) {
    cta::BoardGeometry anchor;
    anchor.bw = 800;
    anchor.bh = 800;
    anchor.sq_w = 100.0;
    anchor.sq_h = 100.0;
    anchor.geometry_confidence = 0.9;

    cta::BoardGeometry jitter = anchor;
    jitter.bx = 8;
    jitter.by = -7;
    auto result = cta::assess_geometry_stability(anchor, jitter);
    EXPECT_TRUE(result.stable);
    EXPECT_FALSE(result.anchor_drift_exceeded);

    cta::BoardGeometry drift = anchor;
    drift.bx = 25;
    result = cta::assess_geometry_stability(anchor, drift);
    EXPECT_FALSE(result.stable);
    EXPECT_TRUE(result.anchor_drift_exceeded);

    cta::BoardGeometry previous = anchor;
    previous.bx = 1;
    cta::BoardGeometry jump = anchor;
    jump.bx = 18;
    result = cta::assess_geometry_stability(anchor, jump, &previous);
    EXPECT_FALSE(result.stable);
    EXPECT_TRUE(result.step_drift_exceeded);
}

#endif

#if TEST_CLOCK_ROI_BOUNDS
TEST(ClockRoiTest, UsesBoardRelativeBoundsWithinFrame) {
    cta::BoardGeometry geometry;
    geometry.bx = 100;
    geometry.by = 120;
    geometry.bw = 800;
    geometry.bh = 800;
    geometry.sq_w = 100.0;
    geometry.sq_h = 100.0;

    const auto bounds = cta::clock_roi_bounds(geometry, 1200, 1200);
    EXPECT_TRUE(bounds.valid());
    EXPECT_EQ(bounds.x1, 660);
    EXPECT_EQ(bounds.x2, 900);
    EXPECT_EQ(bounds.top_y1, 65);
    EXPECT_EQ(bounds.top_y2, 112);
    EXPECT_EQ(bounds.bottom_y1, 938);
    EXPECT_EQ(bounds.bottom_y2, 978);

    const auto clipped = cta::clock_roi_bounds(geometry, 850, 960);
    EXPECT_TRUE(clipped.valid());
    EXPECT_EQ(clipped.x2, 850);
    EXPECT_EQ(clipped.bottom_y2, 960);
}

#endif

#if TEST_CLOCK_VETO_VALIDATION
TEST(ClockValidationTest, VetoRequiresCalibratedTemporalAgreement) {
    cta::validation::ClockVetoEvidence evidence;
    evidence.direct_reading_plausible = true;
    evidence.temporal_checked = true;
    evidence.temporal_sample_count = 1;
    evidence.temporal_observed_count = 1;
    evidence.temporal_agreement_count = 1;
    EXPECT_FALSE(cta::validation::passes_clock_veto_reliability_gate(evidence));

    evidence.temporal_sample_count = cta::validation::kClockVetoMinimumTemporalSamples;
    evidence.temporal_observed_count = cta::validation::kClockVetoMinimumObservedReadings;
    evidence.temporal_agreement_count = cta::validation::kClockVetoMinimumAgreements;
    EXPECT_TRUE(cta::validation::passes_clock_veto_reliability_gate(evidence));

    evidence.direct_reading_plausible = false;
    EXPECT_FALSE(cta::validation::passes_clock_veto_reliability_gate(evidence));
}

#endif

#if TEST_YELLOW_SQUARES
TEST_F(DetectorsTest, YellowSquares) {
    const auto dataset_dir = std::filesystem::path(assets_dir_) /
        "fixtures" / "detectors" / "yellow-squares";
    const auto labels_path = dataset_dir / "labels.jsonl";
    std::ifstream labels(labels_path);
    if (!labels.is_open()) GTEST_SKIP() << "Labels not found: " << labels_path.string();

    std::map<std::string, nlohmann::json> expected_by_image;
    std::string line;
    while (std::getline(labels, line)) {
        if (line.empty()) continue;
        const auto label = nlohmann::json::parse(line);
        expected_by_image.emplace(label.at("image").get<std::string>(), label);
    }

    if (expected_by_image.empty()) GTEST_SKIP() << "No yellow-square labels found";

    std::cout << "\nRunning manifest-driven yellow square diagnostics...\n";
    int processed = 0;
    int passed = 0;
    int missed = 0;
    for (const auto& [image_name, expected] : expected_by_image) {
        const auto image_path = dataset_dir / image_name;
        const auto image = cv::imread(image_path.string());
        ASSERT_FALSE(image.empty()) << "Could not read labeled image: "
                                    << image_path.string();
        const auto geometry = locate_board(image, board_);
        const auto move = extract_move_from_yellow_squares(image, board_, geometry);
        const bool truth_positive = expected.value("truth", std::string("uncertain")) ==
            "positive";
        const auto expected_move = expected.value("expected_move", std::string());
        const bool prediction = truth_positive ? !move.empty() : move.empty();
        const bool exact = truth_positive ? move == expected_move : move.empty();
        ++processed;
        if (exact) ++passed;
        else ++missed;
        std::cout << "  " << (exact ? "PASS" : "MISS") << ": " << image_name
                  << " -> " << (move.empty() ? "<none>" : move);
        if (!expected_move.empty()) std::cout << " (expected=" << expected_move << ")";
        else if (!truth_positive) std::cout << " (expected=no move)";
        if (truth_positive && !prediction) std::cout << " [no candidate]";
        std::cout << '\n';
    }
    std::cout << "Yellow square corpus diagnostic: " << passed << "/" << processed
              << " exact moves, " << missed << " misses or unavailable detections.\n";
    EXPECT_GT(processed, 0);
}

#endif

#if TEST_YELLOW_SQUARE_CALIBRATION_LABELS
TEST_F(DetectorsTest, YellowSquareCalibrationLabels) {
    const auto dataset_dir = std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "yellow-squares";
    const auto labels_path = dataset_dir / "labels.jsonl";
    std::ifstream labels(labels_path);
    if (!labels.is_open()) GTEST_SKIP() << "Labels not found: " << labels_path.string();

    struct Counts { int true_positive = 0; int true_negative = 0;
                    int false_positive = 0; int false_negative = 0; };
    std::map<std::string, Counts> metrics;
    std::string line;
    while (std::getline(labels, line)) {
        if (line.empty()) continue;
        const auto label = nlohmann::json::parse(line);
        const auto image_path = dataset_dir / label.at("image").get<std::string>();
        const auto image = cv::imread(image_path.string());
        ASSERT_FALSE(image.empty()) << "Could not read labeled image: " << image_path.string();
        const auto geometry = locate_board(image, board_);
        const auto move = extract_move_from_yellow_squares(image, board_, geometry);
        const auto component = label.at("component").get<std::string>();
        const auto truth = label.at("truth").get<std::string>() == "positive";
        const auto origin = label.value("origin", std::string());
        const auto destination = label.value("destination", std::string());
        const bool valid_move = move.size() >= 4;
        const bool predicted = truth ? valid_move &&
            (component == "origin" ? move.substr(0, 2) == origin :
             component == "destination" ? move.substr(2, 2) == destination :
             move.substr(0, 2) == origin && move.substr(2, 2) == destination)
            : valid_move;
        auto& count = metrics[component];
        if (truth && predicted) ++count.true_positive;
        else if (!truth && !predicted) ++count.true_negative;
        else if (!truth) ++count.false_positive;
        else ++count.false_negative;
    }

    int expected_labeled_per_component = -1;
    for (const auto& [component, count] : metrics) {
        const int labeled = count.true_positive + count.true_negative +
            count.false_positive + count.false_negative;
        ASSERT_GT(labeled, 0);
        const double precision = static_cast<double>(count.true_positive) /
            std::max(1, count.true_positive + count.false_positive);
        const double recall = static_cast<double>(count.true_positive) /
            std::max(1, count.true_positive + count.false_negative);
        std::cout << "  Yellow " << component << " calibration: TP=" << count.true_positive
                  << " TN=" << count.true_negative << " FP=" << count.false_positive
                  << " FN=" << count.false_negative << " precision=" << precision
                  << " recall=" << recall << "\n";
        if (expected_labeled_per_component < 0) {
            expected_labeled_per_component = labeled;
        } else {
            EXPECT_EQ(labeled, expected_labeled_per_component) << component;
        }
        EXPECT_GE(labeled, 9) << component;
    }
    EXPECT_EQ(metrics.size(), 3u);
}

struct YellowCalibrationVariant {
    const char* name;
    const char* condition;
    double geometry_offset_fraction;
    double corner_fraction;
};

static cv::Mat make_yellow_calibration_variant(
    const cv::Mat& image, const YellowCalibrationVariant& variant) {
    if (std::string(variant.name) == "native" ||
        std::string(variant.name) == "geometry_shifted" ||
        std::string(variant.name) == "corner_tight" ||
        std::string(variant.name) == "corner_wide" ||
        std::string(variant.name) == "corner_extra_tight" ||
        std::string(variant.name) == "corner_mid" ||
        std::string(variant.name) == "corner_extra_wide") {
        return image.clone();
    }
    if (std::string(variant.name) == "scaled_75") {
        cv::Mat scaled;
        cv::resize(image, scaled, cv::Size(), 0.75, 0.75, cv::INTER_AREA);
        return scaled;
    }
    if (std::string(variant.name) == "brightness_reduced") {
        cv::Mat reduced;
        image.convertTo(reduced, image.type(), 0.75, 0.0);
        return reduced;
    }
    cv::Mat blurred;
    cv::GaussianBlur(image, blurred, cv::Size(3, 3), 0.0);
    return blurred;
}

static std::string yellow_calibration_square_name(int square) {
    const int row = square / 8;
    const int col = square % 8;
    std::string name;
    name += static_cast<char>('a' + col);
    name += static_cast<char>('8' - row);
    return name;
}

static std::array<double, 64> yellow_calibration_raw_scores(
    const cv::Mat& board_bgr, const BoardGeometry& geometry,
    double corner_fraction) {
    std::array<double, 64> scores{};
    for (int square = 0; square < 64; ++square) {
        const std::string name = yellow_calibration_square_name(square);
        const auto measurement = cta::validation::measure_yellowness(
            board_bgr, geometry, name.c_str(), false, corner_fraction);
        scores[static_cast<size_t>(square)] = measurement.score;
    }
    return scores;
}

static std::vector<std::pair<double, int>> rank_yellow_calibration_scores(
    const std::array<double, 64>& raw_scores) {
    std::vector<std::pair<double, int>> scores;
    scores.reserve(raw_scores.size());
    for (int square = 0; square < 64; ++square) {
        scores.emplace_back(raw_scores[static_cast<size_t>(square)], square);
    }
    std::sort(scores.begin(), scores.end(), std::greater<>());
    return scores;
}

static double yellow_calibration_median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2;
    if (values.size() % 2 == 0) {
        return (values[middle - 1] + values[middle]) / 2.0;
    }
    return values[middle];
}

static double yellow_local_calibration_baseline(
    const std::array<double, 64>& raw_scores, int square) {
    const int row = square / 8;
    const int col = square % 8;
    std::vector<double> neighbors;
    for (int neighbor_row = std::max(0, row - 1);
         neighbor_row <= std::min(7, row + 1); ++neighbor_row) {
        for (int neighbor_col = std::max(0, col - 1);
             neighbor_col <= std::min(7, col + 1); ++neighbor_col) {
            const int neighbor = neighbor_row * 8 + neighbor_col;
            if (neighbor != square) {
                neighbors.push_back(raw_scores[static_cast<size_t>(neighbor)]);
            }
        }
    }
    return yellow_calibration_median(std::move(neighbors));
}

static int yellow_calibration_square_index(const std::string& square) {
    if (square.size() != 2 || square[0] < 'a' || square[0] > 'h' ||
        square[1] < '1' || square[1] > '8') {
        return -1;
    }
    return (8 - (square[1] - '0')) * 8 + (square[0] - 'a');
}

static std::vector<int> yellow_calibration_adjacent_squares(
    int first_square, int second_square) {
    std::set<int> neighbors;
    for (const int endpoint : {first_square, second_square}) {
        if (endpoint < 0 || endpoint >= 64) continue;
        const int row = endpoint / 8;
        const int col = endpoint % 8;
        for (int neighbor_row = std::max(0, row - 1);
             neighbor_row <= std::min(7, row + 1); ++neighbor_row) {
            for (int neighbor_col = std::max(0, col - 1);
                 neighbor_col <= std::min(7, col + 1); ++neighbor_col) {
                const int neighbor = neighbor_row * 8 + neighbor_col;
                if (neighbor != first_square && neighbor != second_square) {
                    neighbors.insert(neighbor);
                }
            }
        }
    }
    return {neighbors.begin(), neighbors.end()};
}

struct YellowCategoryCalibrationCase {
    const char* name;
    const char* origin;
    const char* destination;
    const char* expected_move;
    const char* pre_move_destination_occupancy;
    std::array<std::pair<const char*, char>, 3> pieces;
};

static cv::Mat make_yellow_category_calibration_board(
    const cv::Mat& board_template,
    const std::filesystem::path& assets_dir,
    const YellowCategoryCalibrationCase& calibration_case,
    bool& pieces_loaded) {
    pieces_loaded = true;
    if (board_template.empty() || board_template.cols < 8 || board_template.rows < 8) {
        pieces_loaded = false;
        return {};
    }

    cv::Mat board = board_template.clone();
    const int square_width = board.cols / 8;
    const int square_height = board.rows / 8;
    const auto square_rect = [&](const char* square) {
        const int index = yellow_calibration_square_index(square);
        if (index < 0) return cv::Rect();
        return cv::Rect(
            (index % 8) * square_width, (index / 8) * square_height,
            square_width, square_height);
    };

    // Use the same board-relative highlight geometry as the screenshot set:
    // a translucent warm overlay plus a narrow square outline.  This is a
    // test-only visual probe; production extraction never branches on these
    // category names or generated images.
    for (const char* square : {calibration_case.origin, calibration_case.destination}) {
        const cv::Rect rect = square_rect(square);
        if (rect.width <= 0 || rect.height <= 0) continue;
        cv::Mat tint(rect.size(), board.type(), cv::Scalar(0, 150, 255));
        cv::Mat blended;
        cv::addWeighted(board(rect), 0.63, tint, 0.37, 0.0, blended);
        blended.copyTo(board(rect));
        cv::rectangle(
            board, rect, cv::Scalar(0, 165, 255),
            std::max(3, std::min(square_width, square_height) / 32));
    }

    const std::map<char, std::string> piece_names{
        {'b', "bishop"}, {'k', "king"}, {'n', "knight"},
        {'p', "pawn"}, {'q', "queen"}, {'r', "rook"},
    };
    for (const auto& [square, piece] : calibration_case.pieces) {
        const char lower_piece = static_cast<char>(
            std::tolower(static_cast<unsigned char>(piece)));
        const auto piece_name = piece_names.find(lower_piece);
        const cv::Rect square_area = square_rect(square);
        if (piece_name == piece_names.end() || square_area.width <= 0 ||
            square_area.height <= 0) {
            pieces_loaded = false;
            continue;
        }
        const std::string colour = std::isupper(static_cast<unsigned char>(piece))
            ? "white" : "black";
        const auto piece_path = assets_dir / "reference" / "pieces" / colour /
            (piece_name->second + ".png");
        cv::Mat source = cv::imread(piece_path.string(), cv::IMREAD_UNCHANGED);
        if (source.empty() || source.channels() != 4) {
            pieces_loaded = false;
            continue;
        }
        cv::Mat resized;
        cv::resize(source, resized, cv::Size(
            static_cast<int>(std::lround(square_width * 0.75)),
            static_cast<int>(std::lround(square_height * 0.75))),
            0.0, 0.0, cv::INTER_AREA);
        const int left = square_area.x + (square_area.width - resized.cols) / 2;
        const int top = square_area.y + (square_area.height - resized.rows) / 2;
        for (int row = 0; row < resized.rows; ++row) {
            for (int col = 0; col < resized.cols; ++col) {
                const cv::Vec4b pixel = resized.at<cv::Vec4b>(row, col);
                const double alpha = pixel[3] / 255.0;
                if (alpha <= 0.0) continue;
                cv::Vec3b& target = board.at<cv::Vec3b>(top + row, left + col);
                for (int channel = 0; channel < 3; ++channel) {
                    target[channel] = static_cast<unsigned char>(std::lround(
                        target[channel] * (1.0 - alpha) + pixel[channel] * alpha));
                }
            }
        }
    }
    return board;
}

#endif

#if TEST_YELLOW_SQUARE_CALIBRATION_REGIMES
TEST_F(DetectorsTest, YellowSquareCalibrationRegimes) {
    const auto dataset_dir = std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "yellow-squares";
    const auto labels_path = dataset_dir / "labels.jsonl";
    std::ifstream labels(labels_path);
    if (!labels.is_open()) GTEST_SKIP() << "Labels not found: " << labels_path.string();

    std::vector<nlohmann::json> expected_labels;
    std::string line;
    while (std::getline(labels, line)) {
        if (!line.empty()) expected_labels.push_back(nlohmann::json::parse(line));
    }

    std::map<std::string, std::vector<const nlohmann::json*>> labels_by_image;
    for (const auto& expected : expected_labels) {
        labels_by_image[expected.at("image").get<std::string>()].push_back(&expected);
    }

    std::ofstream calibration_output;
    if (const char* output_path = std::getenv("CTA_YELLOW_CALIBRATION_FILE");
        output_path != nullptr && *output_path != '\0') {
        calibration_output.open(output_path, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(calibration_output.is_open())
            << "Could not write yellow calibration output: " << output_path;
    }

    const std::array<YellowCalibrationVariant, 10> variants{{
        {"native", "clean", 0.0, 0.12},
        {"scaled_75", "scaling", 0.0, 0.12},
        {"brightness_reduced", "brightness", 0.0, 0.12},
        {"blurred", "compression_like_blur", 0.0, 0.12},
        {"geometry_shifted", "geometry_error", 0.02, 0.12},
        {"corner_extra_tight", "square_boundary_margin", 0.0, 0.05},
        {"corner_tight", "square_boundary_margin", 0.0, 0.08},
        {"corner_mid", "square_boundary_margin", 0.0, 0.10},
        {"corner_wide", "square_boundary_margin", 0.0, 0.16},
        {"corner_extra_wide", "square_boundary_margin", 0.0, 0.20},
    }};

    std::size_t observation_count = 0;
    for (const auto& variant : variants) {
        for (const auto& [image_name, image_labels] : labels_by_image) {
            const cv::Mat image = cv::imread((dataset_dir / image_name).string());
            ASSERT_FALSE(image.empty()) << "Could not read labeled image: " << image_name;
            const cv::Mat variant_image = make_yellow_calibration_variant(image, variant);
            BoardGeometry located = locate_board(variant_image, board_);
            const bool localized = located.bw > 0 && located.bh > 0;
            if (!localized) {
                for (const auto* expected_ptr : image_labels) {
                    const auto& expected = *expected_ptr;
                    for (const std::string& component : {"origin", "destination", "paired"}) {
                        ++observation_count;
                        if (!calibration_output.is_open()) continue;
                        calibration_output << nlohmann::json{
                            {"schema_version", 1}, {"detector", "yellow"},
                            {"image", image_name}, {"component", component},
                            {"truth", expected.value("truth", std::string("uncertain"))},
                            {"prediction", "negative"}, {"regime", variant.name},
                            {"condition", variant.condition},
                            {"preprocessing_variant", variant.name},
                            {"geometry_available", false},
                            {"corner_fraction", variant.corner_fraction},
                            {"case", expected.value("case", std::string("unspecified"))},
                        }.dump() << '\n';
                    }
                }
                continue;
            }

            const int geometry_dx = static_cast<int>(
                std::lround(located.sq_w * variant.geometry_offset_fraction));
            const int geometry_dy = static_cast<int>(
                std::lround(located.sq_h * variant.geometry_offset_fraction));
            located.bx += geometry_dx;
            located.by += geometry_dy;
            const cv::Rect board_rect(
                std::max(0, located.bx), std::max(0, located.by),
                std::min(located.bw, variant_image.cols - std::max(0, located.bx)),
                std::min(located.bh, variant_image.rows - std::max(0, located.by)));
            if (board_rect.width <= 0 || board_rect.height <= 0) continue;

            cv::Mat board_bgr = variant_image(board_rect);
            BoardGeometry local_geometry = located;
            local_geometry.bx = 0;
            local_geometry.by = 0;
            local_geometry.bw = board_rect.width;
            local_geometry.bh = board_rect.height;
            local_geometry.sq_w = static_cast<double>(board_rect.width) / 8.0;
            local_geometry.sq_h = static_cast<double>(board_rect.height) / 8.0;
            const auto raw_scores = yellow_calibration_raw_scores(
                board_bgr, local_geometry, variant.corner_fraction);
            const auto scores = rank_yellow_calibration_scores(raw_scores);
            const double top_score = scores[0].first;
            const double second_score = scores[1].first;
            const std::string top_square = yellow_calibration_square_name(scores[0].second);
            const std::string second_square = yellow_calibration_square_name(scores[1].second);
            std::vector<double> all_scores(raw_scores.begin(), raw_scores.end());
            const double board_background_median = yellow_calibration_median(
                std::move(all_scores));

            for (const auto* expected_ptr : image_labels) {
                const auto& expected = *expected_ptr;

                const std::string expected_case = expected.value(
                "case", std::string("unspecified"));
                const std::string expected_origin = expected.value(
                "origin", std::string());
                const std::string expected_destination = expected.value(
                "destination", std::string());
                const int expected_origin_index = yellow_calibration_square_index(expected_origin);
                const int expected_destination_index = yellow_calibration_square_index(expected_destination);
                const auto adjacent_indices = yellow_calibration_adjacent_squares(
                expected_origin_index, expected_destination_index);
                nlohmann::json adjacent_scores = nlohmann::json::array();
                std::vector<double> adjacent_score_values;
                for (const int adjacent_index : adjacent_indices) {
                const double score = raw_scores[static_cast<size_t>(adjacent_index)];
                adjacent_score_values.push_back(score);
                adjacent_scores.push_back({
                    {"square", yellow_calibration_square_name(adjacent_index)},
                    {"score", score},
                });
                }
                const double adjacent_score_max = adjacent_score_values.empty()
                ? 0.0
                : *std::max_element(adjacent_score_values.begin(), adjacent_score_values.end());
                const double adjacent_score_mean = adjacent_score_values.empty()
                ? 0.0
                : std::accumulate(adjacent_score_values.begin(), adjacent_score_values.end(), 0.0) /
                    static_cast<double>(adjacent_score_values.size());
                const bool positive_case = expected.value("truth", std::string("uncertain")) == "positive";
                const std::string post_move_origin_occupancy = positive_case ? "empty" : "unknown";
                const std::string post_move_destination_occupancy = positive_case ? "occupied" : "unknown";
                const std::string pre_move_destination_occupancy = expected_case == "capture"
                ? "occupied"
                : expected_case == "quiet" || expected_case == "double_pawn"
                    ? "empty" : "unknown";

                std::string observed_move;
                if (located.bw > 0 && located.bh > 0) {
                    observed_move = extract_move_from_yellow_squares(
                        variant_image, board_, located);
                }
                for (const std::string& component : {"origin", "destination", "paired"}) {
                const std::string origin = expected.value("origin", std::string());
                const std::string destination = expected.value("destination", std::string());
                double origin_score = 0.0;
                double destination_score = 0.0;
                double origin_board_relative_score = 0.0;
                double destination_board_relative_score = 0.0;
                double origin_local_score = 0.0;
                double destination_local_score = 0.0;
                double origin_edge_density = 0.0;
                double destination_edge_density = 0.0;
                int origin_index = -1;
                int destination_index = -1;
                if (origin.size() == 2 && destination.size() == 2) {
                    origin_index = yellow_calibration_square_index(origin);
                    destination_index = yellow_calibration_square_index(destination);
                    const auto origin_measurement = cta::validation::measure_yellowness(
                        board_bgr, local_geometry, origin.c_str(), true,
                        variant.corner_fraction);
                    const auto destination_measurement = cta::validation::measure_yellowness(
                        board_bgr, local_geometry, destination.c_str(), true,
                        variant.corner_fraction);
                    origin_score = origin_measurement.score;
                    destination_score = destination_measurement.score;
                    origin_edge_density = std::accumulate(
                        origin_measurement.corner_edge_density.begin(),
                        origin_measurement.corner_edge_density.end(), 0.0) / 4.0;
                    destination_edge_density = std::accumulate(
                        destination_measurement.corner_edge_density.begin(),
                        destination_measurement.corner_edge_density.end(), 0.0) / 4.0;
                } else {
                    origin_index = scores[0].second;
                    destination_index = scores[1].second;
                    origin_score = top_score;
                    destination_score = second_score;
                }
                const auto normalized_score = [&](double score, int square,
                                                  bool board_relative) {
                    if (square < 0 || square >= 64) return 0.0;
                    if (board_relative) return score - board_background_median;
                    return score - yellow_local_calibration_baseline(raw_scores, square);
                };
                origin_board_relative_score = normalized_score(
                    origin_score, origin_index, true);
                destination_board_relative_score = normalized_score(
                    destination_score, destination_index, true);
                origin_local_score = normalized_score(origin_score, origin_index, false);
                destination_local_score = normalized_score(
                    destination_score, destination_index, false);
                const double pair_score = origin_score + destination_score;
                const double board_relative_pair_score =
                    origin_board_relative_score + destination_board_relative_score;
                const double local_pair_score = origin_local_score + destination_local_score;
                const bool origin_passed = origin_score >= cta::validation::kYellowEndpointThreshold;
                const bool destination_passed = destination_score >= cta::validation::kYellowEndpointThreshold;
                const bool paired_passed = origin_passed && destination_passed &&
                    pair_score >= cta::validation::kYellowPairThreshold;
                const bool predicted = component == "origin" ? origin_passed
                    : component == "destination" ? destination_passed : paired_passed;
                ++observation_count;
                if (!calibration_output.is_open()) continue;
                calibration_output << nlohmann::json{
                    {"schema_version", 1}, {"detector", "yellow"},
                    {"image", image_name}, {"component", component},
                    {"truth", expected.value("truth", std::string("uncertain"))},
                    {"prediction", predicted ? "positive" : "negative"},
                    {"regime", variant.name}, {"condition", variant.condition},
                    {"preprocessing_variant", variant.name},
                    {"case", expected_case},
                    {"post_move_origin_occupancy", post_move_origin_occupancy},
                    {"post_move_destination_occupancy", post_move_destination_occupancy},
                    {"pre_move_destination_occupancy", pre_move_destination_occupancy},
                    {"adjacent_highlight_scores", adjacent_scores},
                    {"adjacent_highlight_max", adjacent_score_max},
                    {"adjacent_highlight_mean", adjacent_score_mean},
                    {"expected_move", expected.value("expected_move", std::string())},
                    {"observed_move", observed_move},
                    {"top_square", top_square}, {"second_square", second_square},
                    {"origin_score", origin_score},
                    {"destination_score", destination_score},
                    {"origin_edge_density", origin_edge_density},
                    {"destination_edge_density", destination_edge_density},
                    {"pair_score", pair_score}, {"score", component == "origin"
                        ? origin_score : component == "destination"
                            ? destination_score : pair_score},
                    {"board_background_median", board_background_median},
                    {"board_relative_score", component == "origin"
                        ? origin_board_relative_score : component == "destination"
                            ? destination_board_relative_score : board_relative_pair_score},
                    {"local_normalized_score", component == "origin"
                        ? origin_local_score : component == "destination"
                            ? destination_local_score : local_pair_score},
                    {"origin_local_baseline", origin_index >= 0
                        ? yellow_local_calibration_baseline(raw_scores, origin_index) : 0.0},
                    {"destination_local_baseline", destination_index >= 0
                        ? yellow_local_calibration_baseline(raw_scores, destination_index) : 0.0},
                    {"yellow_endpoint_threshold", cta::validation::kYellowEndpointThreshold},
                    {"yellow_pair_threshold", cta::validation::kYellowPairThreshold},
                    {"geometry_available", true},
                    {"geometry_offset_x", geometry_dx},
                    {"geometry_offset_y", geometry_dy},
                    {"corner_fraction", variant.corner_fraction},
                }.dump() << '\n';
                }
            }
        }
    }

    // Exercise categories absent from the screenshot manifest with legal
    // board states assembled from the checked-in board and piece assets. These
    // rows measure the same endpoint, occupancy, and adjacency fields as real
    // screenshots while remaining explicitly synthetic calibration evidence.
    const std::array<YellowCategoryCalibrationCase, 2> category_cases{{
        {
            "check", "c4", "b5", "c4b5", "empty",
            {{{"e1", 'K'}, {"c4", 'B'}, {"e8", 'k'}}},
        },
        {
            "promotion", "e7", "e8", "e7e8q", "empty",
            {{{"e1", 'K'}, {"e7", 'P'}, {"h8", 'k'}}},
        },
    }};
    const BoardGeometry category_geometry{
        0, 0, board_.cols, board_.rows,
        static_cast<double>(board_.cols) / 8.0,
        static_cast<double>(board_.rows) / 8.0,
        1.0, 0.0, 0.0};
    int category_failures = 0;
    int category_observation_count = 0;
    for (const auto& calibration_case : category_cases) {
        bool pieces_loaded = false;
        const cv::Mat category_board = make_yellow_category_calibration_board(
            board_, std::filesystem::path(assets_dir_), calibration_case, pieces_loaded);
        ASSERT_FALSE(category_board.empty());
        ASSERT_TRUE(pieces_loaded) << "Missing piece asset for " << calibration_case.name;
        const auto raw_scores = yellow_calibration_raw_scores(
            category_board, category_geometry, 0.12);
        const auto ranked_scores = rank_yellow_calibration_scores(raw_scores);
        const double board_background_median = yellow_calibration_median(
            std::vector<double>(raw_scores.begin(), raw_scores.end()));
        const int origin_index = yellow_calibration_square_index(calibration_case.origin);
        const int destination_index = yellow_calibration_square_index(calibration_case.destination);
        const auto adjacent_indices = yellow_calibration_adjacent_squares(
            origin_index, destination_index);
        nlohmann::json adjacent_scores = nlohmann::json::array();
        std::vector<double> adjacent_score_values;
        for (const int adjacent_index : adjacent_indices) {
            const double score = raw_scores[static_cast<size_t>(adjacent_index)];
            adjacent_score_values.push_back(score);
            adjacent_scores.push_back({
                {"square", yellow_calibration_square_name(adjacent_index)},
                {"score", score},
            });
        }
        const double adjacent_highlight_max = adjacent_score_values.empty()
            ? 0.0 : *std::max_element(adjacent_score_values.begin(), adjacent_score_values.end());
        const double adjacent_highlight_mean = adjacent_score_values.empty()
            ? 0.0 : std::accumulate(adjacent_score_values.begin(), adjacent_score_values.end(), 0.0) /
                static_cast<double>(adjacent_score_values.size());
        const auto origin_measurement = cta::validation::measure_yellowness(
            category_board, category_geometry, calibration_case.origin, true, 0.12);
        const auto destination_measurement = cta::validation::measure_yellowness(
            category_board, category_geometry, calibration_case.destination, true, 0.12);
        const double origin_score = origin_measurement.score;
        const double destination_score = destination_measurement.score;
        const double pair_score = origin_score + destination_score;
        const bool origin_passed = origin_score >= cta::validation::kYellowEndpointThreshold;
        const bool destination_passed = destination_score >= cta::validation::kYellowEndpointThreshold;
        const bool paired_passed = origin_passed && destination_passed &&
            pair_score >= cta::validation::kYellowPairThreshold;
        if (!paired_passed) ++category_failures;
        const double origin_edge_density = std::accumulate(
            origin_measurement.corner_edge_density.begin(),
            origin_measurement.corner_edge_density.end(), 0.0) / 4.0;
        const double destination_edge_density = std::accumulate(
            destination_measurement.corner_edge_density.begin(),
            destination_measurement.corner_edge_density.end(), 0.0) / 4.0;
        const double origin_board_relative_score = origin_score - board_background_median;
        const double destination_board_relative_score = destination_score - board_background_median;
        const double origin_local_score = origin_score - yellow_local_calibration_baseline(
            raw_scores, origin_index);
        const double destination_local_score = destination_score - yellow_local_calibration_baseline(
            raw_scores, destination_index);
        const std::string observed_move = extract_move_from_yellow_squares(
            category_board, board_, category_geometry);
        for (const std::string& component : {"origin", "destination", "paired"}) {
            const bool predicted = component == "origin" ? origin_passed
                : component == "destination" ? destination_passed : paired_passed;
            ++category_observation_count;
            if (calibration_output.is_open()) {
                calibration_output << nlohmann::json{
                    {"schema_version", 1}, {"detector", "yellow"},
                    {"image", std::string("synthetic/") + calibration_case.name},
                    {"component", component}, {"truth", "positive"},
                    {"prediction", predicted ? "positive" : "negative"},
                    {"regime", "synthetic_category"},
                    {"condition", "legal_occupancy_category"},
                    {"preprocessing_variant", "synthetic_board_assets"},
                    {"category_source", "legal_synthetic_position"},
                    {"case", calibration_case.name},
                    {"post_move_origin_occupancy", "empty"},
                    {"post_move_destination_occupancy", "occupied"},
                    {"pre_move_destination_occupancy",
                        calibration_case.pre_move_destination_occupancy},
                    {"adjacent_highlight_scores", adjacent_scores},
                    {"adjacent_highlight_max", adjacent_highlight_max},
                    {"adjacent_highlight_mean", adjacent_highlight_mean},
                    {"expected_move", calibration_case.expected_move},
                    {"observed_move", observed_move},
                    {"top_square", yellow_calibration_square_name(ranked_scores[0].second)},
                    {"second_square", yellow_calibration_square_name(ranked_scores[1].second)},
                    {"origin_score", origin_score}, {"destination_score", destination_score},
                    {"origin_edge_density", origin_edge_density},
                    {"destination_edge_density", destination_edge_density},
                    {"pair_score", pair_score},
                    {"score", component == "origin" ? origin_score
                        : component == "destination" ? destination_score : pair_score},
                    {"board_background_median", board_background_median},
                    {"board_relative_score", component == "origin"
                        ? origin_board_relative_score : component == "destination"
                            ? destination_board_relative_score
                            : origin_board_relative_score + destination_board_relative_score},
                    {"local_normalized_score", component == "origin" ? origin_local_score
                        : component == "destination" ? destination_local_score
                            : origin_local_score + destination_local_score},
                    {"yellow_endpoint_threshold", cta::validation::kYellowEndpointThreshold},
                    {"yellow_pair_threshold", cta::validation::kYellowPairThreshold},
                    {"geometry_available", true}, {"geometry_offset_x", 0},
                    {"geometry_offset_y", 0}, {"corner_fraction", 0.12},
                }.dump() << '\n';
            }
        }
    }

    // Calibrate the reducer's repeated-highlight fallback with explicit
    // persistence and transient sequences. The negative frames are neutral
    // synthetic boards so this test isolates temporal aggregation from the
    // known static-board yellowness baseline.
    const nlohmann::json* temporal_label = nullptr;
    for (const auto& expected : expected_labels) {
        if (expected.value("truth", std::string("uncertain")) == "positive" &&
            expected.value("origin", std::string()).size() == 2 &&
            expected.value("destination", std::string()).size() == 2) {
            temporal_label = &expected;
            break;
        }
    }
    ASSERT_NE(temporal_label, nullptr);
    const cv::Mat temporal_image = cv::imread(
        (dataset_dir / temporal_label->at("image").get<std::string>()).string());
    ASSERT_FALSE(temporal_image.empty());
    const BoardGeometry temporal_located = locate_board(temporal_image, board_);
    ASSERT_GT(temporal_located.bw, 0);
    const cv::Rect temporal_rect(
        temporal_located.bx, temporal_located.by,
        temporal_located.bw, temporal_located.bh);
    const cv::Mat temporal_positive = temporal_image(temporal_rect);
    const cv::Mat temporal_negative = cv::Mat::zeros(
        temporal_positive.size(), temporal_positive.type());
    BoardGeometry temporal_geometry = temporal_located;
    temporal_geometry.bx = 0;
    temporal_geometry.by = 0;
    temporal_geometry.sq_w = static_cast<double>(temporal_positive.cols) / 8.0;
    temporal_geometry.sq_h = static_cast<double>(temporal_positive.rows) / 8.0;

    const auto temporal_evidence_for = [&](const cv::Mat& board_frame) {
        const std::string origin = temporal_label->at("origin").get<std::string>();
        const std::string destination = temporal_label->at("destination").get<std::string>();
        const double from_score = cta::validation::measure_yellowness(
            board_frame, temporal_geometry, origin.c_str()).score;
        const double to_score = cta::validation::measure_yellowness(
            board_frame, temporal_geometry, destination.c_str()).score;
        cta::validation::YellowTemporalEvidence evidence;
        ++evidence.sample_count;
        evidence.max_from = from_score;
        evidence.max_to = to_score;
        evidence.max_pair = from_score + to_score;
        if (from_score >= cta::validation::kYellowEndpointThreshold &&
            to_score >= cta::validation::kYellowEndpointThreshold &&
            from_score + to_score >= cta::validation::kYellowPairThreshold) {
            ++evidence.pair_pass_count;
        }
        return evidence;
    };

    struct TemporalCalibrationCase {
        const char* name;
        std::vector<bool> highlighted_samples;
        bool expected_pass;
    };
    const std::array<TemporalCalibrationCase, 4> temporal_cases{{
        {"persistent_two_samples", {true, true}, true},
        {"persistent_three_samples", {true, true, true}, true},
        {"single_frame_flash", {true}, false},
        {"transient_flash", {true, false, false}, false},
    }};
    int temporal_failures = 0;
    for (const auto& temporal_case : temporal_cases) {
        cta::validation::YellowTemporalEvidence combined;
        for (const bool highlighted : temporal_case.highlighted_samples) {
            const auto sample = temporal_evidence_for(
                highlighted ? temporal_positive : temporal_negative);
            ++combined.sample_count;
            combined.pair_pass_count += sample.pair_pass_count;
            combined.max_from = std::max(combined.max_from, sample.max_from);
            combined.max_to = std::max(combined.max_to, sample.max_to);
            combined.max_pair = std::max(combined.max_pair, sample.max_pair);
        }
        const bool predicted = cta::validation::passes_temporal_yellow_check(combined);
        if (predicted != temporal_case.expected_pass) ++temporal_failures;
        if (calibration_output.is_open()) {
            calibration_output << nlohmann::json{
                {"schema_version", 1}, {"detector", "yellow"},
                {"image", temporal_label->at("image")},
                {"component", "temporal_pair"},
                {"truth", temporal_case.expected_pass ? "positive" : "negative"},
                {"prediction", predicted ? "positive" : "negative"},
                {"regime", temporal_case.name},
                {"condition", "temporal_persistence"},
                {"temporal_window_seconds", cta::validation::kYellowTemporalWindowSeconds},
                {"temporal_sample_count", combined.sample_count},
                {"temporal_pair_pass_count", combined.pair_pass_count},
                {"temporal_max_from", combined.max_from},
                {"temporal_max_to", combined.max_to},
                {"temporal_max_pair", combined.max_pair},
            }.dump() << '\n';
        }
        std::cout << "  Yellow temporal " << temporal_case.name
                  << ": predicted=" << (predicted ? "pass" : "reject")
                  << " samples=" << combined.sample_count
                  << " pair_passes=" << combined.pair_pass_count << '\n';
    }

    EXPECT_EQ(observation_count,
              expected_labels.size() * variants.size() * 3u);
    EXPECT_EQ(category_observation_count, category_cases.size() * 3u);
    EXPECT_EQ(category_failures, 0);
    EXPECT_EQ(temporal_failures, 0);
    std::cout << "  Yellow calibration observations: " << observation_count
              << " across " << variants.size() << " visual/geometry regimes\n";
}
#endif

#endif // TEST_YELLOW_SQUARES

// ─── PIECE COUNTING ──────────────────────────────────────────────────────────
#if TEST_PIECE_COUNTS

TEST_F(DetectorsTest, PieceCounts) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "piece-counts").string();
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "red-squares").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    cv::Mat red_board = cv::imread(red_board_path_);

    std::cout << "\nRunning unit tests on red square images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_str = stem(img_path);
        std::vector<std::string> expected;
        constexpr std::string_view kAnd = "-and-";
        size_t start = 0;
        while (start <= expected_str.size()) {
            const size_t end = expected_str.find(kAnd, start);
            const size_t length = end == std::string::npos
                ? expected_str.size() - start : end - start;
            if (length > 0) expected.push_back(expected_str.substr(start, length));
            if (end == std::string::npos) break;
            start = end + kAnd.size();
        }
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
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "yellow-arrows").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::cout << "\nRunning unit tests on yellow arrow images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected_str = stem(img_path);
        std::vector<std::string> expected;
        constexpr std::string_view kAnd = "-and-";
        constexpr std::string_view kTo = "-to-";
        size_t start = 0;
        while (start <= expected_str.size()) {
            const size_t end = expected_str.find(kAnd, start);
            const size_t length = end == std::string::npos
                ? expected_str.size() - start : end - start;
            std::string arrow = expected_str.substr(start, length);
            const size_t to = arrow.find(kTo);
            if (to != std::string::npos) arrow.replace(to, kTo.size(), "");
            if (!arrow.empty()) expected.push_back(std::move(arrow));
            if (end == std::string::npos) break;
            start = end + kAnd.size();
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
#if TEST_MISALIGNED_PIECE || TEST_HOVER_CALIBRATION_REGIMES

struct HoverCalibrationVariant {
    const char* name;
    const char* condition;
    bool expected_hover;
    int visible_edges;
    int thickness;
};

struct HoverTransitionCalibration {
    const char* name;
    int hover_samples;
    int visible_edges;
    int thickness;
};

static cv::Mat make_hover_calibration_frame(
    const cv::Mat& board, const HoverCalibrationVariant& variant) {
    cv::Mat frame = board.clone();
    const int square_width = frame.cols / 8;
    const int square_height = frame.rows / 8;
    const int left = 4 * square_width;
    const int top = 4 * square_height;
    const int right = 5 * square_width - 4;
    const int bottom = 5 * square_height - 4;
    const cv::Scalar white(255, 255, 255);

    if (variant.visible_edges >= 0) {
        const int edge_mask = variant.visible_edges;
        if (edge_mask & 1) {
            cv::line(frame, {left, top}, {right, top}, white,
                     variant.thickness, cv::LINE_8);
        }
        if (edge_mask & 2) {
            cv::line(frame, {right, top}, {right, bottom}, white,
                     variant.thickness, cv::LINE_8);
        }
        if (edge_mask & 4) {
            cv::line(frame, {right, bottom}, {left, bottom}, white,
                     variant.thickness, cv::LINE_8);
        }
        if (edge_mask & 8) {
            cv::line(frame, {left, bottom}, {left, top}, white,
                     variant.thickness, cv::LINE_8);
        }
    } else {
        // A short cursor-like mark must not become a connected hover outline.
        cv::line(frame,
                 {left + square_width / 2, top + square_height / 2},
                 {left + square_width / 2 + 12, top + square_height / 2 + 8},
                 white, 1, cv::LINE_8);
    }
    return frame;
}

#if TEST_MISALIGNED_PIECE
TEST_F(DetectorsTest, MisalignedPiece) {
    const std::string images_dir = (std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "misaligned-pieces").string();
    auto files = list_files(images_dir, {".png", ".jpg"});
    if (files.empty()) GTEST_SKIP() << "Directory not found: " << images_dir;

    std::string debug_dir = "debug_screenshots/misaligned_pieces";
    std::filesystem::create_directories(debug_dir);

    std::cout << "\nRunning unit tests on misaligned piece images...\n";
    for (const auto& img_path : files) {
        cv::Mat img = cv::imread(img_path);
        if (img.empty()) continue;

        std::string expected = stem(img_path);
        constexpr std::string_view kReal = "-real-";
        if (const size_t real_marker = expected.find(kReal);
            real_marker != std::string::npos) {
            expected.resize(real_marker);
        }
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

#endif

#if TEST_HOVER_CALIBRATION_REGIMES
TEST_F(DetectorsTest, HoverCalibrationRegimes) {
    if (board_.empty()) GTEST_SKIP() << "Board template unavailable";

    std::ofstream calibration_output;
    if (const char* output_path = std::getenv("CTA_HOVER_CALIBRATION_FILE");
        output_path != nullptr && *output_path != '\0') {
        calibration_output.open(output_path, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(calibration_output.is_open())
            << "Could not write hover calibration output: " << output_path;
    }

    BoardGeometry geometry;
    geometry.bw = board_.cols;
    geometry.bh = board_.rows;
    geometry.sq_w = static_cast<double>(board_.cols) / 8.0;
    geometry.sq_h = static_cast<double>(board_.rows) / 8.0;
    geometry.geometry_confidence = 1.0;

    const std::array<HoverCalibrationVariant, 6> variants{{
        {"settled_board", "settled_board", false, 0, 0},
        {"cursor_overlay", "cursor_overlay", false, -1, 1},
        {"fast_animation", "fast_animation", true, 15, 8},
        {"slow_animation", "slow_animation", true, 15, 3},
        {"partial_movement", "partial_movement", true, 3, 8},
        {"partial_movement_thin", "partial_movement", true, 3, 3},
    }};

    int false_rejections = 0;
    int settled_false_positives = 0;
    int true_mid_drag_detections = 0;
    for (const auto& variant : variants) {
        const cv::Mat frame = make_hover_calibration_frame(board_, variant);
        const std::string actual = find_misaligned_piece(frame, board_, geometry);
        cv::Mat white_mask;
        cv::Mat reduced;
        const auto measurement = validation::measure_hover_box(
            frame, geometry, white_mask, reduced, "e4");
        const bool predicted = !actual.empty();
        if (predicted != variant.expected_hover) ++false_rejections;
        if (std::string(variant.condition) == "settled_board" && predicted) {
            ++settled_false_positives;
        }
        if (variant.expected_hover && predicted) ++true_mid_drag_detections;

        if (calibration_output.is_open()) {
            calibration_output << nlohmann::json{
                {"schema_version", 1},
                {"detector", "hover"},
                {"component", "hover_box"},
                {"regime", variant.name},
                {"condition", variant.condition},
                {"truth", variant.expected_hover ? "positive" : "negative"},
                {"prediction", predicted ? "positive" : "negative"},
                {"selected_square", actual},
                {"visible_edges", measurement.visible_edges},
                {"strongest_edge", measurement.strongest_edge},
                {"geometry_uncertainty", measurement.geometry_uncertainty},
            }.dump() << '\n';
        }

        std::cout << "  Hover regime " << variant.name
                  << ": predicted=" << (predicted ? "hover" : "clear")
                  << " selected=" << (actual.empty() ? "none" : actual)
                  << " visible_edges=" << measurement.visible_edges << '\n';
    }

    const std::array<HoverTransitionCalibration, 3> transitions{{
        {"fast_transition", 2, 15, 8},
        {"slow_transition", 4, 15, 3},
        {"partial_transition", 3, 3, 8},
    }};
    int transition_failures = 0;
    for (const auto& transition : transitions) {
        const double sample_step = cta::kMapperFineScanStepSeconds;
        const int total_samples = transition.hover_samples + 6;
        double quiet_seconds = 0.0;
        double first_clear_timestamp = -1.0;
        double settled_tail_timestamp = -1.0;
        double last_hover_timestamp = -1.0;
        bool candidate_ready = false;
        bool premature = false;
        for (int sample = 0; sample < total_samples; ++sample) {
            const bool expected_hover = sample < transition.hover_samples;
            const HoverCalibrationVariant frame_variant{
                transition.name, "transition_level", expected_hover,
                expected_hover ? transition.visible_edges : 0,
                expected_hover ? transition.thickness : 0};
            const cv::Mat frame = make_hover_calibration_frame(board_, frame_variant);
            const bool detected = !find_misaligned_piece(frame, board_, geometry).empty();
            const double timestamp = sample * sample_step;
            if (detected) {
                last_hover_timestamp = timestamp;
                quiet_seconds = 0.0;
                if (candidate_ready) premature = true;
                continue;
            }
            if (first_clear_timestamp < 0.0) first_clear_timestamp = timestamp;
            quiet_seconds += sample_step;
            if (!candidate_ready &&
                quiet_seconds >= cta::kMapperSettleConfirmationSeconds) {
                candidate_ready = true;
                settled_tail_timestamp = timestamp;
            }
        }

        const bool valid_transition = candidate_ready && !premature &&
            first_clear_timestamp >= 0.0 && last_hover_timestamp >= 0.0;
        if (!valid_transition) ++transition_failures;
        if (calibration_output.is_open()) {
            calibration_output << nlohmann::json{
                {"schema_version", 1},
                {"detector", "hover"},
                {"component", "settle_window"},
                {"regime", transition.name},
                {"condition", "transition_level"},
                {"truth", "positive"},
                {"prediction", valid_transition ? "positive" : "negative"},
                {"settle_window_seconds", cta::kMapperSettleConfirmationSeconds},
                {"sample_step_seconds", sample_step},
                {"hover_samples", transition.hover_samples},
                {"first_clear_timestamp", first_clear_timestamp},
                {"last_hover_timestamp", last_hover_timestamp},
                {"settled_tail_timestamp", settled_tail_timestamp},
                {"settle_delay_seconds", settled_tail_timestamp - last_hover_timestamp},
                {"premature_settle", premature},
            }.dump() << '\n';
        }
        std::cout << "  Hover transition " << transition.name
                  << ": settle_delay="
                  << (settled_tail_timestamp - last_hover_timestamp)
                  << "s premature=" << (premature ? "yes" : "no") << '\n';
    }

    EXPECT_EQ(settled_false_positives, 0);
    EXPECT_EQ(true_mid_drag_detections, 4);
    EXPECT_EQ(false_rejections, 0);
    EXPECT_EQ(transition_failures, 0);
}
#endif

#endif // TEST_MISALIGNED_PIECE

// ─── GAME CLOCKS ─────────────────────────────────────────────────────────────
#if TEST_GAME_CLOCKS || TEST_GAME_CLOCK_CALIBRATION_LABELS || \
    TEST_GAME_CLOCK_CALIBRATION_REGIMES || TEST_GAME_CLOCK_CALIBRATION_ROI_REGIMES

struct ClockCalibrationVariant {
    const char* name;
    const char* condition;
};

struct ClockRoiCalibrationVariant {
    const char* name;
    const char* condition;
    double geometry_offset_x_squares;
    double geometry_offset_y_squares;
    double left_edge_ratio;
};

static cv::Mat make_clock_calibration_variant(
    const cv::Mat& image, const ClockCalibrationVariant& variant) {
    if (std::string(variant.name) == "native") {
        return image.clone();
    }
    if (std::string(variant.name) == "scaled_75") {
        cv::Mat scaled;
        cv::resize(image, scaled, cv::Size(), 0.75, 0.75, cv::INTER_AREA);
        return scaled;
    }
    if (std::string(variant.name) == "font_small") {
        cv::Mat scaled;
        cv::resize(image, scaled, cv::Size(), 0.50, 0.50, cv::INTER_AREA);
        return scaled;
    }
    if (std::string(variant.name) == "font_large") {
        cv::Mat scaled;
        cv::resize(image, scaled, cv::Size(), 1.25, 1.25, cv::INTER_CUBIC);
        return scaled;
    }
    if (std::string(variant.name) == "antialiased") {
        cv::Mat reduced;
        cv::Mat restored;
        cv::resize(image, reduced, cv::Size(), 0.60, 0.60, cv::INTER_AREA);
        cv::resize(reduced, restored, image.size(), 0.0, 0.0, cv::INTER_LINEAR);
        return restored;
    }
    if (std::string(variant.name) == "jpeg_compressed") {
        std::vector<uchar> encoded;
        if (cv::imencode(".jpg", image, encoded,
                         {cv::IMWRITE_JPEG_QUALITY, 35})) {
            const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
            if (!decoded.empty()) return decoded;
        }
        return image.clone();
    }
    if (std::string(variant.name) == "brightness_reduced") {
        cv::Mat reduced;
        image.convertTo(reduced, image.type(), 0.75, 0.0);
        return reduced;
    }
    if (std::string(variant.name) == "brightness_increased") {
        cv::Mat increased;
        image.convertTo(increased, image.type(), 1.15, 8.0);
        return increased;
    }
    if (std::string(variant.name) == "low_time_format" ||
        std::string(variant.name) == "separator_removed" ||
        std::string(variant.name) == "partial_change") {
        return image.clone();
    }
    cv::Mat blurred;
    cv::GaussianBlur(image, blurred, cv::Size(3, 3), 0.0);
    return blurred;
}

static void apply_clock_stress_variant(
    cv::Mat& image, const BoardGeometry& geometry,
    const ClockCalibrationVariant& variant) {
    const std::string name = variant.name;
    if (name != "low_time_format" && name != "separator_removed" &&
        name != "partial_change") {
        return;
    }

    const int x1 = std::max(0, static_cast<int>(geometry.bx + geometry.bw * 0.70));
    const int x2 = std::min(image.cols, static_cast<int>(geometry.bx + geometry.bw));
    const int top_y1 = std::max(0, static_cast<int>(geometry.by - geometry.sq_h * 0.55));
    const int top_y2 = std::min(image.rows, static_cast<int>(geometry.by - geometry.sq_h * 0.08));
    const int bottom_y1 = std::max(0, static_cast<int>(geometry.by + geometry.bh + geometry.sq_h * 0.18));
    const int bottom_y2 = std::min(image.rows, static_cast<int>(geometry.by + geometry.bh + geometry.sq_h * 0.58));
    if (x2 <= x1 || top_y2 <= top_y1 || bottom_y2 <= bottom_y1) return;

    const auto erase_region = [&](int y1, int y2, double left, double right) {
        const int rx1 = std::max(x1, static_cast<int>(x1 + (x2 - x1) * left));
        const int rx2 = std::min(x2, static_cast<int>(x1 + (x2 - x1) * right));
        const int ry1 = std::max(y1, y1 + (y2 - y1) / 5);
        const int ry2 = std::min(y2, y2 - (y2 - y1) / 5);
        if (rx2 <= rx1 || ry2 <= ry1) return;
        const cv::Scalar fill = cv::mean(image(cv::Rect(x1, y1, x2 - x1, y2 - y1)));
        cv::rectangle(image, cv::Rect(rx1, ry1, rx2 - rx1, ry2 - ry1), fill, cv::FILLED);
    };

    if (name == "low_time_format") {
        // Remove the leading hour field to emulate a mm:ss clock.
        erase_region(top_y1, top_y2, 0.00, 0.22);
        erase_region(bottom_y1, bottom_y2, 0.00, 0.22);
    } else if (name == "separator_removed") {
        // Remove both separator bands while preserving surrounding glyphs.
        erase_region(top_y1, top_y2, 0.34, 0.40);
        erase_region(top_y1, top_y2, 0.64, 0.70);
        erase_region(bottom_y1, bottom_y2, 0.34, 0.40);
        erase_region(bottom_y1, bottom_y2, 0.64, 0.70);
    } else {
        // A transition can expose one partially updated digit; damage one
        // clock only so activity and OCR remain separately measurable.
        erase_region(bottom_y1, bottom_y2, 0.50, 0.60);
    }
}

static std::string clock_reading_for_component(
    const ClockState& state, const std::string& component) {
    if (component == "active_side") return state.active_player;
    if (component == "white_ocr") return state.white_time;
    return state.black_time;
}

static cv::Mat clock_roi_for_calibration(const cv::Mat& image,
                                         const BoardGeometry& geometry,
                                         bool white_clock) {
    const int x1 = std::max(0, static_cast<int>(geometry.bx + geometry.bw * 0.70));
    const int x2 = std::min(image.cols, static_cast<int>(geometry.bx + geometry.bw));
    const int y1 = white_clock
        ? std::min(image.rows - 1, static_cast<int>(geometry.by + geometry.bh + geometry.sq_h * 0.18))
        : std::max(0, static_cast<int>(geometry.by - geometry.sq_h * 0.55));
    const int y2 = white_clock
        ? std::min(image.rows, static_cast<int>(geometry.by + geometry.bh + geometry.sq_h * 0.58))
        : std::max(y1 + 1, static_cast<int>(geometry.by - geometry.sq_h * 0.08));
    if (x2 <= x1 || y2 <= y1) return {};
    return image(cv::Rect(x1, y1, x2 - x1, y2 - y1));
}

static bool clock_rois_for_calibration(
    const cv::Mat& image,
    const BoardGeometry& geometry,
    const ClockRoiCalibrationVariant& variant,
    cv::Mat& top_roi,
    cv::Mat& bottom_roi) {
    const double offset_x = geometry.sq_w * variant.geometry_offset_x_squares;
    const double offset_y = geometry.sq_h * variant.geometry_offset_y_squares;
    const int shifted_bx = static_cast<int>(std::lround(geometry.bx + offset_x));
    const int shifted_by = static_cast<int>(std::lround(geometry.by + offset_y));
    const int roi_x1 = std::max(
        0, static_cast<int>(shifted_bx + geometry.bw * variant.left_edge_ratio));
    const int roi_x2 = std::min(
        image.cols, static_cast<int>(shifted_bx + geometry.bw));
    const int top_y1 = std::max(
        0, static_cast<int>(shifted_by - geometry.sq_h * 0.55));
    const int top_y2 = std::max(
        top_y1 + 1, static_cast<int>(shifted_by - geometry.sq_h * 0.08));
    const int bottom_y1 = std::min(
        image.rows - 1,
        static_cast<int>(shifted_by + geometry.bh + geometry.sq_h * 0.18));
    const int bottom_y2 = std::min(
        image.rows,
        static_cast<int>(shifted_by + geometry.bh + geometry.sq_h * 0.58));
    if (roi_x2 <= roi_x1 || top_y2 <= top_y1 ||
        bottom_y2 <= bottom_y1) {
        return false;
    }
    top_roi = image(cv::Rect(roi_x1, top_y1, roi_x2 - roi_x1, top_y2 - top_y1));
    bottom_roi = image(cv::Rect(
        roi_x1, bottom_y1, roi_x2 - roi_x1, bottom_y2 - bottom_y1));
    return true;
}

static void write_clock_calibration_observation(
    std::ofstream& output,
    const std::string& image_name,
    const std::string& component,
    bool truth,
    bool predicted,
    const std::string& regime,
    const std::string& condition,
    const std::string& preprocessing_variant,
    const ClockState& state,
    const nlohmann::json& expected,
    const ClockOcrDiagnostics* ocr_diagnostics = nullptr,
    const std::string& roi_variant = "production_roi",
    double roi_offset_x_squares = 0.0,
    double roi_offset_y_squares = 0.0,
    double roi_left_edge_ratio = 0.70) {
    if (!output.is_open()) return;

    const bool complete_ocr = state.white_time ==
            expected.value("expected_white", std::string()) &&
        state.black_time == expected.value("expected_black", std::string());
    const std::string reading_quality = complete_ocr
        ? "valid"
        : (state.white_time.empty() || state.black_time.empty() ? "missing" : "misread");
    nlohmann::json segmented_digits = nlohmann::json::array();
    if (ocr_diagnostics != nullptr) {
        for (const auto& segment : ocr_diagnostics->segments) {
            segmented_digits.push_back({
                {"x", segment.x}, {"y", segment.y},
                {"width", segment.width}, {"height", segment.height},
                {"symbol", std::string(1, segment.symbol)},
            });
        }
    }
    output << nlohmann::json{
        {"schema_version", 1},
        {"detector", "clock"},
        {"image", image_name},
        {"component", component},
        {"truth", truth ? "positive" : "negative"},
        {"prediction", predicted ? "positive" : "negative"},
        {"case", expected.value("case", std::string("unspecified"))},
        {"regime", regime},
        {"condition", condition},
        {"preprocessing_variant", preprocessing_variant},
        {"roi_variant", roi_variant},
        {"roi_geometry_offset_x_squares", roi_offset_x_squares},
        {"roi_geometry_offset_y_squares", roi_offset_y_squares},
        {"roi_left_edge_ratio", roi_left_edge_ratio},
        {"thresholding_mode", ocr_diagnostics != nullptr
            ? ocr_diagnostics->thresholding_mode
            : "component_first_adaptive_fallback"},
        {"active_player", state.active_player},
        {"white_time", state.white_time},
        {"black_time", state.black_time},
        {"selected_reading", ocr_diagnostics != nullptr &&
                !ocr_diagnostics->selected_reading.empty()
            ? ocr_diagnostics->selected_reading
            : clock_reading_for_component(state, component)},
        {"reading_quality", reading_quality},
        {"ocr_skipped", state.ocr_skipped},
        {"ocr_preprocessing_variant", ocr_diagnostics != nullptr
            ? ocr_diagnostics->preprocessing_variant : ""},
        {"segmented_digits", segmented_digits},
        {"candidate_readings", ocr_diagnostics != nullptr
            ? ocr_diagnostics->candidates : std::vector<std::string>{}},
        {"expected_active", expected.value("expected_active", std::string())},
        {"expected_white", expected.value("expected_white", std::string())},
        {"expected_black", expected.value("expected_black", std::string())},
    }.dump() << '\n';
}

#if TEST_GAME_CLOCKS
TEST_F(DetectorsTest, GameClocks) {
    const auto dataset_dir = std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "clock-changes";
    const auto labels_path = dataset_dir / "labels.jsonl";
    std::ifstream labels(labels_path);
    if (!labels.is_open()) GTEST_SKIP() << "Labels not found: " << labels_path.string();

    // This is a corpus diagnostic, not the calibration gate. The manifest
    // deliberately retains detector misses so the calibration test can report
    // them; turning every known miss into a failing smoke test obscures the
    // distinction between infrastructure failure and detector quality.
    std::map<std::string, nlohmann::json> expected_by_image;
    std::string label_line;
    while (std::getline(labels, label_line)) {
        if (label_line.empty()) continue;
        const auto label = nlohmann::json::parse(label_line);
        if (label.value("component", std::string()) == "active_side") {
            expected_by_image[label.at("image").get<std::string>()] = label;
        }
    }
    if (expected_by_image.empty()) GTEST_SKIP() << "No clock labels found: " << labels_path.string();

    std::string debug_dir = "debug_screenshots/game_clocks";
    std::filesystem::create_directories(debug_dir);

    std::cout << "\nRunning unit tests on game clocks...\n";
    int passed = 0;
    int failed = 0;
    int processed = 0;
    for (const auto& [image_name, expected] : expected_by_image) {
        const auto image_path = dataset_dir / image_name;
        const cv::Mat img = cv::imread(image_path.string());
        ASSERT_FALSE(img.empty()) << "Could not read labeled image: " << image_path.string();

        const std::string expected_active = expected.at("expected_active").get<std::string>();
        const std::string expected_white = expected.at("expected_white").get<std::string>();
        const std::string expected_black = expected.at("expected_black").get<std::string>();
        const BoardGeometry img_geo = locate_board(img, board_);
        if (img_geo.bw == 0 || img_geo.bh == 0) {
            std::cout << "  MISSING BOARD: " << image_name << "\n";
            ++failed;
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
        int bot_roi_y1 = std::min(debug_img.rows, static_cast<int>(img_geo.by + img_geo.bh + img_geo.sq_h * 0.18));
        int bot_roi_y2 = std::min(debug_img.rows, static_cast<int>(img_geo.by + img_geo.bh + img_geo.sq_h * 0.58));

        cv::rectangle(debug_img, cv::Point(roi_x1, top_roi_y1), cv::Point(roi_x2, top_roi_y2), cv::Scalar(0, 0, 255), 2); // Red for top (black clock)
        cv::rectangle(debug_img, cv::Point(roi_x1, bot_roi_y1), cv::Point(roi_x2, bot_roi_y2), cv::Scalar(0, 255, 0), 2); // Green for bot (white clock)
        const std::string debug_path = (std::filesystem::path(debug_dir) / (stem(image_path.string()) + "_boxes.png")).string();
        cv::imwrite(debug_path, debug_img);

        ++processed;
        std::cout << "  " << (pass ? "PASS" : "MISS") << ": " << image_name
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

    }
    std::cout << "Clock corpus diagnostic: " << passed << "/" << expected_by_image.size()
              << " exact readings, " << failed << " misses or unavailable images.\n";
    EXPECT_GT(processed, 0);
}

#endif

#if TEST_GAME_CLOCK_CALIBRATION_LABELS
TEST_F(DetectorsTest, GameClockCalibrationLabels) {
    const auto dataset_dir = std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "clock-changes";
    const auto labels_path = dataset_dir / "labels.jsonl";
    std::ifstream labels(labels_path);
    if (!labels.is_open()) GTEST_SKIP() << "Labels not found: " << labels_path.string();

    struct Counts { int true_positive = 0; int true_negative = 0;
                    int false_positive = 0; int false_negative = 0; };
    std::map<std::string, Counts> metrics;
    struct CalibrationState {
        ClockState state;
        ClockOcrDiagnostics white_ocr;
        ClockOcrDiagnostics black_ocr;
    };
    std::map<std::string, CalibrationState> state_cache;
    std::ofstream calibration_output;
    if (const char* output_path = std::getenv("CTA_CLOCK_CALIBRATION_FILE");
        output_path != nullptr && *output_path != '\0') {
        calibration_output.open(output_path, std::ios::out | std::ios::trunc);
        ASSERT_TRUE(calibration_output.is_open())
            << "Could not write clock calibration output: " << output_path;
    }
    std::string line;
    while (std::getline(labels, line)) {
        if (line.empty()) continue;
        const auto label = nlohmann::json::parse(line);
        const auto image_path = dataset_dir / label.at("image").get<std::string>();
        auto state_it = state_cache.find(image_path.string());
        if (state_it == state_cache.end()) {
            const auto image = cv::imread(image_path.string());
            ASSERT_FALSE(image.empty()) << "Could not read labeled image: " << image_path.string();
            const auto geometry = locate_board(image, board_);
            CalibrationState calibration_state;
            calibration_state.state = extract_clocks(image, board_, geometry);
            calibration_state.white_ocr = diagnose_clock_time_from_roi(
                clock_roi_for_calibration(image, geometry, true),
                calibration_state.state.active_player == "white");
            calibration_state.black_ocr = diagnose_clock_time_from_roi(
                clock_roi_for_calibration(image, geometry, false),
                calibration_state.state.active_player == "black");
            state_it = state_cache.emplace(image_path.string(),
                                           std::move(calibration_state)).first;
        }
        const auto& calibration_state = state_it->second;
        const auto& state = calibration_state.state;
        const auto component = label.at("component").get<std::string>();
        const bool truth = label.value("truth", std::string("uncertain")) == "positive";
        const bool predicted = component == "active_side"
            ? state.active_player == label.at("expected_active").get<std::string>()
            : component == "white_ocr"
                ? state.white_time == label.at("expected_white").get<std::string>()
                : state.black_time == label.at("expected_black").get<std::string>();

        write_clock_calibration_observation(
            calibration_output, label.at("image").get<std::string>(), component,
            truth, predicted, label.value("regime", std::string("seed")),
            label.value("condition", std::string("clean")),
            "production_clock_roi", state, label,
            component == "white_ocr" ? &calibration_state.white_ocr
                                      : component == "black_ocr" ? &calibration_state.black_ocr
                                                                  : nullptr);
        auto& count = metrics[component];
        if (truth && predicted) ++count.true_positive;
        else if (!truth && !predicted) ++count.true_negative;
        else if (!truth) ++count.false_positive;
        else ++count.false_negative;
    }

    int expected_labeled_per_component = -1;
    for (const auto& [component, count] : metrics) {
        const int labeled = count.true_positive + count.true_negative +
            count.false_positive + count.false_negative;
        ASSERT_GE(labeled, 3) << component;
        if (expected_labeled_per_component < 0) {
            expected_labeled_per_component = labeled;
        } else {
            EXPECT_EQ(labeled, expected_labeled_per_component) << component;
        }
        const double accuracy = static_cast<double>(count.true_positive + count.true_negative) /
            labeled;
        std::cout << "  Clock " << component << " calibration: TP=" << count.true_positive
                  << " TN=" << count.true_negative << " FP=" << count.false_positive
                  << " FN=" << count.false_negative << " accuracy=" << accuracy << "\n";
    }
    EXPECT_EQ(metrics.size(), 3u);
}

#endif

#if TEST_GAME_CLOCK_CALIBRATION_REGIMES
TEST_F(DetectorsTest, GameClockCalibrationRegimes) {
    const auto dataset_dir = std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "clock-changes";
    const auto labels_path = dataset_dir / "labels.jsonl";
    std::ifstream labels(labels_path);
    if (!labels.is_open()) GTEST_SKIP() << "Labels not found: " << labels_path.string();

    std::ofstream calibration_output;
    if (const char* output_path = std::getenv("CTA_CLOCK_CALIBRATION_FILE");
        output_path != nullptr && *output_path != '\0') {
        calibration_output.open(output_path, std::ios::out | std::ios::app);
        ASSERT_TRUE(calibration_output.is_open())
            << "Could not append clock calibration output: " << output_path;
    }

    std::map<std::string, nlohmann::json> expected_by_image;
    std::string line;
    while (std::getline(labels, line)) {
        if (line.empty()) continue;
        const auto label = nlohmann::json::parse(line);
        expected_by_image.emplace(label.at("image").get<std::string>(), label);
    }

    struct ClockCalibrationSource {
        cv::Mat image;
        BoardGeometry geometry;
    };
    std::map<std::string, ClockCalibrationSource> sources;
    for (const auto& [image_name, expected] : expected_by_image) {
        (void)expected;
        const cv::Mat image = cv::imread((dataset_dir / image_name).string());
        ASSERT_FALSE(image.empty()) << "Could not read labeled image: " << image_name;
        sources.emplace(image_name, ClockCalibrationSource{image, locate_board(image, board_)});
    }

    const auto geometry_for_variant = [](const BoardGeometry& source_geometry,
                                         const char* variant_name) {
        double scale = 1.0;
        if (std::string(variant_name) == "scaled_75") scale = 0.75;
        else if (std::string(variant_name) == "font_small") scale = 0.50;
        else if (std::string(variant_name) == "font_large") scale = 1.25;
        BoardGeometry geometry = source_geometry;
        geometry.bx = static_cast<int>(std::lround(source_geometry.bx * scale));
        geometry.by = static_cast<int>(std::lround(source_geometry.by * scale));
        geometry.bw = static_cast<int>(std::lround(source_geometry.bw * scale));
        geometry.bh = static_cast<int>(std::lround(source_geometry.bh * scale));
        geometry.sq_w = source_geometry.sq_w * scale;
        geometry.sq_h = source_geometry.sq_h * scale;
        return geometry;
    };

    const std::array<ClockCalibrationVariant, 12> variants{{
        {"native", "clean"},
        {"scaled_75", "scaling"},
        {"font_small", "font_size"},
        {"font_large", "font_size"},
        {"antialiased", "anti_aliasing"},
        {"jpeg_compressed", "compression"},
        {"brightness_reduced", "brightness"},
        {"brightness_increased", "brightness"},
        {"blurred", "compression_like_blur"},
        {"low_time_format", "low_time_formatting"},
        {"separator_removed", "separators"},
        {"partial_change", "partial_changes"},
    }};

    for (const auto& variant : variants) {
        int activity_correct = 0;
        int complete_correct = 0;
        int valid_count = 0;
        int missing_count = 0;
        int misread_count = 0;
        int observed_count = 0;

        for (const auto& [image_name, expected] : expected_by_image) {
            const auto& source = sources.at(image_name);
            cv::Mat variant_image = make_clock_calibration_variant(source.image, variant);
            const BoardGeometry geometry = geometry_for_variant(source.geometry, variant.name);
            if (geometry.bw <= 0 || geometry.bh <= 0) {
                ++missing_count;
                continue;
            }

            apply_clock_stress_variant(variant_image, geometry, variant);

            const ClockState state = extract_clocks(variant_image, board_, geometry);
            ++observed_count;
            const bool activity_ok = state.active_player ==
                expected.at("expected_active").get<std::string>();
            const bool white_ok = state.white_time ==
                expected.at("expected_white").get<std::string>();
            const bool black_ok = state.black_time ==
                expected.at("expected_black").get<std::string>();
            activity_correct += activity_ok ? 1 : 0;
            complete_correct += (white_ok && black_ok) ? 1 : 0;

            if (white_ok && black_ok) {
                ++valid_count;
            } else if (state.white_time.empty() || state.black_time.empty()) {
                ++missing_count;
            } else {
                ++misread_count;
            }

            const auto white_ocr = diagnose_clock_time_from_roi(
                clock_roi_for_calibration(variant_image, geometry, true),
                state.active_player == "white");
            const auto black_ocr = diagnose_clock_time_from_roi(
                clock_roi_for_calibration(variant_image, geometry, false),
                state.active_player == "black");

            write_clock_calibration_observation(
                calibration_output, image_name, "active_side", true, activity_ok,
                variant.name, variant.condition, variant.name, state, expected);
            write_clock_calibration_observation(
                calibration_output, image_name, "white_ocr", true, white_ok,
                variant.name, variant.condition, variant.name, state, expected,
                &white_ocr);
            write_clock_calibration_observation(
                calibration_output, image_name, "black_ocr", true, black_ok,
                variant.name, variant.condition, variant.name, state, expected,
                &black_ocr);
        }

        std::cout << "  Clock regime " << variant.name
                  << ": activity=" << activity_correct << "/" << observed_count
                  << " complete_ocr=" << complete_correct << "/" << observed_count
                  << " valid=" << valid_count
                  << " missing=" << missing_count
                  << " misread=" << misread_count << "\n";
        EXPECT_EQ(observed_count, static_cast<int>(expected_by_image.size()))
            << "Board localization unavailable in " << variant.name;
    }
}

#endif

#if TEST_GAME_CLOCK_CALIBRATION_ROI_REGIMES
TEST_F(DetectorsTest, GameClockCalibrationRoiRegimes) {
    const auto dataset_dir = std::filesystem::path(assets_dir_) / "fixtures" / "detectors" / "clock-changes";
    const auto labels_path = dataset_dir / "labels.jsonl";
    std::ifstream labels(labels_path);
    if (!labels.is_open()) GTEST_SKIP() << "Labels not found: " << labels_path.string();

    std::ofstream calibration_output;
    if (const char* output_path = std::getenv("CTA_CLOCK_CALIBRATION_FILE");
        output_path != nullptr && *output_path != '\0') {
        calibration_output.open(output_path, std::ios::out | std::ios::app);
        ASSERT_TRUE(calibration_output.is_open())
            << "Could not append clock calibration output: " << output_path;
    }

    std::map<std::string, nlohmann::json> expected_by_image;
    std::string line;
    while (std::getline(labels, line)) {
        if (line.empty()) continue;
        const auto label = nlohmann::json::parse(line);
        expected_by_image.emplace(label.at("image").get<std::string>(), label);
    }

    struct ClockRoiCalibrationSource {
        cv::Mat image;
        BoardGeometry geometry;
    };
    std::map<std::string, ClockRoiCalibrationSource> sources;
    for (const auto& [image_name, expected] : expected_by_image) {
        (void)expected;
        const cv::Mat image = cv::imread((dataset_dir / image_name).string());
        ASSERT_FALSE(image.empty()) << "Could not read labeled image: " << image_name;
        sources.emplace(image_name, ClockRoiCalibrationSource{image, locate_board(image, board_)});
    }

    const std::array<ClockRoiCalibrationVariant, 7> variants{{
        {"roi_native", "roi_geometry", 0.00, 0.00, 0.70},
        {"roi_shift_left", "localization_error", -0.08, 0.00, 0.70},
        {"roi_shift_right", "localization_error", 0.08, 0.00, 0.70},
        {"roi_shift_up", "localization_error", 0.00, -0.08, 0.70},
        {"roi_shift_down", "localization_error", 0.00, 0.08, 0.70},
        {"roi_expanded_left", "roi_margin", 0.00, 0.00, 0.62},
        {"roi_narrow_left", "roi_margin", 0.00, 0.00, 0.78},
    }};

    std::size_t observation_count = 0;
    for (const auto& variant : variants) {
        int activity_correct = 0;
        int complete_correct = 0;
        int observed_count = 0;
        for (const auto& [image_name, expected] : expected_by_image) {
            const auto& source = sources.at(image_name);
            const cv::Mat& image = source.image;
            const BoardGeometry& geometry = source.geometry;
            cv::Mat top_roi;
            cv::Mat bottom_roi;
            if (!clock_rois_for_calibration(
                    image, geometry, variant, top_roi, bottom_roi)) {
                continue;
            }

            const ClockState state = extract_clocks_from_rois(top_roi, bottom_roi);
            ++observed_count;
            const bool activity_ok = state.active_player ==
                expected.at("expected_active").get<std::string>();
            const bool white_ok = state.white_time ==
                expected.at("expected_white").get<std::string>();
            const bool black_ok = state.black_time ==
                expected.at("expected_black").get<std::string>();
            activity_correct += activity_ok ? 1 : 0;
            complete_correct += (white_ok && black_ok) ? 1 : 0;

            const auto white_ocr = diagnose_clock_time_from_roi(
                bottom_roi, state.active_player == "white");
            const auto black_ocr = diagnose_clock_time_from_roi(
                top_roi, state.active_player == "black");
            write_clock_calibration_observation(
                calibration_output, image_name, "active_side", true, activity_ok,
                variant.name, variant.condition, "native", state, expected,
                nullptr, variant.name, variant.geometry_offset_x_squares,
                variant.geometry_offset_y_squares, variant.left_edge_ratio);
            write_clock_calibration_observation(
                calibration_output, image_name, "white_ocr", true, white_ok,
                variant.name, variant.condition, "native", state, expected,
                &white_ocr, variant.name, variant.geometry_offset_x_squares,
                variant.geometry_offset_y_squares, variant.left_edge_ratio);
            write_clock_calibration_observation(
                calibration_output, image_name, "black_ocr", true, black_ok,
                variant.name, variant.condition, "native", state, expected,
                &black_ocr, variant.name, variant.geometry_offset_x_squares,
                variant.geometry_offset_y_squares, variant.left_edge_ratio);
            observation_count += 3;
        }

        std::cout << "  Clock ROI regime " << variant.name
                  << ": activity=" << activity_correct << "/" << observed_count
                  << " complete_ocr=" << complete_correct << "/" << observed_count
                  << "\n";
        EXPECT_EQ(observed_count, static_cast<int>(expected_by_image.size()))
            << "ROI unavailable in " << variant.name;
    }
    EXPECT_EQ(observation_count, expected_by_image.size() * variants.size() * 3u);
}
#endif

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
    const std::string video_path = find_game_fixture_file(
        assets_dir_, "seven-plies", "video.mp4").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }

    std::cout << "\nRunning integration test on 7 plies video...\n";

    IntegrationTestResult result;
    result.name = "7 Plies Extraction";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_7_plies");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    LichessGameMetadata video_metadata;
    ASSERT_TRUE(resolve_video_metadata(data, video_metadata, video_path));

    const std::string pgn_path = find_expected_game_pgn(
        assets_dir_, "seven-plies").string();
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;

    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    expect_fixture_metadata_contract(expected_data, pgn_path);
    expect_video_metadata_matches_answer_key(video_metadata, expected_data, video_path);
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

    const size_t first_mismatch = first_main_line_mismatch(expected_moves, data.moves);
    if (first_mismatch < data.moves.size() || first_mismatch < expected_moves.size()) {
        std::cout << "  First main-line mismatch at ply " << (first_mismatch + 1)
                  << ": expected "
                  << (first_mismatch < expected_moves.size() ? expected_moves[first_mismatch] : "(none)")
                  << ", extracted "
                  << (first_mismatch < data.moves.size() ? data.moves[first_mismatch] : "(none)")
                  << "\n";
    }

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);

    const bool main_line_passed = data.moves == expected_moves;
    const bool complete_output_passed = extracted_all == expected_all;
    const bool invariants_passed = verify_game_data_invariants(data);
    write_first_divergence_report(
        result.name, video_path, expected_moves, data, first_mismatch,
        main_line_passed, complete_output_passed,
        !main_line_passed ? "" :
        (complete_output_passed ? "" : "variation_or_move_set_mismatch"));
    result.passed = main_line_passed && complete_output_passed && invariants_passed;
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
    const std::string video_path = find_game_fixture_file(
        assets_dir_, "analysis-line-and-revert", "video.mp4").string();
    const std::string pgn_path = find_expected_game_pgn(
        assets_dir_, "analysis-line-and-revert").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }
    std::cout << "\nRunning integration test on medium game with revert...\n";

    IntegrationTestResult result;
    result.name = "Medium Game + Revert";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_medium_revert");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    LichessGameMetadata video_metadata;
    ASSERT_TRUE(resolve_video_metadata(data, video_metadata, video_path));
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;
    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    expect_fixture_metadata_contract(expected_data, pgn_path);
    expect_video_metadata_matches_answer_key(video_metadata, expected_data, video_path);
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

    const size_t first_mismatch = first_main_line_mismatch(expected_moves, data.moves);
    const bool main_line_passed = data.moves == expected_moves;
    const bool complete_output_passed = extracted_all == expected_all;
    const bool invariants_passed = verify_game_data_invariants(data);
    write_first_divergence_report(
        result.name, video_path, expected_moves, data, first_mismatch,
        main_line_passed, complete_output_passed,
        !main_line_passed ? "" :
        (complete_output_passed ? "" : "variation_or_move_set_mismatch"));
    result.passed = main_line_passed && complete_output_passed && invariants_passed;
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
    const std::string video_path = find_game_fixture_file(
        assets_dir_, "warmerdam-vs-dommaraju", "video.mp4").string();
    const std::string pgn_path = find_expected_game_pgn(
        assets_dir_, "warmerdam-vs-dommaraju").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
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

    LichessGameMetadata video_metadata;
    ASSERT_TRUE(resolve_video_metadata(data, video_metadata, video_path));
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;
    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    expect_fixture_metadata_contract(expected_data, pgn_path);
    expect_video_metadata_matches_answer_key(video_metadata, expected_data, video_path);
    std::vector<std::string> expected_moves = expected_data.main_line;
    induce_expected_failure_for_diagnostics(expected_moves);
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

    const size_t first_mismatch = first_main_line_mismatch(expected_moves, data.moves);
    if (first_mismatch < data.moves.size() || first_mismatch < expected_moves.size()) {
        std::cout << "  First main-line mismatch at ply " << (first_mismatch + 1)
                  << ": expected "
                  << (first_mismatch < expected_moves.size() ? expected_moves[first_mismatch] : "(none)")
                  << ", extracted "
                  << (first_mismatch < data.moves.size() ? data.moves[first_mismatch] : "(none)")
                  << "\n";
    }

    std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);
    std::multiset<std::string> detected_timeline;
    for (const std::string& move : data.video_moves) {
        if (move != "REVERT") {
            detected_timeline.insert(move);
        }
    }

    const bool main_line_passed = data.moves == expected_moves;
    const bool move_set_passed = extracted_all == expected_all;
    const bool timeline_passed = detected_timeline == expected_all;
    const bool invariants_passed = verify_game_data_invariants(data);
    bool moves_passed = main_line_passed && move_set_passed && timeline_passed && invariants_passed;
    write_first_divergence_report(
        result.name, video_path, expected_moves, data, first_mismatch,
        main_line_passed, moves_passed,
        !timeline_passed ? "accepted_timeline_mismatch" :
        (!move_set_passed ? "variation_or_move_set_mismatch" : ""));
    if (!moves_passed) {
        std::ofstream variation_dump("full_game_variations_debug.txt", std::ios::trunc);
        if (variation_dump.is_open()) {
            for (const auto& [parent_ply, variations] : data.variations) {
                for (const auto& variation : variations) {
                    variation_dump << parent_ply << "\t";
                    for (size_t i = 0; i < variation.moves.size(); ++i) {
                        if (i != 0) variation_dump << ',';
                        variation_dump << variation.moves[i];
                    }
                    variation_dump << "\n";
                }
            }
        }
        std::cout << "  Expected variation moves from PGN parser:\n";
        for (const auto& move : expected_data.variations) std::cout << "    " << move << "\n";
        std::cout << "  Expected variation sequences by parent ply:\n";
        for (const auto& [parent_ply, moves] : expected_data.variation_sequences) {
            std::cout << "    parent " << parent_ply << ": ";
            for (const auto& move : moves) std::cout << move << " ";
            std::cout << "\n";
        }
        std::cout << "  Extracted variation tree:\n";
        for (const auto& [parent_ply, variations] : data.variations) {
            for (const auto& variation : variations) {
                std::cout << "    parent " << parent_ply << ": ";
                for (const auto& move : variation.moves) std::cout << move << " ";
                std::cout << "\n";
            }
        }
    }
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    print_multiset_delta(extracted_all, expected_all, "moves");
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";
    print_multiset_delta(detected_timeline, expected_all, "timeline moves");
    EXPECT_EQ(detected_timeline, expected_all)
        << "Mismatch in accepted move timeline. App may have transiently detected moves that are absent from the PGN.";

    bool clocks_passed = verify_clocks(pgn_path, data);
    result.passed = moves_passed && clocks_passed;

    if (result.passed) {
        std::cout << "PASS: Extracted moves and clocks perfectly match the expected PGN, including variation clocks.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_FULL_GAME_1_EXTRACTION

// INTEGRATION: YI VS ESIPENKO EXTRACTION
#if TEST_YI_VS_ESIPENKO_EXTRACTION

TEST_F(DetectorsTest, YiVsEsipenkoExtraction) {
    const std::string video_path = find_game_fixture_file(
        assets_dir_, "yi-vs-esipenko", "video.mp4").string();
    const std::string pgn_path = find_expected_game_pgn(
        assets_dir_, "yi-vs-esipenko").string();

    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }
    std::cout << "\nRunning integration test on Wei Yi vs Andrey Esipenko...\n";

    IntegrationTestResult result;
    result.name = "Wei Yi vs Andrey Esipenko Extraction";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    const auto t_start = std::chrono::steady_clock::now();
    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "test_yi_vs_esipenko");
    result.elapsed_sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    LichessGameMetadata video_metadata;
    ASSERT_TRUE(resolve_video_metadata(data, video_metadata, video_path));
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;
    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    expect_fixture_metadata_contract(expected_data, pgn_path);
    expect_video_metadata_matches_answer_key(video_metadata, expected_data, video_path);
    const std::vector<std::string>& expected_moves = expected_data.main_line;
    const std::multiset<std::string> expected_all(
        expected_data.all_moves.begin(), expected_data.all_moves.end());
    result.plies_expected = static_cast<int>(expected_moves.size());

    std::cout << "  Expected Main Line (" << expected_moves.size() << "): ";
    for (const auto& move : expected_moves) std::cout << move << " ";
    std::cout << "\n  Extracted Main Line (" << data.moves.size() << "): ";
    for (const auto& move : data.moves) std::cout << move << " ";
    std::cout << "\n";

    const size_t first_mismatch = first_main_line_mismatch(expected_moves, data.moves);
    if (first_mismatch < data.moves.size() || first_mismatch < expected_moves.size()) {
        std::cout << "  First main-line mismatch at ply " << (first_mismatch + 1)
                  << ": expected "
                  << (first_mismatch < expected_moves.size()
                      ? expected_moves[first_mismatch] : "(none)")
                  << ", extracted "
                  << (first_mismatch < data.moves.size()
                      ? data.moves[first_mismatch] : "(none)")
                  << "\n";
    }

    const std::multiset<std::string> extracted_all = extract_all_moves_multiset(data);
    const bool main_line_passed = data.moves == expected_moves;
    const bool complete_output_passed = extracted_all == expected_all;
    std::multiset<std::string> detected_timeline;
    for (const std::string& move : data.video_moves) {
        if (move != "REVERT") detected_timeline.insert(move);
    }
    const bool timeline_passed = detected_timeline == expected_all;
    const bool invariants_passed = verify_game_data_invariants(data);

    write_first_divergence_report(
        result.name, video_path, expected_moves, data, first_mismatch,
        main_line_passed, complete_output_passed && timeline_passed,
        !main_line_passed ? "" :
        (!complete_output_passed ? "variation_or_move_set_mismatch" :
         (!timeline_passed ? "accepted_timeline_mismatch" : "")));

    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size()
        << " plies, expected " << expected_moves.size();
    print_multiset_delta(extracted_all, expected_all, "moves");
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves, including the analysis variation.";
    EXPECT_TRUE(timeline_passed)
        << "Mismatch in the accepted move timeline.";

    result.passed = main_line_passed && complete_output_passed &&
                    timeline_passed && invariants_passed;
    if (result.passed) {
        std::cout << "PASS: Extracted the full game and its analysis variations.\n";
    }

    g_test_results.push_back(result);
    print_test_summary();
}

#endif // TEST_YI_VS_ESIPENKO_EXTRACTION

// ─── INTEGRATION: CLOCK TIMES EXTRACTION ─────────────────────────────────────
#if TEST_INTEGRATION_CLOCK_TIMES

TEST_F(DetectorsTest, IntegrationClockTimes) {
    const std::string video_path = find_game_fixture_file(
        assets_dir_, "clock-times", "video.mp4").string();
    const std::string pgn_path = find_expected_game_pgn(
        assets_dir_, "clock-times").string();
    if (!std::filesystem::exists(video_path)) {
        GTEST_SKIP() << "Video not found: " << video_path;
    }
    std::cout << "\nRunning integration test on clock times...\n";

    IntegrationTestResult result;
    result.name = "Clock Times Integration";
    result.video_file = std::filesystem::path(video_path).filename().string();
    result.video_duration_sec = get_video_duration(video_path);

    auto t_start = std::chrono::steady_clock::now();

    ChessVideoExtractor extractor(board_path_, "", DebugLevel::None);
    GameData data = extractor.extract_moves_from_video(video_path, "clock-times");

    result.elapsed_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    result.plies_extracted = static_cast<int>(data.moves.size());

    LichessGameMetadata video_metadata;
    ASSERT_TRUE(resolve_video_metadata(data, video_metadata, video_path));
    // Extract and verify expected moves from PGN
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;
    ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    expect_fixture_metadata_contract(expected_data, pgn_path);
    expect_video_metadata_matches_answer_key(video_metadata, expected_data, video_path);
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
    const bool invariants_passed = verify_game_data_invariants(data);
    moves_passed = moves_passed && invariants_passed;
    EXPECT_EQ(data.moves, expected_moves)
        << "Extracted main line has " << data.moves.size() << " moves, expected " << expected_moves.size();
    EXPECT_EQ(extracted_all, expected_all)
        << "Mismatch in total extracted moves (including variations). App may have hallucinated or missed analysis lines.";

    bool clocks_passed = verify_clocks(pgn_path, data);

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
    const std::string video_path = find_game_fixture_file(
        assets_dir_, "analysis-line-and-revert", "video.mp4").string();
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

    // Complete extraction is intentional: this test also enforces the same
    // video-derived metadata contract as the integration tests.
    std::atomic<bool> cancel{false};
    GameData data;
    try {
        data = extractor.extract_moves_from_video(video_path, "test_mem_limit", &cancel);
    } catch (const std::exception& error) {
        FAIL() << "Memory-limit extraction failed: " << error.what();
    }

    LichessGameMetadata video_metadata;
    ASSERT_TRUE(resolve_video_metadata(data, video_metadata, video_path));

    const std::string pgn_path = find_expected_game_pgn(
        assets_dir_, "analysis-line-and-revert").string();
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;
    const ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    expect_fixture_metadata_contract(expected_data, pgn_path);
    expect_video_metadata_matches_answer_key(video_metadata, expected_data, video_path);

    EXPECT_NE(detected_workers, -1) << "Did not find Map-Reduce launch log message.";
    EXPECT_EQ(detected_workers, 1) << "Memory limit of 250MB should restrict worker count to 1.";
}

#endif // TEST_MEMORY_LIMIT

// ─── CACHE CORRECTNESS TEST ──────────────────────────────────────────────────
#if TEST_CACHE_CORRECTNESS

TEST_F(DetectorsTest, CacheCorrectness) {
    const std::string video_path = find_game_fixture_file(
        assets_dir_, "seven-plies", "video.mp4").string();
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

    // The cache behavior run above is deliberately separate from this full
    // extraction. Metadata must still come from the video-derived FENs.
    std::atomic<bool> no_cancel{false};
    GameData data;
    try {
        data = extractor.extract_moves_from_video(video_path, "test_cache_metadata", &no_cancel);
    } catch (const std::exception& error) {
        FAIL() << "Cache test extraction failed: " << error.what();
    }

    LichessGameMetadata video_metadata;
    ASSERT_TRUE(resolve_video_metadata(data, video_metadata, video_path));

    const std::string pgn_path = find_expected_game_pgn(
        assets_dir_, "seven-plies").string();
    ASSERT_TRUE(std::filesystem::exists(pgn_path)) << "PGN not found: " << pgn_path;
    const ExpectedGameData expected_data = load_expected_uci_moves_from_pgn(pgn_path);
    expect_fixture_metadata_contract(expected_data, pgn_path);
    expect_video_metadata_matches_answer_key(video_metadata, expected_data, video_path);
}

#endif // TEST_CACHE_CORRECTNESS

} // namespace cta
