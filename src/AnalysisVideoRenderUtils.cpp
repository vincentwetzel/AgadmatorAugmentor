#include "AnalysisVideoRenderUtils.h"
#include "ChessFenUtils.h" // For get_line_score_cp
#include "libchess/position.hpp" // For libchess::Position

namespace cta {
namespace AnalysisVideoRenderUtils {

cv::Scalar arrow_gradient_color_at(cv::Point start, cv::Point end, int x, int y, const EngineArrowStyle& style) {
    const double dx = static_cast<double>(end.x - start.x);
    const double dy = static_cast<double>(end.y - start.y);
    const double len2 = dx * dx + dy * dy;
    double t = 1.0;
    if (len2 > 1.0) {
        t = ((x - start.x) * dx + (y - start.y) * dy) / len2;
        t = std::clamp(t, 0.0, 1.0);
    }
    t = t * t * (3.0 - 2.0 * t);

    return style.tail_color * (1.0 - t) + style.head_color * t;
}

void drawEngineArrow(cv::Mat& overlay, cv::Point start, cv::Point end, cv::Scalar color, double squareSize, int thicknessPct) {
    cv::Point2f startf(static_cast<float>(start.x), static_cast<float>(start.y));
    cv::Point2f endf(static_cast<float>(end.x), static_cast<float>(end.y));

    cv::Point2f delta = endf - startf;
    float length = std::sqrt(delta.dot(delta));
    if (length <= 1.0f) return;

    cv::Point2f dir = delta * (1.0f / length);
    cv::Point2f perp(-dir.y, dir.x);

    const float baseThickness = static_cast<float>(std::max(squareSize * (thicknessPct / 100.0), squareSize * 0.11));
    const float shaftThickness = baseThickness;
    const float baseRadius = shaftThickness * 0.72f;
    const float headLength = std::min(std::max(shaftThickness * 2.35f, static_cast<float>(squareSize * 0.34)), length * 0.52f);
    const float headWidth = std::max(shaftThickness * 2.45f, static_cast<float>(squareSize * 0.40));
    const float neckWidth = std::max(shaftThickness * 1.20f, headWidth * 0.52f);
    const float tipInset = std::min(static_cast<float>(squareSize * 0.08), length * 0.12f);
    const float startInset = std::min(baseRadius * 0.55f, length * 0.14f);

    cv::Point2f tip = endf - dir * tipInset;
    cv::Point2f headBase = tip - dir * headLength;
    cv::Point2f bodyStart = startf + dir * startInset;
    cv::Point2f bodyEnd = headBase + dir * std::min(headLength * 0.18f, length * 0.08f);

    auto toPoint = [](const cv::Point2f& p) {
        return cv::Point(cvRound(p.x), cvRound(p.y));
    };

    // Rounded shaft and base match the softer site-style analysis arrows better
    cv::line(overlay, toPoint(bodyStart), toPoint(bodyEnd), color, std::max(1, cvRound(shaftThickness)), cv::LINE_AA);
    cv::circle(overlay, toPoint(startf), cvRound(baseRadius), color, cv::FILLED, cv::LINE_AA);
    cv::circle(overlay, toPoint(bodyEnd), std::max(1, cvRound(neckWidth * 0.32f)), color, cv::FILLED, cv::LINE_AA);

    std::vector<cv::Point> headPts;
    headPts.reserve(5);
    headPts.push_back(toPoint(bodyEnd + perp * (neckWidth * 0.5f)));
    headPts.push_back(toPoint(headBase + perp * (headWidth * 0.5f)));
    headPts.push_back(toPoint(tip));
    headPts.push_back(toPoint(headBase - perp * (headWidth * 0.5f)));
    headPts.push_back(toPoint(bodyEnd - perp * (neckWidth * 0.5f)));
    cv::fillConvexPoly(overlay, headPts, color, cv::LINE_AA);
}

void blend_arrow_on_bgr(cv::Mat& image,
                        cv::Point start,
                        cv::Point end,
                        const EngineArrowStyle& style,
                        double squareSize) {
    if (style.opacity <= 0.0) return;

    cv::Mat fill_mask(image.size(), CV_8UC1, cv::Scalar(0));
    drawEngineArrow(fill_mask, start, end, cv::Scalar(255), squareSize, style.thickness_pct);

    for (int y = 0; y < image.rows; ++y) {
        const uchar* mask_ptr = fill_mask.ptr<uchar>(y);
        cv::Vec3b* image_ptr = image.ptr<cv::Vec3b>(y);
        for (int x = 0; x < image.cols; ++x) {
            if (mask_ptr[x] == 0) continue;

            const double alpha = (mask_ptr[x] / 255.0) * style.opacity;
            const cv::Scalar color = arrow_gradient_color_at(start, end, x, y, style);
            cv::Vec3b& px = image_ptr[x];
            px[0] = static_cast<uchar>(std::clamp(px[0] * (1.0 - alpha) + color[0] * alpha, 0.0, 255.0));
            px[1] = static_cast<uchar>(std::clamp(px[1] * (1.0 - alpha) + color[1] * alpha, 0.0, 255.0));
            px[2] = static_cast<uchar>(std::clamp(px[2] * (1.0 - alpha) + color[2] * alpha, 0.0, 255.0));
        }
    }
}

void blend_arrow_on_bgra(cv::Mat& image,
                         cv::Point start,
                         cv::Point end,
                         const EngineArrowStyle& style,
                         double squareSize) {
    if (style.opacity <= 0.0) return;

    cv::Mat fill_mask(image.size(), CV_8UC1, cv::Scalar(0));
    drawEngineArrow(fill_mask, start, end, cv::Scalar(255), squareSize, style.thickness_pct);

    for (int y = 0; y < image.rows; ++y) {
        const uchar* mask_ptr = fill_mask.ptr<uchar>(y);
        cv::Vec4b* image_ptr = image.ptr<cv::Vec4b>(y);
        for (int x = 0; x < image.cols; ++x) {
            if (mask_ptr[x] == 0) continue;

            const double src_alpha = (mask_ptr[x] / 255.0) * style.opacity;
            cv::Vec4b& px = image_ptr[x];
            const double dst_alpha = px[3] / 255.0;
            const double out_alpha = src_alpha + dst_alpha * (1.0 - src_alpha);
            if (out_alpha <= 0.0) continue;

            const cv::Scalar color = arrow_gradient_color_at(start, end, x, y, style);
            const double src_b = color[0] / 255.0;
            const double src_g = color[1] / 255.0;
            const double src_r = color[2] / 255.0;
            const double dst_b = px[0] / 255.0;
            const double dst_g = px[1] / 255.0;
            const double dst_r = px[2] / 255.0;

            const double out_b = (src_b * src_alpha + dst_b * dst_alpha * (1.0 - src_alpha)) / out_alpha;
            const double out_g = (src_g * src_alpha + dst_g * dst_alpha * (1.0 - src_alpha)) / out_alpha;
            const double out_r = (src_r * src_alpha + dst_r * dst_alpha * (1.0 - src_alpha)) / out_alpha;

            px[0] = static_cast<uchar>(std::clamp(std::round(out_b * 255.0), 0.0, 255.0));
            px[1] = static_cast<uchar>(std::clamp(std::round(out_g * 255.0), 0.0, 255.0));
            px[2] = static_cast<uchar>(std::clamp(std::round(out_r * 255.0), 0.0, 255.0));
            px[3] = static_cast<uchar>(std::clamp(std::round(out_alpha * 255.0), 0.0, 255.0));
        }
    }
}

void drawMoveAnnotationOnBoard(cv::Mat& img, const std::string& uci, const std::string& sym, double sq_w, double sq_h) {
    if (uci.length() < 4) return;
    std::string clean_sym = sym;
    clean_sym.erase(0, clean_sym.find_first_not_of(" "));
    clean_sym.erase(clean_sym.find_last_not_of(" ") + 1);
    if (clean_sym.empty()) return;

    int to_col = uci[2] - 'a';
    int to_row = 7 - (uci[3] - '1');
    
    if (to_col < 0 || to_col > 7 || to_row < 0 || to_row > 7) return;

    double cx = (to_col + 0.5) * sq_w;
    double cy = (to_row + 0.5) * sq_h;

    int ax = static_cast<int>(cx + sq_w * 0.35);
    int ay = static_cast<int>(cy - sq_h * 0.35);
    int radius = static_cast<int>(sq_w * 0.18);

    cv::Scalar bgColor;
    bool drawStar = false;
    bool drawBook = false;
    bool drawThumbsUp = false;
    
    if (clean_sym == "!!") bgColor = cv::Scalar(80, 175, 75);      // Green (OpenCV uses BGR)
    else if (clean_sym == "!") bgColor = cv::Scalar(215, 130, 40); // Blue
    else if (clean_sym == "*") { bgColor = cv::Scalar(80, 175, 75); drawStar = true; } // Green Star
    else if (clean_sym == "(Good)") { bgColor = cv::Scalar(80, 175, 75); drawThumbsUp = true; } // Green Thumbs Up
    else if (clean_sym == "?") bgColor = cv::Scalar(50, 200, 240); // Yellow
    else if (clean_sym == "X") bgColor = cv::Scalar(100, 100, 255); // Light red
    else if (clean_sym == "??") bgColor = cv::Scalar(40, 40, 200);  // Darker red
    else if (clean_sym == "(Book)") { bgColor = cv::Scalar(150, 180, 200); drawBook = true; } // Tan Book
    else return; 

    cv::circle(img, cv::Point(ax, ay), radius, bgColor, cv::FILLED, cv::LINE_AA);
    cv::circle(img, cv::Point(ax, ay), radius, cv::Scalar(20, 20, 20), 1, cv::LINE_AA); // Dark outline

    if (drawStar) {
        std::vector<cv::Point> star_pts;
        double out_r = radius * 0.65;
        double in_r = out_r * 0.382; // Magic ratio for standard 5-pointed star
        for (int i = 0; i < 10; ++i) {
            double angle = i * CV_PI / 5.0 - CV_PI / 2.0;
            double r = (i % 2 == 0) ? out_r : in_r;
            star_pts.push_back(cv::Point(static_cast<int>(ax + r * std::cos(angle)), static_cast<int>(ay + r * std::sin(angle))));
        }
        cv::fillPoly(img, std::vector<std::vector<cv::Point>>{star_pts}, cv::Scalar(255, 255, 255), cv::LINE_AA);
    } else if (drawBook) {
        std::vector<cv::Point> left_page = {
            cv::Point(ax - radius*0.5, ay - radius*0.25),
            cv::Point(ax - radius*0.05, ay - radius*0.1),
            cv::Point(ax - radius*0.05, ay + radius*0.4),
            cv::Point(ax - radius*0.5, ay + radius*0.25)
        };
        std::vector<cv::Point> right_page = {
            cv::Point(ax + radius*0.5, ay - radius*0.25),
            cv::Point(ax + radius*0.05, ay - radius*0.1),
            cv::Point(ax + radius*0.05, ay + radius*0.4),
            cv::Point(ax + radius*0.5, ay + radius*0.25)
        };
        cv::fillPoly(img, std::vector<std::vector<cv::Point>>{left_page, right_page}, cv::Scalar(255, 255, 255), cv::LINE_AA);
        cv::line(img, cv::Point(ax, ay - radius*0.1), cv::Point(ax, ay + radius*0.4), cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
    } else if (drawThumbsUp) {
        // Rounded silhouette styled to read closer to the chess.com "Good" badge
        // at very small sizes: chunky palm, upright thumb, and soft finger bumps.
        std::vector<cv::Point> palm = {
            cv::Point(static_cast<int>(ax - radius * 0.34), static_cast<int>(ay + radius * 0.32)),
            cv::Point(static_cast<int>(ax - radius * 0.02), static_cast<int>(ay + radius * 0.32)),
            cv::Point(static_cast<int>(ax + radius * 0.18), static_cast<int>(ay + radius * 0.24)),
            cv::Point(static_cast<int>(ax + radius * 0.30), static_cast<int>(ay + radius * 0.08)),
            cv::Point(static_cast<int>(ax + radius * 0.30), static_cast<int>(ay - radius * 0.18)),
            cv::Point(static_cast<int>(ax + radius * 0.15), static_cast<int>(ay - radius * 0.28)),
            cv::Point(static_cast<int>(ax - radius * 0.08), static_cast<int>(ay - radius * 0.28)),
            cv::Point(static_cast<int>(ax - radius * 0.18), static_cast<int>(ay - radius * 0.10)),
            cv::Point(static_cast<int>(ax - radius * 0.28), static_cast<int>(ay + radius * 0.02)),
            cv::Point(static_cast<int>(ax - radius * 0.34), static_cast<int>(ay + radius * 0.18))
        };
        cv::fillConvexPoly(img, palm, cv::Scalar(255, 255, 255), cv::LINE_AA);

        std::vector<cv::Point> thumb = {
            cv::Point(static_cast<int>(ax - radius * 0.24), static_cast<int>(ay - radius * 0.05)),
            cv::Point(static_cast<int>(ax - radius * 0.12), static_cast<int>(ay - radius * 0.12)),
            cv::Point(static_cast<int>(ax - radius * 0.07), static_cast<int>(ay - radius * 0.56)),
            cv::Point(static_cast<int>(ax - radius * 0.20), static_cast<int>(ay - radius * 0.64)),
            cv::Point(static_cast<int>(ax - radius * 0.32), static_cast<int>(ay - radius * 0.56)),
            cv::Point(static_cast<int>(ax - radius * 0.35), static_cast<int>(ay - radius * 0.18))
        };
        cv::fillConvexPoly(img, thumb, cv::Scalar(255, 255, 255), cv::LINE_AA);

        cv::circle(img, cv::Point(static_cast<int>(ax - radius * 0.18), static_cast<int>(ay - radius * 0.60)),
                   std::max(1, static_cast<int>(radius * 0.11)), cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);

        const int fingerRadius = std::max(1, static_cast<int>(radius * 0.12));
        cv::circle(img, cv::Point(static_cast<int>(ax + radius * 0.23), static_cast<int>(ay - radius * 0.14)),
                   fingerRadius, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(img, cv::Point(static_cast<int>(ax + radius * 0.16), static_cast<int>(ay + radius * 0.03)),
                   fingerRadius, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);
        cv::circle(img, cv::Point(static_cast<int>(ax + radius * 0.07), static_cast<int>(ay + radius * 0.17)),
                   fingerRadius, cv::Scalar(255, 255, 255), cv::FILLED, cv::LINE_AA);

        cv::line(img,
                 cv::Point(static_cast<int>(ax - radius * 0.07), static_cast<int>(ay - radius * 0.02)),
                 cv::Point(static_cast<int>(ax + radius * 0.18), static_cast<int>(ay - radius * 0.06)),
                 bgColor, 1, cv::LINE_AA);
        cv::line(img,
                 cv::Point(static_cast<int>(ax - radius * 0.03), static_cast<int>(ay + radius * 0.12)),
                 cv::Point(static_cast<int>(ax + radius * 0.14), static_cast<int>(ay + radius * 0.10)),
                 bgColor, 1, cv::LINE_AA);
    } else {
        std::string txt = clean_sym;
        int fontFace = cv::FONT_HERSHEY_SIMPLEX;
        double fontScale = (txt.length() > 1) ? radius * 0.045 : radius * 0.055;
        int thickness = (txt.length() > 1) ? 1 : 2;
        int baseline = 0;
        cv::Size textSize = cv::getTextSize(txt, fontFace, fontScale, thickness, &baseline);
        
        cv::Point textOrg(ax - textSize.width / 2, ay + textSize.height / 2 - 1);
        cv::putText(img, txt, textOrg, fontFace, fontScale, cv::Scalar(255, 255, 255), thickness, cv::LINE_AA);
    }
}

void drawAnalysisBar(cv::Mat& img, cv::Rect rect, double cpScore) {
    // Base dark background (advantage to Black)
    cv::rectangle(img, rect, cv::Scalar(40, 40, 40), cv::FILLED);
    
    // Simplified logistic sigmoid matching standard engine win probability scaling
    double winProb = 1.0 / (1.0 + std::exp(-0.00368208 * cpScore));
    
    // Calculate the height of the light/white portion (advantage to White)
    int whiteHeight = static_cast<int>(rect.height * winProb);
    
    // Draw white portion rising from the bottom up
    cv::Rect whiteRect(rect.x, rect.y + rect.height - whiteHeight, rect.width, whiteHeight);
    cv::rectangle(img, whiteRect, cv::Scalar(240, 240, 240), cv::FILLED);
    
    // Draw a prominent indicator line precisely at the current evaluation point
    int indicatorY = rect.y + rect.height - whiteHeight;
    cv::line(img, cv::Point(rect.x, indicatorY), cv::Point(rect.x + rect.width, indicatorY), cv::Scalar(100, 100, 100), 2, cv::LINE_AA);

    // Overlay a border
    cv::rectangle(img, rect, cv::Scalar(100, 100, 100), 2);
}

EngineArrowStyle compute_engine_arrow_style(int line_index, double diff_cp, int arrow_thickness_pct) {
    const double similarity = std::clamp(1.0 - (diff_cp / 200.0), 0.0, 1.0);

    EngineArrowStyle style;
    style.thickness_pct = std::max(4, static_cast<int>(std::round(arrow_thickness_pct * (0.72 + similarity * 0.28))));
    style.tail_color = cv::Scalar(138, 152, 162);
    style.head_color = cv::Scalar(236, 240, 242);

    if (line_index == 0 || diff_cp <= 10.0) {
        style.opacity = 0.46;
        style.thickness_pct = std::max(style.thickness_pct, arrow_thickness_pct + 2);
    } else if (diff_cp <= 25.0) {
        style.opacity = 0.38;
    } else if (diff_cp <= 60.0) {
        style.opacity = 0.32;
    } else if (diff_cp <= 110.0) {
        style.opacity = 0.28;
    } else {
        const double far_ratio = std::clamp((diff_cp - 110.0) / 160.0, 0.0, 1.0);
        style.opacity = 0.25 - far_ratio * 0.05;
    }

    style.opacity = std::clamp(style.opacity, 0.20, 0.46);
    return style;
}

void render_main_board_arrows(cv::Mat& image,
                              const std::optional<StockfishResult>& analysis,
                              const std::string& fen,
                              int width, int height,
                              int arrow_thickness_pct) {
    image = cv::Mat::zeros(cv::Size(width, height), CV_8UC4);
    if (!analysis.has_value() || analysis->lines.empty()) return;

    double sq_w = static_cast<double>(width) / 8.0;
    double sq_h = static_cast<double>(height) / 8.0;

    try {
        libchess::Position pos(fen);
        double best_score = ChessFenUtils::get_line_score_cp(analysis->lines.front());

        // Draw worse lines first so best lines render on top
        for (int i = static_cast<int>(analysis->lines.size()) - 1; i >= 0; --i) {
            const auto& line = analysis->lines[i];
            if (line.move_uci.empty() || line.move_uci == "ANNOTATION") continue;
            
            double line_score = ChessFenUtils::get_line_score_cp(line);
            double diff_cp = std::max(0.0, best_score - line_score);
            
            EngineArrowStyle style = compute_engine_arrow_style(i, diff_cp, arrow_thickness_pct);

            libchess::Move move = pos.parse_move(line.move_uci);
            auto from_sq = static_cast<int>(static_cast<unsigned int>(move.from()));
            auto to_sq = static_cast<int>(static_cast<unsigned int>(move.to()));

            int from_row = 7 - (from_sq / 8);
            int from_col = from_sq % 8;
            int to_row = 7 - (to_sq / 8);
            int to_col = to_sq % 8;

            cv::Point start(static_cast<int>((from_col + 0.5) * sq_w), static_cast<int>((from_row + 0.5) * sq_h));
            cv::Point end(static_cast<int>((to_col + 0.5) * sq_w), static_cast<int>((to_row + 0.5) * sq_h));

            blend_arrow_on_bgra(image, start, end, style, sq_w);
        }
        
        // Draw graphical move quality annotations on top of the main board
        for (const auto& line : analysis->lines) {
            if (line.move_uci == "ANNOTATION") {
                std::string uci, sym;
                size_t uci_len = 0;
                while (uci_len < line.pv_line.length()) {
                    char c = line.pv_line[uci_len];
                    if ((c >= 'a' && c <= 'h') || (c >= '1' && c <= '8') || c == 'q' || c == 'r' || c == 'b' || c == 'n') uci_len++;
                    else break;
                }
                uci = line.pv_line.substr(0, uci_len);
                sym = line.pv_line.substr(uci_len);
                drawMoveAnnotationOnBoard(image, uci, sym, sq_w, sq_h);
            }
        }
    } catch(...) {}
}

} // namespace AnalysisVideoRenderUtils
} // namespace cta
