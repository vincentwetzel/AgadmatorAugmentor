#pragma once

#include <string>
#include <vector>
#include <sstream>

namespace aa {

class FFmpegFilterGraph {
public:
    FFmpegFilterGraph();

    // Adds a filter to the graph.
    // Example: add_filter("[0:v]", "scale=1280:-1", "[scaled]");
    void add_filter(const std::string& input_streams, const std::string& filter_spec, const std::string& output_stream = "");

    // Builds the final filter_complex string.
    std::string build() const;

private:
    std::vector<std::string> filters_;
};

} // namespace aa