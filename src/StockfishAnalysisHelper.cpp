#include "StockfishAnalysisHelper.h"
#include "VideoProcessorWorker.h"
#include "ChessFenUtils.h" 
#include <QFileInfo>
#include <QStandardPaths>
#include <QDebug>
#include <algorithm>
#include <sstream>
#include <cmath>

namespace cta {

namespace {
int calculate_material(const std::string& fen) {
    int material = 0;
    for (char c : fen) {
        if (c == ' ') break;
        switch (c) {
            case 'Q': material += 9; break; case 'q': material -= 9; break;
            case 'R': material += 5; break; case 'r': material -= 5; break;
            case 'B': material += 3; break; case 'b': material -= 3; break;
            case 'N': material += 3; break; case 'n': material -= 3; break;
            case 'P': material += 1; break; case 'p': material -= 1; break;
        }
    }
    return material;
}
} // namespace

StockfishAnalysisHelper::StockfishAnalysisHelper(const ProcessingSettings& settings,
                                                 GameData& gameData,
                                                 std::atomic<bool>* cancelFlag,
                                                 QObject* parent)
    : QObject(parent),
      settings_(settings),
      gameData_(gameData),
      cancelFlag_(cancelFlag) {
    results_.moveAnnotations.resize(gameData_.moves.size(), "");
}

StockfishAnalysisHelper::~StockfishAnalysisHelper() = default;

void StockfishAnalysisHelper::processVideoMoveAnnotations() {
    if (results_.videoResults.empty() || results_.videoResults.size() <= gameData_.video_moves.size()) {
        return;
    }

    for (size_t i = 0; i < gameData_.video_moves.size(); ++i) {
        if (gameData_.video_moves[i] == "REVERT") continue;

        const auto& fen_before = gameData_.video_fens[i];
        bool is_white_to_move = (fen_before.find(" w ") != std::string::npos);
        const auto& analysis_before = results_.videoResults[i];
        const auto& analysis_after = results_.videoResults[i + 1];

        if (analysis_before.lines.empty() || analysis_after.lines.empty()) continue;

        int best_eval_white_pov;
        if (analysis_before.lines[0].is_mate) {
            int mate_in = analysis_before.lines[0].mate_in;
            best_eval_white_pov = is_white_to_move ? ((mate_in > 0) ? 30000 : -30000) : ((mate_in > 0) ? -30000 : 30000);
        } else {
            best_eval_white_pov = is_white_to_move ? analysis_before.lines[0].centipawns : -analysis_before.lines[0].centipawns;
        }

        int played_eval_white_pov;
        if (analysis_after.lines[0].is_mate) {
            int mate_in = analysis_after.lines[0].mate_in;
            played_eval_white_pov = !is_white_to_move ? ((mate_in > 0) ? -30000 : 30000) : ((mate_in > 0) ? 30000 : -30000);
        } else {
            played_eval_white_pov = !is_white_to_move ? analysis_after.lines[0].centipawns : -analysis_after.lines[0].centipawns;
        }

        int cp_loss = is_white_to_move ? (best_eval_white_pov - played_eval_white_pov) : (played_eval_white_pov - best_eval_white_pov);
        if (cp_loss < 0) cp_loss = 0;

        std::string video_ann = "";
        int full_move = 1;
        size_t last_space = fen_before.find_last_of(' ');
        if (last_space != std::string::npos) {
            try { full_move = std::stoi(fen_before.substr(last_space + 1)); } catch (...) {}
        }
        int ply_count = (full_move - 1) * 2 + (is_white_to_move ? 0 : 1);

        if (ply_count < 10 && cp_loss <= 25) {
            video_ann = " (Book)";
        } else if (gameData_.video_moves[i] == analysis_before.lines[0].move_uci) {
            if (analysis_before.lines.size() > 1) {
                int material_before = calculate_material(fen_before);
                int material_after = calculate_material(gameData_.video_fens[i+1]);
                int material_diff = material_after - material_before;
                bool is_sacrifice = (is_white_to_move && material_diff < 0) || (!is_white_to_move && material_diff > 0);
                bool is_good_eval = is_white_to_move ? (best_eval_white_pov > -200) : (best_eval_white_pov < 200);

                int diff_to_second = std::abs(analysis_before.lines[0].centipawns - analysis_before.lines[1].centipawns);
                if (is_sacrifice && is_good_eval) {
                    video_ann = (diff_to_second > 200 && cp_loss <= 10) ? "!!" : "!";
                } else {
                    video_ann = "*";
                }
            } else {
                video_ann = "*";
            }
        } else if (cp_loss <= 25) {
            video_ann = " (Good)";
        } else if (cp_loss >= 300) {
            video_ann = "??";
        } else if (cp_loss >= 150) {
            int side_eval_before = is_white_to_move ? best_eval_white_pov : -best_eval_white_pov;
            video_ann = (side_eval_before >= 200) ? "X" : "?";
        } else if (cp_loss >= 75) {
            video_ann = "?";
        }

        cta::StockfishLine dummy;
        dummy.move_uci = "ANNOTATION";
        dummy.pv_line = gameData_.video_moves[i] + video_ann;
        // This is a bit of a hack: add the annotation as an extra line
        // to the StockfishResult for the *next* position in the video results.
        // This allows AnalysisVideoGenerator to pick it up.
        results_.videoResults[i + 1].lines.push_back(dummy);
    }
}

void StockfishAnalysisHelper::processMainLineAnnotationsAndStats() {
    if (!settings_.generatePgn || results_.mainLineResults.empty() || results_.mainLineResults.size() <= gameData_.moves.size()) {
        return;
    }

    double total_loss_white = 0.0;
    double total_loss_black = 0.0;
    int moves_white = 0;
    int moves_black = 0;

    for (size_t i = 0; i < gameData_.moves.size(); ++i) {
        const auto& fen_before = gameData_.fens[i];
        bool is_white_to_move = (fen_before.find(" w ") != std::string::npos);
        const auto& analysis_before = results_.mainLineResults[i];
        const auto& analysis_after = results_.mainLineResults[i + 1];

        if (analysis_before.lines.empty() || analysis_after.lines.empty()) continue;

        int best_eval_white_pov;
        if (analysis_before.lines[0].is_mate) {
            int mate_in = analysis_before.lines[0].mate_in;
            best_eval_white_pov = is_white_to_move ? ((mate_in > 0) ? 30000 : -30000) : ((mate_in > 0) ? -30000 : 30000);
        } else {
            best_eval_white_pov = is_white_to_move ? analysis_before.lines[0].centipawns : -analysis_before.lines[0].centipawns;
        }

        int played_eval_white_pov;
        if (analysis_after.lines[0].is_mate) {
            int mate_in = analysis_after.lines[0].mate_in;
            played_eval_white_pov = !is_white_to_move ? ((mate_in > 0) ? -30000 : 30000) : ((mate_in > 0) ? 30000 : -30000);
        } else {
            played_eval_white_pov = !is_white_to_move ? analysis_after.lines[0].centipawns : -analysis_after.lines[0].centipawns;
        }

        int cp_loss = is_white_to_move ? (best_eval_white_pov - played_eval_white_pov) : (played_eval_white_pov - best_eval_white_pov);
        if (cp_loss < 0) cp_loss = 0;

        int capped_loss = std::min(cp_loss, 1000);
        if (is_white_to_move) {
            total_loss_white += capped_loss;
            moves_white++;
        } else {
            total_loss_black += capped_loss;
            moves_black++;
        }

        std::string annotation = "";
        int full_move = 1;
        size_t last_space = fen_before.find_last_of(' ');
        if (last_space != std::string::npos) {
            try { full_move = std::stoi(fen_before.substr(last_space + 1)); } catch (...) {}
        }
        int ply_count = (full_move - 1) * 2 + (is_white_to_move ? 0 : 1);

        if (ply_count < 10 && cp_loss <= 25) {
            annotation = " \xF0\x9F\x93\x96"; // Book
        } else if (gameData_.moves[i] == analysis_before.lines[0].move_uci) {
            if (analysis_before.lines.size() > 1) {
                            int material_before = calculate_material(fen_before);
                            int material_after = calculate_material(gameData_.fens[i+1]);
                int material_diff = material_after - material_before;
                bool is_sacrifice = (is_white_to_move && material_diff < 0) || (!is_white_to_move && material_diff > 0);
                bool is_good_eval = is_white_to_move ? (best_eval_white_pov > -200) : (best_eval_white_pov < 200);

                int diff_to_second = std::abs(analysis_before.lines[0].centipawns - analysis_before.lines[1].centipawns);
                if (is_sacrifice && is_good_eval) {
                    annotation = (diff_to_second > 200 && cp_loss <= 10) ? "!!" : "!";
                } else {
                    annotation = "*";
                }
            } else {
                annotation = "*";
            }
        } else if (cp_loss <= 25) {
            annotation = ""; // No annotation for good moves
        } else if (cp_loss >= 300) {
            annotation = "??";
        } else if (cp_loss >= 150) {
            int side_eval_before = is_white_to_move ? best_eval_white_pov : -best_eval_white_pov;
            annotation = (side_eval_before >= 200) ? "X" : "?";
        } else if (cp_loss >= 75) {
            annotation = "?";
        }
        
        results_.moveAnnotations[i] = annotation;
    }

    if (moves_white > 0 || moves_black > 0) {
        int depth = settings_.stockfishDepth;
        double depth_multiplier = 10.0 + std::max(0, 20 - depth) * 0.5;
        int base_elo = 3000 - std::max(0, 20 - depth) * 50;
        
        if (moves_white > 0) {
            results_.whiteAcpl = total_loss_white / moves_white;
            results_.whiteEstimatedElo = std::clamp(static_cast<int>(base_elo - results_.whiteAcpl * depth_multiplier), 100, 3000);
        }
        if (moves_black > 0) {
            results_.blackAcpl = total_loss_black / moves_black;
            results_.blackEstimatedElo = std::clamp(static_cast<int>(base_elo - results_.blackAcpl * depth_multiplier), 100, 3000);
        }
        
        double w_acc = results_.whiteAcpl >= 0 ? std::clamp(100.0 * std::exp(-0.007 * results_.whiteAcpl), 0.0, 100.0) : 0.0;
        double b_acc = results_.blackAcpl >= 0 ? std::clamp(100.0 * std::exp(-0.007 * results_.blackAcpl), 0.0, 100.0) : 0.0;

        emit logMessage(QString("Estimated Performance:") + 
            QString("\n  White: ~%1 Elo | %2% Accuracy | %3 ACPL").arg(results_.whiteEstimatedElo).arg(w_acc, 0, 'f', 1).arg(results_.whiteAcpl >= 0 ? results_.whiteAcpl : 0.0, 0, 'f', 1) +
            QString("\n  Black: ~%1 Elo | %2% Accuracy | %3 ACPL").arg(results_.blackEstimatedElo).arg(b_acc, 0, 'f', 1).arg(results_.blackAcpl >= 0 ? results_.blackAcpl : 0.0, 0, 'f', 1));
    }
}

bool StockfishAnalysisHelper::runAnalysis() {
    if (gameData_.fens.empty()) {
        emit logMessage("No FENs available for Stockfish analysis.");
        return true;
    }

    std::vector<std::string> unique_fens;
    for (const auto& fen : gameData_.video_fens) {
        if (std::find(unique_fens.begin(), unique_fens.end(), fen) == unique_fens.end()) {
            unique_fens.push_back(fen);
        }
    }

    QString limitsStr = QString("Depth=%1").arg(settings_.stockfishDepth);
    if (settings_.stockfishTime > 0) limitsStr += QString(", Time=%1s").arg(settings_.stockfishTime);
    if (settings_.stockfishNodes > 0) limitsStr += QString(", Nodes=%1").arg(settings_.stockfishNodes);

    emit logMessage("Starting Stockfish analysis (MultiPV=" + QString::number(settings_.multiPv) + ", " + limitsStr + ", " + QString::number(unique_fens.size()) + " unique positions)...");

    analyzer_ = std::make_unique<StockfishAnalyzer>(settings_.multiPv, settings_.stockfishPath.toStdString(), settings_.ffmpegThreads);
    analyzer_->set_progress_callback([this](int current, int total) {
        QString msg = QString("Analyzing position %1 of %2...").arg(current).arg(total);
        emit logMessage(msg);
        if (total > 0) {
            emit progressUpdated((current * 100) / total);
        }
    });

    std::vector<StockfishResult> unique_results = analyzer_->analyze_positions(unique_fens, settings_.stockfishDepth, settings_.stockfishTime * 1000, settings_.stockfishNodes, cancelFlag_);
    if (cancelFlag_ && *cancelFlag_) {
        return false;
    }

    for (auto& result : unique_results) {
        for (auto& line : result.lines) {
            std::istringstream pv_stream(line.pv_line);
            std::string move_uci;
            std::string truncated_pv;
            int count = 0;
            while (count < settings_.stockfishAnalysisDepth && (pv_stream >> move_uci)) {
                if (!truncated_pv.empty()) truncated_pv += " ";
                truncated_pv += move_uci;
                count++;
            }
            line.pv_line = truncated_pv;
        }
        results_.fenCache[result.fen] = result;
    }

    results_.mainLineResults.reserve(gameData_.fens.size());
    for (const auto& fen : gameData_.fens) {
        results_.mainLineResults.push_back(results_.fenCache[fen]);
    }
    results_.videoResults.reserve(gameData_.video_fens.size());
    for (const auto& fen : gameData_.video_fens) {
        results_.videoResults.push_back(results_.fenCache[fen]);
    }

    emit logMessage("Stockfish analysis complete.");

    if (settings_.enableMoveAnnotations) {
        emit logMessage("Generating move quality annotations...");
        processVideoMoveAnnotations();
        processMainLineAnnotationsAndStats();
    }
    
    return true;
}

} // namespace cta