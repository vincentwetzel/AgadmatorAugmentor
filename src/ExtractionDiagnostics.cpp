#include "ExtractionDiagnostics.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <string_view>

namespace cta::diagnostics {

namespace {

bool is_event(const char* event, std::string_view wanted) {
    return event != nullptr && wanted == event;
}

bool has_prefix(const char* event, std::string_view prefix) {
    if (event == nullptr) return false;
    const std::string_view value(event);
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

void classify(const char* event, Phase& phase, Outcome& outcome, Reason& reason) {
    if (is_event(event, "QUIET")) {
        phase = Phase::Detector;
        outcome = Outcome::Observed;
        reason = Reason::QuietFrame;
    } else if (is_event(event, "CANDIDATE")) {
        phase = Phase::Scoring;
        outcome = Outcome::Observed;
        reason = Reason::CandidateObserved;
    } else if (is_event(event, "ACCEPT")) {
        phase = Phase::Reducer;
        outcome = Outcome::Accepted;
        reason = Reason::MoveAccepted;
    } else if (is_event(event, "VALIDATION_REJECTED")) {
        phase = Phase::Validation;
        outcome = Outcome::Rejected;
        reason = Reason::ValidationRejected;
    } else if (is_event(event, "REJECTED_FRAME")) {
        phase = Phase::Validation;
        outcome = Outcome::Rejected;
        reason = Reason::FrameRejected;
    } else if (is_event(event, "COALESCED_STOP")) {
        phase = Phase::Mapper;
        outcome = Outcome::Deferred;
        reason = Reason::FrameCoalesced;
    } else if (has_prefix(event, "CLOCK_")) {
        phase = Phase::Clock;
        outcome = Outcome::Observed;
        reason = Reason::ClockObservation;
    } else if (is_event(event, "REVERT_SEARCH") || is_event(event, "REVERT_NEAREST") ||
               has_prefix(event, "HISTORICAL")) {
        phase = Phase::Revert;
        outcome = Outcome::Observed;
        reason = Reason::RevertSearch;
    } else if (is_event(event, "REVERT_APPLIED") ||
               is_event(event, "PRESERVED_MAINLINE_RESTORED") ||
               is_event(event, "HISTORICAL_HANDOFF") ||
               is_event(event, "REPEATED_BRANCH_HANDOFF") ||
               is_event(event, "HANDOFF_RESULT") ||
               is_event(event, "REBASED_CONTINUATION")) {
        phase = Phase::Revert;
        outcome = Outcome::Recovered;
        reason = Reason::RevertApplied;
    } else if (has_prefix(event, "HANDOFF_") || has_prefix(event, "REBASE_")) {
        phase = Phase::Reducer;
        outcome = Outcome::Observed;
        reason = Reason::StateHandoff;
    } else if (has_prefix(event, "VARIATION") || is_event(event, "FINAL_VARIATION")) {
        phase = Phase::Reducer;
        outcome = Outcome::Recovered;
        reason = Reason::VariationUpdate;
    } else if (event != nullptr && *event != '\0') {
        phase = Phase::Reducer;
        outcome = Outcome::Informational;
        reason = Reason::LegacyEvent;
    }
}

} // namespace

const char* to_string(Phase phase) {
    switch (phase) {
    case Phase::Mapper: return "mapper";
    case Phase::Detector: return "detector";
    case Phase::Scoring: return "scoring";
    case Phase::Validation: return "validation";
    case Phase::Reducer: return "reducer";
    case Phase::Clock: return "clock";
    case Phase::Revert: return "revert";
    case Phase::Invariant: return "invariant";
    case Phase::Unknown: return "unknown";
    }
    return "unknown";
}

const char* to_string(Outcome outcome) {
    switch (outcome) {
    case Outcome::Informational: return "informational";
    case Outcome::Observed: return "observed";
    case Outcome::Accepted: return "accepted";
    case Outcome::Rejected: return "rejected";
    case Outcome::Deferred: return "deferred";
    case Outcome::Recovered: return "recovered";
    }
    return "informational";
}

const char* to_string(Reason reason) {
    switch (reason) {
    case Reason::QuietFrame: return "quiet_frame";
    case Reason::CandidateObserved: return "candidate_observed";
    case Reason::MoveAccepted: return "move_accepted";
    case Reason::ValidationRejected: return "validation_rejected";
    case Reason::FrameRejected: return "frame_rejected";
    case Reason::FrameCoalesced: return "frame_coalesced";
    case Reason::ClockObservation: return "clock_observation";
    case Reason::RevertSearch: return "revert_search";
    case Reason::RevertApplied: return "revert_applied";
    case Reason::StateHandoff: return "state_handoff";
    case Reason::VariationUpdate: return "variation_update";
    case Reason::LegacyEvent: return "legacy_event";
    case Reason::Unknown: return "unknown";
    }
    return "unknown";
}

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
                         std::uint64_t observation_id,
                         std::uint64_t transition_id,
                         const Evidence& evidence,
                         std::uint64_t candidate_id,
                         std::uint64_t state_generation,
                         std::uint64_t revert_generation,
                         std::uint64_t branch_id,
                         const std::string& reducer_state) {
    Record record;
    record.sequence = sequence;
    record.observation_id = observation_id;
    record.candidate_id = candidate_id;
    record.transition_id = transition_id;
    record.state_generation = state_generation;
    record.revert_generation = revert_generation;
    record.branch_id = branch_id;
    record.timestamp = timestamp;
    record.active_ply = active_ply;
    record.event = event != nullptr ? event : "";
    record.fen = fen;
    record.best_move = best_move;
    record.best_score = best_score;
    record.max_square_diff = max_square_diff;
    record.from_yellowness = from_yellowness;
    record.to_yellowness = to_yellowness;
    record.metadata = metadata;
    record.reducer_state = reducer_state;
    record.evidence = evidence;
    classify(event, record.phase, record.outcome, record.reason);
    return record;
}

void write_json_line(std::ostream& output, const Record& record) {
    const auto assessment_json = [](const DetectorAssessment& assessment) {
        nlohmann::json measurements = nlohmann::json::object();
        for (const auto& [name, value] : assessment.measurements) {
            measurements[name] = value;
        }
        return nlohmann::json{
            {"state", assessment.state},
            {"confidence", assessment.confidence},
            {"thresholds", assessment.thresholds},
            {"measurements", measurements},
            {"uncertainty_reason", assessment.uncertainty_reason},
        };
    };

    nlohmann::json json = {
        {"schema_version", 1},
        {"sequence", record.sequence},
        {"observation_id", record.observation_id},
        {"candidate_id", record.candidate_id},
        {"transition_id", record.transition_id},
        {"state_generation", record.state_generation},
        {"revert_generation", record.revert_generation},
        {"branch_id", record.branch_id},
        {"timestamp", record.timestamp},
        {"active_ply", record.active_ply},
        {"event", record.event},
        {"phase", to_string(record.phase)},
        {"outcome", to_string(record.outcome)},
        {"reason", to_string(record.reason)},
        {"reducer_state", record.reducer_state},
        {"fen", record.fen},
        {"best_move", record.best_move},
        {"best_score", record.best_score},
        {"max_square_diff", record.max_square_diff},
        {"from_yellowness", record.from_yellowness},
        {"to_yellowness", record.to_yellowness},
        {"metadata", record.metadata},
        {"evidence", {
            {"mapper_chunk", record.evidence.mapper_chunk},
            {"source_frame_index", record.evidence.source_frame_index},
            {"mapper_emission_reason", record.evidence.mapper_emission_reason},
            {"diagnostic_frame_path", record.evidence.diagnostic_frame_path},
            {"diagnostic_board_path", record.evidence.diagnostic_board_path},
            {"diagnostic_clock_top_path", record.evidence.diagnostic_clock_top_path},
            {"diagnostic_clock_bottom_path", record.evidence.diagnostic_clock_bottom_path},
            {"observation_tags", record.evidence.observation_tags},
            {"yellow_arrows_checked", record.evidence.yellow_arrows_checked},
            {"yellow_arrows", record.evidence.yellow_arrows},
            {"red_squares_checked", record.evidence.red_squares_checked},
            {"red_squares", record.evidence.red_squares},
            {"template_identity", record.evidence.template_identity},
            {"board_x", record.evidence.board_x},
            {"board_y", record.evidence.board_y},
            {"board_width", record.evidence.board_width},
            {"board_height", record.evidence.board_height},
            {"square_width", record.evidence.square_width},
            {"square_height", record.evidence.square_height},
            {"localization_score", record.evidence.localization_score},
            {"localization_scale", record.evidence.localization_scale},
            {"board_hash", record.evidence.board_hash},
            {"geometry_checked", record.evidence.geometry_checked},
            {"geometry_anomaly", record.evidence.geometry_anomaly},
            {"geometry_drift_x", record.evidence.geometry_drift_x},
            {"geometry_drift_y", record.evidence.geometry_drift_y},
            {"geometry_size_drift", record.evidence.geometry_size_drift},
            {"geometry_step_drift_x", record.evidence.geometry_step_drift_x},
            {"geometry_step_drift_y", record.evidence.geometry_step_drift_y},
            {"geometry_step_size_drift", record.evidence.geometry_step_size_drift},
            {"geometry_relocalization_score", record.evidence.geometry_relocalization_score},
            {"geometry_decision", record.evidence.geometry_decision},
            {"changed_square_count", record.evidence.changed_square_count},
            {"changed_squares", [&record] {
                nlohmann::json squares = nlohmann::json::array();
                for (const auto& square : record.evidence.changed_squares) {
                    squares.push_back({
                        {"rank", square.rank},
                        {"square", square.square},
                        {"score", square.score},
                    });
                }
                return squares;
            }()},
            {"score_from_square_diff", record.evidence.score_from_square_diff},
            {"score_to_square_diff", record.evidence.score_to_square_diff},
            {"score_adjustment", record.evidence.score_adjustment},
            {"score_margin", record.evidence.score_margin},
            {"minimum_score_threshold", record.evidence.minimum_score_threshold},
            {"score_threshold_checked", record.evidence.score_threshold_checked},
            {"score_threshold_passed", record.evidence.score_threshold_passed},
            {"score_threshold_decision", record.evidence.score_threshold_decision},
            {"yellow_from", record.evidence.yellow_from},
            {"yellow_to", record.evidence.yellow_to},
            {"yellow_endpoint_threshold", record.evidence.yellow_endpoint_threshold},
            {"yellow_pair_threshold", record.evidence.yellow_pair_threshold},
            {"yellow_checked", record.evidence.yellow_checked},
            {"yellow_decision", record.evidence.yellow_decision},
            {"yellow_candidates", [&record] {
                nlohmann::json squares = nlohmann::json::array();
                for (const auto& square : record.evidence.yellow_candidates) {
                    squares.push_back({
                        {"rank", square.rank},
                        {"square", square.square},
                        {"score", square.score},
                    });
                }
                return squares;
            }()},
            {"yellow_measurements", [&record] {
                nlohmann::json measurements = nlohmann::json::array();
                for (const auto& measurement : record.evidence.yellow_measurements) {
                    measurements.push_back({
                        {"square", measurement.square},
                        {"corner_scores", measurement.corner_scores},
                        {"corner_bgr", measurement.corner_bgr},
                        {"corner_edge_density", measurement.corner_edge_density},
                        {"score", measurement.score},
                    });
                }
                return measurements;
            }()},
            {"yellow_temporal_checked", record.evidence.yellow_temporal_checked},
            {"yellow_temporal_window_seconds", record.evidence.yellow_temporal_window_seconds},
            {"yellow_temporal_sample_count", record.evidence.yellow_temporal_sample_count},
            {"yellow_temporal_pair_pass_count", record.evidence.yellow_temporal_pair_pass_count},
            {"yellow_temporal_max_from", record.evidence.yellow_temporal_max_from},
            {"yellow_temporal_max_to", record.evidence.yellow_temporal_max_to},
            {"yellow_temporal_max_pair", record.evidence.yellow_temporal_max_pair},
            {"yellow_assessment", assessment_json(record.evidence.yellow_assessment)},
            {"hover_assessment", assessment_json(record.evidence.hover_assessment)},
            {"clock_assessment", assessment_json(record.evidence.clock_assessment)},
            {"geometry_assessment", assessment_json(record.evidence.geometry_assessment)},
            {"hover_checked", record.evidence.hover_checked},
            {"hover_detected", record.evidence.hover_detected},
            {"hover_decision", record.evidence.hover_decision},
            {"hover_measurements", [&record] {
                nlohmann::json measurements = nlohmann::json::array();
                for (const auto& measurement : record.evidence.hover_measurements) {
                    measurements.push_back({
                        {"square", measurement.square},
                        {"top_edge", measurement.top_edge},
                        {"bottom_edge", measurement.bottom_edge},
                        {"left_edge", measurement.left_edge},
                        {"right_edge", measurement.right_edge},
                        {"strongest_edge", measurement.strongest_edge},
                        {"visible_edges", measurement.visible_edges},
                        {"detected", measurement.detected},
                    });
                }
                return measurements;
            }()},
            {"clock_checked", record.evidence.clock_checked},
            {"clock_ocr_skipped", record.evidence.clock_ocr_skipped},
            {"clock_top_width", record.evidence.clock_top_width},
            {"clock_top_height", record.evidence.clock_top_height},
            {"clock_bottom_width", record.evidence.clock_bottom_width},
            {"clock_bottom_height", record.evidence.clock_bottom_height},
            {"clock_top_bright_ratio", record.evidence.clock_top_bright_ratio},
            {"clock_bottom_bright_ratio", record.evidence.clock_bottom_bright_ratio},
            {"clock_bright_ratio_delta", record.evidence.clock_bright_ratio_delta},
            {"clock_decision", record.evidence.clock_decision},
            {"settle_decision", record.evidence.settle_decision},
            {"active_clock_player", record.evidence.active_clock_player},
            {"moved_clock", record.evidence.moved_clock},
            {"previous_moved_clock", record.evidence.previous_moved_clock},
            {"clock_candidates", record.evidence.clock_candidates},
            {"legal_candidates", [&record] {
                nlohmann::json candidates = nlohmann::json::array();
                for (const auto& candidate : record.evidence.legal_candidates) {
                    candidates.push_back({
                        {"rank", candidate.rank},
                        {"move", candidate.move},
                        {"score", candidate.score},
                    });
                }
                return candidates;
            }()},
            {"rejection_reason", record.evidence.rejection_reason},
        }},
    };
    output << json.dump() << '\n';
}

} // namespace cta::diagnostics
