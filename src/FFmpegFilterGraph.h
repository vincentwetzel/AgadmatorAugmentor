#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>

// Represents different types of hardware acceleration FFmpeg might use.
namespace cta {

enum class HardwareAccelerationType {
    None,
    NVDEC_CUDA, // NVIDIA CUDA/NVDEC
    AMF,        // AMD AMF
    QSV         // Intel Quick Sync Video
};

class FFmpegFilterGraph {
public:
    // Constructor initializes the graph, optionally specifying hardware acceleration type.
    FFmpegFilterGraph(HardwareAccelerationType hw_type = HardwareAccelerationType::None);

    // Adds a filter to the graph.
    // inputs: Comma-separated stream labels (e.g., "[0:v][1:v]" or "[my_stream][another_stream]").
    // filter_desc: The FFmpeg filter string (e.g., "scale=1920:1080").
    // output_label: The label for the output of this filter (e.g., "[scaled_video]").
    //               If empty, a unique one will be generated.
    // is_hw_filter: Indicates if this filter is hardware-accelerated and should run on the GPU.
    //               If true and HW acceleration is active, the graph will attempt to keep data on GPU
    //               and potentially translate generic filters to HW-specific ones.
    // Returns the actual output label used for this filter.
    std::string add_filter(const std::vector<std::string>& input_labels_vec, const std::string& filter_desc, const std::string& output_label = "", bool is_hw_filter = false);

    // Builds the complete FFmpeg filter_complex string from all added filters.
    std::string build() const;

private:
    struct FilterNode {
        std::string inputs;
        std::string filter_desc;
        std::string output_label;
    };

    std::vector<FilterNode> filter_nodes_;
    int next_filter_id_; // Used to generate unique anonymous output labels.

    HardwareAccelerationType hw_type_; // Type of hardware acceleration enabled.
    std::map<std::string, bool> stream_hw_states_; // Tracks if a stream (by label) is currently on GPU (true) or CPU (false).

    void set_stream_hw_state(const std::string& stream_label, bool is_hw);
    bool get_stream_hw_state(const std::string& stream_label) const;
};

} // namespace cta