#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace cta::diagnostics {

// These categories are intentionally independent of the extractor's current
// event names. They provide a stable vocabulary for tools that consume
// diagnostic output while the reducer continues to evolve.
enum class Phase {
    Unknown,
    Mapper,
    Detector,
    Scoring,
    Validation,
    Reducer,
    Clock,
    Revert,
    Invariant,
};

enum class Outcome {
    Informational,
    Observed,
    Accepted,
    Rejected,
    Deferred,
    Ambiguous,
    Recovered,
};

enum class Reason {
    Unknown,
    QuietFrame,
    CandidateObserved,
    MoveAccepted,
    ValidationRejected,
    FrameRejected,
    FrameCoalesced,
    CandidateHeldForSettling,
    CandidateAmbiguous,
    ScoreThreshold,
    ClockObservation,
    RevertSearch,
    RevertApplied,
    StateHandoff,
    VariationUpdate,
    LegacyEvent,
};

struct CandidateScore {
    std::string move;
    double score = 0.0;
    std::size_t rank = 0;
};

struct SquareScore {
    std::string square;
    double score = 0.0;
    std::size_t rank = 0;
};

struct YellowMeasurement {
    std::string square;
    std::array<double, 4> corner_scores{};
    std::array<std::array<double, 3>, 4> corner_bgr{};
    std::array<double, 4> corner_edge_density{};
    double score = 0.0;
    double geometry_uncertainty = 1.0;
};

// Common detector-result envelope. Confidence remains -1 until that detector
// has a calibrated probability model; raw measurements and uncertainty are
// still emitted so an uncalibrated score is never mistaken for certainty.
struct DetectorAssessment {
    std::string state = "not_checked";
    double confidence = -1.0;
    std::vector<double> thresholds;
    std::vector<std::pair<std::string, double>> measurements;
    std::string uncertainty_reason;
};

struct HoverMeasurement {
    std::string square;
    double top_edge = 0.0;
    double bottom_edge = 0.0;
    double left_edge = 0.0;
    double right_edge = 0.0;
    double strongest_edge = 0.0;
    std::size_t visible_edges = 0;
    bool detected = false;
    double geometry_uncertainty = 1.0;
};

struct Evidence {
    std::uint32_t mapper_chunk = 0;
    std::uint64_t source_frame_index = 0;
    std::string mapper_emission_reason;
    std::string diagnostic_frame_path;
    std::string diagnostic_board_path;
    std::string diagnostic_clock_top_path;
    std::string diagnostic_clock_bottom_path;
    // Stable labels describing the evidence state of the mapped observation.
    // These are additive diagnostics; they never select or reject a move.
    std::vector<std::string> observation_tags;
    bool yellow_arrows_checked = false;
    std::vector<std::string> yellow_arrows;
    bool red_squares_checked = false;
    std::vector<std::string> red_squares;
    std::uint64_t template_identity = 0;
    int board_x = 0;
    int board_y = 0;
    int board_width = 0;
    int board_height = 0;
    double square_width = 0.0;
    double square_height = 0.0;
    double localization_score = 0.0;
    double localization_scale = 0.0;
    double localization_confidence = 0.0;
    double geometry_uncertainty = 1.0;
    std::vector<double> board_hash;
    bool geometry_checked = false;
    bool geometry_anomaly = false;
    double geometry_drift_x = 0.0;
    double geometry_drift_y = 0.0;
    double geometry_size_drift = 0.0;
    double geometry_step_drift_x = 0.0;
    double geometry_step_drift_y = 0.0;
    double geometry_step_size_drift = 0.0;
    double geometry_relocalization_score = -1.0;
    std::string geometry_decision;
    std::size_t changed_square_count = 0;
    std::vector<SquareScore> changed_squares;
    double score_from_square_diff = 0.0;
    double score_to_square_diff = 0.0;
    double score_adjustment = 0.0;
    double score_margin = 0.0;
    double minimum_score_threshold = 0.0;
    bool score_threshold_checked = false;
    bool score_threshold_passed = false;
    std::string score_threshold_decision;
    double yellow_from = 0.0;
    double yellow_to = 0.0;
    double yellow_endpoint_threshold = 0.0;
    double yellow_pair_threshold = 0.0;
    bool yellow_checked = false;
    std::string yellow_decision;
    std::vector<SquareScore> yellow_candidates;
    std::vector<YellowMeasurement> yellow_measurements;
    bool yellow_temporal_checked = false;
    double yellow_temporal_window_seconds = 0.0;
    std::size_t yellow_temporal_sample_count = 0;
    std::size_t yellow_temporal_pair_pass_count = 0;
    double yellow_temporal_max_from = 0.0;
    double yellow_temporal_max_to = 0.0;
    double yellow_temporal_max_pair = 0.0;
    DetectorAssessment yellow_assessment;
    DetectorAssessment hover_assessment;
    DetectorAssessment clock_assessment;
    DetectorAssessment geometry_assessment;
    bool hover_checked = false;
    bool hover_detected = false;
    std::string hover_decision;
    std::vector<HoverMeasurement> hover_measurements;
    bool clock_checked = false;
    bool clock_ocr_skipped = false;
    int clock_top_width = 0;
    int clock_top_height = 0;
    int clock_bottom_width = 0;
    int clock_bottom_height = 0;
    double clock_top_bright_ratio = 0.0;
    double clock_bottom_bright_ratio = 0.0;
    double clock_bright_ratio_delta = 0.0;
    std::string clock_decision;
    bool clock_temporal_checked = false;
    std::size_t clock_temporal_sample_count = 0;
    std::size_t clock_temporal_observed_count = 0;
    std::size_t clock_temporal_agreement_count = 0;
    std::string clock_temporal_decision;
    std::string clock_provenance;
    std::size_t clock_temporal_plausible_count = 0;
    bool clock_temporal_reconciled = false;
    std::string settle_decision;
    std::string active_clock_player;
    std::string moved_clock;
    std::string previous_moved_clock;
    std::vector<std::string> clock_candidates;
    std::vector<CandidateScore> legal_candidates;
    std::string rejection_reason;
};

struct Record {
    std::uint64_t sequence = 0;
    std::uint64_t observation_id = 0;
    std::uint64_t candidate_id = 0;
    std::uint64_t transition_id = 0;
    std::uint64_t state_generation = 0;
    std::uint64_t revert_generation = 0;
    std::uint64_t branch_id = 0;
    double timestamp = 0.0;
    std::size_t active_ply = 0;
    std::string event;
    std::string fen;
    std::string best_move;
    double best_score = 0.0;
    double max_square_diff = 0.0;
    double from_yellowness = 0.0;
    double to_yellowness = 0.0;
    std::string metadata;
    Phase phase = Phase::Unknown;
    Outcome outcome = Outcome::Informational;
    Reason reason = Reason::Unknown;
    std::string reducer_state;
    Evidence evidence;
};

const char* to_string(Phase phase);
const char* to_string(Outcome outcome);
const char* to_string(Reason reason);

// Classifies existing reducer event names without changing their behavior.
// This keeps the first diagnostic layer useful while future instrumentation
// migrates callers to structured records directly.
Record from_legacy_trace(std::uint64_t sequence,
                         const char* event,
                         double timestamp,
                         std::size_t active_ply,
                         const std::string& fen,
                         const std::string& best_move,
                         double best_score,
                         double max_square_diff,
                         double from_yellowness,
                         double to_yellowness,
                         const std::string& metadata,
                         std::uint64_t observation_id = 0,
                         std::uint64_t transition_id = 0,
                         const Evidence& evidence = {},
                         std::uint64_t candidate_id = 0,
                         std::uint64_t state_generation = 0,
                         std::uint64_t revert_generation = 0,
                         std::uint64_t branch_id = 0,
                         const std::string& reducer_state = {});

// Writes one JSON object followed by a newline. The format is JSON Lines so
// bounded traces can be streamed without keeping the whole run in memory.
void write_json_line(std::ostream& output, const Record& record);

} // namespace cta::diagnostics
