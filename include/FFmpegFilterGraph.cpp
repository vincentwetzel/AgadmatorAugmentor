#include "FFmpegFilterGraph.h"

namespace aa {

FFmpegFilterGraph::FFmpegFilterGraph() {}

void FFmpegFilterGraph::add_filter(const std::string& input_streams, const std::string& filter_spec, const std::string& output_stream) {
    filters_.push_back(input_streams + filter_spec + output_stream);
}

std::string FFmpegFilterGraph::build() const {
    if (filters_.empty()) {
        return "";
    }

    std::stringstream ss;
    for (size_t i = 0; i < filters_.size(); ++i) {
        ss << filters_[i];
        if (i < filters_.size() - 1) {
            ss << "; "; // Use semicolon and space for readability
        }
    }
    return ss.str();
}

} // namespace aa