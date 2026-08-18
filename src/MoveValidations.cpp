#include "MoveValidations.h"
#include "BoardLocalizer.h"
#include <algorithm>

namespace cta {
namespace validation {

const char* to_string(EvidenceStrength strength) {
    switch (strength) {
    case EvidenceStrength::Strong: return "strong";
    case EvidenceStrength::Weak: return "weak";
    case EvidenceStrength::Advisory: return "advisory";
    case EvidenceStrength::Missing: return "missing";
    case EvidenceStrength::Conflicting: return "conflicting";
    }
    return "conflicting";
}

EvidenceStrength classify_yellow_evidence(bool accepted_inverse,
                                          bool direct_pass,
                                          bool temporal_pass,
                                          bool absent) {
    if (accepted_inverse || direct_pass) return EvidenceStrength::Advisory;
    if (temporal_pass) return EvidenceStrength::Weak;
    if (absent) return EvidenceStrength::Missing;
    return EvidenceStrength::Conflicting;
}

EvidenceStrength classify_clock_evidence(bool temporal_reconciled,
                                         bool contextual_reconciled,
                                         bool direct_observed,
                                         bool missing) {
    if (temporal_reconciled) return EvidenceStrength::Strong;
    if (contextual_reconciled || direct_observed) return EvidenceStrength::Advisory;
    if (missing) return EvidenceStrength::Missing;
    return EvidenceStrength::Conflicting;
}

bool passes_temporal_yellow_check(const YellowTemporalEvidence& evidence) {
    return evidence.sample_count >= kYellowTemporalMinimumSamples &&
        evidence.pair_pass_count >= kYellowTemporalMinimumPairPasses &&
        evidence.max_from >= kYellowEndpointThreshold &&
        evidence.max_to >= kYellowEndpointThreshold &&
        evidence.max_pair >= kYellowPairThreshold;
}

bool passes_clock_veto_reliability_gate(const ClockVetoEvidence& evidence) {
    return evidence.direct_reading_plausible && evidence.temporal_checked &&
        evidence.temporal_sample_count >= kClockVetoMinimumTemporalSamples &&
        evidence.temporal_observed_count >= kClockVetoMinimumObservedReadings &&
        evidence.temporal_agreement_count >= kClockVetoMinimumAgreements;
}

YellowSquareMeasurement measure_yellowness(const cv::Mat& board_bgr,
                                           const BoardGeometry& geo,
                                           const char* sq_name,
                                           bool include_edge_metrics,
                                           double corner_fraction) {
    int col = sq_name[0] - 'a';
    int rank = sq_name[1] - '1';
    int row = 7 - rank;
    int y1 = static_cast<int>(row * geo.sq_h);
    int y2 = static_cast<int>((row + 1) * geo.sq_h);
    int x1 = static_cast<int>(col * geo.sq_w);
    int x2 = static_cast<int>((col + 1) * geo.sq_w);
    const double clamped_corner_fraction = std::clamp(corner_fraction, 0.02, 0.45);
    int ch = static_cast<int>(geo.sq_h * clamped_corner_fraction);
    int cw = static_cast<int>(geo.sq_w * clamped_corner_fraction);

    // Clamp to frame bounds
    int fh = board_bgr.rows, fw = board_bgr.cols;
    x1 = std::max(0, std::min(x1, fw - 1));
    y1 = std::max(0, std::min(y1, fh - 1));
    x2 = std::max(x1 + 1, std::min(x2, fw));
    y2 = std::max(y1 + 1, std::min(y2, fh));

    cv::Rect corners[4] = {
        {x1, y1, std::min(cw, x2 - x1), std::min(ch, y2 - y1)},
        {std::max(x1, x2 - cw), y1, std::min(cw, x2 - std::max(x1, x2 - cw)), std::min(ch, y2 - y1)},
        {x1, std::max(y1, y2 - ch), std::min(cw, x2 - x1), std::min(ch, y2 - std::max(y1, y2 - ch))},
        {std::max(x1, x2 - cw), std::max(y1, y2 - ch), std::min(cw, x2 - std::max(x1, x2 - cw)), std::min(ch, y2 - std::max(y1, y2 - ch))}
    };

    YellowSquareMeasurement measurement;
    measurement.geometry_uncertainty = geometry_uncertainty(geo);
    for (size_t corner_index = 0; corner_index < 4; ++corner_index) {
        const auto& c = corners[corner_index];
        if (c.width <= 0 || c.height <= 0) continue;
        cv::Mat patch = board_bgr(c);
        double sum_y = 0.0;
        double sum_b = 0.0;
        double sum_g = 0.0;
        double sum_r = 0.0;
        for (int r = 0; r < patch.rows; ++r) {
            const auto* ptr = patch.ptr<cv::Vec3b>(r);
            for (int pc = 0; pc < patch.cols; ++pc) {
                double b = ptr[pc][0];
                double g = ptr[pc][1];
                double red = ptr[pc][2];
                sum_y += std::min(red, g) - b;
                sum_b += b;
                sum_g += g;
                sum_r += red;
            }
        }
        const double pixel_count = static_cast<double>(patch.rows * patch.cols);
        measurement.corner_scores[corner_index] = sum_y / pixel_count;
        measurement.corner_bgr[corner_index] = {
            sum_b / pixel_count, sum_g / pixel_count, sum_r / pixel_count};
        if (include_edge_metrics) {
            cv::Mat gray;
            cv::Mat edges;
            cv::cvtColor(patch, gray, cv::COLOR_BGR2GRAY);
            cv::Canny(gray, edges, 50.0, 150.0);
            measurement.corner_edge_density[corner_index] =
                static_cast<double>(cv::countNonZero(edges)) / pixel_count;
        }
    }
    measurement.score = (measurement.corner_scores[0] + measurement.corner_scores[1] +
                         measurement.corner_scores[2] + measurement.corner_scores[3]) / 4.0;
    return measurement;
}

double check_yellowness(const cv::Mat& board_bgr, const BoardGeometry& geo, const char* sq_name) {
    return measure_yellowness(board_bgr, geo, sq_name).score;
}

HoverBoxMeasurement measure_hover_box(const cv::Mat& board_bgr,
                                      const BoardGeometry& geo,
                                      cv::Mat& white_mask,
                                      cv::Mat& reduced,
                                      const char* sq_name) {
    int col = sq_name[0] - 'a';
    int rank = sq_name[1] - '1';
    int row = 7 - rank;
    int y1 = static_cast<int>(row * geo.sq_h);
    int y2 = static_cast<int>((row + 1) * geo.sq_h);
    int x1 = static_cast<int>(col * geo.sq_w);
    int x2 = static_cast<int>((col + 1) * geo.sq_w);

    // Clamp to frame bounds
    int fh = board_bgr.rows, fw = board_bgr.cols;
    x1 = std::max(0, std::min(x1, fw - 1));
    y1 = std::max(0, std::min(y1, fh - 1));
    x2 = std::max(x1 + 1, std::min(x2, fw));
    y2 = std::max(y1 + 1, std::min(y2, fh));

    int sw = x2 - x1, sh = y2 - y1;
    if (white_mask.rows < sh || white_mask.cols < sw) {
        white_mask = cv::Mat(sh, sw, CV_8UC1);
    }
    cv::Mat white_mask_roi = white_mask(cv::Rect(0, 0, sw, sh));
    cv::inRange(board_bgr(cv::Rect(x1, y1, sw, sh)), cv::Scalar(160, 160, 160), cv::Scalar(255, 255, 255), white_mask_roi);

    int thickness = std::max(3, static_cast<int>(geo.sq_w * 0.08));
    cv::Mat top = white_mask_roi(cv::Rect(0, 0, sw, thickness));
    cv::Mat bottom = white_mask_roi(cv::Rect(0, sh - thickness, sw, thickness));
    cv::Mat left = white_mask_roi(cv::Rect(0, 0, thickness, sh));
    cv::Mat right = white_mask_roi(cv::Rect(sw - thickness, 0, thickness, sh));

    cv::reduce(top, reduced, 0, cv::REDUCE_MAX);
    double r0 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sw);
    cv::reduce(bottom, reduced, 0, cv::REDUCE_MAX);
    double r1 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sw);
    cv::reduce(left, reduced, 1, cv::REDUCE_MAX);
    double r2 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sh);
    cv::reduce(right, reduced, 1, cv::REDUCE_MAX);
    double r3 = static_cast<double>(cv::countNonZero(reduced)) / std::max(1, sh);

    HoverBoxMeasurement measurement;
    measurement.geometry_uncertainty = geometry_uncertainty(geo);
    measurement.top_edge = r0;
    measurement.bottom_edge = r1;
    measurement.left_edge = r2;
    measurement.right_edge = r3;
    measurement.visible_edges = static_cast<std::size_t>(
        (r0 > 0.10) + (r1 > 0.10) + (r2 > 0.10) + (r3 > 0.10));
    measurement.strongest_edge = std::max({r0, r1, r2, r3});
    measurement.detected = measurement.visible_edges >= 2 || measurement.strongest_edge > 0.65;
    return measurement;
}

bool check_hover_box(const cv::Mat& board_bgr, const BoardGeometry& geo, cv::Mat& white_mask, cv::Mat& reduced, const char* sq_name) {
    return measure_hover_box(board_bgr, geo, white_mask, reduced, sq_name).detected;
}

} // namespace validation
} // namespace cta
