#pragma once
#include <opencv2/opencv.hpp>
#include <array>
#include <cstddef>
#include "UIDetectors.h" // Ensures BoardGeometry is defined

namespace cta {
namespace validation {

enum class EvidenceStrength {
    Strong,
    Weak,
    Advisory,
    Missing,
    Conflicting,
};

const char* to_string(EvidenceStrength strength);

// These classifiers describe what the current measured evidence can justify;
// they do not select a move. Direct detector thresholds remain advisory until
// calibration supports a stronger production claim.
EvidenceStrength classify_yellow_evidence(bool accepted_inverse,
                                           bool direct_pass,
                                           bool temporal_pass,
                                           bool absent);

EvidenceStrength classify_clock_evidence(bool temporal_reconciled,
                                         bool contextual_reconciled,
                                         bool direct_observed,
                                         bool missing);

inline constexpr double kYellowEndpointThreshold = 25.0;
inline constexpr double kYellowPairThreshold = 70.0;
inline constexpr double kYellowTemporalWindowSeconds = 0.75;
inline constexpr std::size_t kYellowTemporalMinimumSamples = 2;
inline constexpr std::size_t kYellowTemporalMinimumPairPasses = 2;
inline constexpr std::size_t kClockVetoMinimumTemporalSamples = 2;
inline constexpr std::size_t kClockVetoMinimumObservedReadings = 2;
inline constexpr std::size_t kClockVetoMinimumAgreements = 2;

struct YellowTemporalEvidence {
    std::size_t sample_count = 0;
    std::size_t pair_pass_count = 0;
    double max_from = 0.0;
    double max_to = 0.0;
    double max_pair = 0.0;
};

// Temporal acceptance requires repeated complete endpoint evidence. A single
// bright frame remains insufficient because transient UI animation can mimic
// a highlighted move.
bool passes_temporal_yellow_check(const YellowTemporalEvidence& evidence);

struct ClockVetoEvidence {
    bool direct_reading_plausible = false;
    bool temporal_checked = false;
    std::size_t temporal_sample_count = 0;
    std::size_t temporal_observed_count = 0;
    std::size_t temporal_agreement_count = 0;
};

// Clock OCR is advisory until it has both a plausible direct reading and
// repeated, agreeing settled observations. This gate prevents an uncalibrated
// or single-frame OCR anomaly from rejecting an otherwise legal visual move.
bool passes_clock_veto_reliability_gate(const ClockVetoEvidence& evidence);

struct YellowSquareMeasurement {
    std::array<double, 4> corner_scores{};
    std::array<std::array<double, 3>, 4> corner_bgr{};
    std::array<double, 4> corner_edge_density{};
    double score = 0.0;
    double geometry_uncertainty = 1.0;
};

struct HoverBoxMeasurement {
    double top_edge = 0.0;
    double bottom_edge = 0.0;
    double left_edge = 0.0;
    double right_edge = 0.0;
    double strongest_edge = 0.0;
    std::size_t visible_edges = 0;
    bool detected = false;
    double geometry_uncertainty = 1.0;
};

double check_yellowness(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* sq_name);

YellowSquareMeasurement measure_yellowness(const cv::Mat& board_bgr,
                                           const BoardGeometry& geo,
                                           const char* sq_name,
                                           bool include_edge_metrics = false,
                                           double corner_fraction = 0.12);

HoverBoxMeasurement measure_hover_box(const cv::Mat& board_bgr,
                                      const BoardGeometry& geo,
                                      cv::Mat& white_mask,
                                      cv::Mat& reduced,
                                      const char* sq_name);

bool check_hover_box(const cv::Mat& board_bgr, const BoardGeometry& geo, cv::Mat& white_mask, cv::Mat& reduced, const char* sq_name);

} // namespace validation
} // namespace cta
