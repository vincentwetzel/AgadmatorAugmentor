// Extracted from cpp directory
#include "ChessVideoExtractor.h"
#include "ChessVideoExtractor_Internal.h"
#include "ExtractionDiagnostics.h"
#include "BoardAnalysis.h"
#include "GPUAccelerator.h"
#include "BoardLocalizer.h"
#include "ExtractorUtils.h"
#include "MoveValidations.h"
#include "MoveScorer.h"
#include "RevertManager.h"
#include "VideoChunkMapper.h"
#include "BoardCache.h"
#include "libchess/position.hpp"
#include "libchess/move.hpp"

#include <opencv2/opencv.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace cta {

static std::optional<int> parse_clock_seconds(const std::string& clock) {
    std::vector<int> parts;
    std::string current;
    for (char ch : clock) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            current += ch;
        } else if (ch == ':' || ch == '.') {
            if (current.empty()) return std::nullopt;
            parts.push_back(std::stoi(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        parts.push_back(std::stoi(current));
    }
    if (parts.size() == 2) {
        return parts[0] * 60 + parts[1];
    }
    if (parts.size() == 3) {
        return parts[0] * 3600 + parts[1] * 60 + parts[2];
    }
    return std::nullopt;
}

static bool plausible_clock_after_move(const std::string& candidate, const std::string& previous) {
    auto cand_seconds = parse_clock_seconds(candidate);
    if (!cand_seconds) return false;
    auto prev_seconds = parse_clock_seconds(previous);
    if (!prev_seconds) return true;

    // Increment clocks can rise a little after a move, but a jump by minutes
    // almost always means OCR included the clock icon or nearby video text.
    return *cand_seconds <= *prev_seconds + 65;
}

static std::optional<std::string> recover_clock_suffix_form(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : value) {
        if (ch == ':' || ch == '.') {
            if (current.empty()) return std::nullopt;
            parts.push_back(current);
            current.clear();
        } else if (std::isdigit(static_cast<unsigned char>(ch))) {
            current += ch;
        } else {
            return std::nullopt;
        }
    }
    if (!current.empty()) parts.push_back(current);
    if (parts.size() != 3 || parts[0] != "1" || parts[1].size() != 2 ||
        parts[1][0] != '1' || parts[2].size() != 2) {
        return std::nullopt;
    }
    return parts[1].substr(1) + ":" + parts[2];
}

static std::string choose_contextual_clock_candidate(const std::vector<std::string>& candidates,
                                                     const std::string& current,
                                                     const std::string& previous) {
    if (previous.empty() || candidates.empty()) {
        return current;
    }

    auto previous_seconds = parse_clock_seconds(previous);
    if (!previous_seconds) {
        return current;
    }
    const auto current_seconds = parse_clock_seconds(current);
    if (current_seconds && plausible_clock_after_move(current, previous) &&
        *current_seconds > *previous_seconds && *current_seconds < 120) {
        // At very low times, a segmented digit can be read as a nearby
        // smaller value.  Compare only nearby plausible interpretations in
        // this narrow regime; selecting the largest OCR candidate globally
        // can turn an ordinary clock drop into a false increment.
        std::string highest = current;
        for (const auto& candidate : candidates) {
            const auto candidate_seconds = parse_clock_seconds(candidate);
            if (candidate_seconds &&
                plausible_clock_after_move(candidate, previous) &&
                *candidate_seconds > *current_seconds &&
                *candidate_seconds - *current_seconds <= 5 &&
                *candidate_seconds > parse_clock_seconds(highest).value_or(0)) {
                highest = candidate;
            }
        }
        return highest;
    }
    if (current_seconds && plausible_clock_after_move(current, previous)) {
        return current;
    }

    const bool current_has_hour_field =
        std::count(current.begin(), current.end(), ':') >= 2;
    if ((!current_seconds || !plausible_clock_after_move(current, previous)) &&
        *previous_seconds < 120 && !current_has_hour_field) {
        // When the primary OCR form is unreadable or implausibly large,
        // low-clock candidates are often all that remains. Prefer the highest
        // plausible low-clock reading instead of the numerically closest one;
        // the latter can be the result of a single segmented digit being
        // clipped.
        std::string highest;
        for (const auto& candidate : candidates) {
            const auto candidate_seconds = parse_clock_seconds(candidate);
            if (!candidate_seconds || *candidate_seconds >= 120 ||
                !plausible_clock_after_move(candidate, previous)) {
                continue;
            }
            if (highest.empty() ||
                *candidate_seconds > parse_clock_seconds(highest).value_or(0)) {
                highest = candidate;
            }
        }
        if (!highest.empty()) return highest;
    }

    std::string best = current;
    auto best_seconds = parse_clock_seconds(best);
    for (const auto& candidate : candidates) {
        auto candidate_seconds = parse_clock_seconds(candidate);
        if (!candidate_seconds || !plausible_clock_after_move(candidate, previous)) {
            continue;
        }

        if (!best_seconds || !plausible_clock_after_move(best, previous)) {
            best = candidate;
            best_seconds = candidate_seconds;
            continue;
        }

        const int best_drop = std::abs(*previous_seconds - *best_seconds);
        const int candidate_drop = std::abs(*previous_seconds - *candidate_seconds);
        if (candidate_drop < best_drop) {
            best = candidate;
            best_seconds = candidate_seconds;
        }
    }
    return best;
}

static std::optional<int> clock_drop_seconds(const std::string& candidate, const std::string& previous) {
    auto cand_seconds = parse_clock_seconds(candidate);
    auto prev_seconds = parse_clock_seconds(previous);
    if (!cand_seconds || !prev_seconds) return std::nullopt;
    return *prev_seconds - *cand_seconds;
}

struct ObservationReplayBootstrap {
    BoardGeometry geometry;
    cv::Mat first_frame;
    double duration = 0.1;
    std::string initial_fen;
};

static ObservationReplayBootstrap load_observation_replay_bootstrap(
    const std::string& observation_path) {
    std::ifstream input(observation_path);
    if (!input.is_open()) {
        throw std::runtime_error("Could not open observation replay trace: " + observation_path);
    }

    const std::filesystem::path trace_path(observation_path);
    auto resolve_asset = [&](const nlohmann::json& images, const char* name) {
        if (!images.contains(name) || !images.at(name).is_string()) {
            return std::filesystem::path();
        }
        std::filesystem::path asset(images.at(name).get<std::string>());
        if (asset.is_relative()) asset = trace_path.parent_path() / asset;
        return asset;
    };

    ObservationReplayBootstrap bootstrap;
    std::string line;
    bool loaded_first_observation = false;
    try {
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            const auto observation = nlohmann::json::parse(line);
            if (!observation.is_object()) {
                throw std::runtime_error("observation record is not an object");
            }
            bootstrap.duration = std::max(
                bootstrap.duration, observation.value("timestamp", 0.0) + 0.1);
            if (loaded_first_observation) continue;
            loaded_first_observation = true;

            const auto board = observation.value("board", nlohmann::json::object());
            bootstrap.geometry.bx = board.value("x", 0);
            bootstrap.geometry.by = board.value("y", 0);
            bootstrap.geometry.bw = board.value("width", 0);
            bootstrap.geometry.bh = board.value("height", 0);
            bootstrap.geometry.sq_w = board.value("square_width", 0.0);
            bootstrap.geometry.sq_h = board.value("square_height", 0.0);
            bootstrap.geometry.localization_score = -1.0;
            if (bootstrap.geometry.bw <= 0 || bootstrap.geometry.bh <= 0) {
                throw std::runtime_error("first observation has invalid board geometry");
            }
            if (bootstrap.geometry.sq_w <= 0.0) {
                bootstrap.geometry.sq_w = static_cast<double>(bootstrap.geometry.bw) / 8.0;
            }
            if (bootstrap.geometry.sq_h <= 0.0) {
                bootstrap.geometry.sq_h = static_cast<double>(bootstrap.geometry.bh) / 8.0;
            }

            const auto images = observation.value("images", nlohmann::json::object());
            const auto board_path = resolve_asset(images, "board");
            if (board_path.empty()) {
                throw std::runtime_error("first observation has no board artifact");
            }
            const cv::Mat board_image = cv::imread(board_path.string(), cv::IMREAD_COLOR);
            if (board_image.empty()) {
                throw std::runtime_error("could not read first observation board artifact: " +
                                         board_path.string());
            }

            const auto frame_path = resolve_asset(images, "frame");
            if (!frame_path.empty()) {
                bootstrap.first_frame = cv::imread(frame_path.string(), cv::IMREAD_COLOR);
            }
            if (bootstrap.first_frame.empty()) {
                const int frame_width = std::max(
                    bootstrap.geometry.bx + bootstrap.geometry.bw, board_image.cols);
                const int frame_height = std::max(
                    bootstrap.geometry.by + bootstrap.geometry.bh, board_image.rows);
                bootstrap.first_frame = cv::Mat(
                    std::max(1, frame_height), std::max(1, frame_width), CV_8UC3,
                    cv::Scalar(0, 0, 0));
                const cv::Rect destination(
                    bootstrap.geometry.bx, bootstrap.geometry.by,
                    std::min(bootstrap.geometry.bw, board_image.cols),
                    std::min(bootstrap.geometry.bh, board_image.rows));
                board_image(cv::Rect(0, 0, destination.width, destination.height))
                    .copyTo(bootstrap.first_frame(destination));
            }

            const auto events = observation.value("events", nlohmann::json::array());
            if (events.is_array()) {
                for (const auto& event : events) {
                    if (event.contains("fen") && event.at("fen").is_string() &&
                        !event.at("fen").get<std::string>().empty()) {
                        bootstrap.initial_fen = event.at("fen").get<std::string>();
                        break;
                    }
                }
            }
        }
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Could not load observation replay bootstrap '" + observation_path + "': " +
            error.what());
    }

    if (!loaded_first_observation || bootstrap.first_frame.empty()) {
        throw std::runtime_error("Observation replay trace contains no usable observations: " +
                                 observation_path);
    }
    if (bootstrap.initial_fen.empty()) {
        bootstrap.initial_fen =
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    }
    return bootstrap;
}

// ── Main extraction loop ────────────────────────────────────────────────────

GameData ChessVideoExtractor::extract_moves_from_video(const std::string& video_path,
                                                        const std::string& debug_label,
                                                        std::atomic<bool>* cancel_flag) {
    auto t_start = std::chrono::steady_clock::now();
    auto elapsed = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    };

    auto log_info = [&](const std::string& msg, int percent = -1) {
        if (progress_callback_) {
            progress_callback_(percent, msg);
        } else {
            std::cout << msg << std::endl;
        }
    };

    const char* replay_path_env = std::getenv("CTA_REPLAY_OBSERVATIONS");
    const std::string observation_replay_path =
        replay_path_env != nullptr ? replay_path_env : std::string();
    const bool observation_replay_requested = !observation_replay_path.empty();
    std::optional<ObservationReplayBootstrap> replay_bootstrap;
    if (observation_replay_requested) {
        replay_bootstrap = load_observation_replay_bootstrap(observation_replay_path);
    }

    // A bounded trace starts from the state recorded in its first event. This
    // keeps replay local to the diagnostic window while retaining the same
    // legal-move reducer used by ordinary video extraction.
    const std::string initial_fen = replay_bootstrap && !replay_bootstrap->initial_fen.empty()
        ? replay_bootstrap->initial_fen
        : "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    pos_ptr_ = std::make_unique<libchess::Position>(initial_fen);

    std::string safe_video_path = utils::get_safe_path(video_path);
    cv::VideoCapture cap;
    double fps = 30.0;
    double duration = 0.0;
    cv::Mat first_frame;
    if (observation_replay_requested) {
        first_frame = replay_bootstrap->first_frame;
        duration = replay_bootstrap->duration;
        log_info(utils::ts(elapsed()) + " Loading board geometry from observation replay trace...");
    } else {
        cap = extractor_detail::open_video_capture(safe_video_path);
        if (!cap.isOpened()) {
            throw std::runtime_error("Cannot open video: " + video_path);
        }

        fps = cap.get(cv::CAP_PROP_FPS);
        double total_frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
        duration = total_frames / fps;
        log_info(utils::ts(elapsed()) + " Locating board coordinates using template matching...");
        cap >> first_frame;
        if (first_frame.empty()) {
            throw std::runtime_error("Cannot read first frame of video.");
        }

        // Free the hardware video decoder instance to save VRAM and avoid
        // contention; map-reduce workers use their own VideoCapture instances.
        cap.release();
    }

    if (observation_replay_requested) {
        geo_ = std::make_unique<BoardGeometry>(replay_bootstrap->geometry);
    } else {
        geo_ = BoardCache::load_or_locate(safe_video_path, first_frame, board_template_, [&](const std::string& msg) {
            log_info(utils::ts(elapsed()) + " " + msg);
        });
    }

    gpu_pipeline_ = std::make_unique<GPUPipeline>();

    std::string debug_dir_name = debug_label;
    if (debug_dir_name.empty()) {
        size_t slash = video_path.find_last_of("/\\");
        std::string filename = (slash == std::string::npos) ? video_path : video_path.substr(slash + 1);
        size_t dot = filename.find_last_of(".");
        debug_dir_name = (dot == std::string::npos) ? filename : filename.substr(0, dot);
    }
    for (char& c : debug_dir_name) {
        if (static_cast<unsigned char>(c) > 127 || c == ':' || c == '*' || c == '?' || c == '\"' || c == '<' || c == '>' || c == '|' || c == '\n' || c == '\r') {
            c = '_';
        }
    }
    std::filesystem::path temp_base = std::filesystem::temp_directory_path() / "ChessTubeAnalyzer" / "debug_screenshots" / "cpp_extraction";
    std::string debug_dir = (temp_base / debug_dir_name).string();

    if (std::filesystem::exists(debug_dir)) {
        std::filesystem::remove_all(debug_dir);
    }

    if (debug_level_ != DebugLevel::None) {
        log_info(utils::ts(elapsed()) + " Generating debug screenshot for initial board...");
        std::filesystem::create_directories(debug_dir);
        cv::Mat debug_board = first_frame.clone();
        draw_board_grid(debug_board, *geo_, cv::Scalar(0, 255, 0), 2, true);
        cv::imwrite(debug_dir + "/00_initial_board_0.00s.png", debug_board);
    }

    // Set up margins for batch square mean computation
    margin_h_ = static_cast<int>(geo_->sq_h * 0.15);
    margin_w_ = static_cast<int>(geo_->sq_w * 0.15);

    // Initialize game data
    GameData data;
    data.fens.push_back(pos_ptr_->get_fen());
    data.video_fens.push_back(pos_ptr_->get_fen());
    if (fen_cb_) fen_cb_(pos_ptr_->get_fen());
    std::vector<double> move_scores;
    std::vector<std::optional<size_t>> move_video_indices;
    std::vector<std::string> variation_trace_events;

    auto is_prefix = [](const std::vector<std::string>& maybe_prefix,
                        const std::vector<std::string>& line) {
        if (maybe_prefix.size() > line.size()) return false;
        return std::equal(maybe_prefix.begin(), maybe_prefix.end(), line.begin());
    };

    // Board matching cannot observe FEN's half-move or full-move counters.
    // Preserve the placement, side to move, castling rights, and en-passant
    // state when comparing a visual state to a legal chess transition.
    auto visual_fen_key = [](const std::string& fen) {
        size_t field_end = 0;
        for (int field = 0; field < 4; ++field) {
            field_end = fen.find(' ', field_end);
            if (field_end == std::string::npos) return fen;
            ++field_end;
        }
        return fen.substr(0, field_end - 1);
    };

    auto prune_superseded_variations = [&](size_t parent_ply, const std::vector<std::string>& moves) {
        for (auto it = data.variations.begin(); it != data.variations.end();) {
            if (it->first <= parent_ply || it->first > parent_ply + moves.size()) {
                ++it;
                continue;
            }

            const size_t offset = it->first - parent_ply;
            if (offset == moves.size()) {
                it = data.variations.erase(it);
                continue;
            }
            std::vector<VariationData>& vars = it->second;
            vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const VariationData& existing) {
                if (offset + existing.moves.size() > moves.size()) return false;
                return std::equal(existing.moves.begin(), existing.moves.end(), moves.begin() + offset);
            }), vars.end());

            if (vars.empty()) {
                it = data.variations.erase(it);
            } else {
                ++it;
            }
        }
    };

    auto add_variation = [&](size_t parent_ply, VariationData var_data,
                             bool replay_observation = false) {
        if (parent_ply > data.moves.size()) {
            variation_trace_events.push_back(
                "kind=variation_parent_out_of_range_suppressed;parent=" +
                std::to_string(parent_ply) + ";main_size=" +
                std::to_string(data.moves.size()));
            return;
        }
        var_data.replay_observation = var_data.replay_observation || replay_observation;
        std::string incoming_moves;
        for (size_t i = 0; i < var_data.moves.size(); ++i) {
            if (i != 0) incoming_moves += ',';
            incoming_moves += var_data.moves[i];
        }
        // Variations are reconstructed from several temporal paths.  Their
        // map key is a historical ply, so it is not always a reliable state
        // index after a replay/rebase.  Prefer the branch's recorded root FEN
        // when validating it; this keeps the check tied to chess state rather
        // than to a temporal index.
        const std::string variation_root_fen = !var_data.fens.empty()
            ? var_data.fens.front()
            : (parent_ply < data.fens.size() ? data.fens[parent_ply] : std::string{});
        if (!variation_root_fen.empty() && !var_data.moves.empty()) {
            try {
                libchess::Position variation_position(variation_root_fen);
                size_t legal_count = 0;
                for (; legal_count < var_data.moves.size(); ++legal_count) {
                    libchess::Move variation_move =
                        variation_position.parse_move(var_data.moves[legal_count]);
                    variation_position.makemove(variation_move);
                }
                if (legal_count < var_data.moves.size()) {
                    var_data.moves.resize(legal_count);
                    var_data.timestamps.resize(std::min(legal_count, var_data.timestamps.size()));
                    var_data.fens.resize(std::min(legal_count, var_data.fens.size()));
                    var_data.scores.resize(std::min(legal_count, var_data.scores.size()));
                    var_data.clocks.resize(std::min(legal_count, var_data.clocks.size()));
                    variation_trace_events.push_back(
                        "kind=variation_illegal_suffix_trim;parent=" +
                        std::to_string(parent_ply) +
                        ";remaining=" + std::to_string(legal_count));
                }
            } catch (...) {
                // A malformed parent state should not make extraction fail;
                // the existing variation path remains available for output.
            }
        }

        // A branch can be emitted when the video briefly leaves a position
        // and then returns to the same main-line state.  Suppress it only if
        // its complete recorded state path is an exact main-line replay.  A
        // FEN match plus transition/FEN agreement is required, so a repeated
        // UCI move in a different position cannot be mistaken for a replay.
        if (!variation_root_fen.empty() && !var_data.moves.empty()) {
            bool replay_root_seen = false;
            bool replay_path_seen = false;
            for (size_t main_ply = 0; main_ply + var_data.moves.size() < data.fens.size(); ++main_ply) {
                if (data.fens[main_ply] != variation_root_fen) {
                    continue;
                }
                replay_root_seen = true;
                if (main_ply + var_data.moves.size() > data.moves.size()) continue;

                bool exact_replay = true;
                for (size_t offset = 0; offset < var_data.moves.size(); ++offset) {
                    if (data.moves[main_ply + offset] != var_data.moves[offset]) {
                        exact_replay = false;
                        break;
                    }
                    if (offset + 1 < var_data.fens.size() &&
                        data.fens[main_ply + offset + 1] != var_data.fens[offset + 1]) {
                        exact_replay = false;
                        break;
                    }
                }
                if (exact_replay) replay_path_seen = true;
                // If the same line was first observed before the retained
                // main-line occurrence, it is a durable analysis line (the
                // main line was selected later after the revert).  Only
                // suppress a replay that was observed at or after the main
                // occurrence, when it cannot add historical information.
                const bool replay_observed_after_main =
                    !var_data.timestamps.empty() &&
                    main_ply < data.timestamps.size() &&
                    var_data.timestamps.front() > data.timestamps[main_ply] + 0.05;
                // `replay_observation` describes how this tail was reached
                // by the state machine, not whether it is semantically a
                // duplicate.  During a post-game handoff the working line
                // can temporarily contain the newly observed analysis line;
                // suppressing it merely because of that transient match
                // loses a genuine variation.  Only the timeline can prove a
                // duplicate: the branch itself must have been observed after
                // the already-retained main-line occurrence.
                if (exact_replay && replay_observed_after_main) {
                    std::ostringstream replay_event;
                    replay_event << "kind=variation_exact_mainline_replay_suppressed;parent="
                                 << parent_ply << ";main_ply=" << main_ply
                                 << ";replay_observation=" << (replay_observation ? 1 : 0)
                                 << ";branch_ts="
                                 << (var_data.timestamps.empty() ? -1.0 : var_data.timestamps.front())
                                 << ";main_ts="
                                 << (main_ply >= data.timestamps.size() ? -1.0 : data.timestamps[main_ply])
                                 << ";moves=" << incoming_moves;
                    variation_trace_events.push_back(replay_event.str());
                    return;
                }
            }
            if (replay_observation) {
                variation_trace_events.push_back(
                    "kind=replay_observation_not_exact;parent=" +
                    std::to_string(parent_ply) + ";root_seen=" +
                    std::to_string(replay_root_seen ? 1 : 0) + ";path_seen=" +
                    std::to_string(replay_path_seen ? 1 : 0) + ";moves=" +
                    incoming_moves);
            }
        }
        const size_t incoming_count = var_data.moves.size();
        const size_t sibling_count_before = data.variations.contains(parent_ply)
            ? data.variations.at(parent_ply).size() : 0;
        prune_superseded_variations(parent_ply, var_data.moves);

        std::vector<VariationData>& siblings = data.variations[parent_ply];
        bool superseded_by_existing = false;
        siblings.erase(std::remove_if(siblings.begin(), siblings.end(), [&](const VariationData& existing) {
            if (is_prefix(existing.moves, var_data.moves)) return true;
            if (is_prefix(var_data.moves, existing.moves)) {
                superseded_by_existing = true;
            }
            return false;
        }), siblings.end());

        if (!superseded_by_existing) {
            siblings.push_back(std::move(var_data));
        }
        if (siblings.empty()) {
            data.variations.erase(parent_ply);
        }
        std::ostringstream variation_event;
        variation_event << "kind=variation_add;parent=" << parent_ply
                        << ";incoming_count=" << incoming_count
                        << ";siblings_before=" << sibling_count_before
                        << ";siblings_after="
                        << (data.variations.contains(parent_ply)
                            ? data.variations.at(parent_ply).size() : 0)
                        << ";superseded_by_existing=" << (superseded_by_existing ? 1 : 0)
                        << ";moves=" << incoming_moves;
        variation_trace_events.push_back(variation_event.str());
    };

    auto suppress_timeline_for_move_range = [&](size_t start_ply, size_t end_ply) {
        const size_t clamped_end = std::min(end_ply, move_video_indices.size());
        for (size_t ply = start_ply; ply < clamped_end; ++ply) {
            if (!move_video_indices[ply]) continue;
            const size_t video_idx = *move_video_indices[ply];
            if (video_idx < data.video_moves.size()) {
                data.video_moves[video_idx] = "REVERT";
            }
        }
    };

    // Extract initial clocks
    clock_cache_ = std::make_unique<ClockCache>();
    ClockState init_clocks;
    if (!observation_replay_requested) {
        init_clocks = extract_clocks(first_frame, board_template_, *geo_, clock_cache_.get());
    }
    data.clocks.push_back({init_clocks.active_player, init_clocks.white_time, init_clocks.black_time});

    // Board image history for revert detection
    std::vector<cv::Mat> board_image_history;
    std::vector<std::vector<double>> history_hashes;
    std::unordered_map<std::uint64_t, std::vector<int>> history_hash_index;
    cv::Mat board_gray_crop;
    cv::cvtColor(first_frame(cv::Rect(geo_->bx, geo_->by, geo_->bw, geo_->bh)), board_gray_crop, cv::COLOR_BGR2GRAY);
    RevertManager revert_mgr(*geo_, margin_h_, margin_w_);
    revert_mgr.initialize(board_gray_crop, compute_all_square_means(board_gray_crop, *geo_, margin_h_, margin_w_));
    std::vector<size_t> revert_history_ply_counts{0};

    // ── Initialize zero-copy GPU pipeline ────────────────────────────────────
    // Uploads the first board grayscale to GPU. The GPU pipeline performs
    // absdiff on GPU (eliminating 2x H→D copies per frame), then downloads
    // the diff for CPU-based square means computation to maintain precision.
    gpu_pipeline_->init();
    gpu_pipeline_active_ = gpu_pipeline_->is_available();
    if (gpu_pipeline_active_) {
        log_info(utils::ts(elapsed()) + " Zero-copy GPU pipeline enabled — absdiff on GPU, CPU integral for precision");
        gpu_pipeline_->update_current(board_gray_crop);
    } else {
        log_info(utils::ts(elapsed()) + " Using CPU pipeline for frame diff computation");
    }

    log_info(utils::ts(elapsed()) + " Scanning video frames to calculate plies...");

    auto round_t = [](double val) { return std::round(val * 100.0) / 100.0; };

    // ── Profiling counters ────────────────────────────────────────────────
    int frame_count = 0;

    constexpr double fine_step = 0.1;
    constexpr double quiet_coarse_step = 1.0;
    constexpr double quiet_before_coarse_scan = 2.0;
    double t = 0.0;
    int branch_counter = 0;
    constexpr double kMinMoveScore = 35.0;
    constexpr double kWeakCoalescedMoveScore = 35.0;
    constexpr double kMinCoalescedFollowupScore = 60.0;
    constexpr double kRevertMaxSquareHashDiff = 15.0;
    constexpr double kRevertMeanHashDiff = 8.0;
    constexpr double kRevertFullImageMeanDiff = 3.0;

    auto read_env_int = [](const char* name, int fallback) {
#ifdef _WIN32
        char* env_val = nullptr;
        size_t env_len = 0;
        if (_dupenv_s(&env_val, &env_len, name) == 0 && env_val != nullptr) {
            int parsed = std::atoi(env_val);
            free(env_val);
            return parsed > 0 ? parsed : fallback;
        }
        return fallback;
#else
        const char* env_val = std::getenv(name);
        if (!env_val) return fallback;
        int parsed = std::atoi(env_val);
        return parsed > 0 ? parsed : fallback;
#endif
    };

    auto read_env_double = [](const char* name, double fallback) {
#ifdef _WIN32
        char* env_val = nullptr;
        size_t env_len = 0;
        if (_dupenv_s(&env_val, &env_len, name) == 0 && env_val != nullptr) {
            double parsed = std::atof(env_val);
            free(env_val);
            return parsed > 0.0 ? parsed : fallback;
        }
        return fallback;
#else
        const char* env_val = std::getenv(name);
        if (!env_val) return fallback;
        double parsed = std::atof(env_val);
        return parsed > 0.0 ? parsed : fallback;
#endif
    };

    std::ofstream trace_stream;
    std::ofstream diagnostic_stream;
    std::uint64_t diagnostic_sequence = 0;
    std::uint64_t current_observation_id = 0;
    std::uint64_t diagnostic_candidate_sequence = 0;
    std::uint64_t current_candidate_id = 0;
    std::uint64_t current_transition_id = 0;
    diagnostics::Evidence diagnostic_evidence_context;
    double trace_start = read_env_double("CTA_TRACE_START", -1.0);
    double trace_end = read_env_double("CTA_TRACE_END", -1.0);
    const bool trace_historical = read_env_int("CTA_TRACE_HISTORICAL", 0) > 0;
    const bool trace_nearest = read_env_int("CTA_TRACE_NEAREST", 0) > 0;
    const char* trace_path = std::getenv("CTA_TRACE_FILE");
    const char* diagnostic_path = std::getenv("CTA_DIAGNOSTIC_FILE");
    const char* diagnostic_frame_dir_env = std::getenv("CTA_DIAGNOSTIC_FRAME_DIR");
    const std::string diagnostic_frame_dir =
        diagnostic_frame_dir_env != nullptr ? diagnostic_frame_dir_env : std::string();
    const bool trace_requested = trace_path != nullptr && *trace_path != '\0';
    const bool diagnostic_requested = diagnostic_path != nullptr && *diagnostic_path != '\0';
    const double geometry_check_interval_seconds = std::clamp(
        read_env_double("CTA_GEOMETRY_CHECK_INTERVAL_SECONDS", 5.0), 1.0, 30.0);
    const double diagnostic_frame_interval_seconds = std::clamp(
        read_env_double("CTA_DIAGNOSTIC_FRAME_INTERVAL_SECONDS",
                       geometry_check_interval_seconds), 1.0, 30.0);
    std::uint64_t diagnostic_template_identity = 0;
    if (diagnostic_requested && !board_template_.empty()) {
        const auto* bytes = board_template_.ptr<unsigned char>(0);
        const std::size_t byte_count = board_template_.total() * board_template_.elemSize();
        diagnostic_template_identity = 14695981039346656037ull;
        for (std::size_t index = 0; index < byte_count; ++index) {
            diagnostic_template_identity ^= bytes[index];
            diagnostic_template_identity *= 1099511628211ull;
        }
    }
    if (trace_requested || diagnostic_requested) {
        if (trace_start < 0.0) trace_start = 0.0;
        if (trace_end < 0.0) trace_end = duration;
    }
    if (trace_requested && trace_end >= trace_start && trace_start >= 0.0) {
        trace_stream.open(trace_path, std::ios::out | std::ios::trunc);
        if (trace_stream.is_open()) {
            trace_stream << "event\tt\tactive_ply\tfen\tbest_move\tbest_score\tmax_sd\ty_from\ty_to\tmeta\n";
        }
    }
    if (diagnostic_requested && trace_end >= trace_start && trace_start >= 0.0) {
        diagnostic_stream.open(diagnostic_path, std::ios::out | std::ios::trunc);
    }
    auto trace_candidate = [&](const char* event,
                               double event_t,
                               size_t active_ply,
                               const std::string& fen,
                               const std::string& best_move,
                               double best_score,
                               double max_sd,
                               double y_from,
                               double y_to,
                               const std::string& meta = std::string()) {
        if (event != nullptr && std::strcmp(event, "CANDIDATE") == 0) {
            // Candidate IDs represent reducer proposals, while observation IDs
            // identify the mapped frame that supplied each proposal.  Keeping
            // them separate lets a later accepted/rejected event be correlated
            // across settle frames without losing frame provenance.
            current_candidate_id = ++diagnostic_candidate_sequence;
        }
        if ((!trace_stream.is_open() && !diagnostic_stream.is_open()) ||
            event_t < trace_start || event_t > trace_end) return;
        if (trace_stream.is_open()) {
            trace_stream << event << '\t'
                         << std::fixed << std::setprecision(3) << event_t << '\t'
                         << active_ply << '\t'
                         << fen << '\t'
                         << best_move << '\t'
                         << std::setprecision(3) << best_score << '\t'
                         << max_sd << '\t'
                         << y_from << '\t'
                         << y_to << '\t'
                         << meta << '\n';
        }

        if (diagnostic_stream.is_open()) {
            if (event != nullptr && std::strcmp(event, "ACCEPT") == 0) {
                ++current_transition_id;
            }
            const bool is_revert_event =
                event != nullptr && (std::strncmp(event, "REVERT", 6) == 0 ||
                                     std::strncmp(event, "REBASE", 6) == 0 ||
                                     std::strncmp(event, "HANDOFF", 7) == 0 ||
                                     std::strncmp(event, "HISTORICAL", 10) == 0);
            const std::string reducer_state =
                event != nullptr && std::strcmp(event, "ACCEPT") == 0 ? "accepted" :
                event != nullptr && std::strcmp(event, "CANDIDATE") == 0 ? "candidate" :
                event != nullptr && std::strcmp(event, "QUIET") == 0 ? "quiet" :
                event != nullptr && std::strcmp(event, "VALIDATION_REJECTED") == 0 ? "validation_rejected" :
                event != nullptr && std::strcmp(event, "REJECTED_FRAME") == 0 ? "frame_rejected" :
                is_revert_event ? "recovering" : "observed";
            const auto record = diagnostics::from_legacy_trace(
                ++diagnostic_sequence, event, event_t, active_ply, fen, best_move,
                best_score, max_sd, y_from, y_to, meta,
                current_observation_id, current_transition_id,
                diagnostic_evidence_context,
                current_candidate_id != 0 ? current_candidate_id : current_observation_id,
                current_transition_id,
                static_cast<std::uint64_t>(branch_counter),
                static_cast<std::uint64_t>(branch_counter),
                reducer_state);
            diagnostics::write_json_line(diagnostic_stream, record);
            diagnostic_stream.flush();
        }
    };

    auto trace_revert_applied = [&](double event_t, size_t restored_ply,
                                    double max_square_diff,
                                    const char* restore_reason) {
        trace_candidate(
            "REVERT_APPLIED", event_t, restored_ply, data.fens.back(), "", 0.0,
            max_square_diff, 0.0, 0.0,
            "restored_ply=" + std::to_string(restored_ply) +
            ";reason=" + (restore_reason != nullptr ? restore_reason : "unknown"));
    };

    auto add_diagnostic_tag = [&](const char* tag) {
        if (!diagnostic_stream.is_open() || tag == nullptr || *tag == '\0') return;
        if (std::find(diagnostic_evidence_context.observation_tags.begin(),
                      diagnostic_evidence_context.observation_tags.end(), tag) ==
            diagnostic_evidence_context.observation_tags.end()) {
            diagnostic_evidence_context.observation_tags.emplace_back(tag);
        }
    };

    double chunk_duration = std::clamp(read_env_double("CTA_CHUNK_SECONDS", 300.0), 30.0, 300.0);
    // Diagnostic-only bounded replay.  This keeps the normal extractor on
    // the complete video while allowing focused state-machine investigations
    // to stop after a timestamp without inventing fixture-specific behavior.
    const double stop_after_seconds = read_env_double("CTA_STOP_AFTER_SECONDS", duration);
    const double extraction_duration = std::min(duration, stop_after_seconds);
    int total_chunks = std::max(1, static_cast<int>(std::ceil(extraction_duration / chunk_duration)));

    int frame_width = first_frame.cols;
    int frame_height = first_frame.rows;
    int roi_x1 = std::max(0, static_cast<int>(geo_->bx + geo_->bw * 0.76));
    int roi_x2 = std::min(frame_width, static_cast<int>(geo_->bx + geo_->bw));
    int top_roi_y1 = std::max(0, static_cast<int>(geo_->by - geo_->sq_h * 0.55));
    int top_roi_y2 = std::max(top_roi_y1 + 1, static_cast<int>(geo_->by - geo_->sq_h * 0.08));
    int bot_roi_y1 = std::min(frame_height - 1, static_cast<int>(geo_->by + geo_->bh + geo_->sq_h * 0.07));
    int bot_roi_y2 = std::min(frame_height, static_cast<int>(geo_->by + geo_->bh + geo_->sq_h * 0.40));
    bool has_clocks = (roi_x2 > roi_x1 && top_roi_y2 > top_roi_y1 && bot_roi_y2 > bot_roi_y1);

    // Candidate mapping feeds a stateful reducer.  Independent decoder seeks
    // can produce different leading/trailing frames at chunk boundaries, so
    // parallel mapping is not deterministic for the same video.  Keep the
    // correctness-first sequential path as the default; callers doing a
    // controlled performance experiment may opt into a bounded worker count.
    int num_threads = std::max(1u, std::thread::hardware_concurrency());
    
    int ffmpeg_threads = read_env_int("OPENCV_FFMPEG_THREADS", 1);

    // Prevent massive thread contention from OpenCV's internal FFmpeg multi-threading.
    int max_safe_workers = std::max(1u, static_cast<unsigned int>(std::thread::hardware_concurrency()) / ffmpeg_threads);
    num_threads = std::min(num_threads, max_safe_workers);

    // A bounded worker count is available for controlled diagnostics and
    // performance experiments.  It is intentionally opt-in because changing
    // decoder concurrency must not change detected moves.
    const int configured_max_workers = read_env_int("CTA_MAX_WORKERS", 1);
    num_threads = std::min(num_threads, configured_max_workers);

    if (memory_limit_mb_ > 0) {
        // Bound memory (assume ~500MB overhead per active mapped chunk + VideoCapture context)
        int max_threads_mem = std::max(1, memory_limit_mb_ / 500);
        num_threads = std::min(num_threads, max_threads_mem);
    }
    num_threads = std::min(num_threads, total_chunks);
    long long reducer_candidates_seen = 0;
    long long revert_hash_tests = 0;
    long long revert_full_tests = 0;
    long long revert_index_queries = 0;
    long long revert_index_fallbacks = 0;
    long long score_calls = 0;
    long long clock_activity_calls = 0;
    long long clock_ocr_calls = 0;
    long long revert_us = 0;
    long long score_us = 0;
    long long clock_activity_us = 0;
    long long clock_ocr_us = 0;
    bool exhaustive_revert_fallback = read_env_int("CTA_REVERT_EXHAUSTIVE_FALLBACK", 0) > 0;
    bool debug_clock_candidates = read_env_int("CTA_DEBUG_CLOCK_CANDIDATES", 0) > 0;
    bool trace_settle = read_env_int("CTA_TRACE_SETTLE", 0) > 0;
    const int debug_clock_roi_ply = read_env_int("CTA_DEBUG_CLOCK_ROI_PLY", -1);
    const char* debug_clock_roi_dir_env = std::getenv("CTA_DEBUG_CLOCK_ROI_DIR");
    const std::filesystem::path debug_clock_roi_dir =
        debug_clock_roi_dir_env && *debug_clock_roi_dir_env
            ? std::filesystem::path(debug_clock_roi_dir_env)
            : std::filesystem::path();
    int revert_generation = 0;
    bool first_move_after_revert = false;
    std::array<std::string, 2> last_moved_clock = {data.clocks.front().white_time, data.clocks.front().black_time};
    bool pending_stale_branch = false;
    size_t pending_stale_ply = 0;
    std::string pending_stale_first_move;
    std::string pending_stale_prev_move;
    double pending_stale_timestamp = 0.0;
    bool suppressed_stale_branch = false;
    size_t suppressed_stale_ply = 0;
    std::string suppressed_stale_first_move;
    std::optional<size_t> postgame_branch_start_ply;
    std::optional<size_t> ignore_replay_reverts_before_ply;
    bool postgame_replay_mode = false;
    bool postgame_boundary_from_clock_gap = false;
    struct PreservedMainline {
        std::vector<std::string> moves;
        std::vector<double> timestamps;
        std::vector<std::string> fens;
        std::vector<ClockInfo> clocks;
        std::vector<std::string> video_fens;
        std::vector<double> video_timestamps;
        std::vector<std::string> video_moves;
        std::vector<double> move_scores;
        std::vector<std::optional<size_t>> move_video_indices;
        size_t replay_parent = 0;
    };
    struct RebasedContinuation {
        std::string move;
        double timestamp = 0.0;
        ClockInfo clock;
        double score = 0.0;
        std::optional<size_t> video_index;
        size_t original_ply = 0;
    };
    // A clock OCR failure can hide a real analysis branch after the board,
    // yellow registration, hover, and legality checks have all succeeded.
    // Keep that evidence separate from the verified main line.  It is only
    // committed when a later state-derived historical handoff proves that
    // the branch was actually superseded; otherwise it is discarded.
    struct ClockVetoedMove {
        std::string move;
        std::string before_fen;
        std::string after_fen;
        double timestamp = 0.0;
        double score = 0.0;
    };
    std::vector<ClockVetoedMove> clock_vetoed_tail;
    std::optional<size_t> clock_vetoed_base_ply;
    std::string clock_vetoed_base_fen;
    std::string clock_vetoed_current_fen;
    cv::Mat clock_vetoed_last_board_gray;
    double clock_vetoed_started_at = 0.0;
    std::optional<PreservedMainline> preserved_replay_mainline;
    std::vector<size_t> rebased_clock_ply_indices;
    auto reset_stale_branch_state = [&]() {
        pending_stale_branch = false;
        pending_stale_first_move.clear();
        pending_stale_prev_move.clear();
        suppressed_stale_branch = false;
        suppressed_stale_first_move.clear();
    };
    auto tail_has_unreliable_moved_clock = [&](size_t start_ply) {
        if (start_ply == 0 || start_ply >= data.clocks.size() ||
            start_ply >= data.fens.size()) {
            return false;
        }
        for (size_t ply = start_ply + 1;
             ply < data.clocks.size() && ply < data.fens.size(); ++ply) {
            const std::string& parent_fen = data.fens[ply - 1];
            const size_t active_field = parent_fen.find(' ');
            if (active_field == std::string::npos || active_field + 1 >= parent_fen.size()) {
                continue;
            }
            const bool white_moved = parent_fen[active_field + 1] == 'w';
            const std::string& parent_clock = white_moved
                ? data.clocks[ply - 1].white_time : data.clocks[ply - 1].black_time;
            const std::string& tail_clock = white_moved
                ? data.clocks[ply].white_time : data.clocks[ply].black_time;
            if (parse_clock_seconds(parent_clock) && !parse_clock_seconds(tail_clock)) {
                return true;
            }
        }
        return false;
    };
    auto demote_tail_to_variation = [&](size_t start_ply,
                                        bool replay_observation = false) {
        if (start_ply >= data.moves.size()) return false;

        VariationData var_data;
        var_data.moves.assign(data.moves.begin() + start_ply, data.moves.end());
        var_data.timestamps.assign(data.timestamps.begin() + start_ply, data.timestamps.end());
        var_data.fens.assign(data.fens.begin() + start_ply, data.fens.end() - 1);
        var_data.scores.assign(move_scores.begin() + start_ply, move_scores.end());
        var_data.clocks.assign(data.clocks.begin() + start_ply + 1, data.clocks.end());
        // Analysis variations do not represent independently timed game
        // play.  Their clock annotation is the settled clock at the branch
        // point, regardless of how long the replay remained visible.  Keep
        // this invariant in the shared demotion path so exact reverts and
        // historical/post-game handoffs produce the same output.
        if (start_ply < data.clocks.size()) {
            const ClockInfo branch_clock = data.clocks[start_ply];
            for (ClockInfo& variation_clock : var_data.clocks) {
                variation_clock.white_time = branch_clock.white_time;
                variation_clock.black_time = branch_clock.black_time;
                variation_clock.active = branch_clock.active;
            }
        }

        // If the demoted tail is the parent of a clock-vetoed visual branch,
        // retain the confirmed branch continuation in the same variation.
        // The FEN equality prevents unrelated later clock glitches from being
        // attached to this line.
        bool committed_clock_veto_tail = false;
        std::optional<size_t> clock_vetoed_base_index;
        if (!clock_vetoed_base_fen.empty() && !clock_vetoed_tail.empty()) {
            for (size_t candidate_ply = start_ply;
                 candidate_ply < data.fens.size(); ++candidate_ply) {
                if (data.fens[candidate_ply] == clock_vetoed_base_fen) {
                    clock_vetoed_base_index = candidate_ply;
                    break;
                }
            }
        }
        if (clock_vetoed_base_index && *clock_vetoed_base_index < data.clocks.size()) {
            const ClockInfo branch_clock = data.clocks[*clock_vetoed_base_index];
            for (const ClockVetoedMove& vetoed : clock_vetoed_tail) {
                // A handoff can expose the same last transition both in the
                // demoted tail and in the deferred observer.  The observer is
                // an alternate view of the state transition, not a second
                // chess move, so retain it once.
                if (!var_data.moves.empty() && var_data.moves.back() == vetoed.move) {
                    variation_trace_events.push_back(
                        "kind=clock_veto_branch_deduplicated;parent=" +
                        std::to_string(start_ply) + ";move=" + vetoed.move);
                    continue;
                }
                var_data.moves.push_back(vetoed.move);
                var_data.timestamps.push_back(vetoed.timestamp);
                var_data.fens.push_back(vetoed.before_fen);
                var_data.scores.push_back(vetoed.score);
                var_data.clocks.push_back(branch_clock);
            }
            committed_clock_veto_tail = true;
            variation_trace_events.push_back(
                "kind=clock_veto_branch_committed;parent=" + std::to_string(start_ply) +
                ";base_ply=" + std::to_string(*clock_vetoed_base_index) +
                ";count=" + std::to_string(clock_vetoed_tail.size()));
        }

        if (trace_stream.is_open() && !var_data.moves.empty()) {
            std::ostringstream trace;
            trace << "kind=demote;parent=" << start_ply << ";moves=";
            for (size_t i = 0; i < var_data.moves.size(); ++i) {
                if (i != 0) trace << ',';
                trace << var_data.moves[i];
            }
            trace << ";clocks=";
            for (size_t i = 0; i < var_data.clocks.size(); ++i) {
                if (i != 0) trace << ',';
                trace << var_data.clocks[i].white_time << '/' << var_data.clocks[i].black_time;
            }
            variation_trace_events.push_back(trace.str());
        }
        add_variation(start_ply, std::move(var_data), replay_observation);

        data.moves.resize(start_ply);
        data.timestamps.resize(start_ply);
        data.fens.resize(start_ply + 1);
        data.clocks.resize(start_ply + 1);
        move_scores.resize(start_ply);
        move_video_indices.resize(start_ply);

        auto history_it = std::find(revert_history_ply_counts.begin(), revert_history_ply_counts.end(), start_ply);
        if (history_it != revert_history_ply_counts.end()) {
            int new_history_size = static_cast<int>(std::distance(revert_history_ply_counts.begin(), history_it) + 1);
            revert_mgr.resize_history(new_history_size);
            revert_history_ply_counts.resize(new_history_size);
        }

        pos_ptr_ = std::make_unique<libchess::Position>(data.fens.back());
        last_moved_clock = {data.clocks.back().white_time, data.clocks.back().black_time};
        if (committed_clock_veto_tail) {
            clock_vetoed_tail.clear();
            clock_vetoed_base_ply.reset();
            clock_vetoed_base_fen.clear();
            clock_vetoed_current_fen.clear();
            clock_vetoed_last_board_gray.release();
            clock_vetoed_started_at = 0.0;
        }
        return true;
    };

    auto save_tail_as_variation = [&](size_t start_ply,
                                      bool replay_observation = false) {
        if (start_ply >= data.moves.size()) return false;
        VariationData var_data;
        var_data.moves.assign(data.moves.begin() + start_ply, data.moves.end());
        var_data.timestamps.assign(data.timestamps.begin() + start_ply, data.timestamps.end());
        var_data.fens.assign(data.fens.begin() + start_ply, data.fens.end() - 1);
        var_data.scores.assign(move_scores.begin() + start_ply, move_scores.end());
        var_data.clocks.assign(data.clocks.begin() + start_ply + 1, data.clocks.end());
        if (start_ply < data.clocks.size()) {
            const ClockInfo branch_clock = data.clocks[start_ply];
            for (ClockInfo& variation_clock : var_data.clocks) {
                variation_clock.white_time = branch_clock.white_time;
                variation_clock.black_time = branch_clock.black_time;
                variation_clock.active = branch_clock.active;
            }
        }
        add_variation(start_ply, std::move(var_data), replay_observation);
        return true;
    };

    auto discard_variations_containing_state = [&](const std::string& state_fen) {
        if (state_fen.empty()) return;
        for (auto variations_it = data.variations.begin();
             variations_it != data.variations.end();) {
            auto& variations = variations_it->second;
            const size_t before = variations.size();
            variations.erase(
                std::remove_if(variations.begin(), variations.end(),
                               [&](const VariationData& variation) {
                                   // A replacement that starts from a state
                                   // inside a saved branch supersedes that
                                   // branch's remaining tail.  A match at the
                                   // variation root is intentionally retained:
                                   // it is an independent alternative from
                                   // the same parent, not a branch traversed
                                   // by the replacement.
                                   const auto state_it = std::find(
                                       variation.fens.begin() + std::min<size_t>(1, variation.fens.size()),
                                       variation.fens.end(), state_fen);
                                   if (state_it != variation.fens.end() && trace_stream.is_open()) {
                                       std::ostringstream matched_variation;
                                       matched_variation << "kind=historical_variation_match;state=" << state_fen
                                                         << ";parent=" << variations_it->first
                                                         << ";state_index=" << (state_it - variation.fens.begin())
                                                         << ";moves=";
                                       for (size_t move_index = 0; move_index < variation.moves.size(); ++move_index) {
                                           if (move_index != 0) matched_variation << ',';
                                           matched_variation << variation.moves[move_index];
                                       }
                                       variation_trace_events.push_back(matched_variation.str());
                                   }
                                   return state_it != variation.fens.end();
                               }),
                variations.end());
            if (variations.size() != before && trace_stream.is_open()) {
                variation_trace_events.push_back(
                    "kind=discard_historical_interior;state=" + state_fen +
                    ";parent=" + std::to_string(variations_it->first) +
                    ";count=" + std::to_string(before - variations.size()));
            }
            if (variations.empty()) {
                variations_it = data.variations.erase(variations_it);
            } else {
                ++variations_it;
            }
        }
    };

    // Static block allocation prevents workers from hopping around the video and
    // destroying FFmpeg's sequential read efficiency by constantly seeking.
    int max_lookahead = read_env_int("CTA_MAX_CHUNK_LOOKAHEAD", std::max(12, num_threads * 3));

    std::ostringstream schedule_ss;
    schedule_ss << utils::ts(elapsed()) << " Launching Map-Reduce visual extraction ("
                << num_threads << " workers, " << total_chunks
                << " chunks, chunk=" << chunk_duration
                << "s, lookahead=" << max_lookahead << ")";
    log_info(schedule_ss.str());

    std::vector<bool> unresolved_consumed_squares(64, false);
    double next_geometry_check_t = 0.0;
    BoardGeometry previous_diagnostic_geometry;
    bool have_previous_diagnostic_geometry = false;

    VideoChunkMapper mapper(safe_video_path, extraction_duration, chunk_duration, total_chunks,
                            *geo_, margin_h_, margin_w_, static_cast<int>(debug_level_),
                            has_clocks, max_lookahead, num_threads, frame_width, frame_height,
                            diagnostic_stream.is_open(), diagnostic_frame_interval_seconds,
                            diagnostic_frame_dir, trace_start, trace_end,
                            observation_replay_path);
    mapper.start(cancel_flag);

    double last_progress_t = -1.0;
    
    for (int current_chunk = 0; current_chunk < total_chunks; ++current_chunk) {
        mapper.set_current_reducing_chunk(current_chunk);
        std::vector<CandidateFrame> candidates;
        
        if (!mapper.get_chunk_results(current_chunk, candidates, cancel_flag)) {
            if (cancel_flag && *cancel_flag) {
                log_info("\nExtraction cancelled by user.");
                break;
            }
            if (mapper.has_failed()) {
                const std::string reason = mapper.failure_reason();
                throw std::runtime_error(
                    reason.empty() ? "Mapper worker failed without a diagnostic reason." : reason);
            }
            break;
        }

        double pct = (double)(current_chunk) / total_chunks * 100.0;
        if (pct - last_progress_t >= 1.0) {
            if (progress_callback_) {
                char buf[128];
                snprintf(buf, sizeof(buf), "Reducing chunk %d/%d | plies: %zu", current_chunk + 1, total_chunks, data.moves.size());
                progress_callback_(static_cast<int>(pct), std::string(buf));
            } else {
                std::cout << "\r  [" << std::fixed << std::setprecision(1) << pct << "%] Reducing chunk " 
                          << current_chunk + 1 << "/" << total_chunks << "  |  plies: " << data.moves.size() << std::flush;
            }
            last_progress_t = pct;
        }

        std::optional<std::pair<int, int>> pending_resolved_endpoint;
        double pending_resolved_endpoint_t = 0.0;
        constexpr double kPendingResolvedEndpointWindowSeconds = 0.75;

        for (size_t i = 0; i < candidates.size(); ++i) {
            if (cancel_flag && *cancel_flag) break;
            
            auto& cf = candidates[i];
            current_observation_id = cf.observation_id;
            diagnostic_evidence_context = {};
            diagnostic_evidence_context.mapper_chunk = cf.mapper_chunk;
            diagnostic_evidence_context.source_frame_index = cf.source_frame_index;
            diagnostic_evidence_context.mapper_emission_reason = cf.emission_reason;
            diagnostic_evidence_context.diagnostic_frame_path = cf.diagnostic_frame_path;
            diagnostic_evidence_context.diagnostic_board_path = cf.diagnostic_board_path;
            diagnostic_evidence_context.diagnostic_clock_top_path = cf.diagnostic_clock_top_path;
            diagnostic_evidence_context.diagnostic_clock_bottom_path = cf.diagnostic_clock_bottom_path;
            diagnostic_evidence_context.board_hash = cf.board_hash;
            diagnostic_evidence_context.yellow_arrows_checked = diagnostic_stream.is_open();
            diagnostic_evidence_context.yellow_arrows = cf.yellow_arrows;
            diagnostic_evidence_context.template_identity = diagnostic_template_identity;
            diagnostic_evidence_context.board_x = geo_->bx;
            diagnostic_evidence_context.board_y = geo_->by;
            diagnostic_evidence_context.board_width = geo_->bw;
            diagnostic_evidence_context.board_height = geo_->bh;
            diagnostic_evidence_context.square_width = geo_->sq_w;
            diagnostic_evidence_context.square_height = geo_->sq_h;
            diagnostic_evidence_context.localization_score = geo_->localization_score;
            diagnostic_evidence_context.localization_scale = geo_->localization_scale;
            diagnostic_evidence_context.yellow_endpoint_threshold =
                validation::kYellowEndpointThreshold;
            diagnostic_evidence_context.yellow_pair_threshold =
                validation::kYellowPairThreshold;
            diagnostic_evidence_context.yellow_assessment.thresholds = {
                validation::kYellowEndpointThreshold,
                validation::kYellowPairThreshold};
            if (cf.emission_reason == "initial_frame") {
                add_diagnostic_tag("initial");
            } else if (cf.emission_reason == "settled_tail") {
                add_diagnostic_tag("settled");
            } else if (cf.emission_reason == "motion_burst_cap") {
                add_diagnostic_tag("motion");
                add_diagnostic_tag("animation");
            } else if (cf.emission_reason == "motion_leading_edge" ||
                       cf.emission_reason == "motion_spike_prev_frame") {
                add_diagnostic_tag("motion");
            }
            if (!cf.yellow_arrows.empty()) {
                add_diagnostic_tag("arrow_activity");
            }
            ++reducer_candidates_seen;
            t = cf.t;
            double next_t = round_t(t + fine_step);
            ++frame_count;

        cv::Mat& full_bgr = cf.full_bgr;
        cv::Mat& board_bgr = cf.board_bgr;
        cv::Mat& board_gray = cf.board_gray;
        auto record_diagnostic_yellow_measurement = [&](const char* square) {
            if (!diagnostic_stream.is_open() || square == nullptr || *square == '\0') return;
            const auto measured = validation::measure_yellowness(
                board_bgr, *geo_, square, true);
            const auto existing = std::find_if(
                diagnostic_evidence_context.yellow_measurements.begin(),
                diagnostic_evidence_context.yellow_measurements.end(),
                [square](const diagnostics::YellowMeasurement& item) {
                    return item.square == square;
                });
            diagnostics::YellowMeasurement item;
            item.square = square;
            item.corner_scores = measured.corner_scores;
            item.corner_bgr = measured.corner_bgr;
            item.corner_edge_density = measured.corner_edge_density;
            item.score = measured.score;
            if (existing == diagnostic_evidence_context.yellow_measurements.end()) {
                diagnostic_evidence_context.yellow_measurements.push_back(std::move(item));
            } else {
                *existing = std::move(item);
            }
        };

        // Re-localization is intentionally diagnostic-only. The reducer keeps
        // its anchored geometry for a run, while this periodic probe exposes
        // camera/UI movement before it can be mistaken for square evidence.
        if (diagnostic_stream.is_open() && t >= next_geometry_check_t) {
            diagnostic_evidence_context.geometry_checked = true;
            next_geometry_check_t = t + geometry_check_interval_seconds;
            if (!full_bgr.empty() && !board_template_.empty()) {
                const BoardGeometry observed_geometry = locate_board(full_bgr, board_template_);
                diagnostic_evidence_context.geometry_relocalization_score =
                    observed_geometry.localization_score;
                diagnostic_evidence_context.geometry_drift_x =
                    static_cast<double>(observed_geometry.bx - geo_->bx);
                diagnostic_evidence_context.geometry_drift_y =
                    static_cast<double>(observed_geometry.by - geo_->by);
                diagnostic_evidence_context.geometry_size_drift = std::max(
                    std::abs(static_cast<double>(observed_geometry.bw - geo_->bw)),
                    std::abs(static_cast<double>(observed_geometry.bh - geo_->bh)));
                if (have_previous_diagnostic_geometry) {
                    diagnostic_evidence_context.geometry_step_drift_x =
                        static_cast<double>(observed_geometry.bx - previous_diagnostic_geometry.bx);
                    diagnostic_evidence_context.geometry_step_drift_y =
                        static_cast<double>(observed_geometry.by - previous_diagnostic_geometry.by);
                    diagnostic_evidence_context.geometry_step_size_drift = std::max(
                        std::abs(static_cast<double>(observed_geometry.bw - previous_diagnostic_geometry.bw)),
                        std::abs(static_cast<double>(observed_geometry.bh - previous_diagnostic_geometry.bh)));
                }

                // A cached/template match can move several pixels between
                // otherwise identical frames. Treat that as calibration
                // jitter; only a temporal step or a large persistent offset
                // is an unexpected geometry jump.
                constexpr double kGeometryPositionJumpPixels = 16.0;
                constexpr double kGeometrySizeJumpPixels = 16.0;
                constexpr double kGeometryPersistentOffsetPixels = 24.0;
                diagnostic_evidence_context.geometry_anomaly =
                    (have_previous_diagnostic_geometry &&
                     (std::abs(diagnostic_evidence_context.geometry_step_drift_x) >
                          kGeometryPositionJumpPixels ||
                      std::abs(diagnostic_evidence_context.geometry_step_drift_y) >
                          kGeometryPositionJumpPixels ||
                      diagnostic_evidence_context.geometry_step_size_drift >
                          kGeometrySizeJumpPixels)) ||
                    (have_previous_diagnostic_geometry &&
                     (std::abs(diagnostic_evidence_context.geometry_drift_x) >
                          kGeometryPersistentOffsetPixels ||
                      std::abs(diagnostic_evidence_context.geometry_drift_y) >
                          kGeometryPersistentOffsetPixels ||
                      diagnostic_evidence_context.geometry_size_drift >
                          kGeometryPersistentOffsetPixels));
                diagnostic_evidence_context.geometry_decision =
                    diagnostic_evidence_context.geometry_anomaly ? "jump_detected" : "stable";
                diagnostic_evidence_context.geometry_assessment.state =
                    diagnostic_evidence_context.geometry_decision;
                diagnostic_evidence_context.geometry_assessment.thresholds = {
                    16.0, 16.0, 24.0};
                diagnostic_evidence_context.geometry_assessment.measurements = {
                    {"anchor_drift_x", diagnostic_evidence_context.geometry_drift_x},
                    {"anchor_drift_y", diagnostic_evidence_context.geometry_drift_y},
                    {"size_drift", diagnostic_evidence_context.geometry_size_drift},
                    {"step_drift_x", diagnostic_evidence_context.geometry_step_drift_x},
                    {"step_drift_y", diagnostic_evidence_context.geometry_step_drift_y},
                    {"step_size_drift", diagnostic_evidence_context.geometry_step_size_drift},
                    {"match_score", diagnostic_evidence_context.geometry_relocalization_score},
                };
                diagnostic_evidence_context.geometry_assessment.uncertainty_reason =
                    diagnostic_evidence_context.geometry_anomaly
                        ? "geometry_jump"
                        : "uncalibrated_localization_confidence";
                previous_diagnostic_geometry = observed_geometry;
                have_previous_diagnostic_geometry = true;

                if (!red_board_template_.empty()) {
                    diagnostic_evidence_context.red_squares_checked = true;
                    diagnostic_evidence_context.red_squares = find_red_squares(
                        full_bgr, board_template_, red_board_template_, *geo_);
                    if (!diagnostic_evidence_context.red_squares.empty()) {
                        add_diagnostic_tag("red_square_activity");
                    }
                }
            } else {
                diagnostic_evidence_context.geometry_decision = "unavailable";
                diagnostic_evidence_context.geometry_assessment.state = "unavailable";
                diagnostic_evidence_context.geometry_assessment.uncertainty_reason =
                    "full_frame_not_retained";
            }
        }
        const cv::Mat& prev_gray = revert_mgr.get_latest_gray();

        std::vector<double> sq_means;
        double max_sd = 0;
        std::string diagnostic_move;
        double diagnostic_score = 0.0;
        double diagnostic_y_from = 0.0;
        double diagnostic_y_to = 0.0;
        static thread_local cv::Mat diff;

        // Compute the accurate diff against the anchored pristine snapshot (prev_gray),
        // which is essential for correct move scoring and rejecting partial animations.
        GPUAccelerator::absdiff(board_gray, prev_gray, diff);
        sq_means = compute_all_square_means(diff, *geo_, margin_h_, margin_w_);
        for (double sd : sq_means) {
            if (sd > max_sd) max_sd = sd;
        }
        diagnostic_evidence_context.changed_square_count = static_cast<std::size_t>(std::count_if(
            sq_means.begin(), sq_means.end(), [](double value) { return value >= 15.0; }));
        if (diagnostic_stream.is_open()) {
            std::vector<std::pair<double, int>> changed_squares;
            for (int square = 0; square < static_cast<int>(sq_means.size()); ++square) {
                if (sq_means[square] >= 15.0) {
                    changed_squares.emplace_back(sq_means[square], square);
                }
            }
            std::sort(changed_squares.begin(), changed_squares.end(), std::greater<>());
            for (size_t rank = 0; rank < changed_squares.size(); ++rank) {
                diagnostic_evidence_context.changed_squares.push_back({
                    utils::sq_name(changed_squares[rank].second),
                    changed_squares[rank].first,
                    rank + 1,
                });
            }
        }

        if (max_sd < 15.0) {
            add_diagnostic_tag("quiet");
            trace_candidate("QUIET", t, data.moves.size(), pos_ptr_->get_fen(), "", 0.0, max_sd, 0.0, 0.0,
                            "candidate_index=" + std::to_string(i));
            continue;
        }

        add_diagnostic_tag("motion");

        {
            const auto diagnostic_best = this->score_moves_for_board(sq_means);
            diagnostic_score = diagnostic_best.score;
            if (diagnostic_best.from_sq >= 0 && diagnostic_best.to_sq >= 0) {
                diagnostic_evidence_context.score_from_square_diff = sq_means[diagnostic_best.from_sq];
                diagnostic_evidence_context.score_to_square_diff = sq_means[diagnostic_best.to_sq];
                diagnostic_evidence_context.score_adjustment = diagnostic_best.score -
                    diagnostic_evidence_context.score_from_square_diff -
                    diagnostic_evidence_context.score_to_square_diff;
            }
            std::string diagnostic_top_legal;
            if (diagnostic_best.from_sq >= 0 && diagnostic_best.to_sq >= 0) {
                const char* diagnostic_from = utils::sq_name(diagnostic_best.from_sq);
                const char* diagnostic_to = utils::sq_name(diagnostic_best.to_sq);
                diagnostic_move = std::string(diagnostic_from) + diagnostic_to;
                diagnostic_y_from = validation::check_yellowness(board_bgr, *geo_, diagnostic_from);
                diagnostic_y_to = validation::check_yellowness(board_bgr, *geo_, diagnostic_to);
                record_diagnostic_yellow_measurement(diagnostic_from);
                record_diagnostic_yellow_measurement(diagnostic_to);
                diagnostic_evidence_context.yellow_checked = true;
                diagnostic_evidence_context.yellow_from = diagnostic_y_from;
                diagnostic_evidence_context.yellow_to = diagnostic_y_to;
                if (std::max(diagnostic_y_from, diagnostic_y_to) >= 25.0) {
                    add_diagnostic_tag("highlight_activity");
                }
            }
            if (diagnostic_stream.is_open()) {
                std::vector<std::pair<double, int>> yellow_scores;
                yellow_scores.reserve(64);
                for (int square = 0; square < 64; ++square) {
                    yellow_scores.emplace_back(
                        validation::check_yellowness(board_bgr, *geo_, utils::sq_name(square)),
                        square);
                }
                std::sort(yellow_scores.begin(), yellow_scores.end(), std::greater<>());
                const size_t yellow_count = std::min<size_t>(8, yellow_scores.size());
                for (size_t rank = 0; rank < yellow_count; ++rank) {
                    diagnostic_evidence_context.yellow_candidates.push_back({
                        utils::sq_name(yellow_scores[rank].second),
                        yellow_scores[rank].first,
                        rank + 1,
                    });
                    record_diagnostic_yellow_measurement(
                        utils::sq_name(yellow_scores[rank].second));
                }
            }
            if (trace_stream.is_open() || diagnostic_stream.is_open()) {
                struct RankedLegalMove {
                    std::string uci;
                    double score = 0.0;
                };
                std::vector<RankedLegalMove> ranked_legal_moves;
                for (const auto& legal_move : pos_ptr_->legal_moves()) {
                    const int from_sq = static_cast<int>(static_cast<unsigned int>(legal_move.from()));
                    int to_sq = static_cast<int>(static_cast<unsigned int>(legal_move.to()));
                    if (legal_move.type() == libchess::MoveType::ksc) {
                        to_sq = (from_sq == 4) ? 6 : 62;
                    } else if (legal_move.type() == libchess::MoveType::qsc) {
                        to_sq = (from_sq == 4) ? 2 : 58;
                    }
                    std::string uci = static_cast<std::string>(legal_move);
                    if (uci == "e1h1") uci = "e1g1";
                    else if (uci == "e1a1") uci = "e1c1";
                    else if (uci == "e8h8") uci = "e8g8";
                    else if (uci == "e8a8") uci = "e8c8";
                    ranked_legal_moves.push_back({uci, sq_means[from_sq] + sq_means[to_sq]});
                }
                std::sort(ranked_legal_moves.begin(), ranked_legal_moves.end(),
                          [](const RankedLegalMove& lhs, const RankedLegalMove& rhs) {
                              return lhs.score > rhs.score;
                          });
                if (ranked_legal_moves.size() >= 2) {
                    diagnostic_evidence_context.score_margin =
                        ranked_legal_moves[0].score - ranked_legal_moves[1].score;
                }
                const size_t top_count = std::min<size_t>(8, ranked_legal_moves.size());
                std::ostringstream top_moves;
                for (size_t rank = 0; rank < top_count; ++rank) {
                    if (rank != 0) top_moves << ',';
                    top_moves << ranked_legal_moves[rank].uci << ':'
                              << std::fixed << std::setprecision(1) << ranked_legal_moves[rank].score;
                    diagnostic_evidence_context.legal_candidates.push_back({
                        ranked_legal_moves[rank].uci,
                        ranked_legal_moves[rank].score,
                        rank + 1,
                    });
                }
                diagnostic_top_legal = top_moves.str();
            }
            trace_candidate("CANDIDATE", t, data.moves.size(), pos_ptr_->get_fen(), diagnostic_move,
                            diagnostic_score, max_sd, diagnostic_y_from, diagnostic_y_to,
                            "candidate_index=" + std::to_string(i) +
                            ";history_size=" + std::to_string(revert_mgr.history_size()) +
                            ";yellow_arrows=" + std::to_string(cf.yellow_arrows.size()) +
                            ";top_legal=" + diagnostic_top_legal);

            // Diagnostic-only historical transition ranking.  The reducer normally
            // scores against the current position, but an analysis line can be
            // undone and immediately replaced by a new move before the exact board
            // snapshot is seen again.  Ranking the same visual diff against the
            // retained snapshots tells us whether the correct source state is
            // available to a future generic transition resolver.
            if (trace_stream.is_open() && trace_historical) {
                const size_t history_count = revert_mgr.history_size();
                // The source of a replacement move must be near the active
                // tail.  Limiting this diagnostic to the recent tail keeps a
                // trace usable on long videos while still covering the states
                // that can plausibly be restored by an undo.
                const size_t first_history_index = history_count > 32 ? history_count - 32 : 0;
                for (size_t history_index = first_history_index; history_index < history_count; ++history_index) {
                    if (history_index >= revert_history_ply_counts.size()) {
                        break;
                    }
                    const size_t source_ply = revert_history_ply_counts[history_index];
                    if (source_ply >= data.fens.size()) {
                        continue;
                    }

                    cv::Mat historical_diff;
                    GPUAccelerator::absdiff(board_gray, revert_mgr.get_history_gray(history_index), historical_diff);
                    const std::vector<double> historical_sq_means =
                        compute_all_square_means(historical_diff, *geo_, margin_h_, margin_w_);
                    const ExtractedMoveScore historical_best = MoveScorer::score_moves_for_board(
                        libchess::Position(data.fens[source_ply]), historical_sq_means);

                    std::string historical_move;
                    if (historical_best.from_sq >= 0 && historical_best.to_sq >= 0) {
                        historical_move = std::string(utils::sq_name(historical_best.from_sq)) +
                                           utils::sq_name(historical_best.to_sq);
                        if (historical_best.promotion != '\0') {
                            historical_move += historical_best.promotion;
                        }
                    }

                    double historical_max = 0.0;
                    for (double square_diff : historical_sq_means) {
                        historical_max = std::max(historical_max, square_diff);
                    }
                    const double historical_full_mean = cv::mean(historical_diff)[0];
                    trace_candidate("HISTORICAL", t, data.moves.size(), data.fens[source_ply],
                                    historical_move, historical_best.score, historical_max, 0.0, 0.0,
                                    "candidate_index=" + std::to_string(i) +
                                    ";history_index=" + std::to_string(history_index) +
                                    ";source_ply=" + std::to_string(source_ply) +
                                    ";full_mean=" + std::to_string(historical_full_mean));
                }
            }
        }

        std::vector<double> current_hash;
        bool hash_computed = false;

        // O(1) Perceptual Hash Revert Detection
        if (revert_mgr.history_size() > 1) {
                auto revert_start = std::chrono::steady_clock::now();
                current_hash = cf.board_hash.empty()
                    ? compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_)
                    : cf.board_hash;
                hash_computed = true;

                int best_history_idx = revert_mgr.find_revert_idx(board_gray, current_hash,
                                                                   revert_hash_tests, revert_full_tests,
                                                                   revert_index_queries, revert_index_fallbacks,
                                                                   exhaustive_revert_fallback);

                // A frame can be visually close to an old board while being
                // even closer to the active board plus one well-supported
                // legal move.  In that conflict, treating the near-threshold
                // historical match as an undo discards a real transition.
                // Exact reverts still win: this only defers a historical match
                // when the active snapshot is materially closer and its legal
                // transition has strong board-diff evidence.
                if (best_history_idx >= 0 &&
                    best_history_idx < static_cast<int>(revert_history_ply_counts.size()) &&
                    diagnostic_score >= 100.0) {
                    cv::Mat matched_history_diff;
                    GPUAccelerator::absdiff(board_gray,
                                             revert_mgr.get_history_gray(best_history_idx),
                                             matched_history_diff);
                    const double matched_history_mean = cv::mean(matched_history_diff)[0];
                    const double active_history_mean = cv::mean(diff)[0];
                    constexpr double kActiveRevertProximityMargin = 0.50;
                    if (active_history_mean + kActiveRevertProximityMargin < matched_history_mean) {
                        trace_candidate(
                            "REVERT_DEFERRED_FOR_ACTIVE_TRANSITION", t, data.moves.size(),
                            pos_ptr_->get_fen(), diagnostic_move, diagnostic_score, max_sd,
                            diagnostic_y_from, diagnostic_y_to,
                            "history_index=" + std::to_string(best_history_idx) +
                            ";source_ply=" +
                            std::to_string(revert_history_ply_counts[best_history_idx]) +
                            ";active_mean=" + std::to_string(active_history_mean) +
                            ";historical_mean=" + std::to_string(matched_history_mean));
                        best_history_idx = -1;
                    }
                }

                trace_candidate("REVERT_SEARCH", t, data.moves.size(), pos_ptr_->get_fen(), "", 0.0, max_sd, 0.0, 0.0,
                                "history_index=" + std::to_string(best_history_idx) +
                                ";history_size=" + std::to_string(revert_mgr.history_size()));

                if (trace_stream.is_open() && trace_nearest && revert_mgr.history_size() > 1) {
                    int nearest_history_idx = -1;
                    double nearest_full_mean = std::numeric_limits<double>::max();
                    for (size_t history_index = 0; history_index + 1 < revert_mgr.history_size(); ++history_index) {
                        cv::Mat nearest_diff;
                        GPUAccelerator::absdiff(board_gray, revert_mgr.get_history_gray(history_index), nearest_diff);
                        const double full_mean = cv::mean(nearest_diff)[0];
                        if (full_mean >= nearest_full_mean) continue;
                        nearest_history_idx = static_cast<int>(history_index);
                        nearest_full_mean = full_mean;
                    }
                    trace_candidate("REVERT_NEAREST", t, data.moves.size(), pos_ptr_->get_fen(), "", 0.0, max_sd, 0.0, 0.0,
                                    "history_index=" + std::to_string(nearest_history_idx) +
                                    ";source_ply=" + (nearest_history_idx >= 0 &&
                                        nearest_history_idx < static_cast<int>(revert_history_ply_counts.size())
                                        ? std::to_string(revert_history_ply_counts[nearest_history_idx]) : "none") +
                                    ";full_mean=" + std::to_string(nearest_full_mean));
                }

                revert_us += std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - revert_start).count();

                // A mean pixel difference < 3.0 indicates a near-identical board state.
                if (best_history_idx >= 0 && best_history_idx < static_cast<int>(revert_history_ply_counts.size())) {
                    size_t best_ply = revert_history_ply_counts[best_history_idx];

                    // When the UI returns to a canonical historical successor,
                    // a short analysis alternative can have been shown from
                    // that successor's immediate predecessor.  Recover only
                    // a highly corroborated one-ply alternative: it must be
                    // legal from the predecessor, differ from the recorded
                    // continuation, be strongly highlighted in this frame,
                    // and occur shortly after that parent.  This preserves
                    // real one-move analysis without treating arbitrary old
                    // board resemblance as a variation.
                    if (best_history_idx > 0) {
                        const size_t alternative_history_idx =
                            static_cast<size_t>(best_history_idx - 1);
                        const size_t alternative_parent =
                            revert_history_ply_counts[alternative_history_idx];
                        constexpr double kAdjacentAlternativeMinScore = 120.0;
                        constexpr double kAdjacentAlternativeMinYellowness = 35.0;
                        constexpr double kAdjacentAlternativeMinCombinedYellowness = 90.0;
                        constexpr double kAdjacentAlternativeMaxAgeSeconds = 15.0;
                        if (alternative_parent < data.moves.size() &&
                            alternative_parent < data.fens.size() &&
                            alternative_parent < data.timestamps.size() &&
                            alternative_parent < data.clocks.size() &&
                            best_ply == alternative_parent + 1 &&
                            (t - data.timestamps[alternative_parent]) <=
                                kAdjacentAlternativeMaxAgeSeconds) {
                            cv::Mat alternative_diff;
                            GPUAccelerator::absdiff(
                                board_gray,
                                revert_mgr.get_history_gray(alternative_history_idx),
                                alternative_diff);
                            const std::vector<double> alternative_sq_means =
                                compute_all_square_means(
                                    alternative_diff, *geo_, margin_h_, margin_w_);
                            const ExtractedMoveScore alternative_best =
                                MoveScorer::score_moves_for_board(
                                    libchess::Position(data.fens[alternative_parent]),
                                    alternative_sq_means);
                            if (alternative_best.from_sq >= 0 && alternative_best.to_sq >= 0 &&
                                alternative_best.score >= kAdjacentAlternativeMinScore) {
                                std::string alternative_move =
                                    std::string(utils::sq_name(alternative_best.from_sq)) +
                                    utils::sq_name(alternative_best.to_sq);
                                if (alternative_best.promotion != '\0') {
                                    alternative_move += alternative_best.promotion;
                                }
                                const double alternative_y_from = validation::check_yellowness(
                                    board_bgr, *geo_, utils::sq_name(alternative_best.from_sq));
                                const double alternative_y_to = validation::check_yellowness(
                                    board_bgr, *geo_, utils::sq_name(alternative_best.to_sq));
                                const bool distinct_from_canonical =
                                    alternative_parent >= data.moves.size() ||
                                    alternative_move != data.moves[alternative_parent];
                                const bool strongly_registered =
                                    alternative_y_from >= kAdjacentAlternativeMinYellowness &&
                                    alternative_y_to >= kAdjacentAlternativeMinYellowness &&
                                    alternative_y_from + alternative_y_to >=
                                        kAdjacentAlternativeMinCombinedYellowness;
                                bool legal_alternative = false;
                                try {
                                    libchess::Position alternative_position(
                                        data.fens[alternative_parent]);
                                    alternative_position.makemove(
                                        alternative_position.parse_move(alternative_move));
                                    legal_alternative = true;
                                } catch (...) {
                                    legal_alternative = false;
                                }
                                if (distinct_from_canonical && strongly_registered && legal_alternative) {
                                    bool already_recorded = false;
                                    if (const auto existing = data.variations.find(alternative_parent);
                                        existing != data.variations.end()) {
                                        already_recorded = std::any_of(
                                            existing->second.begin(), existing->second.end(),
                                            [&](const VariationData& variation) {
                                                return variation.moves.size() == 1 &&
                                                    variation.moves.front() == alternative_move;
                                            });
                                    }
                                    if (!already_recorded) {
                                        VariationData alternative_variation;
                                        alternative_variation.moves.push_back(alternative_move);
                                        alternative_variation.timestamps.push_back(t);
                                        alternative_variation.fens.push_back(
                                            data.fens[alternative_parent]);
                                        alternative_variation.scores.push_back(alternative_best.score);
                                        alternative_variation.clocks.push_back(
                                            data.clocks[alternative_parent]);
                                        add_variation(alternative_parent,
                                                      std::move(alternative_variation));
                                        data.video_timestamps.push_back(t);
                                        data.video_fens.push_back(data.fens[alternative_parent]);
                                        data.video_moves.push_back(alternative_move);
                                        trace_candidate(
                                            "ADJACENT_HISTORICAL_VARIATION", t,
                                            data.moves.size(), data.fens[alternative_parent],
                                            alternative_move, alternative_best.score, max_sd,
                                            alternative_y_from, alternative_y_to,
                                            "parent=" + std::to_string(alternative_parent) +
                                            ";matched_ply=" + std::to_string(best_ply) +
                                            ";history_index=" +
                                            std::to_string(alternative_history_idx));
                                    }
                                }
                            }
                        }
                    }
                    const bool returns_to_preserved_mainline =
                        preserved_replay_mainline &&
                        best_ply == preserved_replay_mainline->replay_parent + 1 &&
                        data.moves.size() > preserved_replay_mainline->replay_parent;
                    const bool returns_to_postgame_parent =
                        postgame_branch_start_ply &&
                        best_ply == *postgame_branch_start_ply &&
                        data.moves.size() > *postgame_branch_start_ply;

                    if (returns_to_preserved_mainline) {
                        const size_t parent = preserved_replay_mainline->replay_parent;
                        // A frame can land directly on a historical state
                        // after a legal move from a temporary analysis line.
                        // Trace any retained-state edge that produces the
                        // matched state, so transpositions can be recovered
                        // from board state and legality rather than notation.
                        if (trace_stream.is_open() && !diagnostic_move.empty() &&
                            best_ply < data.fens.size()) {
                            const std::string& matched_state = data.fens[best_ply];
                            for (size_t candidate_parent = 0;
                                 candidate_parent < data.fens.size(); ++candidate_parent) {
                                try {
                                    libchess::Position candidate_position(data.fens[candidate_parent]);
                                    libchess::Move candidate_move =
                                        candidate_position.parse_move(diagnostic_move);
                                    candidate_position.makemove(candidate_move);
                                    if (visual_fen_key(candidate_position.get_fen()) !=
                                        visual_fen_key(matched_state)) continue;
                                    trace_candidate(
                                        "REVERT_TRANSPOSE_EDGE", t, data.moves.size(),
                                        data.fens[candidate_parent], diagnostic_move,
                                        diagnostic_score, max_sd,
                                        diagnostic_y_from, diagnostic_y_to,
                                        "matched_ply=" + std::to_string(best_ply) +
                                        ";candidate_parent=" +
                                        std::to_string(candidate_parent) +
                                        ";preserved_parent=" + std::to_string(parent));
                                } catch (...) {
                                    // Not a legal edge from this state.
                                }
                            }
                            const PreservedMainline& preserved = *preserved_replay_mainline;
                            for (size_t candidate_parent = 0;
                                 candidate_parent < preserved.fens.size(); ++candidate_parent) {
                                try {
                                    libchess::Position candidate_position(
                                        preserved.fens[candidate_parent]);
                                    libchess::Move candidate_move =
                                        candidate_position.parse_move(diagnostic_move);
                                    candidate_position.makemove(candidate_move);
                                    if (visual_fen_key(candidate_position.get_fen()) !=
                                        visual_fen_key(matched_state)) continue;
                                    trace_candidate(
                                        "REVERT_PRESERVED_TRANSPOSE_EDGE", t,
                                        data.moves.size(),
                                        preserved.fens[candidate_parent], diagnostic_move,
                                        diagnostic_score, max_sd,
                                        diagnostic_y_from, diagnostic_y_to,
                                        "matched_ply=" + std::to_string(best_ply) +
                                        ";candidate_parent=" +
                                        std::to_string(candidate_parent) +
                                        ";preserved_parent=" + std::to_string(parent));
                                } catch (...) {
                                    // Not a legal edge from this preserved state.
                                }
                            }
                        }
                        save_tail_as_variation(parent, true);

                        // save_tail_as_variation observes the transient
                        // replay before the preserved line is restored.  If
                        // that tail is an exact state/path copy of the
                        // preserved mainline, remove only that replay copy;
                        // alternate legal continuations remain untouched.
                        auto replay_variations_it = data.variations.find(parent);
                        if (replay_variations_it != data.variations.end()) {
                            const PreservedMainline& preserved = *preserved_replay_mainline;
                            auto& replay_variations = replay_variations_it->second;
                            replay_variations.erase(
                                std::remove_if(replay_variations.begin(), replay_variations.end(),
                                               [&](const VariationData& variation) {
                                                   if (variation.moves.empty() ||
                                                       parent >= preserved.fens.size() ||
                                                       variation.fens.empty() ||
                                                       variation.fens.front() != preserved.fens[parent] ||
                                                       parent + variation.moves.size() > preserved.moves.size()) {
                                                       return false;
                                                   }
                                                   for (size_t offset = 0; offset < variation.moves.size(); ++offset) {
                                                       if (variation.moves[offset] != preserved.moves[parent + offset]) {
                                                           return false;
                                                       }
                                                       if (offset + 1 < variation.fens.size() &&
                                                           parent + offset + 1 < preserved.fens.size() &&
                                                           variation.fens[offset + 1] != preserved.fens[parent + offset + 1]) {
                                                           return false;
                                                       }
                                                   }
                                                   variation_trace_events.push_back(
                                                       "kind=preserved_replay_exact_path_suppressed;parent=" +
                                                       std::to_string(parent) + ";moves=" +
                                                       std::to_string(variation.moves.size()));
                                                   return true;
                                               }),
                                replay_variations.end());
                            if (replay_variations.empty()) data.variations.erase(parent);
                        }

                        PreservedMainline restored = std::move(*preserved_replay_mainline);
                        // Keep the restored source available for a second
                        // generic handoff: analysis can replace the branch
                        // again before the real continuation is emitted.
                        PreservedMainline preserved_source = restored;
                        const size_t preserved_video_count = restored.video_moves.size();
                        if (data.video_moves.size() > preserved_video_count) {
                            restored.video_moves.insert(restored.video_moves.end(),
                                                        data.video_moves.begin() + preserved_video_count,
                                                        data.video_moves.end());
                        }
                        if (data.video_fens.size() > restored.video_fens.size()) {
                            restored.video_fens.insert(restored.video_fens.end(),
                                                      data.video_fens.begin() + restored.video_fens.size(),
                                                      data.video_fens.end());
                        }
                        if (data.video_timestamps.size() > restored.video_timestamps.size()) {
                            restored.video_timestamps.insert(restored.video_timestamps.end(),
                                                             data.video_timestamps.begin() + restored.video_timestamps.size(),
                                                             data.video_timestamps.end());
                        }
                        data.moves = std::move(restored.moves);
                        data.timestamps = std::move(restored.timestamps);
                        data.fens = std::move(restored.fens);
                        data.clocks = std::move(restored.clocks);
                        data.video_fens = std::move(restored.video_fens);
                        data.video_timestamps = std::move(restored.video_timestamps);
                        data.video_moves = std::move(restored.video_moves);
                        move_scores = std::move(restored.move_scores);
                        move_video_indices = std::move(restored.move_video_indices);
                        preserved_replay_mainline = std::move(preserved_source);
                        pos_ptr_ = std::make_unique<libchess::Position>(data.fens.back());
                        revert_mgr.resize_history(best_history_idx + 1);
                        revert_history_ply_counts.resize(best_history_idx + 1);
                        data.video_timestamps.push_back(t);
                        data.video_fens.push_back(data.fens.back());
                        data.video_moves.push_back("REVERT");
                        trace_revert_applied(t, data.moves.size(), max_sd, "exact_revert");
                        first_move_after_revert = true;
                        last_moved_clock = {data.clocks.back().white_time, data.clocks.back().black_time};
                        reset_stale_branch_state();
                        std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);
                        trace_candidate("PRESERVED_MAINLINE_RESTORED", t, data.moves.size(), data.fens.back(), "", 0.0, max_sd,
                                        0.0, 0.0, "parent=" + std::to_string(parent));
                        continue;
                    }
                    int reverted_count = static_cast<int>(data.moves.size() - best_ply);
                    const bool durable_replay_candidate =
                        !preserved_replay_mainline &&
                        reverted_count >= 20 &&
                        best_ply < data.timestamps.size() &&
                        (t - data.timestamps[best_ply]) >= 60.0;
                    const bool postgame_tail_must_be_preserved =
                        postgame_branch_start_ply &&
                        best_ply < *postgame_branch_start_ply &&
                        data.moves.size() > *postgame_branch_start_ply &&
                        durable_replay_candidate;
                    bool transient_single_ply_revert = false;
                    if (reverted_count == 1) {
                        constexpr double kSinglePlyRevertBounceWindowSeconds = 0.75;
                        for (size_t lookahead = i + 1; lookahead < candidates.size(); ++lookahead) {
                            const CandidateFrame& future_cf = candidates[lookahead];
                            if (future_cf.t - t > kSinglePlyRevertBounceWindowSeconds) {
                                break;
                            }
                            static thread_local cv::Mat future_latest_diff;
                            GPUAccelerator::absdiff(future_cf.board_gray, revert_mgr.get_latest_gray(), future_latest_diff);
                            if (cv::mean(future_latest_diff)[0] < kRevertFullImageMeanDiff) {
                                transient_single_ply_revert = true;
                                break;
                            }
                        }
                    }
                    if (transient_single_ply_revert) {
                        if (debug_level_ != DebugLevel::None) {
                            log_info(utils::ts(elapsed()) + " [Debug] " + std::to_string(t) +
                                     "s: ignored transient one-ply revert that bounced back to the latest board state");
                        }
                    } else if (ignore_replay_reverts_before_ply &&
                               !postgame_tail_must_be_preserved &&
                               best_ply < *ignore_replay_reverts_before_ply) {
                        continue;
                    } else if (postgame_branch_start_ply &&
                               best_ply < *postgame_branch_start_ply &&
                               data.moves.size() > *postgame_branch_start_ply &&
                               !postgame_tail_must_be_preserved) {
                        ++branch_counter;
                        log_info("\n" + utils::ts(elapsed()) + " --- ANALYSIS REVERT at " + std::to_string(t) + "s (post-game branch ended) ---");
                        log_info(utils::ts(elapsed()) + " Preserving main line through ply " +
                                 std::to_string(*postgame_branch_start_ply) +
                                 " and saving the post-game analysis tail as a variation.");
                        demote_tail_to_variation(*postgame_branch_start_ply, true);
                        ignore_replay_reverts_before_ply = postgame_branch_start_ply;
                        postgame_replay_mode = true;
                        std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);
                        continue;
                    } else {
                        ++branch_counter;
                        log_info("\n" + utils::ts(elapsed()) + " --- ANALYSIS REVERT at " + std::to_string(t) + "s (board matched past state) ---");
                        log_info(utils::ts(elapsed()) + " Snapped back to ply " + std::to_string(best_ply) + " (Branch " + std::to_string(branch_counter) + ")");
                        const bool durable_replay = durable_replay_candidate;
                        const bool rewind_to_preserved_parent =
                            preserved_replay_mainline &&
                            best_ply == preserved_replay_mainline->replay_parent &&
                            reverted_count > 0;
                        if (!postgame_branch_start_ply &&
                            tail_has_unreliable_moved_clock(best_ply)) {
                            // A branch whose first moved-side clock becomes
                            // unreadable after a settled low-time position is
                            // an analysis/UI handoff, not a new game line.
                            // Keep this boundary derived from observed state;
                            // no fixture or move identity is involved.
                            postgame_branch_start_ply = best_ply;
                            ignore_replay_reverts_before_ply = best_ply;
                            postgame_replay_mode = true;
                            postgame_boundary_from_clock_gap = true;
                            log_info(utils::ts(elapsed()) +
                                     " Marking ply " + std::to_string(best_ply) +
                                     " as an analysis boundary because the branch lost its moved-side clock reading.");
                        }
                        const bool preserve_only_postgame_boundary =
                            postgame_tail_must_be_preserved && postgame_branch_start_ply;
                        if (preserve_only_postgame_boundary) {
                            // Keep the already verified prefix as the main
                            // line, while retaining the just-observed tail as
                            // a variation before following the replay.
                            demote_tail_to_variation(*postgame_branch_start_ply, true);
                        }
                        if (durable_replay) {
                            PreservedMainline preserved;
                            const size_t preserved_move_count = preserve_only_postgame_boundary
                                ? *postgame_branch_start_ply : data.moves.size();
                            preserved.moves.assign(data.moves.begin(), data.moves.begin() + preserved_move_count);
                            preserved.timestamps.assign(data.timestamps.begin(), data.timestamps.begin() + preserved_move_count);
                            preserved.fens.assign(data.fens.begin(), data.fens.begin() + preserved_move_count + 1);
                            preserved.clocks.assign(data.clocks.begin(), data.clocks.begin() + preserved_move_count + 1);
                            preserved.video_fens = data.video_fens;
                            preserved.video_timestamps = data.video_timestamps;
                            preserved.video_moves = data.video_moves;
                            preserved.move_scores.assign(move_scores.begin(), move_scores.begin() + preserved_move_count);
                            preserved.move_video_indices.assign(move_video_indices.begin(), move_video_indices.begin() + preserved_move_count);
                            preserved.replay_parent = best_ply;
                            preserved_replay_mainline = std::move(preserved);
                            ignore_replay_reverts_before_ply = best_ply;
                            postgame_replay_mode = true;
                            log_info(utils::ts(elapsed()) + " Preserving the durable main line and reducing this long replay as a variation.");
                        }
                        bool saved_reverted_branch = false;
                        if (reverted_count > 0 && !durable_replay) {
                            log_info(utils::ts(elapsed()) + "   Saving " + std::to_string(reverted_count) + " analysis plies as a variation.");

                            if (rewind_to_preserved_parent) {
                                // The preserved move is the verified main
                                // line; the alternate branch will be saved
                                // when the preserved post-move board returns.
                                saved_reverted_branch = true;
                            }

                            const double first_reverted_score = best_ply < move_scores.size() ? move_scores[best_ply] : 0.0;
                            const double first_reverted_lifetime =
                                best_ply < data.timestamps.size() ? (t - data.timestamps[best_ply]) : 0.0;
                            const bool stable_single_ply_variation =
                                first_reverted_score >= 57.0 && first_reverted_lifetime >= 0.75;
                            if (!rewind_to_preserved_parent && (reverted_count > 1 || stable_single_ply_variation)) {
                                VariationData var_data;
                                var_data.moves.assign(data.moves.begin() + best_ply, data.moves.end());
                                var_data.timestamps.assign(data.timestamps.begin() + best_ply, data.timestamps.end());
                                var_data.fens.assign(data.fens.begin() + best_ply, data.fens.end() - 1);
                                var_data.scores.assign(move_scores.begin() + best_ply, move_scores.end());
                                var_data.clocks.assign(data.clocks.begin() + best_ply + 1, data.clocks.end());
                                if (best_ply < data.clocks.size()) {
                                    const ClockInfo branch_clock = data.clocks[best_ply];
                                    for (ClockInfo& variation_clock : var_data.clocks) {
                                        variation_clock.white_time = branch_clock.white_time;
                                        variation_clock.black_time = branch_clock.black_time;
                                        variation_clock.active = branch_clock.active;
                                    }
                                }
                                add_variation(best_ply, std::move(var_data));
                                saved_reverted_branch = true;
                            }
                        }

                        if (!saved_reverted_branch && !durable_replay) {
                            suppress_timeline_for_move_range(best_ply, data.moves.size());
                        }

                        data.moves.resize(best_ply);
                        data.timestamps.resize(best_ply);
                        data.fens.resize(best_ply + 1);
                        data.clocks.resize(best_ply + 1);
                        move_scores.resize(best_ply);
                        move_video_indices.resize(best_ply);
                        if (!rewind_to_preserved_parent) {
                            revert_mgr.resize_history(best_history_idx + 1);
                            revert_history_ply_counts.resize(best_history_idx + 1);
                        }

                        // Rebuild libchess position from the correct FEN
                        pos_ptr_ = std::make_unique<libchess::Position>(data.fens.back());

                        data.video_timestamps.push_back(t);
                        data.video_fens.push_back(data.fens.back());
                        data.video_moves.push_back("REVERT");
                        trace_revert_applied(t, data.moves.size(), max_sd, "analysis_revert");
                        first_move_after_revert = true;
                        last_moved_clock = {data.clocks.back().white_time, data.clocks.back().black_time};
                        reset_stale_branch_state();
                        std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);
                        if (returns_to_postgame_parent) {
                            log_info(utils::ts(elapsed()) + " Cleared the post-game branch boundary after returning to its exact parent ply " +
                                     std::to_string(*postgame_branch_start_ply));
                            if (!postgame_boundary_from_clock_gap) {
                                postgame_branch_start_ply.reset();
                                ignore_replay_reverts_before_ply.reset();
                                postgame_replay_mode = false;
                            }
                        }

                        continue;
                    }
                }
        }

        bool historical_handoff_applied = false;
        bool historical_handoff_clock_override = false;
        bool historical_handoff_repeated_branch = false;
        std::optional<size_t> historical_handoff_source_ply;
        std::vector<RebasedContinuation> historical_rebased_moves;
        std::vector<std::string> historical_stale_tail;
        std::optional<ClockInfo> historical_replaced_clock;
        std::string historical_rebase_diagnostic = "not_considered";
        std::string historical_handoff_result = "none";
        double historical_handoff_active_score = 0.0;
        double historical_handoff_active_mean = 0.0;
        double historical_handoff_source_mean = 0.0;
        double historical_handoff_source_score = 0.0;
        double historical_handoff_y_from = 0.0;
        double historical_handoff_y_to = 0.0;
        std::string historical_handoff_move;

        // A streamer can undo one or more analysis moves and immediately play
        // a replacement before the exact source board is emitted as a stable
        // frame.  In that case exact revert detection quite correctly returns
        // no match: the observed board is source + one new legal move.  Search
        // the retained history for that general transition only after exact
        // reverts have had first refusal.
        if (revert_mgr.history_size() > 1 && data.moves.size() > 0) {
            const ExtractedMoveScore active_visual_best = MoveScorer::score_moves_for_board(
                libchess::Position(data.fens.back()), sq_means);
            const double active_full_mean = cv::mean(diff)[0];
            historical_handoff_active_score = active_visual_best.score;
            historical_handoff_active_mean = active_full_mean;

            struct HistoricalTransition {
                size_t history_index = 0;
                size_t source_ply = 0;
                ExtractedMoveScore move;
                double source_full_mean = std::numeric_limits<double>::max();
                double quality = -std::numeric_limits<double>::max();
            };
            std::optional<HistoricalTransition> selected_transition;

            const size_t history_count = revert_mgr.history_size();
            const size_t first_history_index = history_count > 64 ? history_count - 64 : 0;
            for (size_t history_index = history_count; history_index-- > first_history_index;) {
                if (history_index >= revert_history_ply_counts.size()) continue;
                const size_t source_ply = revert_history_ply_counts[history_index];

                // Once a stable post-game replay boundary has been recorded,
                // an older historical source belongs to the preserved main
                // line.  Letting the replacement search cross that boundary
                // would incorrectly turn a later replay into a new main-line
                // edit and could discard an already verified suffix.
                if (postgame_branch_start_ply &&
                    source_ply < *postgame_branch_start_ply) {
                    continue;
                }

                // The active position is already handled by the normal reducer.
                // A handoff is meaningful only when at least one accepted tail
                // move would be demoted into a variation.
                if (source_ply >= data.moves.size() || source_ply + 1 >= data.moves.size() ||
                    source_ply >= data.fens.size()) {
                    continue;
                }

                cv::Mat historical_diff;
                GPUAccelerator::absdiff(board_gray, revert_mgr.get_history_gray(history_index), historical_diff);
                const double historical_full_mean = cv::mean(historical_diff)[0];
                const std::vector<double> historical_sq_means =
                    compute_all_square_means(historical_diff, *geo_, margin_h_, margin_w_);
                const ExtractedMoveScore historical_move = MoveScorer::score_moves_for_board(
                    libchess::Position(data.fens[source_ply]), historical_sq_means);
                const bool is_postgame_parent_transition =
                    postgame_branch_start_ply &&
                    source_ply == *postgame_branch_start_ply &&
                    data.moves.size() > *postgame_branch_start_ply;
                if (is_postgame_parent_transition &&
                    historical_move.score < 100.0) {
                    continue;
                }
                if (historical_move.from_sq < 0 || historical_move.to_sq < 0 ||
                    historical_move.score <= kMinMoveScore ||
                    historical_move.score + 12.0 < active_visual_best.score) {
                    continue;
                }

                // A stale branch can be visually closer to the current frame
                // than the retained source even when the source transition is
                // much more strongly registered.  Do not discard that source
                // solely on whole-board distance: require a decisive legal
                // score advantage and keep the distance bound tight so this
                // remains a source handoff, not an arbitrary history jump.
                const bool decisive_historical_transition =
                    preserved_replay_mainline &&
                    source_ply == preserved_replay_mainline->replay_parent + 1 &&
                    historical_move.score >= active_visual_best.score + 30.0 &&
                    historical_full_mean < active_full_mean + 2.0;
                if (historical_full_mean + 0.5 >= active_full_mean &&
                    !decisive_historical_transition) {
                    continue;
                }

                const char* historical_from = utils::sq_name(historical_move.from_sq);
                const char* historical_to = utils::sq_name(historical_move.to_sq);
                const double historical_y_from =
                    validation::check_yellowness(board_bgr, *geo_, historical_from);
                const double historical_y_to =
                    validation::check_yellowness(board_bgr, *geo_, historical_to);
                const bool decisive_preserved_source =
                    decisive_historical_transition && historical_move.score >= 120.0;
                if (trace_stream.is_open() && preserved_replay_mainline &&
                    source_ply == preserved_replay_mainline->replay_parent + 1) {
                    trace_candidate("HANDOFF_GATE", t, data.moves.size(), data.fens[source_ply],
                                    std::string(historical_from) + historical_to,
                                    historical_move.score, max_sd,
                                    historical_y_from, historical_y_to,
                                    "active_score=" + std::to_string(active_visual_best.score) +
                                    ";active_mean=" + std::to_string(active_full_mean) +
                                    ";source_mean=" + std::to_string(historical_full_mean) +
                                    ";decisive=" + (decisive_historical_transition ? "1" : "0") +
                                    ";strong_preserved=" + (decisive_preserved_source ? "1" : "0") +
                                    ";yellow_ok=" +
                                        ((historical_y_from >= 25.0 && historical_y_to >= 25.0 &&
                                          historical_y_from + historical_y_to >= 70.0) ? "1" : "0"));
                }
                if ((historical_y_from < 25.0 || historical_y_to < 25.0 ||
                     historical_y_from + historical_y_to < 70.0) &&
                    !decisive_preserved_source) {
                    continue;
                }

                const double quality = historical_move.score +
                    (active_full_mean - historical_full_mean) * 4.0;
                if (selected_transition && quality <= selected_transition->quality) {
                    continue;
                }

                selected_transition = HistoricalTransition{
                    history_index,
                    source_ply,
                    historical_move,
                    historical_full_mean,
                    quality};
                historical_handoff_clock_override =
                    (historical_move.score >= 100.0 && historical_y_from + historical_y_to >= 90.0) ||
                    decisive_preserved_source;
                historical_handoff_source_mean = historical_full_mean;
                historical_handoff_source_score = historical_move.score;
                historical_handoff_y_from = historical_y_from;
                historical_handoff_y_to = historical_y_to;
                historical_handoff_move = std::string(historical_from) + historical_to;
                if (historical_move.promotion != '\0') {
                    historical_handoff_move += historical_move.promotion;
                }
            }

            trace_candidate("HANDOFF_SCAN", t, data.moves.size(), pos_ptr_->get_fen(),
                            historical_handoff_move, historical_handoff_source_score, max_sd,
                            historical_handoff_y_from, historical_handoff_y_to,
                            "active_score=" + std::to_string(historical_handoff_active_score) +
                            ";active_mean=" + std::to_string(historical_handoff_active_mean) +
                            ";source_mean=" + std::to_string(historical_handoff_source_mean) +
                            ";source_ply=" + (selected_transition
                                ? std::to_string(selected_transition->source_ply) : "none") +
                            ";clock_override=" + (historical_handoff_clock_override ? "1" : "0"));

            if (selected_transition) {
                const size_t source_ply = selected_transition->source_ply;
                const char* historical_from = utils::sq_name(selected_transition->move.from_sq);
                const char* historical_to = utils::sq_name(selected_transition->move.to_sq);
                std::string historical_uci = std::string(historical_from) + historical_to;
                if (selected_transition->move.promotion != '\0') {
                    historical_uci += selected_transition->move.promotion;
                }
                historical_handoff_result = "applied";

                // If the replacement is byte-for-byte the first move of the
                // tail being demoted, the observed board is replaying an
                // already-recorded analysis branch.  Do not promote that
                // replay back onto the main line.  This is deliberately
                // expressed only in terms of retained game state and the
                // visual legal transition; it has no fixture knowledge.
                const bool repeats_stale_branch =
                    source_ply < data.moves.size() && data.moves[source_ply] == historical_uci;
                historical_handoff_repeated_branch = repeats_stale_branch;

                // A replacement can arrive after several very fast moves
                // were already displayed.  Preserve only a short, recent
                // continuation that remains legal after the replacement.
                // The age bound prevents an old analysis line from being
                // promoted long after the UI has moved to a new position.
                if (!repeats_stale_branch && source_ply + 1 < data.moves.size() &&
                    source_ply < data.fens.size()) {
                    historical_rebase_diagnostic = "eligible";
                    historical_stale_tail.assign(data.moves.begin() + source_ply, data.moves.end());
                    for (size_t tail_index = 0; tail_index < historical_stale_tail.size(); ++tail_index) {
                        const size_t original_ply = source_ply + tail_index;
                        historical_rebase_diagnostic += ";tail" + std::to_string(tail_index) + "=" +
                            historical_stale_tail[tail_index] + ":score=" +
                            (original_ply < move_scores.size() ? std::to_string(move_scores[original_ply]) : "missing");
                    }
                    try {
                        libchess::Position replacement_position(data.fens[source_ply]);
                        libchess::Move replacement_move = replacement_position.parse_move(historical_uci);
                        replacement_position.makemove(replacement_move);
                        std::string rebased_fen = replacement_position.get_fen();

                        std::vector<bool> used_tail(historical_stale_tail.size(), false);
                        constexpr size_t kMaxRebasedContinuationPlies = 2;
                        constexpr double kMaxRebasedContinuationAgeSeconds = 12.0;
                        for (size_t continuation = 0;
                             continuation < kMaxRebasedContinuationPlies;
                             ++continuation) {
                            std::optional<size_t> best_tail_index;
                            double best_tail_score = kMinCoalescedFollowupScore;
                            std::string next_fen;
                            for (size_t tail_index = 0; tail_index < historical_stale_tail.size(); ++tail_index) {
                                if (continuation == 0 && tail_index == 0) continue;
                                if (used_tail[tail_index]) continue;
                                const size_t original_ply = source_ply + tail_index;
                                if (original_ply >= move_scores.size() ||
                                    move_scores[original_ply] < kMinCoalescedFollowupScore ||
                                    original_ply >= data.timestamps.size() ||
                                    t - data.timestamps[original_ply] > kMaxRebasedContinuationAgeSeconds) {
                                    continue;
                                }
                                try {
                                    libchess::Position trial(rebased_fen);
                                    libchess::Move candidate_move = trial.parse_move(historical_stale_tail[tail_index]);
                                    trial.makemove(candidate_move);
                                    if (!best_tail_index || move_scores[original_ply] > best_tail_score) {
                                        best_tail_index = tail_index;
                                        best_tail_score = move_scores[original_ply];
                                        next_fen = trial.get_fen();
                                    }
                                } catch (...) {
                                    // The old move is not legal after the
                                    // replacement or after the rebased prefix.
                                }
                            }
                            if (!best_tail_index) break;
                            used_tail[*best_tail_index] = true;
                            const size_t original_ply = source_ply + *best_tail_index;
                            historical_rebased_moves.push_back({
                                historical_stale_tail[*best_tail_index],
                                data.timestamps[original_ply],
                                data.clocks[original_ply + 1],
                                move_scores[original_ply],
                                move_video_indices[original_ply],
                                original_ply});
                            rebased_fen = next_fen;
                        }
                    } catch (...) {
                        historical_rebase_diagnostic = "position_or_clock_exception";
                        historical_rebased_moves.clear();
                    }
                }

                trace_candidate("REBASE_SCAN", t, data.moves.size(), data.fens.back(), historical_uci,
                                historical_handoff_source_score, max_sd, 0.0, 0.0,
                                "diagnostic=" + historical_rebase_diagnostic +
                                ";tail_count=" + std::to_string(historical_stale_tail.size()) +
                                ";selected_count=" + std::to_string(historical_rebased_moves.size()));

                ++branch_counter;
                log_info("\n" + utils::ts(elapsed()) +
                         " --- ANALYSIS REPLACEMENT at " + std::to_string(t) +
                         "s (historical source matched legal move) ---");
                log_info(utils::ts(elapsed()) + " Demoting stale tail to ply " +
                         std::to_string(source_ply) + " and applying " + historical_uci +
                         " (Branch " + std::to_string(branch_counter) + ")");

                if (repeats_stale_branch) {
                    if (demote_tail_to_variation(source_ply)) {
                        postgame_branch_start_ply = source_ply;
                        ignore_replay_reverts_before_ply = source_ply;
                        postgame_replay_mode = true;
                        postgame_boundary_from_clock_gap = false;
                        data.video_timestamps.push_back(t);
                        data.video_fens.push_back(data.fens.back());
                        data.video_moves.push_back("REVERT");
                        trace_revert_applied(t, data.moves.size(), max_sd, "repeated_branch");
                        reset_stale_branch_state();
                        std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);
                        historical_handoff_result = "repeated_branch_suppressed";
                        trace_candidate("REPEATED_BRANCH_HANDOFF", t, data.moves.size(), data.fens.back(),
                                        historical_uci, selected_transition->move.score, max_sd,
                                        validation::check_yellowness(board_bgr, *geo_, historical_from),
                                        validation::check_yellowness(board_bgr, *geo_, historical_to),
                                        "source_ply=" + std::to_string(source_ply) +
                                        ";history_index=" + std::to_string(selected_transition->history_index) +
                                        ";source_full_mean=" + std::to_string(selected_transition->source_full_mean) +
                                        ";active_score=" + std::to_string(historical_handoff_active_score));
                    }
                    continue;
                }

                if (source_ply + 1 < data.clocks.size()) {
                    historical_replaced_clock = data.clocks[source_ply + 1];
                }
                if (demote_tail_to_variation(source_ply)) {
                    const std::string historical_source_fen = data.fens[source_ply];
                    discard_variations_containing_state(historical_source_fen);
                    for (const RebasedContinuation& continuation : historical_rebased_moves) {
                        if (continuation.video_index && *continuation.video_index < data.video_moves.size()) {
                            data.video_moves[*continuation.video_index] = "REVERT";
                        }
                    }
                    if (!historical_rebased_moves.empty() && !historical_stale_tail.empty()) {
                        auto variation_it = data.variations.find(source_ply);
                        if (variation_it != data.variations.end()) {
                            variation_it->second.erase(
                                std::remove_if(variation_it->second.begin(), variation_it->second.end(),
                                               [&](const VariationData& variation) {
                                                   return variation.moves == historical_stale_tail;
                                               }),
                                variation_it->second.end());
                            if (variation_it->second.empty()) {
                                data.variations.erase(variation_it);
                            }
                        }
                    }
                    // A replacement transition uses ordinary clock
                    // validation.  A separately computed, strong visual
                    // handoff may narrowly authorize the same stale-clock
                    // exception used for a registered move.
                    first_move_after_revert = false;
                    historical_handoff_applied = true;
                    historical_handoff_source_ply = source_ply;
                    data.video_timestamps.push_back(t);
                    data.video_fens.push_back(data.fens.back());
                    data.video_moves.push_back("REVERT");
                    trace_revert_applied(t, data.moves.size(), max_sd, "historical_handoff");
                    reset_stale_branch_state();
                    std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);

                    // The same observed board must now be scored against the
                    // selected source snapshot, not the stale branch tail.
                    GPUAccelerator::absdiff(board_gray, revert_mgr.get_latest_gray(), diff);
                    sq_means = compute_all_square_means(diff, *geo_, margin_h_, margin_w_);
                    max_sd = 0.0;
                    for (double square_diff : sq_means) {
                        max_sd = std::max(max_sd, square_diff);
                    }
                    trace_candidate("HISTORICAL_HANDOFF", t, data.moves.size(), data.fens.back(),
                                    historical_uci, selected_transition->move.score, max_sd,
                                    validation::check_yellowness(board_bgr, *geo_, historical_from),
                                    validation::check_yellowness(board_bgr, *geo_, historical_to),
                                    "source_ply=" + std::to_string(source_ply) +
                                    ";history_index=" + std::to_string(selected_transition->history_index) +
                                    ";source_full_mean=" + std::to_string(selected_transition->source_full_mean) +
                                    ";active_score=" + std::to_string(historical_handoff_active_score) +
                                    ";clock_override=" + (historical_handoff_clock_override ? "1" : "0"));
                }
            }
        }

        bool extracted_in_frame = false;
        bool all_validations_passed = true;
        std::vector<bool> consumed_squares = unresolved_consumed_squares;
        const double initial_frame_t = t;
        std::string last_validation_move;
        std::string last_validation_from;
        std::string last_validation_to;
        std::string last_validation_before_fen;
        double last_validation_score = 0.0;
        std::string validation_rejection_reason;
        std::string validation_rejection_detail;

        // A clock-vetoed move is not promoted to the verified position, but
        // its board image can still be the parent of a short analysis line.
        // Observe that line independently so a later reply is scored against
        // the speculative position rather than the retained main-line FEN.
        auto observe_clock_vetoed_branch = [&]() {
            try {
            if (!clock_vetoed_tail.empty() &&
                t - clock_vetoed_started_at > 20.0) {
                trace_candidate(
                    "CLOCK_VETOED_BRANCH_RESET", t, data.moves.size(),
                    clock_vetoed_current_fen, "", 0.0, 0.0, 0.0, 0.0,
                    "reason=expired;age=" +
                    std::to_string(t - clock_vetoed_started_at));
                clock_vetoed_tail.clear();
                clock_vetoed_base_ply.reset();
                clock_vetoed_base_fen.clear();
                clock_vetoed_current_fen.clear();
                clock_vetoed_last_board_gray.release();
                clock_vetoed_started_at = 0.0;
            }
            if (clock_vetoed_tail.empty() ||
                clock_vetoed_current_fen.empty() ||
                clock_vetoed_last_board_gray.empty()) {
                return;
            }

            cv::Mat speculative_diff;
            GPUAccelerator::absdiff(board_gray, clock_vetoed_last_board_gray, speculative_diff);
            const std::vector<double> speculative_sq_means =
                compute_all_square_means(speculative_diff, *geo_, margin_h_, margin_w_);
            double speculative_max_sd = 0.0;
            for (double square_diff : speculative_sq_means) {
                speculative_max_sd = std::max(speculative_max_sd, square_diff);
            }
            if (speculative_max_sd < 8.0) return;

            libchess::Position speculative_position(clock_vetoed_current_fen);
            const ExtractedMoveScore speculative_best =
                MoveScorer::score_moves_for_board(speculative_position, speculative_sq_means);
            constexpr double kSpeculativeFollowupScore = 80.0;
            if (speculative_best.score < kSpeculativeFollowupScore ||
                speculative_best.from_sq < 0 || speculative_best.to_sq < 0) {
                return;
            }

            const char* speculative_from = utils::sq_name(speculative_best.from_sq);
                const char* speculative_to = utils::sq_name(speculative_best.to_sq);
                const double speculative_y_from =
                    validation::check_yellowness(board_bgr, *geo_, speculative_from);
            const double speculative_y_to =
                validation::check_yellowness(board_bgr, *geo_, speculative_to);
            if (speculative_y_from < 25.0 || speculative_y_to < 25.0 ||
                speculative_y_from + speculative_y_to < 70.0) {
                return;
            }

                std::string speculative_uci = std::string(speculative_from) + speculative_to;
                if (speculative_best.promotion != '\0') {
                    speculative_uci += speculative_best.promotion;
                }
                try {
                    libchess::Move speculative_move =
                        speculative_position.parse_move(speculative_uci);
                    speculative_position.makemove(speculative_move);
                    const std::string speculative_after_fen = speculative_position.get_fen();

                    const ClockInfo branch_clock = data.clocks.back();
                    clock_vetoed_tail.push_back({
                        speculative_uci,
                        clock_vetoed_current_fen,
                        speculative_after_fen,
                        t,
                        speculative_best.score});
                    clock_vetoed_current_fen = speculative_after_fen;
                    clock_vetoed_last_board_gray = board_gray.clone();
                    trace_candidate(
                        "CLOCK_VETOED_BRANCH_MOVE", t, data.moves.size(),
                        clock_vetoed_tail.back().before_fen, speculative_uci,
                        speculative_best.score, speculative_max_sd,
                    speculative_y_from, speculative_y_to,
                    "base_ply=" + std::to_string(clock_vetoed_base_ply.value_or(data.moves.size())) +
                    ";source=speculative_frame_pair;branch_clock=" +
                    branch_clock.white_time + "/" + branch_clock.black_time);
                } catch (...) {
                    // A visual transition that cannot be applied to the
                    // separate speculative position is not a continuation.
                }
            } catch (const cv::Exception& error) {
                trace_candidate(
                    "CLOCK_VETOED_BRANCH_RESET", t, data.moves.size(),
                    clock_vetoed_current_fen, "", 0.0, 0.0, 0.0, 0.0,
                    "reason=opencv_exception;message=" + std::string(error.what()));
                clock_vetoed_tail.clear();
                clock_vetoed_base_ply.reset();
                clock_vetoed_base_fen.clear();
                clock_vetoed_current_fen.clear();
                clock_vetoed_last_board_gray.release();
                clock_vetoed_started_at = 0.0;
            } catch (...) {
                clock_vetoed_tail.clear();
                clock_vetoed_base_ply.reset();
                clock_vetoed_base_fen.clear();
                clock_vetoed_current_fen.clear();
                clock_vetoed_last_board_gray.release();
                clock_vetoed_started_at = 0.0;
            }
        };

        // Loop to extract potentially multiple overlapping moves from a single coalesced frame
        while (true) {
            observe_clock_vetoed_branch();
            last_validation_before_fen = pos_ptr_->get_fen();
            const bool postgame_replay_branch =
                postgame_replay_mode && postgame_branch_start_ply && data.moves.size() >= *postgame_branch_start_ply;
            if (extracted_in_frame && t <= initial_frame_t + 0.05) {
                trace_candidate("COALESCED_STOP", t, data.moves.size(), pos_ptr_->get_fen(), "", 0.0, max_sd,
                                0.0, 0.0, "reason=unchanged_visual_timestamp");
                break;
            }
            // Apply consumed_squares to the current sq_means just in case
            for (int sq = 0; sq < 64; ++sq) {
                if (consumed_squares[sq]) sq_means[sq] = 0.0;
            }

            // Score moves using libchess legal move generation
            auto score_start = std::chrono::steady_clock::now();
            auto best = this->score_moves_for_board(sq_means);
            ++score_calls;
            score_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - score_start).count();
            if (best.from_sq >= 0 && best.to_sq >= 0) {
                diagnostic_evidence_context.score_from_square_diff = sq_means[best.from_sq];
                diagnostic_evidence_context.score_to_square_diff = sq_means[best.to_sq];
                diagnostic_evidence_context.score_adjustment = best.score -
                    diagnostic_evidence_context.score_from_square_diff -
                    diagnostic_evidence_context.score_to_square_diff;
            }

            if (postgame_replay_branch && !postgame_boundary_from_clock_gap &&
                !historical_handoff_applied &&
                best.score < 100.0) {
                validation_rejection_reason = "waiting_for_strong_parent_transition";
                all_validations_passed = false;
                trace_candidate("VALIDATION_REJECTED", t, data.moves.size(), pos_ptr_->get_fen(), "",
                                best.score, max_sd, 0.0, 0.0,
                                "candidate_index=" + std::to_string(i) +
                                ";reason=" + validation_rejection_reason);
                break;
            }

                
            double min_move_score = extracted_in_frame ? 15.0 : kMinMoveScore;
            diagnostic_evidence_context.score_threshold_checked = true;
            diagnostic_evidence_context.minimum_score_threshold = min_move_score;
            diagnostic_evidence_context.score_threshold_passed =
                best.from_sq >= 0 && best.score > min_move_score;
            if (best.from_sq < 0) {
                diagnostic_evidence_context.score_threshold_decision = "no_legal_candidate";
            } else if (best.score <= min_move_score) {
                diagnostic_evidence_context.score_threshold_decision = "below_minimum";
            } else {
                diagnostic_evidence_context.score_threshold_decision = "passed";
            }
            if (best.score <= min_move_score || best.from_sq < 0) {
                bool found_check_response = false;
                if (postgame_replay_branch && pos_ptr_->in_check()) {
                    int best_response_from = -1;
                    int best_response_to = -1;
                    double best_response_score = 0.0;
                    for (const auto& legal_move : pos_ptr_->legal_moves()) {
                        const int candidate_from = static_cast<int>(static_cast<unsigned int>(legal_move.from()));
                        const int candidate_to = static_cast<int>(static_cast<unsigned int>(legal_move.to()));
                        const std::array<char, 64> board_map = utils::expand_fen(pos_ptr_->get_fen());
                        const char candidate_piece = board_map[candidate_from];
                        if (std::tolower(static_cast<unsigned char>(candidate_piece)) != 'k') {
                            continue;
                        }

                        const char* candidate_from_name = utils::sq_name(candidate_from);
                        const char* candidate_to_name = utils::sq_name(candidate_to);
                        const double y_from = validation::check_yellowness(board_bgr, *geo_, candidate_from_name);
                        const double y_to = validation::check_yellowness(board_bgr, *geo_, candidate_to_name);
                        if (y_from < 30.0 || y_to < 30.0 || (y_from + y_to) < 70.0) {
                            continue;
                        }

                        const double response_score = y_from + y_to + sq_means[candidate_to];
                        if (response_score > best_response_score) {
                            best_response_score = response_score;
                            best_response_from = candidate_from;
                            best_response_to = candidate_to;
                        }
                    }
                    if (best_response_from >= 0) {
                        best.from_sq = best_response_from;
                        best.to_sq = best_response_to;
                        best.score = best_response_score;
                        best.promotion = '\0';
                        found_check_response = true;
                    }
                }
                if (!found_check_response) {
                    trace_candidate(
                        "SCORE_THRESHOLD_REJECTED", t, data.moves.size(), pos_ptr_->get_fen(),
                        "", best.score, max_sd, diagnostic_y_from, diagnostic_y_to,
                        "candidate_index=" + std::to_string(i) +
                        ";threshold=" + std::to_string(min_move_score) +
                        ";decision=" + diagnostic_evidence_context.score_threshold_decision);
                    break; // No more moves in this frame
                }
            }
            if (extracted_in_frame && (consumed_squares[best.from_sq] || consumed_squares[best.to_sq])) {
                break;
            }

            // ── Move settling: peek ahead to confirm the move has settled ──
            // Always peek ahead to capture the peak of the animation and prevent ghost diffs.
            constexpr double kMaxSettleWindowSeconds = 0.75;
            // A nearby square can win by a few grayscale points while the
            // piece is still moving.  Retargeting on that small fluctuation
            // turns a valid registered slider move into a neighboring legal
            // move.  Require a meaningful improvement before changing the
            // endpoint; the yellow-square checks below remain authoritative.
            constexpr double kMinSettleRetargetScoreGain = 8.0;
            const double settle_start_t = t;
            const double initial_best_score = best.score;
            double visual_move_t = t;
            const auto settle_already_accepted = [&]() {
                return diagnostic_evidence_context.settle_decision.rfind("accepted_", 0) == 0;
            };
            while (true) {
                bool found_settle = false;
                CandidateFrame settle_cf;
                
                if (i + 1 < candidates.size()) {
                    settle_cf = candidates[i + 1];
                    found_settle = true;
                } else if (current_chunk + 1 < total_chunks) {
                    if (mapper.peek_next_chunk_front(current_chunk + 1, settle_cf, cancel_flag)) {
                        found_settle = true;
                    }
                }

                if (found_settle) {
                    if (settle_cf.t - settle_start_t > kMaxSettleWindowSeconds) {
                        if (!settle_already_accepted()) {
                            diagnostic_evidence_context.settle_decision = "window_expired";
                        }
                        break;
                    }

                    static thread_local cv::Mat settle_diff;
                    GPUAccelerator::absdiff(settle_cf.board_gray, revert_mgr.get_latest_gray(), settle_diff);
                    auto settle_sq_means = compute_all_square_means(settle_diff, *geo_, margin_h_, margin_w_);
                        for (int sq = 0; sq < 64; ++sq) {
                            if (consumed_squares[sq]) settle_sq_means[sq] = 0.0;
                        }
                    auto settle_score_start = std::chrono::steady_clock::now();
                    auto settle_best_tmp = this->score_moves_for_board(settle_sq_means);
                    ++score_calls;
                    score_us += std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - settle_score_start).count();

                    if (settle_best_tmp.score > min_move_score && settle_best_tmp.from_sq >= 0) {
                        if (!settle_already_accepted()) {
                            diagnostic_evidence_context.settle_decision = "candidate_found";
                        }
                        
                        // Check for unrelated motion relative to BOTH the current best and new candidate.
                        // This prevents fading trails of the current move from triggering unrelated motion,
                        // while still catching completely separate overlapping moves (e.g. opponent premove).
                        bool unrelated_motion = false;
                        for (int sq = 0; sq < 64; ++sq) {
                            if (settle_sq_means[sq] > 25.0) { // Tolerate large piece shadow/animation spillover
                                if (sq == settle_best_tmp.from_sq || sq == settle_best_tmp.to_sq) continue;
                                if (sq == best.from_sq || sq == best.to_sq) continue;
                                
                                // Ignore castling rook squares
                                if (settle_best_tmp.from_sq == 4 && settle_best_tmp.to_sq == 6 && (sq == 7 || sq == 5)) continue;
                                if (settle_best_tmp.from_sq == 4 && settle_best_tmp.to_sq == 2 && (sq == 0 || sq == 3)) continue;
                                if (settle_best_tmp.from_sq == 60 && settle_best_tmp.to_sq == 62 && (sq == 63 || sq == 61)) continue;
                                if (settle_best_tmp.from_sq == 60 && settle_best_tmp.to_sq == 58 && (sq == 56 || sq == 59)) continue;
                                if (best.from_sq == 4 && best.to_sq == 6 && (sq == 7 || sq == 5)) continue;
                                if (best.from_sq == 4 && best.to_sq == 2 && (sq == 0 || sq == 3)) continue;
                                if (best.from_sq == 60 && best.to_sq == 62 && (sq == 63 || sq == 61)) continue;
                                if (best.from_sq == 60 && best.to_sq == 58 && (sq == 56 || sq == 59)) continue;
                                
                                // Ignore en passant capture square
                                if (((settle_best_tmp.to_sq & 7) | (settle_best_tmp.from_sq & 0x38)) == sq) continue;
                                if (((best.to_sq & 7) | (best.from_sq & 0x38)) == sq) continue;
                                
                                unrelated_motion = true;
                                break;
                            }
                        }

                        if (trace_stream.is_open() && trace_settle) {
                            const auto move_name = [](const auto& move) {
                                if (move.from_sq < 0 || move.to_sq < 0) return std::string("none");
                                return std::string(utils::sq_name(move.from_sq)) + utils::sq_name(move.to_sq);
                            };
                            trace_candidate(
                                "SETTLE_PROBE", settle_cf.t, data.moves.size(), pos_ptr_->get_fen(),
                                move_name(settle_best_tmp), settle_best_tmp.score, 0.0,
                                validation::check_yellowness(board_bgr, *geo_, utils::sq_name(best.from_sq)),
                                validation::check_yellowness(board_bgr, *geo_, utils::sq_name(best.to_sq)),
                                "initial=" + move_name(best) +
                                ";initial_score=" + std::to_string(best.score) +
                                ";settle_score=" + std::to_string(settle_best_tmp.score) +
                                ";unrelated=" + std::to_string(unrelated_motion ? 1 : 0));
                        }
                        
                        if (unrelated_motion) {
                            if (!settle_already_accepted()) {
                                diagnostic_evidence_context.settle_decision = "rejected_unrelated_motion";
                            }
                            break; // Stop settling, next move is overlapping
                        }

                        bool is_same_move = (settle_best_tmp.from_sq == best.from_sq && settle_best_tmp.to_sq == best.to_sq);

                        bool can_evolve = false;
                        if (!is_same_move &&
                            settle_best_tmp.score > best.score + kMinSettleRetargetScoreGain) {
                            const char* current_from = utils::sq_name(best.from_sq);
                            const char* current_to = utils::sq_name(best.to_sq);
                            bool current_move_is_registered = extractor_detail::passes_yellowness_check(board_bgr, *geo_, current_from, current_to);
                            bool allow_registered_retarget = false;
                            double current_y_from = validation::check_yellowness(board_bgr, *geo_, current_from);
                            double current_y_to = validation::check_yellowness(board_bgr, *geo_, current_to);
                            double settle_y_from = -1.0;
                            double settle_y_to = -1.0;
                            if (current_move_is_registered && settle_best_tmp.from_sq == best.from_sq) {
                                std::array<char, 64> settle_board_map = utils::expand_fen(pos_ptr_->get_fen());
                                char moving_piece = static_cast<char>(std::tolower(static_cast<unsigned char>(settle_board_map[best.from_sq])));
                                if (moving_piece != 'b' && moving_piece != 'r' && moving_piece != 'q') {
                                    allow_registered_retarget = true;
                                } else {
                                    const char* s_to = utils::sq_name(settle_best_tmp.to_sq);
                                    settle_y_from = validation::check_yellowness(settle_cf.board_bgr, *geo_, current_from);
                                    settle_y_to = validation::check_yellowness(settle_cf.board_bgr, *geo_, s_to);
                                    const bool strong_settle_retarget =
                                        settle_best_tmp.score >= best.score + 15.0 &&
                                        settle_y_from >= 25.0 && settle_y_to >= 25.0 &&
                                        (settle_y_from + settle_y_to) >= 60.0;
                                    allow_registered_retarget = settle_y_from >= 25.0
                                        && settle_y_to >= current_y_to + 12.0
                                        && (settle_y_from + settle_y_to) >= 70.0;
                                    allow_registered_retarget = allow_registered_retarget || strong_settle_retarget;
                                }
                            } else if (!current_move_is_registered && settle_best_tmp.from_sq == best.from_sq) {
                                std::array<char, 64> settle_board_map = utils::expand_fen(pos_ptr_->get_fen());
                                char moving_piece = static_cast<char>(std::tolower(static_cast<unsigned char>(settle_board_map[best.from_sq])));
                                 if ((moving_piece == 'b' || moving_piece == 'r' || moving_piece == 'q') &&
                                     best.score >= 60.0) {
                                     const char* s_to = utils::sq_name(settle_best_tmp.to_sq);
                                     settle_y_to = validation::check_yellowness(settle_cf.board_bgr, *geo_, s_to);
                                     current_move_is_registered = current_y_to >= 35.0;
                                     allow_registered_retarget = settle_y_to >= current_y_to + 12.0;
                                 }
                            }
                            if (trace_stream.is_open()) {
                                const std::string settle_from = settle_best_tmp.from_sq >= 0
                                    ? utils::sq_name(settle_best_tmp.from_sq) : "?";
                                const std::string settle_to = settle_best_tmp.to_sq >= 0
                                    ? utils::sq_name(settle_best_tmp.to_sq) : "?";
                                trace_candidate(
                                    "SETTLE_RETARGET", settle_cf.t, data.moves.size(), pos_ptr_->get_fen(),
                                    std::string(current_from) + current_to + "->" + settle_from + settle_to,
                                    settle_best_tmp.score, 0.0, current_y_from, current_y_to,
                                    "initial_score=" + std::to_string(initial_best_score) +
                                    ";settle_y_from=" + std::to_string(settle_y_from) +
                                    ";settle_y_to=" + std::to_string(settle_y_to) +
                                    ";registered=" + std::to_string(current_move_is_registered ? 1 : 0) +
                                    ";allow=" + std::to_string(allow_registered_retarget ? 1 : 0));
                            }
                            if (!current_move_is_registered || allow_registered_retarget) {
                                const char* s_from = utils::sq_name(settle_best_tmp.from_sq);
                                const char* s_to = utils::sq_name(settle_best_tmp.to_sq);
                                char s_uci[6];
                                s_uci[0] = s_from[0]; s_uci[1] = s_from[1];
                                s_uci[2] = s_to[0];   s_uci[3] = s_to[1];
                                if (settle_best_tmp.promotion != '\0') {
                                    s_uci[4] = settle_best_tmp.promotion;
                                    s_uci[5] = '\0';
                                } else {
                                    s_uci[4] = '\0';
                                }
                                try {
                                    (void)pos_ptr_->parse_move(s_uci); // Throws if illegal for CURRENT player turn
                                    can_evolve = true;
                                } catch (...) {
                                    can_evolve = false;
                                }
                            }
                        }

                        // If the new candidate is the exact same move, update to capture the very end of the animation.
                        // If it's a DIFFERENT move, only switch if its score is strictly higher AND it is a legal
                        // move for the current player. This allows transient mid-air hallucinations to evolve into
                        // the true landing square, while safely rejecting overlapping opponent premoves.
                        if (is_same_move || can_evolve) {
                            diagnostic_evidence_context.settle_decision = is_same_move
                                ? "accepted_same_move"
                                : "accepted_retarget";
                            add_diagnostic_tag("settled");
                            if (can_evolve && !is_same_move) {
                                visual_move_t = settle_cf.t;
                            }
                            t = settle_cf.t;
                            board_gray = settle_cf.board_gray;
                            board_bgr = settle_cf.board_bgr;
                            full_bgr = settle_cf.full_bgr;
                            cf.clock_top_bgr = settle_cf.clock_top_bgr;
                            cf.clock_bot_bgr = settle_cf.clock_bot_bgr;
                            cf.board_hash = settle_cf.board_hash;
                            cf.observation_id = settle_cf.observation_id;
                            current_observation_id = cf.observation_id;
                            current_hash = settle_cf.board_hash;
                            hash_computed = !current_hash.empty();
                            diff = settle_diff;
                            sq_means = settle_sq_means;
                            best = settle_best_tmp;
                            
                            // Consume the settle frame directly
                            if (i + 1 < candidates.size()) {
                                i++;
                            } else {
                                mapper.consume_next_chunk_front(current_chunk + 1);
                            }
                            continue; // Peek ahead again
                        }
                    }
                }
                break; // No better settle frame found
            }

            if (extracted_in_frame && best.score < kMinCoalescedFollowupScore) {
                std::array<char, 64> board_map = utils::expand_fen(pos_ptr_->get_fen());
                char moving_piece = board_map[best.from_sq];
                if (std::tolower(static_cast<unsigned char>(moving_piece)) == 'p') {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: stopping coalesced extraction before weak pawn follow-up move");
                    }
                    break;
                }
            }
            if (extracted_in_frame && (consumed_squares[best.from_sq] || consumed_squares[best.to_sq])) {
                break;
            }

            const char* from_name = utils::sq_name(best.from_sq);
            const char* to_name = utils::sq_name(best.to_sq);

            std::array<char, 64> current_board_map = utils::expand_fen(pos_ptr_->get_fen());
            char moving_piece = current_board_map[best.from_sq];
            bool strong_visual_landing_override = false;
            bool strong_immediate_recapture = false;

            // Several legal pieces can share a destination (most commonly a
            // bishop and a knight during analysis playback). The square-diff
            // score alone can prefer a stale source square, while the UI's
            // origin highlight identifies the move that was actually
            // registered. Use that visual evidence only when the alternatives
            // already explain the same destination change.
            const double best_origin_yellowness =
                validation::check_yellowness(board_bgr, *geo_, from_name);
            for (const auto& legal_move : pos_ptr_->legal_moves()) {
                const int candidate_from = static_cast<int>(static_cast<unsigned int>(legal_move.from()));
                const int candidate_to = static_cast<int>(static_cast<unsigned int>(legal_move.to()));
                if (candidate_to != best.to_sq || candidate_from == best.from_sq) continue;
                const double candidate_score = sq_means[candidate_from] + sq_means[candidate_to];
                const char* candidate_from_name = utils::sq_name(candidate_from);
                const double candidate_origin_yellowness =
                    validation::check_yellowness(board_bgr, *geo_, candidate_from_name);
                if (trace_stream.is_open()) {
                    trace_candidate(
                        "ORIGIN_CANDIDATE", t, data.moves.size(), pos_ptr_->get_fen(),
                        std::string(candidate_from_name) + to_name,
                        candidate_score, max_sd, best_origin_yellowness,
                        candidate_origin_yellowness,
                        "best_move=" + std::string(from_name) + to_name +
                        ";best_origin_y=" + std::to_string(best_origin_yellowness) +
                        ";candidate_origin_y=" + std::to_string(candidate_origin_yellowness));
                }
                if (candidate_origin_yellowness < 30.0 ||
                    candidate_origin_yellowness <= best_origin_yellowness + 10.0) {
                    continue;
                }
                // A registered origin is stronger evidence than a stale
                // source-square diff.  Permit a clearly highlighted origin
                // to win even when animation makes its grayscale score
                // weaker than the current candidate.
                if (candidate_score + 15.0 < best.score &&
                    candidate_origin_yellowness < 45.0) {
                    continue;
                }
                best.from_sq = candidate_from;
                best.score = std::max(best.score, candidate_score + candidate_origin_yellowness);
                from_name = candidate_from_name;
                moving_piece = current_board_map[best.from_sq];
            }

            if (data.moves.size() >= 2) {
                const std::string& prev_same_side_move = data.moves[data.moves.size() - 2];
                const bool lands_on_same_side_origin = prev_same_side_move.size() >= 4 &&
                    to_name[0] == prev_same_side_move[0] && to_name[1] == prev_same_side_move[1];
                if (lands_on_same_side_origin && std::tolower(static_cast<unsigned char>(moving_piece)) == 'r') {
                    int rescue_from = -1;
                    int rescue_to = -1;
                    double rescue_evidence = best.score;
                    for (const auto& legal_move : pos_ptr_->legal_moves()) {
                        int candidate_from = static_cast<int>(static_cast<unsigned int>(legal_move.from()));
                        int candidate_to = static_cast<int>(static_cast<unsigned int>(legal_move.to()));
                        char candidate_piece = current_board_map[candidate_from];
                        char lower_piece = static_cast<char>(std::tolower(static_cast<unsigned char>(candidate_piece)));
                        if (lower_piece != 'q' && lower_piece != 'b') continue;
                        const double candidate_score = sq_means[candidate_from] + sq_means[candidate_to];
                        if (candidate_score + 3.0 < best.score) continue;
                        int from_file = candidate_from & 7;
                        int from_rank = candidate_from >> 3;
                        int to_file = candidate_to & 7;
                        int to_rank = candidate_to >> 3;
                        int df = (to_file > from_file) ? 1 : (to_file < from_file ? -1 : 0);
                        int dr = (to_rank > from_rank) ? 1 : (to_rank < from_rank ? -1 : 0);
                        if (df == 0 && dr == 0) continue;
                        if (std::abs(to_file - from_file) != std::abs(to_rank - from_rank)) continue;
                        int step = dr * 8 + df;
                        for (int sq = candidate_to + step; sq >= 0 && sq < 64; sq += step) {
                            int prev_sq = sq - step;
                            if (std::abs((sq & 7) - (prev_sq & 7)) != 1) break;
                            const char* far_name = utils::sq_name(sq);
                            char far_uci[5] = {
                                utils::sq_name(candidate_from)[0], utils::sq_name(candidate_from)[1],
                                far_name[0], far_name[1], '\0'
                            };
                            try {
                                (void)pos_ptr_->parse_move(far_uci);
                            } catch (...) {
                                break;
                            }
                            const double y = validation::check_yellowness(board_bgr, *geo_, far_name);
                            const double evidence = candidate_score + y + sq_means[sq];
                            if ((y >= 18.0 || sq_means[sq] >= 12.0) && evidence > rescue_evidence + 5.0) {
                                rescue_from = candidate_from;
                                rescue_to = sq;
                                rescue_evidence = evidence;
                            }
                            if (current_board_map[sq] != ' ' && current_board_map[sq] != '.') break;
                        }
                    }
                    if (rescue_from >= 0) {
                        best.from_sq = rescue_from;
                        best.to_sq = rescue_to;
                        best.score = rescue_evidence;
                        from_name = utils::sq_name(best.from_sq);
                        to_name = utils::sq_name(best.to_sq);
                        moving_piece = current_board_map[best.from_sq];
                    }
                }
            }

            const char lower_moving_piece =
                static_cast<char>(std::tolower(static_cast<unsigned char>(moving_piece)));
            bool endpoint_resolved_this_candidate = false;
            if (lower_moving_piece == 'r' || lower_moving_piece == 'q') {
                // Rook/queen endpoint ambiguity has the same geometry. Apply
                // the generic slider resolver before queen-specific rescue
                // logic so a legal adjacent landing is considered first.
                const int endpoint_before = best.to_sq;
                const double endpoint_before_y =
                    validation::check_yellowness(board_bgr, *geo_, to_name);
                extractor_detail::adjust_sliding_target(best.to_sq, to_name, best.from_sq, from_name, sq_means, board_bgr, *geo_, pos_ptr_.get());
                if (trace_stream.is_open() && endpoint_before != best.to_sq) {
                    trace_candidate(
                        "ENDPOINT_RESOLVE", t, data.moves.size(), pos_ptr_->get_fen(),
                        std::string(from_name) + utils::sq_name(endpoint_before) + "->" +
                            std::string(from_name) + to_name,
                        best.score, max_sd, endpoint_before_y,
                        validation::check_yellowness(board_bgr, *geo_, to_name),
                        "resolver=slider;before_sq=" + std::to_string(endpoint_before) +
                        ";after_sq=" + std::to_string(best.to_sq) +
                        ";before_diff=" + std::to_string(sq_means[endpoint_before]) +
                        ";after_diff=" + std::to_string(sq_means[best.to_sq]));
                }
                endpoint_resolved_this_candidate = endpoint_before != best.to_sq;
            }
            if (pending_resolved_endpoint) {
                const double pending_age = t - pending_resolved_endpoint_t;
                const int pending_from = pending_resolved_endpoint->first;
                const int pending_to = pending_resolved_endpoint->second;
                const int current_from_file = best.from_sq & 7;
                const int current_from_rank = best.from_sq >> 3;
                const int current_to_file = best.to_sq & 7;
                const int current_to_rank = best.to_sq >> 3;
                const int pending_to_file = pending_to & 7;
                const int pending_to_rank = pending_to >> 3;
                const bool same_line =
                    (current_from_rank == (pending_from >> 3) &&
                     current_from_rank == current_to_rank &&
                     current_from_rank == pending_to_rank) ||
                    (current_from_file == (pending_from & 7) &&
                     current_from_file == current_to_file &&
                     current_from_file == pending_to_file);
                const int current_distance =
                    std::max(std::abs(current_to_file - current_from_file),
                             std::abs(current_to_rank - current_from_rank));
                const int pending_distance =
                    std::max(std::abs(pending_to_file - current_from_file),
                             std::abs(pending_to_rank - current_from_rank));
                const int current_step_file = current_to_file == current_from_file
                    ? 0 : (current_to_file > current_from_file ? 1 : -1);
                const int current_step_rank = current_to_rank == current_from_rank
                    ? 0 : (current_to_rank > current_from_rank ? 1 : -1);
                const int pending_step_file = pending_to_file == current_from_file
                    ? 0 : (pending_to_file > current_from_file ? 1 : -1);
                const int pending_step_rank = pending_to_rank == current_from_rank
                    ? 0 : (pending_to_rank > current_from_rank ? 1 : -1);
                const bool current_target_is_between = same_line &&
                    current_distance > 0 && pending_distance > current_distance &&
                    current_step_file == pending_step_file &&
                    current_step_rank == pending_step_rank;
                if (pending_age < 0.0 || pending_age > kPendingResolvedEndpointWindowSeconds ||
                    pending_from != best.from_sq || !current_target_is_between ||
                    pending_distance <= current_distance) {
                    if (pending_age > kPendingResolvedEndpointWindowSeconds ||
                        pending_from != best.from_sq) {
                        pending_resolved_endpoint.reset();
                    }
                } else {
                    const char* pending_to_name = utils::sq_name(pending_to);
                    char pending_uci[5] = {
                        from_name[0], from_name[1], pending_to_name[0], pending_to_name[1], '\0'
                    };
                    try {
                        (void)pos_ptr_->parse_move(pending_uci);
                        best.to_sq = pending_to;
                        to_name = pending_to_name;
                        trace_candidate(
                            "RESOLVED_ENDPOINT_REUSE", t, data.moves.size(), pos_ptr_->get_fen(),
                            std::string(from_name) + to_name, best.score, max_sd,
                            validation::check_yellowness(board_bgr, *geo_, from_name),
                            validation::check_yellowness(board_bgr, *geo_, to_name),
                            "age=" + std::to_string(pending_age));
                    } catch (...) {
                        pending_resolved_endpoint.reset();
                    }
                }
            }
            if (lower_moving_piece == 'q') {
                const int queen_from_file = best.from_sq & 7;
                const int queen_to_file = best.to_sq & 7;
                const int queen_from_rank = best.from_sq >> 3;
                const int queen_to_rank = best.to_sq >> 3;
                const bool one_step_horizontal_to_edge =
                    queen_from_rank == queen_to_rank &&
                    std::abs(queen_to_file - queen_from_file) == 1 &&
                    (queen_to_file == 1 || queen_to_file == 6);
                if (one_step_horizontal_to_edge) {
                    bool promoted_to_edge = false;
                    const int step = queen_to_file > queen_from_file ? 1 : -1;
                    const int edge_sq = best.to_sq + step;
                    if (edge_sq >= 0 && edge_sq < 64 && (edge_sq >> 3) == queen_from_rank &&
                        ((edge_sq & 7) == 0 || (edge_sq & 7) == 7)) {
                        const char* edge_name = utils::sq_name(edge_sq);
                        char edge_uci[5] = {from_name[0], from_name[1], edge_name[0], edge_name[1], '\0'};
                        try {
                            (void)pos_ptr_->parse_move(edge_uci);
                            best.to_sq = edge_sq;
                            to_name = edge_name;
                            promoted_to_edge = true;
                        } catch (...) {
                        }
                    }
                    if (!promoted_to_edge) {
                        extractor_detail::adjust_sliding_target(best.to_sq, to_name, best.from_sq, from_name, sq_means, board_bgr, *geo_, pos_ptr_.get());
                    }
                }
                const bool one_step_queen_candidate =
                    std::max(std::abs(queen_to_file - queen_from_file),
                             std::abs(queen_to_rank - queen_from_rank)) == 1;
                const bool short_queen_move_registered =
                    extractor_detail::passes_yellowness_check(board_bgr, *geo_, from_name, to_name);
                if (one_step_queen_candidate && !short_queen_move_registered) {
                    const double current_y = validation::check_yellowness(board_bgr, *geo_, to_name);
                    const double current_evidence = current_y + sq_means[best.to_sq];
                    const double current_edges = extractor_detail::square_piece_edge_score(board_bgr, *geo_, to_name);
                    int far_best_sq = -1;
                    double far_best_evidence = current_evidence;
                    double far_best_piece_landing_score = 0.0;
                    const int directions[8] = {1, -1, 8, -8, 9, -9, 7, -7};
                    for (int dir : directions) {
                        for (int sq = best.from_sq + dir * 2; sq >= 0 && sq < 64; sq += dir) {
                            const int prev_sq = sq - dir;
                            const int file_delta = std::abs((sq & 7) - (prev_sq & 7));
                            if ((dir == 1 || dir == -1 || dir == 9 || dir == -9 || dir == 7 || dir == -7) &&
                                file_delta != 1) {
                                break;
                            }

                            const char* candidate_name = utils::sq_name(sq);
                            char candidate_uci[5] = {from_name[0], from_name[1], candidate_name[0], candidate_name[1], '\0'};
                            try {
                                (void)pos_ptr_->parse_move(candidate_uci);
                            } catch (...) {
                                break;
                            }

                            const double y = validation::check_yellowness(board_bgr, *geo_, candidate_name);
                            const double evidence = y + sq_means[sq];
                            const double piece_edges =
                                extractor_detail::square_piece_edge_score(board_bgr, *geo_, candidate_name);
                            const int distance_steps =
                                std::max(std::abs((sq & 7) - queen_from_file), std::abs((sq >> 3) - queen_from_rank));
                            const bool strong_piece_landing =
                                distance_steps >= 2 &&
                                y >= 35.0 &&
                                evidence >= 60.0 &&
                                piece_edges >= 45.0 &&
                                piece_edges >= current_edges + 25.0;
                            if (strong_piece_landing) {
                                const double piece_landing_score = evidence + piece_edges;
                                if (piece_landing_score > far_best_piece_landing_score) {
                                    far_best_sq = sq;
                                    far_best_evidence = evidence;
                                    far_best_piece_landing_score = piece_landing_score;
                                    strong_visual_landing_override = true;
                                }
                            }
                            if ((y >= 30.0 || evidence >= current_evidence + 8.0) &&
                                evidence > far_best_evidence + 4.0 &&
                                far_best_piece_landing_score <= 0.0) {
                                far_best_sq = sq;
                                far_best_evidence = evidence;
                            }

                            char target_piece_on_ray = current_board_map[sq];
                            if (target_piece_on_ray != ' ' && target_piece_on_ray != '.') {
                                break;
                            }
                        }
                    }
                    if (far_best_sq >= 0) {
                        best.to_sq = far_best_sq;
                        to_name = utils::sq_name(best.to_sq);
                    }
                }
                auto is_occupied = [](char piece) { return piece != ' ' && piece != '.'; };
                char target_piece = current_board_map[best.to_sq];
                if (!is_occupied(target_piece) && !data.moves.empty() && !data.timestamps.empty() &&
                    (t - data.timestamps.back()) < 1.0) {
                    const std::string& prev_move = data.moves.back();
                    double current_y = validation::check_yellowness(board_bgr, *geo_, to_name);
                    double current_evidence = current_y + sq_means[best.to_sq];
                    int best_capture_sq = -1;
                    double best_capture_evidence = current_evidence;
                    int from_file = best.from_sq & 7;
                    int from_rank = best.from_sq >> 3;
                    for (int df = -1; df <= 1; ++df) {
                        for (int dr = -1; dr <= 1; ++dr) {
                            if (df == 0 && dr == 0) continue;
                            int file = from_file + df;
                            int rank = from_rank + dr;
                            if (file < 0 || file >= 8 || rank < 0 || rank >= 8) continue;
                            int sq = rank * 8 + file;
                            char piece = current_board_map[sq];
                            if (!is_occupied(piece)) continue;
                            if (std::tolower(static_cast<unsigned char>(piece)) != 'p') continue;
                            bool enemy = std::isupper(static_cast<unsigned char>(moving_piece)) !=
                                         std::isupper(static_cast<unsigned char>(piece));
                            if (!enemy) continue;
                            const char* capture_name = utils::sq_name(sq);
                            if (prev_move.size() < 4 ||
                                capture_name[0] != prev_move[2] ||
                                capture_name[1] != prev_move[3]) {
                                continue;
                            }
                            char capture_uci[5] = {from_name[0], from_name[1], capture_name[0], capture_name[1], '\0'};
                            try {
                                (void)pos_ptr_->parse_move(capture_uci);
                            } catch (...) {
                                continue;
                            }
                            double y = validation::check_yellowness(board_bgr, *geo_, capture_name);
                            double evidence = y + sq_means[sq] + 15.0;
                            if ((y >= 18.0 || sq_means[sq] >= 18.0) && evidence > best_capture_evidence + 10.0) {
                                best_capture_evidence = evidence;
                                best_capture_sq = sq;
                            }
                        }
                    }
                    if (best_capture_sq >= 0) {
                        best.to_sq = best_capture_sq;
                        to_name = utils::sq_name(best_capture_sq);
                    }
                }
            }

            if (postgame_replay_branch &&
                std::tolower(static_cast<unsigned char>(moving_piece)) == 'k') {
                int best_check_from = -1;
                int best_check_to = -1;
                double best_check_score = best.score;
                int best_check_distance = std::numeric_limits<int>::max();
                for (const auto& legal_move : pos_ptr_->legal_moves()) {
                    const int candidate_from = static_cast<int>(static_cast<unsigned int>(legal_move.from()));
                    const int candidate_to = static_cast<int>(static_cast<unsigned int>(legal_move.to()));
                    const char candidate_piece = current_board_map[candidate_from];
                    if (std::tolower(static_cast<unsigned char>(candidate_piece)) != 'q') {
                        continue;
                    }

                    const char* candidate_from_name = utils::sq_name(candidate_from);
                    const char* candidate_to_name = utils::sq_name(candidate_to);
                    const double y_from = validation::check_yellowness(board_bgr, *geo_, candidate_from_name);
                    const double y_to = validation::check_yellowness(board_bgr, *geo_, candidate_to_name);
                    const double candidate_score = sq_means[candidate_from] + sq_means[candidate_to];
                    if (y_from < 30.0 || y_to < 30.0 || (y_from + y_to) < 70.0 ||
                        candidate_score < 45.0) {
                        continue;
                    }

                    char candidate_uci[5] = {
                        candidate_from_name[0], candidate_from_name[1],
                        candidate_to_name[0], candidate_to_name[1], '\0'
                    };
                    libchess::Move candidate_move;
                    if (!extractor_detail::is_valid_libchess_move(*pos_ptr_, candidate_uci, candidate_move)) {
                        continue;
                    }
                    libchess::Position next_pos = *pos_ptr_;
                    (void)next_pos.makemove(candidate_move);
                    if (!next_pos.in_check()) {
                        continue;
                    }

                    const double check_score = candidate_score + y_from + y_to;
                    const int check_distance = std::max(
                        std::abs((candidate_to & 7) - (candidate_from & 7)),
                        std::abs((candidate_to >> 3) - (candidate_from >> 3)));
                    if ((check_distance < best_check_distance && check_score >= 120.0) ||
                        (check_distance == best_check_distance && check_score > best_check_score + 20.0)) {
                        best_check_score = check_score;
                        best_check_distance = check_distance;
                        best_check_from = candidate_from;
                        best_check_to = candidate_to;
                    }
                }
                if (best_check_from >= 0) {
                    best.from_sq = best_check_from;
                    best.to_sq = best_check_to;
                    best.score = best_check_score;
                    from_name = utils::sq_name(best.from_sq);
                    to_name = utils::sq_name(best.to_sq);
                    moving_piece = current_board_map[best.from_sq];
                }
            }
            if (postgame_replay_branch &&
                std::tolower(static_cast<unsigned char>(moving_piece)) == 'k') {
                bool follows_recent_queen_move = false;
                if (!data.moves.empty() && !data.timestamps.empty() && (t - data.timestamps.back()) <= 10.0) {
                    const std::string& prev_move = data.moves.back();
                    if (prev_move.size() >= 4) {
                        const int prev_to =
                            (prev_move[2] - 'a') + (prev_move[3] - '1') * 8;
                        const char prev_piece = current_board_map[prev_to];
                        follows_recent_queen_move =
                            std::tolower(static_cast<unsigned char>(prev_piece)) == 'q';
                    }
                }
                if (!extracted_in_frame && !follows_recent_queen_move) {
                    // Leave the initial branch move to the normal scorer.
                } else {
                int best_escape_to = -1;
                double best_escape_evidence = 0.0;
                for (const auto& legal_move : pos_ptr_->legal_moves()) {
                    const int candidate_from = static_cast<int>(static_cast<unsigned int>(legal_move.from()));
                    const int candidate_to = static_cast<int>(static_cast<unsigned int>(legal_move.to()));
                    if (candidate_from != best.from_sq) {
                        continue;
                    }

                    const char* candidate_from_name = utils::sq_name(candidate_from);
                    const char* candidate_to_name = utils::sq_name(candidate_to);
                    const double y_from = validation::check_yellowness(board_bgr, *geo_, candidate_from_name);
                    const double y_to = validation::check_yellowness(board_bgr, *geo_, candidate_to_name);
                    if (y_from < 30.0 || y_to < 30.0 || (y_from + y_to) < 70.0) {
                        continue;
                    }
                    const double evidence = y_from + y_to - std::min(sq_means[candidate_to], 80.0) * 0.35;
                    if (evidence > best_escape_evidence) {
                        best_escape_evidence = evidence;
                        best_escape_to = candidate_to;
                    }
                }
                if (best_escape_to >= 0) {
                    best.to_sq = best_escape_to;
                    best.score = std::max(best.score, best_escape_evidence);
                    to_name = utils::sq_name(best.to_sq);
                }
                }
            }
            // Build UCI strings only when needed (avoid allocation in scoring loop)
            char move_uci_buf[6];
            move_uci_buf[0] = from_name[0]; move_uci_buf[1] = from_name[1];
            move_uci_buf[2] = to_name[0];   move_uci_buf[3] = to_name[1];
            if (best.promotion != '\0') {
                move_uci_buf[4] = best.promotion;
                move_uci_buf[5] = '\0';
            } else {
                move_uci_buf[4] = '\0';
            }
            std::string move_uci(move_uci_buf);
            last_validation_move = move_uci;
            last_validation_from = from_name;
            last_validation_to = to_name;
            last_validation_score = best.score;

            // If it's a promotion, visually classify the piece type on the destination square post-settling
            if (best.promotion != '\0') {
                best.promotion = classify_promoted_piece(board_bgr, *geo_, to_name);
                move_uci.back() = best.promotion; // Update the trailing 'q' with the true piece
            }

            if (suppressed_stale_branch &&
                data.moves.size() == suppressed_stale_ply &&
                move_uci == suppressed_stale_first_move) {
                consumed_squares[best.from_sq] = true;
                consumed_squares[best.to_sq] = true;
                continue;
            }

            bool accepted_strong_inverse = false;
            const std::optional<size_t> recent_inverse_index =
                extractor_detail::find_recent_inverse_move_index(data.moves, from_name, to_name);
            if (recent_inverse_index) {
                double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
                double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
                char inverse_piece = static_cast<char>(std::tolower(static_cast<unsigned char>(moving_piece)));
                const bool sliding_inverse = inverse_piece == 'b' || inverse_piece == 'r' || inverse_piece == 'q';
                char reverse_uci_buf[5] = {to_name[0], to_name[1], from_name[0], from_name[1], '\0'};
                const bool inverse_of_previous_move =
                    !data.moves.empty() && data.moves.back() == std::string_view(reverse_uci_buf, 4);
                bool immediate_inverse_bounce =
                    inverse_of_previous_move && !data.timestamps.empty() && (t - data.timestamps.back()) < 0.35;
                const double inverse_age =
                    *recent_inverse_index < data.timestamps.size()
                        ? t - data.timestamps[*recent_inverse_index]
                        : std::numeric_limits<double>::infinity();
                bool strong_registered_inverse = best.score >= 60.0
                    && y_from >= 35.0
                    && y_to >= 35.0
                    // Registered inverse moves can have slightly asymmetric
                    // highlight intensity while the clock/UI settles.  Keep
                    // both endpoints strong, but use the same 75-point
                    // combined evidence floor as the general move detector.
                    && (y_from + y_to) >= 75.0;
                // A real take-back may be a king, knight, or pawn move, so
                // piece type alone is not enough to reject it. A short-lived
                // reverse is still a common UI/animation artifact, however.
                // Accept a settled non-sliding return only after the move it
                // reverses has remained on screen long enough, or while an
                // already-confirmed analysis replay is being reconstructed.
                constexpr double kSettledInverseMinAgeSeconds = 10.0;
                const bool settled_non_sliding_inverse =
                    sliding_inverse || postgame_replay_branch ||
                    inverse_age >= kSettledInverseMinAgeSeconds;
                if (!strong_registered_inverse || immediate_inverse_bounce ||
                    !settled_non_sliding_inverse) {
                    if (trace_stream.is_open()) {
                        trace_candidate(
                            "INVERSE_GUARD", t, data.moves.size(), pos_ptr_->get_fen(),
                            move_uci, best.score, max_sd, y_from, y_to,
                            "sliding=" + std::to_string(sliding_inverse ? 1 : 0) +
                            ";postgame_branch=" +
                            std::to_string(postgame_replay_branch ? 1 : 0) +
                            ";strong=" + std::to_string(strong_registered_inverse ? 1 : 0) +
                            ";immediate=" + std::to_string(immediate_inverse_bounce ? 1 : 0) +
                            ";last_move=" +
                            (data.moves.empty() ? std::string{} : data.moves.back()) +
                            ";last_age=" +
                            (!data.timestamps.empty()
                                ? std::to_string(t - data.timestamps.back())
                                : std::string{"-1"}) +
                            ";inverse_age=" + std::to_string(inverse_age));
                    }
                    validation_rejection_reason = "inverse_validation";
                    all_validations_passed = false;
                    break;
                }
                accepted_strong_inverse = true;
            }

            if (!data.moves.empty() && !data.timestamps.empty() && (t - data.timestamps.back()) < 1.0) {
                const std::string& prev_move = data.moves.back();
                if (prev_move.size() >= 4) {
                    const bool touches_prev_move =
                        (from_name[0] == prev_move[0] && from_name[1] == prev_move[1]) ||
                        (from_name[0] == prev_move[2] && from_name[1] == prev_move[3]) ||
                        (to_name[0] == prev_move[0] && to_name[1] == prev_move[1]) ||
                        (to_name[0] == prev_move[2] && to_name[1] == prev_move[3]);
                    if (touches_prev_move && move_uci != prev_move) {
                        const bool recaptures_previous_destination =
                            to_name[0] == prev_move[2] && to_name[1] == prev_move[3];
                        char target_piece = current_board_map[best.to_sq];
                        const bool target_occupied = target_piece != ' ' && target_piece != '.';
                        const bool captures_enemy = target_occupied &&
                            (std::isupper(static_cast<unsigned char>(moving_piece)) !=
                             std::isupper(static_cast<unsigned char>(target_piece)));
                        double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
                        double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
                        strong_immediate_recapture =
                            recaptures_previous_destination && captures_enemy &&
                            y_from >= 25.0 && y_to >= 35.0 && (y_from + y_to) >= 85.0;
                        if (!strong_immediate_recapture) {
                            validation_rejection_reason = "immediate_recapture_not_strong";
                            all_validations_passed = false;
                            break;
                        }
                    } else {
                        if (!settle_already_accepted()) {
                            diagnostic_evidence_context.settle_decision = "below_threshold";
                        }
                    }
                } else {
                    if (!settle_already_accepted()) {
                        diagnostic_evidence_context.settle_decision = "no_future_candidate";
                    }
                }
            }

            // Validate the move is legal in libchess
            libchess::Move validated_move;
            if (!extractor_detail::is_valid_libchess_move(*pos_ptr_, move_uci, validated_move)) {
                validation_rejection_reason = "illegal_move";
                all_validations_passed = false;
                break;
            }

            const char target_piece_before_move = current_board_map[best.to_sq];
            const bool quiet_destination_before_move =
                target_piece_before_move == ' ' || target_piece_before_move == '.';
            if (quiet_destination_before_move) {
                if (!scratch_) scratch_ = std::make_unique<ScratchBuffers>();
                const bool origin_still_hovered =
                    validation::check_hover_box(board_bgr, *geo_, scratch_->white_mask, scratch_->reduced, from_name);
                const double destination_piece_edges = origin_still_hovered
                    ? extractor_detail::square_piece_edge_score(board_bgr, *geo_, to_name)
                    : 8.0;
                if (origin_still_hovered && destination_piece_edges < 8.0) {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (destination square has not settled: edges=" + std::to_string(std::round(destination_piece_edges)) + ")");
                    }
                    if (endpoint_resolved_this_candidate) {
                        pending_resolved_endpoint = std::make_pair(best.from_sq, best.to_sq);
                        pending_resolved_endpoint_t = t;
                        trace_candidate(
                            "RESOLVED_ENDPOINT_SET", t, data.moves.size(), pos_ptr_->get_fen(), move_uci,
                            best.score, max_sd,
                            validation::check_yellowness(board_bgr, *geo_, from_name),
                            validation::check_yellowness(board_bgr, *geo_, to_name),
                            "reason=destination_not_settled");
                    }
                    validation_rejection_reason = "destination_not_settled";
                    all_validations_passed = false;
                    break;
                }
            }

            // ── Validation 1: Yellow square check ────────────────────────────
            const double validation_y_from =
                validation::check_yellowness(board_bgr, *geo_, from_name);
            const double validation_y_to =
                validation::check_yellowness(board_bgr, *geo_, to_name);
            record_diagnostic_yellow_measurement(from_name);
            record_diagnostic_yellow_measurement(to_name);
            diagnostic_evidence_context.yellow_checked = true;
            diagnostic_evidence_context.yellow_from = validation_y_from;
            diagnostic_evidence_context.yellow_to = validation_y_to;
            const bool yellow_passed = validation_y_from >= validation::kYellowEndpointThreshold &&
                validation_y_to >= validation::kYellowEndpointThreshold &&
                validation_y_from + validation_y_to >= validation::kYellowPairThreshold;
            const bool yellow_absent =
                validation_y_from < validation::kYellowEndpointThreshold &&
                validation_y_to < validation::kYellowEndpointThreshold;
            if (diagnostic_stream.is_open()) {
                constexpr double kYellowTemporalWindowSeconds = 0.75;
                diagnostic_evidence_context.yellow_temporal_checked = true;
                diagnostic_evidence_context.yellow_temporal_window_seconds =
                    kYellowTemporalWindowSeconds;
                for (size_t temporal_index = i;
                     temporal_index < candidates.size(); ++temporal_index) {
                    const auto& temporal_candidate = candidates[temporal_index];
                    const double temporal_delta = temporal_candidate.t - t;
                    if (temporal_delta < 0.0) continue;
                    if (temporal_delta > kYellowTemporalWindowSeconds) break;
                    const double temporal_from = validation::check_yellowness(
                        temporal_candidate.board_bgr, *geo_, from_name);
                    const double temporal_to = validation::check_yellowness(
                        temporal_candidate.board_bgr, *geo_, to_name);
                    const double temporal_pair = temporal_from + temporal_to;
                    ++diagnostic_evidence_context.yellow_temporal_sample_count;
                    diagnostic_evidence_context.yellow_temporal_max_from = std::max(
                        diagnostic_evidence_context.yellow_temporal_max_from, temporal_from);
                    diagnostic_evidence_context.yellow_temporal_max_to = std::max(
                        diagnostic_evidence_context.yellow_temporal_max_to, temporal_to);
                    diagnostic_evidence_context.yellow_temporal_max_pair = std::max(
                        diagnostic_evidence_context.yellow_temporal_max_pair, temporal_pair);
                    if (temporal_from >= validation::kYellowEndpointThreshold &&
                        temporal_to >= validation::kYellowEndpointThreshold &&
                        temporal_pair >= validation::kYellowPairThreshold) {
                        ++diagnostic_evidence_context.yellow_temporal_pair_pass_count;
                    }
                }
            }
            diagnostic_evidence_context.yellow_decision = accepted_strong_inverse
                ? "bypassed_inverse"
                : (yellow_passed ? "passed" : (yellow_absent ? "no_highlight" : "ambiguous"));
            diagnostic_evidence_context.yellow_assessment.state =
                diagnostic_evidence_context.yellow_decision;
            diagnostic_evidence_context.yellow_assessment.measurements = {
                {"from_score", validation_y_from},
                {"to_score", validation_y_to},
                {"pair_score", validation_y_from + validation_y_to},
                {"temporal_max_from", diagnostic_evidence_context.yellow_temporal_max_from},
                {"temporal_max_to", diagnostic_evidence_context.yellow_temporal_max_to},
                {"temporal_max_pair", diagnostic_evidence_context.yellow_temporal_max_pair},
            };
            diagnostic_evidence_context.yellow_assessment.uncertainty_reason =
                yellow_passed || accepted_strong_inverse
                    ? "uncalibrated_detector_confidence"
                    : (yellow_absent ? "highlight_absent" : "highlight_ambiguous");
            if (yellow_passed || accepted_strong_inverse) {
                add_diagnostic_tag("highlight_pair");
            } else if (yellow_absent) {
                add_diagnostic_tag("highlight_absent");
            } else {
                add_diagnostic_tag("highlight_ambiguous");
            }
            if (!accepted_strong_inverse &&
                !extractor_detail::passes_yellowness_check(board_bgr, *geo_, from_name, to_name)) {
                if (debug_level_ != DebugLevel::None) {
                    double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
                    double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
                    log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Missing yellow highlights: from=" + std::to_string(std::round(y_from)) + ", to=" + std::to_string(std::round(y_to)) + ")");
                }
                validation_rejection_reason = "missing_yellow";
                all_validations_passed = false;
                break;
            }

            // ── Validation 2: Hover box rejection ────────────────────────────
            if (!scratch_) scratch_ = std::make_unique<ScratchBuffers>();
            const auto from_hover = validation::measure_hover_box(
                board_bgr, *geo_, scratch_->white_mask, scratch_->reduced, from_name);
            const auto to_hover = validation::measure_hover_box(
                board_bgr, *geo_, scratch_->white_mask, scratch_->reduced, to_name);
            bool hover_detected = from_hover.detected || to_hover.detected;
            diagnostic_evidence_context.hover_checked = true;
            diagnostic_evidence_context.hover_detected = hover_detected;
            diagnostic_evidence_context.hover_decision = hover_detected ? "detected" : "clear";
            if (hover_detected) {
                add_diagnostic_tag("hover");
                add_diagnostic_tag("animation");
            }
            diagnostic_evidence_context.hover_measurements = {
                {from_name, from_hover.top_edge, from_hover.bottom_edge,
                 from_hover.left_edge, from_hover.right_edge,
                 from_hover.strongest_edge, from_hover.visible_edges,
                 from_hover.detected},
                {to_name, to_hover.top_edge, to_hover.bottom_edge,
                 to_hover.left_edge, to_hover.right_edge,
                 to_hover.strongest_edge, to_hover.visible_edges,
                 to_hover.detected},
            };
            diagnostic_evidence_context.hover_assessment.state =
                diagnostic_evidence_context.hover_decision;
            diagnostic_evidence_context.hover_assessment.thresholds = {
                0.10, 0.65, 2.0};
            diagnostic_evidence_context.hover_assessment.measurements = {
                {"from_strongest_edge", from_hover.strongest_edge},
                {"to_strongest_edge", to_hover.strongest_edge},
                {"from_visible_edges", static_cast<double>(from_hover.visible_edges)},
                {"to_visible_edges", static_cast<double>(to_hover.visible_edges)},
            };
            diagnostic_evidence_context.hover_assessment.uncertainty_reason =
                "uncalibrated_hover_confidence";
            if (hover_detected) {
                double y_from = validation::check_yellowness(board_bgr, *geo_, from_name);
                double y_to = validation::check_yellowness(board_bgr, *geo_, to_name);
                bool strong_registered_move = accepted_strong_inverse ||
                                              (best.score >= 60.0 && y_from >= 40.0 && y_to >= 40.0 && (y_from + y_to) >= 90.0);
                if (strong_registered_move) {
                    diagnostic_evidence_context.hover_decision = "detected_but_overridden";
                    diagnostic_evidence_context.hover_assessment.state =
                        diagnostic_evidence_context.hover_decision;
                }
                if (!strong_registered_move) {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (Piece is still mid-drag)");
                    }
                    validation_rejection_reason = "hover_box";
                    all_validations_passed = false;
                    break;
                }
            }

            // ── Validation 3: Clock turn check ───────────────────────────────
            // Once legality, both yellow registration squares, and the
            // hover check agree, a high-confidence visual transition is a
            // stronger move-registration signal than a transient OCR clock
            // anomaly. This is intentionally piece- and fixture-agnostic.
            const double registered_y_from =
                validation::check_yellowness(board_bgr, *geo_, from_name);
            const double registered_y_to =
                validation::check_yellowness(board_bgr, *geo_, to_name);
            const bool strong_visual_move_registration =
                !hover_detected && best.score >= 100.0 &&
                registered_y_from >= 35.0 && registered_y_to >= 35.0 &&
                (registered_y_from + registered_y_to) >= 100.0;
            const bool strong_visual_registration =
                !hover_detected && strong_immediate_recapture && best.score >= 100.0 &&
                registered_y_from >= 35.0 && registered_y_to >= 35.0 &&
                (registered_y_from + registered_y_to) >= 100.0;

            auto clock_activity_start = std::chrono::steady_clock::now();
            std::string active_clock_player;
            if (has_clocks && !cf.clock_top_bgr.empty() && !cf.clock_bot_bgr.empty()) {
                active_clock_player = detect_active_clock_from_rois(cf.clock_top_bgr, cf.clock_bot_bgr);
            } else if (debug_level_ != DebugLevel::None && !full_bgr.empty()) {
                active_clock_player = extract_clocks(full_bgr, board_template_, *geo_, nullptr).active_player;
            }
            diagnostic_evidence_context.clock_checked =
                !active_clock_player.empty() ||
                (has_clocks && !cf.clock_top_bgr.empty() && !cf.clock_bot_bgr.empty());
            if (has_clocks && !cf.clock_top_bgr.empty() && !cf.clock_bot_bgr.empty()) {
                auto bright_ratio = [](const cv::Mat& roi) {
                    std::size_t bright_pixels = 0;
                    for (int row = 0; row < roi.rows; ++row) {
                        const auto* pixels = roi.ptr<cv::Vec3b>(row);
                        for (int column = 0; column < roi.cols; ++column) {
                            const auto& pixel = pixels[column];
                            const int luminance =
                                (static_cast<int>(pixel[0]) + static_cast<int>(pixel[1]) +
                                 static_cast<int>(pixel[2])) / 3;
                            bright_pixels += luminance > 200 ? 1u : 0u;
                        }
                    }
                    const auto pixel_count = static_cast<std::size_t>(roi.total());
                    return pixel_count == 0
                        ? 0.0
                        : static_cast<double>(bright_pixels) / static_cast<double>(pixel_count);
                };
                diagnostic_evidence_context.clock_top_width = cf.clock_top_bgr.cols;
                diagnostic_evidence_context.clock_top_height = cf.clock_top_bgr.rows;
                diagnostic_evidence_context.clock_bottom_width = cf.clock_bot_bgr.cols;
                diagnostic_evidence_context.clock_bottom_height = cf.clock_bot_bgr.rows;
                diagnostic_evidence_context.clock_top_bright_ratio = bright_ratio(cf.clock_top_bgr);
                diagnostic_evidence_context.clock_bottom_bright_ratio = bright_ratio(cf.clock_bot_bgr);
                diagnostic_evidence_context.clock_bright_ratio_delta =
                    diagnostic_evidence_context.clock_bottom_bright_ratio -
                    diagnostic_evidence_context.clock_top_bright_ratio;
            }
            diagnostic_evidence_context.active_clock_player = active_clock_player;
            diagnostic_evidence_context.clock_decision = active_clock_player.empty()
                ? "unavailable"
                : "active_side_detected";
            diagnostic_evidence_context.clock_assessment.state =
                diagnostic_evidence_context.clock_decision;
            diagnostic_evidence_context.clock_assessment.measurements = {
                {"top_bright_ratio", diagnostic_evidence_context.clock_top_bright_ratio},
                {"bottom_bright_ratio", diagnostic_evidence_context.clock_bottom_bright_ratio},
                {"bright_ratio_delta", diagnostic_evidence_context.clock_bright_ratio_delta},
            };
            diagnostic_evidence_context.clock_assessment.uncertainty_reason =
                active_clock_player.empty()
                    ? "active_side_unavailable"
                    : "uncalibrated_clock_activity_confidence";
            ++clock_activity_calls;
            clock_activity_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - clock_activity_start).count();
            if (!active_clock_player.empty()) {
                std::string expected = (pos_ptr_->turn() == libchess::Side::White) ? "black" : "white";
                if (active_clock_player != expected) {
                    if (extracted_in_frame && best.score < kWeakCoalescedMoveScore) {
                        if (debug_level_ != DebugLevel::None) {
                            log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (weak coalesced move before clock flip)");
                        }
                        validation_rejection_reason = "weak_coalesced_clock";
                        all_validations_passed = false;
                        break;
                    }
                    // The UI clock might lag behind the yellow highlights by a few frames (premoves/ping).
                    // Since the squares have passed the strict yellowness validation (>= 70 combined) above,
                    // the server has officially registered the move. We safely bypass the clock wait.
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " clock wait bypassed (UI clock lagging behind yellow highlights)");
                    }
                }
            }

            ClockState clocks;
            auto clock_start = std::chrono::steady_clock::now();
            std::string moved_player = (pos_ptr_->turn() == libchess::Side::White) ? "white" : "black";
            const int moved_clock_idx = (moved_player == "white") ? 0 : 1;
            std::string previous_moved_clock = last_moved_clock[moved_clock_idx];
            bool moved_time_observed = false;
            bool moved_time_missing = false;
            if (has_clocks && !cf.clock_top_bgr.empty() && !cf.clock_bot_bgr.empty()) {
                clocks = extract_clocks_for_moved_player_from_rois(
                    cf.clock_top_bgr, cf.clock_bot_bgr, moved_player, clock_cache_.get(), active_clock_player);
                const std::string direct_moved_clock =
                    (moved_player == "white") ? clocks.white_time : clocks.black_time;
                // Keep direct OCR provenance separate from the contextual
                // clock selected below. A contextual value can legitimately
                // be inherited from the branch parent to maintain state, but
                // it is not evidence that the replay move itself had a
                // readable clock annotation.
                const auto direct_moved_seconds = parse_clock_seconds(direct_moved_clock);
                moved_time_missing = !direct_moved_seconds.has_value();
                moved_time_observed =
                    direct_moved_seconds.has_value() &&
                    plausible_clock_after_move(direct_moved_clock, previous_moved_clock);
                const cv::Mat& moved_roi = (moved_player == "white") ? cf.clock_bot_bgr : cf.clock_top_bgr;
                auto candidates_for_moved_clock = recognize_clock_time_candidates_from_roi(moved_roi, false);
                diagnostic_evidence_context.clock_candidates = candidates_for_moved_clock;
                diagnostic_evidence_context.previous_moved_clock = previous_moved_clock;
                const std::string adjusted_clock = choose_contextual_clock_candidate(
                    candidates_for_moved_clock,
                    (moved_player == "white") ? clocks.white_time : clocks.black_time,
                    previous_moved_clock);
                if (!adjusted_clock.empty()) {
                    if (moved_player == "white") {
                        clocks.white_time = adjusted_clock;
                        if (clock_cache_) clock_cache_->white_time = adjusted_clock;
                    } else {
                        clocks.black_time = adjusted_clock;
                        if (clock_cache_) clock_cache_->black_time = adjusted_clock;
                    }
                }
                diagnostic_evidence_context.moved_clock =
                    (moved_player == "white") ? clocks.white_time : clocks.black_time;
                diagnostic_evidence_context.clock_ocr_skipped = clocks.ocr_skipped;
                diagnostic_evidence_context.clock_decision =
                    diagnostic_evidence_context.moved_clock.empty()
                        ? "ocr_missing"
                        : (moved_time_observed ? "ocr_plausible" : "ocr_implausible");
                diagnostic_evidence_context.clock_assessment.state =
                    diagnostic_evidence_context.clock_decision;
                diagnostic_evidence_context.clock_assessment.measurements = {
                    {"candidate_count", static_cast<double>(candidates_for_moved_clock.size())},
                    {"bright_ratio_delta", diagnostic_evidence_context.clock_bright_ratio_delta},
                };
                diagnostic_evidence_context.clock_assessment.uncertainty_reason =
                    diagnostic_evidence_context.moved_clock.empty()
                        ? "ocr_missing"
                        : (moved_time_observed
                            ? "uncalibrated_ocr_confidence"
                            : "ocr_implausible");
                if (debug_clock_candidates) {
                    std::ostringstream clock_ss;
                    clock_ss << "    " << utils::ts(elapsed()) << " [ClockDebug] ply " << (data.moves.size() + 1)
                             << " " << move_uci << " moved=" << moved_player
                             << " prev=" << previous_moved_clock << " active=" << active_clock_player
                             << " chosen=" << ((moved_player == "white") ? clocks.white_time : clocks.black_time)
                             << " candidates:";
                    for (const auto& c : candidates_for_moved_clock) {
                        clock_ss << " " << c;
                    }
                    log_info(clock_ss.str());
                }
                if (debug_clock_roi_ply > 0 &&
                    static_cast<int>(data.moves.size() + 1) == debug_clock_roi_ply &&
                    !debug_clock_roi_dir.empty()) {
                    std::filesystem::create_directories(debug_clock_roi_dir);
                    std::ostringstream stem;
                    stem << "ply_" << debug_clock_roi_ply << "_" << std::fixed
                         << std::setprecision(2) << t;
                    cv::imwrite((debug_clock_roi_dir / (stem.str() + "_moved.png")).string(), moved_roi);
                    cv::imwrite((debug_clock_roi_dir / (stem.str() + "_top.png")).string(), cf.clock_top_bgr);
                    cv::imwrite((debug_clock_roi_dir / (stem.str() + "_bottom.png")).string(), cf.clock_bot_bgr);
                    if (!full_bgr.empty()) {
                        cv::imwrite((debug_clock_roi_dir / (stem.str() + "_full.png")).string(), full_bgr);
                    }
                }
            } else if (debug_level_ != DebugLevel::None && !full_bgr.empty()) {
                clocks = extract_clocks(full_bgr, board_template_, *geo_, clock_cache_.get());
            }

            // The clock extractor normally reads only the player who just
            // moved.  If that move left an implausible clock in the retained
            // state, the unchanged opponent pill is still valuable evidence
            // for repairing the preceding move after the UI settles.  This
            // keeps OCR work conditional while recovering delayed board/clock
            // registrations without any fixture assumptions.
            if (has_clocks && data.moves.size() > 0 &&
                data.clocks.size() == data.moves.size() + 1 &&
                !cf.clock_top_bgr.empty() && !cf.clock_bot_bgr.empty()) {
                const size_t previous_ply = data.moves.size() - 1;
                if (previous_ply < data.fens.size() && previous_ply + 1 < data.clocks.size()) {
                    const std::string& previous_parent_fen = data.fens[previous_ply];
                    const size_t previous_active_field = previous_parent_fen.find(' ');
                    const bool previous_move_was_white =
                        previous_active_field != std::string::npos &&
                        previous_active_field + 1 < previous_parent_fen.size() &&
                        previous_parent_fen[previous_active_field + 1] == 'w';
                    const size_t previous_clock_index = previous_move_was_white ? 0 : 1;
                    const ClockInfo& previous_parent_clock = data.clocks[previous_ply];
                    ClockInfo& previous_settled_clock = data.clocks[previous_ply + 1];
                    const std::string& parent_time = previous_clock_index == 0
                        ? previous_parent_clock.white_time : previous_parent_clock.black_time;
                    const std::string& settled_time = previous_clock_index == 0
                        ? previous_settled_clock.white_time : previous_settled_clock.black_time;
                    bool previous_clock_repaired = false;
                    const bool previous_move_was_rebased = std::find(
                        rebased_clock_ply_indices.begin(), rebased_clock_ply_indices.end(), previous_ply) !=
                        rebased_clock_ply_indices.end();
                    if (trace_stream.is_open() && previous_move_was_rebased) {
                        trace_candidate("CLOCK_BACKFILL_CHECK", t, data.moves.size(), pos_ptr_->get_fen(),
                                        move_uci, 0.0, max_sd, 0.0, 0.0,
                                        "previous_ply=" + std::to_string(previous_ply) +
                                        ";clock_count=" + std::to_string(data.clocks.size()) +
                                        ";top_empty=" + (cf.clock_top_bgr.empty() ? "1" : "0") +
                                        ";bot_empty=" + (cf.clock_bot_bgr.empty() ? "1" : "0") +
                                        ";settled=" + settled_time +
                                        ";observed=" + (previous_clock_index == 0
                                            ? clocks.white_time : clocks.black_time));
                    }
                    if (previous_move_was_rebased) {
                        const std::string& observed_opponent_time = previous_clock_index == 0
                            ? clocks.white_time : clocks.black_time;
                        const cv::Mat& opponent_clock_roi = previous_clock_index == 0
                            ? cf.clock_bot_bgr : cf.clock_top_bgr;
                        const std::vector<std::string> opponent_candidates =
                            recognize_clock_time_candidates_from_roi(opponent_clock_roi, false);
                        std::string contextual_opponent_time;
                        auto settled_seconds_for_candidate = parse_clock_seconds(settled_time);
                        if (settled_seconds_for_candidate) {
                            int best_candidate_distance = std::numeric_limits<int>::max();
                            for (const std::string& raw_candidate : opponent_candidates) {
                                std::vector<std::string> candidate_forms;
                                if (const auto suffix_form = recover_clock_suffix_form(raw_candidate)) {
                                    // In this delayed-branch repair path the
                                    // suffix form is the useful interpretation
                                    // when the icon-prefixed h:mm:ss OCR is
                                    // also numerically plausible.
                                    candidate_forms.push_back(*suffix_form);
                                } else {
                                    candidate_forms.push_back(raw_candidate);
                                }
                                for (const std::string& candidate_form : candidate_forms) {
                                    const auto candidate_seconds = parse_clock_seconds(candidate_form);
                                    if (!candidate_seconds ||
                                        !plausible_clock_after_move(candidate_form, settled_time)) continue;
                                    const int distance = std::abs(
                                        *candidate_seconds - *settled_seconds_for_candidate);
                                    if (distance < best_candidate_distance) {
                                        best_candidate_distance = distance;
                                        contextual_opponent_time = candidate_form;
                                    }
                                }
                            }
                        }
                        if (contextual_opponent_time.empty()) {
                            contextual_opponent_time = choose_contextual_clock_candidate(
                                opponent_candidates, observed_opponent_time, settled_time);
                            if (contextual_opponent_time.empty()) {
                                if (const auto suffix_form = recover_clock_suffix_form(observed_opponent_time)) {
                                    contextual_opponent_time = *suffix_form;
                                }
                            }
                        }
                        const std::string& corrected_opponent_time = contextual_opponent_time.empty()
                            ? observed_opponent_time : contextual_opponent_time;
                        if (trace_stream.is_open()) {
                            std::ostringstream opponent_clock_trace;
                            opponent_clock_trace << "candidates=";
                            for (size_t candidate_index = 0; candidate_index < opponent_candidates.size(); ++candidate_index) {
                                if (candidate_index != 0) opponent_clock_trace << ',';
                                opponent_clock_trace << opponent_candidates[candidate_index];
                            }
                            opponent_clock_trace << ";contextual=" << contextual_opponent_time;
                            trace_candidate("CLOCK_BACKFILL_CANDIDATES", t, data.moves.size(), pos_ptr_->get_fen(),
                                            move_uci, 0.0, max_sd, 0.0, 0.0,
                                            opponent_clock_trace.str());
                        }
                        const auto observed_seconds = parse_clock_seconds(corrected_opponent_time);
                        const auto settled_seconds = parse_clock_seconds(settled_time);
                        if (observed_seconds && settled_seconds &&
                            *observed_seconds != *settled_seconds &&
                            std::abs(*observed_seconds - *settled_seconds) <= 180 &&
                            plausible_clock_after_move(corrected_opponent_time, settled_time)) {
                            if (previous_clock_index == 0) {
                                previous_settled_clock.white_time = corrected_opponent_time;
                                if (clock_cache_) clock_cache_->white_time = corrected_opponent_time;
                            } else {
                                previous_settled_clock.black_time = corrected_opponent_time;
                                if (clock_cache_) clock_cache_->black_time = corrected_opponent_time;
                            }
                            previous_clock_repaired = true;
                            trace_candidate("CLOCK_BACKFILL", t, data.moves.size(), pos_ptr_->get_fen(),
                                            move_uci, 0.0, max_sd, 0.0, 0.0,
                                            "previous_ply=" + std::to_string(previous_ply) +
                                            ";value=" + corrected_opponent_time +
                                            ";observed=" + observed_opponent_time +
                                            ";candidate_count=" + std::to_string(opponent_candidates.size()));
                        }
                    }
                    if (!previous_clock_repaired && !plausible_clock_after_move(settled_time, parent_time)) {
                        const cv::Mat& previous_clock_roi = previous_move_was_white
                            ? cf.clock_bot_bgr : cf.clock_top_bgr;
                        const std::vector<std::string> previous_candidates =
                            recognize_clock_time_candidates_from_roi(previous_clock_roi, false);
                        const std::string corrected_time = choose_contextual_clock_candidate(
                            previous_candidates, settled_time, parent_time);
                        if (!corrected_time.empty() &&
                            plausible_clock_after_move(corrected_time, parent_time)) {
                            if (previous_clock_index == 0) {
                                previous_settled_clock.white_time = corrected_time;
                                if (clock_cache_) clock_cache_->white_time = corrected_time;
                            } else {
                                previous_settled_clock.black_time = corrected_time;
                                if (clock_cache_) clock_cache_->black_time = corrected_time;
                            }
                            last_moved_clock[previous_clock_index] = corrected_time;
                            trace_candidate("CLOCK_BACKFILL", t, data.moves.size(), pos_ptr_->get_fen(),
                                            move_uci, 0.0, max_sd, 0.0, 0.0,
                                            "previous_ply=" + std::to_string(previous_ply) +
                                            ";value=" + corrected_time);
                        }
                    }
                }
            }
            bool strong_visual_clock_veto_override = false;
            if (has_clocks && !previous_moved_clock.empty()) {
                auto moved_clock_from = [&](const ClockState& state) -> const std::string& {
                    return (moved_player == "white") ? state.white_time : state.black_time;
                };
                auto assign_moved_clock = [&](const ClockState& settled) {
                    if (moved_player == "white") {
                        clocks.white_time = settled.white_time;
                    } else {
                        clocks.black_time = settled.black_time;
                    }
                    if (!settled.active_player.empty()) {
                        clocks.active_player = settled.active_player;
                    }
                };

                const std::string expected_active_after_move = (moved_player == "white") ? "black" : "white";
                const std::string immediate_moved_clock = moved_clock_from(clocks);
                const bool immediate_is_stale = immediate_moved_clock == previous_moved_clock;
                const bool immediate_is_plausible = plausible_clock_after_move(immediate_moved_clock, previous_moved_clock);
                const bool clock_has_flipped = active_clock_player == expected_active_after_move;
                const bool should_wait_for_clock_settle =
                    !data.moves.empty() && (immediate_is_stale || !immediate_is_plausible || !clock_has_flipped);
                constexpr double kMaxClockSettleWindowSeconds = 4.0;

                if (should_wait_for_clock_settle) {
                    for (size_t lookahead = i + 1; lookahead < candidates.size(); ++lookahead) {
                        const CandidateFrame& future_cf = candidates[lookahead];
                        if (future_cf.t - t > kMaxClockSettleWindowSeconds) {
                            break;
                        }
                        if (future_cf.clock_top_bgr.empty() || future_cf.clock_bot_bgr.empty()) {
                            continue;
                        }

                        std::vector<double> future_hash = future_cf.board_hash.empty()
                            ? compute_all_square_means(future_cf.board_gray, *geo_, margin_h_, margin_w_)
                            : future_cf.board_hash;
                        int future_revert_idx = revert_mgr.find_revert_idx(
                            future_cf.board_gray, future_hash,
                            revert_hash_tests, revert_full_tests,
                            revert_index_queries, revert_index_fallbacks,
                            exhaustive_revert_fallback);
                        if (future_revert_idx >= 0) {
                            break;
                        }

                        std::string future_active = detect_active_clock_from_rois(
                            future_cf.clock_top_bgr, future_cf.clock_bot_bgr);
                        if (!future_active.empty() && future_active != expected_active_after_move) {
                            continue;
                        }

                        ClockState settled = extract_clocks_for_moved_player_from_rois(
                            future_cf.clock_top_bgr, future_cf.clock_bot_bgr, moved_player, nullptr, future_active);
                        const std::string& settled_clock = moved_clock_from(settled);
                        if (settled_clock.empty() || settled_clock == previous_moved_clock ||
                            !plausible_clock_after_move(settled_clock, previous_moved_clock)) {
                            continue;
                        }

                        assign_moved_clock(settled);
                        break;
                    }
                } else if (i + 1 < candidates.size() && candidates[i + 1].t - t <= 2.0) {
                    const CandidateFrame& future_cf = candidates[i + 1];
                    if (!future_cf.clock_top_bgr.empty() && !future_cf.clock_bot_bgr.empty()) {
                        std::string future_active = detect_active_clock_from_rois(
                            future_cf.clock_top_bgr, future_cf.clock_bot_bgr);
                        ClockState settled = extract_clocks_for_moved_player_from_rois(
                            future_cf.clock_top_bgr, future_cf.clock_bot_bgr, moved_player, nullptr, future_active);
                        const std::string& settled_clock = moved_clock_from(settled);
                        auto immediate_seconds = parse_clock_seconds(immediate_moved_clock);
                        auto settled_seconds = parse_clock_seconds(settled_clock);
                        if (immediate_seconds && settled_seconds &&
                            std::abs(*settled_seconds - *immediate_seconds) <= 30 &&
                            plausible_clock_after_move(settled_clock, previous_moved_clock)) {
                            assign_moved_clock(settled);
                        }
                    }
                }

                const std::string after_settle_clock = moved_clock_from(clocks);
                auto post_revert_drop = clock_drop_seconds(after_settle_clock, previous_moved_clock);
                auto previous_seconds = parse_clock_seconds(previous_moved_clock);
                if (first_move_after_revert && previous_seconds && *previous_seconds <= 30 * 60 &&
                    post_revert_drop && *post_revert_drop > 180) {
                    if (moved_player == "white") {
                        clocks.white_time = previous_moved_clock;
                        if (clock_cache_) clock_cache_->white_time = previous_moved_clock;
                    } else {
                        clocks.black_time = previous_moved_clock;
                        if (clock_cache_) clock_cache_->black_time = previous_moved_clock;
                    }
                }

                const std::string final_moved_clock = moved_clock_from(clocks);
                const bool final_clock_is_stale = !final_moved_clock.empty() && final_moved_clock == previous_moved_clock;
                const bool final_clock_is_implausible =
                    !final_moved_clock.empty() &&
                    !plausible_clock_after_move(final_moved_clock, previous_moved_clock);
                const bool active_clock_confirms_move =
                    !clocks.active_player.empty() && clocks.active_player == expected_active_after_move;
                const bool active_clock_still_on_mover =
                    !clocks.active_player.empty() && clocks.active_player == moved_player;
                auto previous_moved_seconds = parse_clock_seconds(previous_moved_clock);
                const bool final_clock_missing_or_implausible =
                    final_moved_clock.empty() || final_clock_is_implausible;
                strong_visual_clock_veto_override =
                    !first_move_after_revert &&
                    previous_moved_seconds && *previous_moved_seconds < 10 * 60 &&
                    final_clock_missing_or_implausible && active_clock_still_on_mover &&
                    (strong_visual_landing_override || strong_visual_registration ||
                     strong_visual_move_registration);
                if (final_clock_is_stale && !active_clock_confirms_move &&
                    !historical_handoff_clock_override) {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (moved player's clock did not advance from analysis branch)");
                    }
                    validation_rejection_reason = "stale_clock";
                    all_validations_passed = false;
                    break;
                }
                bool future_clock_drop_seen = false;
                if (final_clock_is_stale && active_clock_confirms_move &&
                    previous_moved_seconds && *previous_moved_seconds <= 2 * 60) {
                    constexpr double kLowTimeClockDeferralWindowSeconds = 20.0;
                    for (size_t lookahead = i + 1; lookahead < candidates.size(); ++lookahead) {
                        const CandidateFrame& future_cf = candidates[lookahead];
                        if (future_cf.t - t > kLowTimeClockDeferralWindowSeconds) {
                            break;
                        }
                        if (future_cf.clock_top_bgr.empty() || future_cf.clock_bot_bgr.empty()) {
                            continue;
                        }

                        std::string future_active = detect_active_clock_from_rois(
                            future_cf.clock_top_bgr, future_cf.clock_bot_bgr);
                        if (!future_active.empty() && future_active != expected_active_after_move) {
                            continue;
                        }

                        ClockState future_clocks = extract_clocks_for_moved_player_from_rois(
                            future_cf.clock_top_bgr, future_cf.clock_bot_bgr, moved_player, nullptr, future_active);
                        const cv::Mat& future_moved_roi =
                            (moved_player == "white") ? future_cf.clock_bot_bgr : future_cf.clock_top_bgr;
                        auto future_candidates = recognize_clock_time_candidates_from_roi(future_moved_roi, false);
                        const std::string future_clock = choose_contextual_clock_candidate(
                            future_candidates, moved_clock_from(future_clocks), previous_moved_clock);
                        auto future_drop = clock_drop_seconds(future_clock, previous_moved_clock);
                        if (future_drop && *future_drop > 0 && *future_drop <= 90) {
                            future_clock_drop_seen = true;
                            break;
                        }
                    }
                }
                if (future_clock_drop_seen) {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " deferred (low-time moved clock is stale and a later clock drop is visible)");
                    }
                    validation_rejection_reason = "future_clock_drop";
                    all_validations_passed = false;
                    break;
                }
                if (!first_move_after_revert &&
                    previous_moved_seconds && *previous_moved_seconds < 10 * 60 &&
                    final_clock_is_implausible && active_clock_still_on_mover &&
                    !strong_visual_clock_veto_override &&
                    !historical_handoff_clock_override) {
                    if (debug_level_ != DebugLevel::None) {
                        log_info("    " + utils::ts(elapsed()) + " [Debug] " + std::to_string(t) + "s: " + move_uci + " rejected (moved player's clock jumped while active clock stayed on mover)");
                    }
                    validation_rejection_reason = "clock_jump";
                    validation_rejection_detail =
                        "previous=" + previous_moved_clock +
                        ";final=" + final_moved_clock +
                        ";active=" + clocks.active_player +
                        ";moved=" + moved_player +
                        ";visual_score=" + std::to_string(best.score) +
                        ";visual_y_from=" + std::to_string(registered_y_from) +
                        ";visual_y_to=" + std::to_string(registered_y_to) +
                        ";strong_visual=" + (strong_visual_registration ? "1" : "0");
                    all_validations_passed = false;
                    break;
                }
            }
            ++clock_ocr_calls;
            clock_ocr_us += std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - clock_start).count();

            // ── All validations passed — accept the move ─────────────────────
            const std::string final_moved_clock =
                (moved_player == "white") ? clocks.white_time : clocks.black_time;
            const auto previous_moved_seconds = parse_clock_seconds(previous_moved_clock);
            const bool low_time_stale_or_bad_clock =
                !previous_moved_clock.empty() && !final_moved_clock.empty() &&
                previous_moved_seconds && *previous_moved_seconds < 10 * 60 &&
                (final_moved_clock == previous_moved_clock ||
                 !plausible_clock_after_move(final_moved_clock, previous_moved_clock)) &&
                !historical_handoff_clock_override;
            if (pending_stale_branch &&
                data.moves.size() == pending_stale_ply + 1 &&
                std::abs(t - pending_stale_timestamp) <= 0.35 &&
                low_time_stale_or_bad_clock) {
                auto starts_from_reference_destination = [](const std::string& candidate, const std::string& reference) {
                    if (candidate.size() < 4 || reference.size() < 4) return false;
                    return candidate[0] == reference[2] && candidate[1] == reference[3];
                };
                if (!pending_stale_prev_move.empty() &&
                    starts_from_reference_destination(move_uci, pending_stale_prev_move)) {
                    validation_rejection_reason = "pending_stale_reference";
                    all_validations_passed = false;
                    break;
                }
                suppressed_stale_branch = true;
                suppressed_stale_ply = pending_stale_ply;
                suppressed_stale_first_move = pending_stale_first_move;
                demote_tail_to_variation(pending_stale_ply);
                pending_stale_branch = false;
                pending_stale_first_move.clear();
                pending_stale_prev_move.clear();
                validation_rejection_reason = "pending_stale_suppressed";
                all_validations_passed = false;
                break;
            }
            bool record_move_in_timeline = true;
            const char accepted_piece =
                static_cast<char>(std::tolower(static_cast<unsigned char>(moving_piece)));
            if (accepted_piece == 'b' && quiet_destination_before_move && best.score < 70.0) {
                constexpr double kTransientBishopWindowSeconds = 2.5;
                for (size_t lookahead = i + 1; lookahead < candidates.size(); ++lookahead) {
                    const CandidateFrame& future_cf = candidates[lookahead];
                    if (future_cf.t - t > kTransientBishopWindowSeconds) {
                        break;
                    }
                    static thread_local cv::Mat future_latest_diff;
                    GPUAccelerator::absdiff(future_cf.board_gray, revert_mgr.get_latest_gray(), future_latest_diff);
                    if (cv::mean(future_latest_diff)[0] < kRevertFullImageMeanDiff) {
                        record_move_in_timeline = false;
                        break;
                    }
                }
            }

            if (suppressed_stale_branch &&
                data.moves.size() == suppressed_stale_ply &&
                move_uci != suppressed_stale_first_move) {
                suppressed_stale_branch = false;
                suppressed_stale_first_move.clear();
            }

            // A replacement move can reveal the settled clock for a retained
            // source move after the source board itself was never emitted as a
            // stable frame. Repair only the side that moved into that source
            // position, and only when the existing source clock is implausible
            // relative to its parent. This is state-derived OCR recovery, not
            // fixture data.
            if (historical_handoff_applied && historical_handoff_source_ply &&
                *historical_handoff_source_ply > 0 &&
                *historical_handoff_source_ply < data.clocks.size() &&
                *historical_handoff_source_ply - 1 < data.fens.size()) {
                const std::string& source_parent_fen = data.fens[*historical_handoff_source_ply - 1];
                const size_t active_field_start = source_parent_fen.find(' ');
                const bool source_move_was_white =
                    active_field_start != std::string::npos &&
                    active_field_start + 1 < source_parent_fen.size() &&
                    source_parent_fen[active_field_start + 1] == 'w';
                ClockInfo& source_clock = data.clocks[*historical_handoff_source_ply];
                const ClockInfo& parent_clock = data.clocks[*historical_handoff_source_ply - 1];
                if (source_move_was_white &&
                    plausible_clock_after_move(clocks.white_time, parent_clock.white_time) &&
                    !plausible_clock_after_move(source_clock.white_time, parent_clock.white_time)) {
                    source_clock.white_time = clocks.white_time;
                } else if (!source_move_was_white &&
                           plausible_clock_after_move(clocks.black_time, parent_clock.black_time) &&
                           !plausible_clock_after_move(source_clock.black_time, parent_clock.black_time)) {
                    source_clock.black_time = clocks.black_time;
                }
            }

            // A replacement frame can arrive after the next clock update. If
            // the first demoted-tail clock is a legal, nearby reading for the
            // replacement mover, retain it as the handoff clock. Subsequent
            // moves must still be observed from the replacement position.
            // This is based only on the retained state and OCR plausibility,
            // so it applies to any branch transition with the same shape.
            if (historical_handoff_applied) {
                const size_t handoff_clock_index = moved_player == "white" ? 0 : 1;
                const std::string& parent_time = handoff_clock_index == 0
                    ? data.clocks.back().white_time : data.clocks.back().black_time;
                std::string current_time = handoff_clock_index == 0
                    ? clocks.white_time : clocks.black_time;
                // The first stale move is the move being replaced. Its clock
                // is stored at source_ply + 1.
                const size_t stale_replaced_clock_index = historical_handoff_source_ply
                    ? *historical_handoff_source_ply + 1 : data.clocks.size();
                const std::string stale_tail_time =
                    historical_replaced_clock
                        ? (handoff_clock_index == 0
                            ? historical_replaced_clock->white_time
                            : historical_replaced_clock->black_time)
                    : stale_replaced_clock_index < data.clocks.size()
                        ? (handoff_clock_index == 0
                            ? data.clocks[stale_replaced_clock_index].white_time
                            : data.clocks[stale_replaced_clock_index].black_time)
                        : std::string();

                // Prefer an earlier frame that shows the same post-move board
                // when the handoff arrives after the clock has already
                // advanced.  Candidate frames are already available in the
                // reducer's bounded lookback, so this adds no video seek and
                // remains independent of any fixture identity.
                std::string historical_clock_time;
                double historical_clock_mean = std::numeric_limits<double>::max();
                double historical_clock_timestamp = 0.0;
                for (size_t prior_index = i; prior_index-- > 0;) {
                    const CandidateFrame& prior_cf = candidates[prior_index];
                    if (t - prior_cf.t > 15.0) break;
                    if (prior_cf.board_gray.empty() || prior_cf.clock_top_bgr.empty() ||
                        prior_cf.clock_bot_bgr.empty()) continue;
                    cv::Mat prior_diff;
                    GPUAccelerator::absdiff(board_gray, prior_cf.board_gray, prior_diff);
                    const double prior_mean = cv::mean(prior_diff)[0];
                    if (prior_mean >= historical_clock_mean || prior_mean > 20.0) continue;
                    ClockState prior_clocks = extract_clocks_for_moved_player_from_rois(
                        prior_cf.clock_top_bgr, prior_cf.clock_bot_bgr, moved_player, nullptr);
                    const std::string& prior_time = handoff_clock_index == 0
                        ? prior_clocks.white_time : prior_clocks.black_time;
                    if (parse_clock_seconds(prior_time)) {
                        historical_clock_time = prior_time;
                        historical_clock_mean = prior_mean;
                        historical_clock_timestamp = prior_cf.t;
                    }
                }
                const auto current_clock_seconds = parse_clock_seconds(current_time);
                if (current_clock_seconds) {
                    for (const ClockInfo& retained_clock : data.clocks) {
                        const std::string& retained_time = handoff_clock_index == 0
                            ? retained_clock.white_time : retained_clock.black_time;
                        const auto retained_seconds = parse_clock_seconds(retained_time);
                        if (!retained_seconds ||
                            std::abs(*current_clock_seconds - *retained_seconds) > 180) continue;
                        if (historical_clock_time.empty()) {
                            historical_clock_time = retained_time;
                            historical_clock_mean = 0.0;
                            historical_clock_timestamp = -1.0;
                        }
                    }
                }
                if (!historical_clock_time.empty()) {
                    const auto current_seconds = parse_clock_seconds(current_time);
                    const auto historical_seconds = parse_clock_seconds(historical_clock_time);
                    if (current_seconds && historical_seconds &&
                        std::abs(*current_seconds - *historical_seconds) <= 180) {
                        current_time = historical_clock_time;
                        if (handoff_clock_index == 0) {
                            clocks.white_time = historical_clock_time;
                        } else {
                            clocks.black_time = historical_clock_time;
                        }
                        trace_candidate("CLOCK_HISTORY_REUSE", t, data.moves.size(), data.fens.back(), move_uci,
                                        best.score, max_sd, 0.0, 0.0,
                                        "value=" + historical_clock_time +
                                        ";timestamp=" + std::to_string(historical_clock_timestamp) +
                                        ";board_mean=" + std::to_string(historical_clock_mean));
                    }
                }
                const auto parent_seconds = parse_clock_seconds(parent_time);
                const auto current_seconds = parse_clock_seconds(current_time);
                const auto stale_tail_seconds = parse_clock_seconds(stale_tail_time);
                trace_candidate("CLOCK_HANDOFF_CHECK", t, data.moves.size(), data.fens.back(), move_uci,
                                best.score, max_sd, 0.0, 0.0,
                                "current=" + current_time +
                                ";stale_tail=" + stale_tail_time +
                                ";parent=" + parent_time +
                                ";rebased_count=0");
                if (parent_seconds && current_seconds && stale_tail_seconds &&
                    plausible_clock_after_move(stale_tail_time, parent_time) &&
                    plausible_clock_after_move(current_time, parent_time) &&
                    std::abs(*current_seconds - *stale_tail_seconds) <= 65) {
                    // A chess clock only runs downward.  During a branch
                    // handoff the live OCR value and the demoted branch's
                    // retained value can both be plausible, so retain the
                    // lower reading instead of preferring the stale branch
                    // unconditionally.
                    const std::string& preferred_time =
                        *current_seconds <= *stale_tail_seconds
                            ? current_time : stale_tail_time;
                    if (handoff_clock_index == 0) {
                        clocks.white_time = preferred_time;
                    } else {
                        clocks.black_time = preferred_time;
                    }
                    trace_candidate("CLOCK_HANDOFF_REUSE", t, data.moves.size(), data.fens.back(), move_uci,
                                    best.score, max_sd, 0.0, 0.0,
                                    "current=" + current_time +
                                    ";stale_tail=" + stale_tail_time +
                                    ";parent=" + parent_time +
                                    ";preferred=" + preferred_time);
                }
            }
            data.moves.push_back(move_uci);
            data.timestamps.push_back(t);
            if (record_move_in_timeline) {
                data.video_timestamps.push_back(visual_move_t);
                data.video_moves.push_back(move_uci);
                move_video_indices.push_back(data.video_moves.size() - 1);
            } else {
                move_video_indices.push_back(std::nullopt);
            }
            move_scores.push_back(best.score);

            if (record_move_in_timeline) {
                std::ostringstream move_log_ss;
                move_log_ss << utils::ts(elapsed()) << " [Branch " << branch_counter << "] Ply " << data.moves.size()
                            << ": detected " << move_uci << " at " << t << "s (confidence: " << round_t(extractor_detail::score_to_confidence(best.score)) << "%)";
                log_info(move_log_ss.str());
            }
            if (debug_level_ != DebugLevel::None) { // Use a lambda to pass `this` context
                extractor_detail::log_top_candidates(sq_means, pos_ptr_.get(), log_info, elapsed());
            }
            // Apply the move in libchess to update position state
            (void)pos_ptr_->makemove(validated_move);
            extracted_in_frame = true;
            trace_candidate("ACCEPT", t, data.moves.size(), pos_ptr_->get_fen(), move_uci, best.score, max_sd,
                            validation::check_yellowness(board_bgr, *geo_, from_name),
                            validation::check_yellowness(board_bgr, *geo_, to_name),
                            "visual_t=" + std::to_string(visual_move_t) +
                            ";coalesced=" + (extracted_in_frame ? "1" : "0") +
                            ";postgame_branch=" + (postgame_replay_branch ? "1" : "0"));

            consumed_squares[best.from_sq] = true;
            consumed_squares[best.to_sq] = true;
            if (validated_move.type() == libchess::MoveType::ksc || validated_move.type() == libchess::MoveType::qsc) {
                if (best.to_sq == 6) { consumed_squares[7] = true; consumed_squares[5] = true; }
                else if (best.to_sq == 62) { consumed_squares[63] = true; consumed_squares[61] = true; }
                else if (best.to_sq == 2) { consumed_squares[0] = true; consumed_squares[3] = true; }
                else if (best.to_sq == 58) { consumed_squares[56] = true; consumed_squares[59] = true; }
            } else if (validated_move.type() == libchess::MoveType::enpassant) {
                consumed_squares[(best.to_sq & 7) | (best.from_sq & 0x38)] = true;
            }

            // Update FEN, board image history, and clock history
            data.fens.push_back(pos_ptr_->get_fen());
            if (record_move_in_timeline) {
                data.video_fens.push_back(pos_ptr_->get_fen());
                if (fen_cb_) fen_cb_(pos_ptr_->get_fen());
            }
            
            data.clocks.push_back({clocks.active_player, clocks.white_time, clocks.black_time,
                                   moved_time_observed, moved_time_missing});
            if (trace_stream.is_open()) {
                trace_candidate("CLOCK_STATE", t, data.moves.size(), data.fens.back(), move_uci,
                                best.score, max_sd, 0.0, 0.0,
                                "white=" + data.clocks.back().white_time +
                                ";black=" + data.clocks.back().black_time +
                                ";active=" + data.clocks.back().active);
            }
            bool rebased_continuation_applied = false;
            if (historical_handoff_applied && extracted_in_frame && !historical_rebased_moves.empty()) {
                // The replacement frame verified the new branch root. Move
                // only the short, recent, state-compatible continuation onto
                // that root; older tail moves remain in the variation.
                for (const RebasedContinuation& continuation : historical_rebased_moves) {
                    const size_t rebased_clock_index =
                        pos_ptr_->turn() == libchess::Side::White ? 0 : 1;
                    libchess::Move rebased_move = pos_ptr_->parse_move(continuation.move);
                    pos_ptr_->makemove(rebased_move);
                    data.moves.push_back(continuation.move);
                    data.timestamps.push_back(continuation.timestamp);
                    data.fens.push_back(pos_ptr_->get_fen());
                    data.clocks.push_back(continuation.clock);
                    data.video_timestamps.push_back(continuation.timestamp);
                    data.video_fens.push_back(pos_ptr_->get_fen());
                    data.video_moves.push_back(continuation.move);
                    move_scores.push_back(continuation.score);
                    move_video_indices.push_back(data.video_moves.size() - 1);
                    rebased_clock_ply_indices.push_back(data.moves.size() - 1);
                    last_moved_clock[rebased_clock_index] =
                        rebased_clock_index == 0 ? continuation.clock.white_time : continuation.clock.black_time;
                    trace_candidate("REBASED_CONTINUATION", t, data.moves.size(), data.fens.back(),
                                    continuation.move, continuation.score, max_sd, 0.0, 0.0,
                                    "original_ply=" + std::to_string(continuation.original_ply));
                }
                rebased_continuation_applied = true;
                historical_rebased_moves.clear();
                preserved_replay_mainline.reset();
                postgame_branch_start_ply = data.moves.size();
                ignore_replay_reverts_before_ply = postgame_branch_start_ply;
                postgame_replay_mode = true;
                postgame_boundary_from_clock_gap = false;
            }
            if (historical_handoff_applied && historical_handoff_source_ply &&
                preserved_replay_mainline &&
                *historical_handoff_source_ply == preserved_replay_mainline->replay_parent + 1 &&
                !rebased_continuation_applied) {
                // The preserved source has now joined the verified main line.
                // Subsequent moves are kept behind a generic branch boundary
                // until the UI returns to this exact parent state; that
                // return is what authorizes the next main-line continuation.
                const size_t branch_parent = data.moves.size();
                preserved_replay_mainline.reset();
                postgame_branch_start_ply = branch_parent;
                ignore_replay_reverts_before_ply = branch_parent;
                postgame_replay_mode = true;
                postgame_boundary_from_clock_gap = false;
            }
            if (postgame_branch_start_ply && postgame_replay_mode &&
                data.moves.size() == *postgame_branch_start_ply + 1 &&
                best.score >= 100.0 && !postgame_boundary_from_clock_gap) {
                log_info(utils::ts(elapsed()) + " Cleared the post-game branch boundary after a strong parent continuation at ply " +
                         std::to_string(data.moves.size()));
                postgame_branch_start_ply.reset();
                ignore_replay_reverts_before_ply.reset();
                postgame_replay_mode = false;
                postgame_boundary_from_clock_gap = false;
            }
            first_move_after_revert = false;

            // Preserve a strongly visually registered move while the UI may
            // briefly explore an alternate line. If the board later returns
            // to this exact post-move state, the intervening line can be
            // recorded as a variation and the verified main line restored.
            if (strong_visual_clock_veto_override && strong_immediate_recapture &&
                !preserved_replay_mainline && data.moves.size() > 0) {
                PreservedMainline preserved;
                preserved.moves = data.moves;
                preserved.timestamps = data.timestamps;
                preserved.fens = data.fens;
                preserved.clocks = data.clocks;
                preserved.video_fens = data.video_fens;
                preserved.video_timestamps = data.video_timestamps;
                preserved.video_moves = data.video_moves;
                preserved.move_scores = move_scores;
                preserved.move_video_indices = move_video_indices;
                preserved.replay_parent = data.moves.size() - 1;
                preserved_replay_mainline = std::move(preserved);
            }
            if (!final_moved_clock.empty() &&
                (previous_moved_clock.empty() || plausible_clock_after_move(final_moved_clock, previous_moved_clock))) {
                last_moved_clock[moved_clock_idx] = final_moved_clock;
            }
            if (low_time_stale_or_bad_clock && !strong_visual_clock_veto_override) {
                pending_stale_branch = true;
                pending_stale_ply = data.moves.size() - 1;
                pending_stale_first_move = move_uci;
                pending_stale_prev_move = data.moves.size() >= 2 ? data.moves[data.moves.size() - 2] : std::string();
                pending_stale_timestamp = t;
                if (strong_visual_clock_veto_override) {
                    postgame_branch_start_ply = data.moves.size();
                    postgame_boundary_from_clock_gap = false;
                }
            } else {
                pending_stale_branch = false;
                pending_stale_first_move.clear();
                pending_stale_prev_move.clear();
            }

            if (strong_visual_clock_veto_override && strong_immediate_recapture) {
                trace_candidate("COALESCED_STOP", t, data.moves.size(), pos_ptr_->get_fen(), "", 0.0, max_sd,
                                0.0, 0.0, "reason=strong_recapture_clock_registration");
                break;
            }

            if (debug_level_ != DebugLevel::None) {
                char fname[80];
                snprintf(fname, sizeof(fname), "%s/%02d_b%d_%s_%.2fs.png",
                         debug_dir.c_str(), static_cast<int>(data.moves.size()),
                         branch_counter, move_uci.c_str(), t);
                cv::imwrite(fname, full_bgr);
            }
        }

        // If the visual handoff candidate was rejected by the normal legal,
        // highlight, hover, or clock validations, it was evidence of the
        // beginning of a discarded analysis line rather than a replacement
        // main-line move.  Keep the selected source as the generic branch
        // boundary so subsequent accepted analysis moves cannot leak into the
        // main line.  A handoff that did accept a move is left untouched.
        if (historical_handoff_applied && !extracted_in_frame &&
            historical_handoff_source_ply && !postgame_branch_start_ply) {
            postgame_branch_start_ply = historical_handoff_source_ply;
            historical_handoff_result = "rejected_boundary";
        } else if (historical_handoff_applied && !extracted_in_frame) {
            historical_handoff_result = "rejected_existing_boundary";
        } else if (historical_handoff_applied && extracted_in_frame) {
            historical_handoff_result = "accepted";
        }

        trace_candidate("HANDOFF_RESULT", t, data.moves.size(), pos_ptr_->get_fen(),
                        historical_handoff_move, historical_handoff_source_score, max_sd,
                        historical_handoff_y_from, historical_handoff_y_to,
                        "result=" + historical_handoff_result +
                        ";source_ply=" + (historical_handoff_source_ply
                            ? std::to_string(*historical_handoff_source_ply) : "none") +
                        ";clock_override=" + (historical_handoff_clock_override ? "1" : "0") +
                        ";repeated_branch=" + (historical_handoff_repeated_branch ? "1" : "0") +
                        ";all_validations=" + (all_validations_passed ? "1" : "0"));

        if (extracted_in_frame) {
            std::vector<double> h;
            if (hash_computed) {
                h = current_hash;
            } else if (!cf.board_hash.empty()) {
                h = cf.board_hash;
            } else {
                h = compute_all_square_means(board_gray, *geo_, margin_h_, margin_w_);
            }
            revert_mgr.push_state(board_gray, h);
            revert_history_ply_counts.push_back(data.moves.size());
            std::fill(unresolved_consumed_squares.begin(), unresolved_consumed_squares.end(), false);
        }
        if (!all_validations_passed &&
            (validation_rejection_reason == "future_clock_drop" ||
             validation_rejection_reason == "clock_jump") &&
            last_validation_score >= 80.0 &&
            !last_validation_from.empty() && !last_validation_to.empty()) {
            const double vetoed_y_from = validation::check_yellowness(
                board_bgr, *geo_, last_validation_from.c_str());
            const double vetoed_y_to = validation::check_yellowness(
                board_bgr, *geo_, last_validation_to.c_str());
            if (vetoed_y_from >= 30.0 && vetoed_y_to >= 30.0 &&
                vetoed_y_from + vetoed_y_to >= 90.0) {
                const bool continues_speculative_line =
                    clock_vetoed_tail.empty() ||
                    last_validation_before_fen == clock_vetoed_tail.back().after_fen;
                if (continues_speculative_line) {
                    const std::string vetoed_before_fen = last_validation_before_fen;
                    try {
                    libchess::Position vetoed_position(vetoed_before_fen);
                    libchess::Move vetoed_move = vetoed_position.parse_move(last_validation_move);
                    vetoed_position.makemove(vetoed_move);
                    const std::string vetoed_after_fen = vetoed_position.get_fen();
                    if (!clock_vetoed_base_ply) {
                        clock_vetoed_base_ply = data.moves.size();
                        clock_vetoed_base_fen = last_validation_before_fen;
                    }
                    if (!clock_vetoed_tail.empty() &&
                        clock_vetoed_tail.back().move == last_validation_move) {
                        clock_vetoed_tail.back().timestamp = t;
                        clock_vetoed_tail.back().score = last_validation_score;
                    } else if (clock_vetoed_tail.empty() ||
                               vetoed_before_fen == clock_vetoed_tail.back().after_fen) {
                        clock_vetoed_tail.push_back({last_validation_move,
                                                     vetoed_before_fen,
                                                     vetoed_after_fen,
                                                     t,
                                                     last_validation_score});
                        clock_vetoed_current_fen = vetoed_after_fen;
                        clock_vetoed_last_board_gray = board_gray.clone();
                        clock_vetoed_started_at = t;
                        trace_candidate(
                            "CLOCK_VETOED_BRANCH_MOVE", t, data.moves.size(),
                            vetoed_before_fen, last_validation_move,
                            last_validation_score, max_sd,
                            vetoed_y_from, vetoed_y_to,
                            "base_ply=" + std::to_string(*clock_vetoed_base_ply) +
                            ";reason=" + validation_rejection_reason);
                    }
                    } catch (...) {
                    // The candidate was legal for the verified position, but
                    // it was not a legal continuation of this speculative
                    // branch. Do not combine unrelated visual transitions.
                    trace_candidate(
                        "CLOCK_VETOED_BRANCH_SKIP", t, data.moves.size(),
                        vetoed_before_fen, last_validation_move,
                        last_validation_score, max_sd,
                        vetoed_y_from, vetoed_y_to,
                        "reason=illegal_or_unparseable_continuation;" +
                        std::string("base_ply=") + std::to_string(data.moves.size()));
                    }
                } else {
                    trace_candidate(
                        "CLOCK_VETOED_BRANCH_SKIP", t, data.moves.size(),
                        last_validation_before_fen, last_validation_move,
                        last_validation_score, max_sd,
                        vetoed_y_from, vetoed_y_to,
                        "reason=main_position_does_not_continue_speculative_line");
                }
            } else {
                trace_candidate(
                    "CLOCK_VETOED_BRANCH_SKIP", t, data.moves.size(),
                    data.fens.back(), last_validation_move,
                    last_validation_score, max_sd,
                    vetoed_y_from, vetoed_y_to,
                    "reason=weak_yellow_evidence;" +
                    std::string("validation_reason=") + validation_rejection_reason);
            }
        } else if (!all_validations_passed &&
                   (validation_rejection_reason == "future_clock_drop" ||
                    validation_rejection_reason == "clock_jump")) {
            trace_candidate(
                "CLOCK_VETOED_BRANCH_SKIP", t, data.moves.size(),
                pos_ptr_->get_fen(), last_validation_move,
                last_validation_score, max_sd,
                0.0, 0.0,
                "reason=insufficient_validation_metadata;" +
                std::string("score=") + std::to_string(last_validation_score) +
                ";from_empty=" + (last_validation_from.empty() ? "1" : "0") +
                ";to_empty=" + (last_validation_to.empty() ? "1" : "0"));
        }
        if (!all_validations_passed) {
            diagnostic_evidence_context.rejection_reason = validation_rejection_reason.empty()
                ? "unknown"
                : validation_rejection_reason;
            if (trace_stream.is_open() || diagnostic_stream.is_open()) {
                trace_candidate(
                    "VALIDATION_REJECTED", t, data.moves.size(), pos_ptr_->get_fen(),
                    last_validation_move, last_validation_score, max_sd,
                    last_validation_from.empty() ? 0.0 : validation::check_yellowness(board_bgr, *geo_, last_validation_from.c_str()),
                    last_validation_to.empty() ? 0.0 : validation::check_yellowness(board_bgr, *geo_, last_validation_to.c_str()),
                    "candidate_index=" + std::to_string(i) +
                    ";handoff_result=" + historical_handoff_result +
                    ";reason=" + (validation_rejection_reason.empty() ? "unknown" : validation_rejection_reason) +
                    (validation_rejection_detail.empty() ? "" : ";" + validation_rejection_detail));
            }
            trace_candidate("REJECTED_FRAME", t, data.moves.size(), pos_ptr_->get_fen(), "", 0.0, max_sd, 0.0, 0.0,
                            "candidate_index=" + std::to_string(i) +
                            ";handoff_result=" + historical_handoff_result +
                            ";handoff_clock_override=" + (historical_handoff_clock_override ? "1" : "0"));
        }
    }
    }

    // Final progress line
    if (last_progress_t >= 0) {
        if (!progress_callback_) std::cout << std::endl;
    }

    if (preserved_replay_mainline) {
        const size_t parent = preserved_replay_mainline->replay_parent;
        if (data.moves.size() > parent) {
            VariationData replay_variation;
            replay_variation.moves.assign(data.moves.begin() + parent, data.moves.end());
            replay_variation.timestamps.assign(data.timestamps.begin() + parent, data.timestamps.end());
            replay_variation.fens.assign(data.fens.begin() + parent, data.fens.end() - 1);
            replay_variation.scores.assign(move_scores.begin() + parent, move_scores.end());
            replay_variation.clocks.assign(data.clocks.begin() + parent + 1, data.clocks.end());
            // A long replay can omit frames while walking the exact
            // preserved main line.  It is a repeated observation of the
            // same line, not an analysis variation.  Compare as an ordered
            // subsequence so dropped visual frames do not manufacture a
            // duplicate variation.
            const size_t replay_count = replay_variation.moves.size();
            const size_t expected_count = std::min(
                preserved_replay_mainline->moves.size() - parent,
                replay_count + static_cast<size_t>(8));
            std::vector<size_t> lcs_previous(expected_count + 1, 0);
            std::vector<size_t> lcs_current(expected_count + 1, 0);
            for (size_t replay_index = 0; replay_index < replay_count; ++replay_index) {
                std::fill(lcs_current.begin(), lcs_current.end(), 0);
                for (size_t expected_index = 0; expected_index < expected_count; ++expected_index) {
                    if (replay_variation.moves[replay_index] ==
                        preserved_replay_mainline->moves[parent + expected_index]) {
                        lcs_current[expected_index + 1] = lcs_previous[expected_index] + 1;
                    } else {
                        lcs_current[expected_index + 1] = std::max(
                            lcs_current[expected_index], lcs_previous[expected_index + 1]);
                    }
                }
                lcs_previous.swap(lcs_current);
            }
            const size_t ordered_matches = lcs_previous[expected_count];
            const bool is_preserved_mainline_replay =
                replay_count >= 8 && ordered_matches * 5 >= replay_count * 4;
            if (!is_preserved_mainline_replay) {
                add_variation(parent, std::move(replay_variation), true);
            } else {
                trace_candidate("REPLAY_DEDUPLICATED", data.timestamps.back(), data.moves.size(),
                                data.fens.back(), "", 0.0, 0.0, 0.0, 0.0,
                                "parent=" + std::to_string(parent) +
                                ";replay_plies=" + std::to_string(data.moves.size() - parent));
            }
        }
        data.moves = std::move(preserved_replay_mainline->moves);
        data.timestamps = std::move(preserved_replay_mainline->timestamps);
        data.fens = std::move(preserved_replay_mainline->fens);
        data.clocks = std::move(preserved_replay_mainline->clocks);
        data.video_fens = std::move(preserved_replay_mainline->video_fens);
        data.video_timestamps = std::move(preserved_replay_mainline->video_timestamps);
        data.video_moves = std::move(preserved_replay_mainline->video_moves);
        move_scores = std::move(preserved_replay_mainline->move_scores);
        move_video_indices = std::move(preserved_replay_mainline->move_video_indices);
    }

    if (postgame_branch_start_ply && data.moves.size() > *postgame_branch_start_ply) {
        demote_tail_to_variation(*postgame_branch_start_ply, true);
    }

    // A post-game replay can temporarily grow the working line beyond the
    // verified game length.  Variations rooted beyond the final retained ply
    // have no representable parent in GameData and are discarded at the
    // state-machine boundary.
    for (auto variation_it = data.variations.begin();
         variation_it != data.variations.end();) {
        if (variation_it->first > data.moves.size()) {
            variation_trace_events.push_back(
                "kind=variation_parent_out_of_range_suppressed;parent=" +
                std::to_string(variation_it->first) + ";main_size=" +
                std::to_string(data.moves.size()));
            variation_it = data.variations.erase(variation_it);
        } else {
            ++variation_it;
        }
    }

    // A multi-ply analysis branch must contain at least one transition with
    // the same confidence required for an ordinary coalesced follow-up.  A
    // sequence made entirely of weak visual guesses is a transient board
    // artifact, not durable variation evidence.  Single-ply branches retain
    // their stricter, lifetime-aware revert validation above.
    for (auto variation_it = data.variations.begin();
         variation_it != data.variations.end();) {
        const size_t parent_ply = variation_it->first;
        auto& siblings = variation_it->second;
        siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
            [&](const VariationData& variation) {
                if (variation.moves.size() < 2 ||
                    variation.scores.size() != variation.moves.size()) {
                    return false;
                }
                const bool has_strong_transition = std::any_of(
                    variation.scores.begin(), variation.scores.end(),
                    [](double score) { return score >= kMinCoalescedFollowupScore; });
                if (has_strong_transition) return false;
                variation_trace_events.push_back(
                    "kind=variation_all_weak_suppressed;parent=" +
                    std::to_string(parent_ply) + ";count=" +
                    std::to_string(variation.moves.size()));
                return true;
            }), siblings.end());
        if (siblings.empty()) {
            variation_it = data.variations.erase(variation_it);
        } else {
            ++variation_it;
        }
    }

    // A one-ply fragment observed long after its nominal parent, whose root
    // board no longer exists anywhere in the retained state graph, is an
    // orphaned replay fragment rather than a durable analysis variation.
    // Keep normal single-ply analysis lines: they either begin from a
    // retained state or are observed at the original branch time.
    constexpr double kOrphanReplayDelaySeconds = 60.0;
    for (auto variation_it = data.variations.begin();
         variation_it != data.variations.end();) {
        const size_t parent_ply = variation_it->first;
        auto& siblings = variation_it->second;
        siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
            [&](const VariationData& variation) {
                if (variation.moves.size() != 1 || variation.fens.empty() ||
                    variation.timestamps.empty() || parent_ply >= data.timestamps.size()) {
                    return false;
                }
                const bool root_in_final_graph = std::any_of(
                    data.fens.begin(), data.fens.end(), [&](const std::string& main_fen) {
                        return visual_fen_key(main_fen) == visual_fen_key(variation.fens.front());
                    });
                if (root_in_final_graph ||
                    variation.timestamps.front() <=
                        data.timestamps[parent_ply] + kOrphanReplayDelaySeconds) {
                    return false;
                }
                variation_trace_events.push_back(
                    "kind=variation_orphan_replay_suppressed;parent=" +
                    std::to_string(parent_ply) + ";delay=" +
                    std::to_string(variation.timestamps.front() - data.timestamps[parent_ply]));
                return true;
            }), siblings.end());
        if (siblings.empty()) {
            variation_it = data.variations.erase(variation_it);
        } else {
            ++variation_it;
        }
    }

    // A variation can be captured while a temporary replay is still the
    // working line, before the durable main line is restored.  Re-evaluate
    // those records against the final state graph: an exact transition path
    // from a final main-line FEN, first seen strictly after that main-line
    // path, carries no independent analysis information.
    for (auto variation_it = data.variations.begin();
         variation_it != data.variations.end();) {
        const size_t parent_ply = variation_it->first;
        auto& siblings = variation_it->second;
        siblings.erase(std::remove_if(siblings.begin(), siblings.end(),
            [&](const VariationData& variation) {
                if (variation.fens.empty() || variation.moves.empty() ||
                    variation.timestamps.empty()) {
                    return false;
                }
                for (size_t main_ply = 0;
                     main_ply + variation.moves.size() <= data.moves.size();
                     ++main_ply) {
                    if (data.fens[main_ply] != variation.fens.front() ||
                        main_ply >= data.timestamps.size() ||
                        variation.timestamps.front() <= data.timestamps[main_ply] + 0.05) {
                        continue;
                    }
                    bool exact_path = true;
                    for (size_t offset = 0; offset < variation.moves.size(); ++offset) {
                        if (data.moves[main_ply + offset] != variation.moves[offset]) {
                            exact_path = false;
                            break;
                        }
                    }
                    if (!exact_path) continue;
                    variation_trace_events.push_back(
                        "kind=variation_final_mainline_replay_suppressed;parent=" +
                        std::to_string(parent_ply) + ";main_ply=" +
                        std::to_string(main_ply) + ";moves=" +
                        std::to_string(variation.moves.size()));
                    return true;
                }
                return false;
            }), siblings.end());
        if (siblings.empty()) {
            variation_it = data.variations.erase(variation_it);
        } else {
            ++variation_it;
        }
    }

    // Temporal handoffs can split one analysis line into separate variation
    // records.  Normalize that representation by joining a variation whose
    // root is an exact FEN state inside another variation.  The outer prefix
    // is retained and the continuation replaces the stale suffix.  This is a
    // state-graph operation: it does not depend on a fixture, move number, or
    // expected notation.
    bool merged_variations = true;
    while (merged_variations) {
        merged_variations = false;
        for (auto outer_it = data.variations.begin();
             outer_it != data.variations.end() && !merged_variations; ++outer_it) {
            std::vector<VariationData>& outer_variations = outer_it->second;
            for (size_t outer_index = 0;
                 outer_index < outer_variations.size() && !merged_variations;
                 ++outer_index) {
                VariationData& outer = outer_variations[outer_index];
                if (outer.fens.size() < 2 || outer.moves.size() < 2) continue;

                for (auto inner_it = data.variations.begin();
                     inner_it != data.variations.end() && !merged_variations;
                     ++inner_it) {
                    for (size_t inner_index = 0;
                         inner_index < inner_it->second.size() && !merged_variations;
                         ++inner_index) {
                        if (&outer == &inner_it->second[inner_index]) continue;
                        const VariationData& inner = inner_it->second[inner_index];
                        if (inner.fens.empty() || inner.moves.empty()) continue;

                        std::optional<size_t> join_offset;
                        for (size_t offset = 1;
                             offset < outer.fens.size() && offset < outer.moves.size();
                             ++offset) {
                            if (outer.fens[offset] == inner.fens.front()) {
                                join_offset = offset;
                                break;
                            }
                        }
                        if (!join_offset) continue;

                        // A nested variation can legitimately begin at an
                        // interior state without replacing the outer line.
                        // Require evidence that the two temporal records
                        // overlap in their stale/continuation moves before
                        // treating the match as a split handoff.
                        bool suffix_overlaps_continuation = false;
                        for (size_t outer_move = *join_offset;
                             outer_move < outer.moves.size() && !suffix_overlaps_continuation;
                             ++outer_move) {
                            suffix_overlaps_continuation = std::find(
                                inner.moves.begin(), inner.moves.end(),
                                outer.moves[outer_move]) != inner.moves.end();
                        }
                        if (!suffix_overlaps_continuation) continue;

                        VariationData joined = outer;
                        joined.moves.resize(*join_offset);
                        joined.timestamps.resize(std::min(*join_offset, joined.timestamps.size()));
                        joined.fens.resize(*join_offset);
                        joined.scores.resize(std::min(*join_offset, joined.scores.size()));
                        joined.clocks.resize(std::min(*join_offset, joined.clocks.size()));
                        joined.moves.insert(joined.moves.end(), inner.moves.begin(), inner.moves.end());
                        joined.timestamps.insert(joined.timestamps.end(), inner.timestamps.begin(), inner.timestamps.end());
                        joined.fens.insert(joined.fens.end(), inner.fens.begin(), inner.fens.end());
                        joined.scores.insert(joined.scores.end(), inner.scores.begin(), inner.scores.end());
                        joined.clocks.insert(joined.clocks.end(), inner.clocks.begin(), inner.clocks.end());

                        const size_t outer_parent = outer_it->first;
                        const size_t inner_parent = inner_it->first;
                        const std::string joined_moves = [&]() {
                            std::ostringstream text;
                            for (size_t i = 0; i < joined.moves.size(); ++i) {
                                if (i != 0) text << ',';
                                text << joined.moves[i];
                            }
                            return text.str();
                        }();

                        outer = std::move(joined);
                        if (outer_parent == inner_parent) {
                            auto& siblings = data.variations[outer_parent];
                            siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(inner_index));
                            if (inner_index < outer_index) --outer_index;
                        } else {
                            auto& inner_siblings = data.variations[inner_parent];
                            inner_siblings.erase(inner_siblings.begin() + static_cast<std::ptrdiff_t>(inner_index));
                            if (inner_siblings.empty()) data.variations.erase(inner_parent);
                        }
                        variation_trace_events.push_back(
                            "kind=variation_state_join;outer_parent=" +
                            std::to_string(outer_parent) + ";inner_parent=" +
                            std::to_string(inner_parent) + ";offset=" +
                            std::to_string(*join_offset) + ";moves=" + joined_moves);
                        merged_variations = true;
                    }
                }
            }
        }
    }

    // A variation whose opening moved clock was completely unreadable must
    // not manufacture annotations from a cached parent clock. A parsed but
    // temporally suspect reading is different: it still identifies the
    // clock pill and is retained for historical analysis variations.
    for (auto& [parent_ply, variations] : data.variations) {
        for (VariationData& variation : variations) {
            if (!variation.clocks.empty() &&
                variation.clocks.front().moved_time_missing) {
                for (ClockInfo& clock : variation.clocks) {
                    clock.white_time.clear();
                    clock.black_time.clear();
                    clock.active.clear();
                }
            }

            if (variation.moves.empty() || variation.fens.empty() ||
                variation.clocks.empty()) {
                continue;
            }

            // A replay line can end in checkmate before a clock state is
            // rendered for the final mover. If that side's displayed value
            // is merely unchanged from its earlier replay turn, omit it
            // rather than presenting cached UI text as a final annotation.
            try {
                libchess::Position terminal_position(variation.fens.back());
                const libchess::Move terminal_move =
                    terminal_position.parse_move(variation.moves.back());
                terminal_position.makemove(terminal_move);
                const size_t terminal_index = variation.moves.size() - 1;
                const bool terminal_is_white =
                    ((parent_ply + terminal_index) % 2) == 0;
                const bool has_same_side_prior = terminal_index >= 2 &&
                    terminal_index < variation.clocks.size();
                const std::string& terminal_time = terminal_is_white
                    ? variation.clocks[terminal_index].white_time
                    : variation.clocks[terminal_index].black_time;
                const std::string& prior_time = has_same_side_prior
                    ? (terminal_is_white
                        ? variation.clocks[terminal_index - 2].white_time
                        : variation.clocks[terminal_index - 2].black_time)
                    : std::string{};
                if (terminal_position.is_checkmate() && has_same_side_prior &&
                    !terminal_time.empty() && terminal_time == prior_time) {
                    // No final clock observation exists, so omit the entry
                    // entirely. An empty ClockInfo would be serialized as a
                    // synthetic 0:00:00 annotation by downstream writers.
                    variation.clocks.pop_back();
                    variation_trace_events.push_back(
                        "kind=terminal_replay_clock_suppressed;parent=" +
                        std::to_string(parent_ply) + ";move=" +
                        variation.moves.back());
                }
            } catch (...) {
                // Clock metadata must never make a recoverable variation
                // state fatal; the move path is retained unchanged.
            }
        }
    }

    std::unordered_map<std::string, int> durable_move_counts;
    for (const std::string& move : data.moves) {
        ++durable_move_counts[move];
    }
    for (const auto& item : data.variations) {
        for (const VariationData& variation : item.second) {
            for (const std::string& move : variation.moves) {
                ++durable_move_counts[move];
            }
        }
    }
    for (std::string& move : data.video_moves) {
        if (move == "REVERT") continue;
        auto it = durable_move_counts.find(move);
        if (it == durable_move_counts.end() || it->second <= 0) {
            move = "REVERT";
        } else {
            --it->second;
        }
    }

    // The timeline is an observation log, while replay reduction can retain a
    // validated branch after the transient source event was pruned. Ensure
    // every final main-line/variation move still has one corresponding
    // timeline observation. This is a consistency repair over detector-backed
    // GameData, not a move inference: only moves already retained in the
    // verified state graph may be added.
    struct DurableTimelineObservation {
        std::string move;
        std::string fen;
        double timestamp = 0.0;
    };
    std::vector<DurableTimelineObservation> durable_observations;
    durable_observations.reserve(data.moves.size());
    for (size_t move_index = 0; move_index < data.moves.size(); ++move_index) {
        durable_observations.push_back({
            data.moves[move_index],
            move_index + 1 < data.fens.size() ? data.fens[move_index + 1] : data.fens.back(),
            move_index < data.timestamps.size() ? data.timestamps[move_index] : 0.0});
    }
    for (const auto& [parent_ply, variations] : data.variations) {
        for (const VariationData& variation : variations) {
            for (size_t move_index = 0; move_index < variation.moves.size(); ++move_index) {
                durable_observations.push_back({
                    variation.moves[move_index],
                    move_index < variation.fens.size() ? variation.fens[move_index] :
                        (parent_ply < data.fens.size() ? data.fens[parent_ply] : data.fens.back()),
                    move_index < variation.timestamps.size() ? variation.timestamps[move_index] :
                        (parent_ply < data.timestamps.size() ? data.timestamps[parent_ply] : 0.0)});
            }
        }
    }
    std::unordered_map<std::string, int> timeline_move_counts;
    for (const std::string& move : data.video_moves) {
        if (move != "REVERT") ++timeline_move_counts[move];
    }
    size_t reconciled_timeline_moves = 0;
    for (const DurableTimelineObservation& observation : durable_observations) {
        int& available = timeline_move_counts[observation.move];
        if (available > 0) {
            --available;
            continue;
        }
        data.video_moves.push_back(observation.move);
        data.video_fens.push_back(observation.fen);
        data.video_timestamps.push_back(observation.timestamp);
        ++reconciled_timeline_moves;
    }
    if (reconciled_timeline_moves > 0) {
        variation_trace_events.push_back(
            "kind=timeline_variation_reconciled;count=" +
            std::to_string(reconciled_timeline_moves));
    }

    if (trace_stream.is_open()) {
        for (const std::string& event : variation_trace_events) {
            trace_candidate("VARIATION_DEMOTED", trace_end, data.moves.size(), data.fens.back(),
                            "", 0.0, 0.0, 0.0, 0.0, event);
        }
        for (const auto& [parent_ply, variations] : data.variations) {
            for (size_t variation_index = 0; variation_index < variations.size(); ++variation_index) {
                const VariationData& variation = variations[variation_index];
                std::ostringstream moves;
                for (size_t i = 0; i < variation.moves.size(); ++i) {
                    if (i != 0) moves << ',';
                    moves << variation.moves[i];
                }
                std::ostringstream clocks;
                for (size_t i = 0; i < variation.clocks.size(); ++i) {
                    if (i != 0) clocks << ',';
                    clocks << variation.clocks[i].white_time << '/' << variation.clocks[i].black_time;
                }
                std::ostringstream scores;
                for (size_t i = 0; i < variation.scores.size(); ++i) {
                    if (i != 0) scores << ',';
                    scores << std::fixed << std::setprecision(1) << variation.scores[i];
                }
                // Emit the relationship between each retained branch root and
                // the final main-line state graph.  This makes replay versus
                // alternate-continuation decisions auditable without tying
                // production behavior to a fixture or expected move list.
                std::ostringstream root_matches;
                std::ostringstream visual_root_matches;
                if (!variation.fens.empty()) {
                    for (size_t main_ply = 0; main_ply < data.fens.size(); ++main_ply) {
                        if (data.fens[main_ply] == variation.fens.front()) {
                            if (root_matches.tellp() > 0) root_matches << ',';
                            root_matches << main_ply;
                        }
                        if (visual_fen_key(data.fens[main_ply]) ==
                            visual_fen_key(variation.fens.front())) {
                            if (visual_root_matches.tellp() > 0) visual_root_matches << ',';
                            visual_root_matches << main_ply;
                        }
                    }
                }
                const double branch_timestamp = variation.timestamps.empty()
                    ? -1.0 : variation.timestamps.front();
                const double parent_timestamp = parent_ply < data.timestamps.size()
                    ? data.timestamps[parent_ply] : -1.0;
                trace_candidate("FINAL_VARIATION", trace_end, parent_ply,
                                parent_ply < data.fens.size() ? data.fens[parent_ply] : std::string(),
                                "", 0.0, 0.0, 0.0, 0.0,
                                "variation_index=" + std::to_string(variation_index) +
                                ";moves=" + moves.str() +
                                ";clocks=" + clocks.str() +
                                ";scores=" + scores.str() +
                                ";root_main_plies=" + root_matches.str() +
                                ";root_visual_plies=" + visual_root_matches.str() +
                                ";branch_ts=" + std::to_string(branch_timestamp) +
                                ";parent_ts=" + std::to_string(parent_timestamp));
            }
        }
    }

    std::ostringstream perf_ss;
    perf_ss << utils::ts(elapsed()) << " Extraction perf: mapped candidates=" << mapper.get_candidates_emitted()
            << ", reduced candidates=" << reducer_candidates_seen
            << ", revert hash tests=" << revert_hash_tests
            << ", revert full tests=" << revert_full_tests
            << ", revert index queries=" << revert_index_queries
            << ", revert fallbacks=" << revert_index_fallbacks
            << ", score calls=" << score_calls << " (" << (score_us / 1000.0) << " ms)"
            << ", clock activity calls=" << clock_activity_calls << " (" << (clock_activity_us / 1000.0) << " ms)"
            << ", clock OCR calls=" << clock_ocr_calls << " (" << (clock_ocr_us / 1000.0) << " ms)"
            << ", revert scan=" << (revert_us / 1000.0) << " ms";
    log_info(perf_ss.str());

    return data;
}

} // namespace cta
