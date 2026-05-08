#include "FFmpegFilterGraph.h"
#include <sstream>
#include <stdexcept>
#include <algorithm> // For std::find_if, std::transform

namespace cta {

    FFmpegFilterGraph::FFmpegFilterGraph(HardwareAccelerationType hw_type) : next_filter_id_(0), hw_type_(hw_type) {
        // Initialize main video input as CPU by default.
        // It will be explicitly uploaded to GPU later if hw_type_ is not None and GPU processing is desired.
        stream_hw_states_["[0:v]"] = false;
    }

    std::string FFmpegFilterGraph::add_filter(const std::string& inputs, const std::string& filter_desc_in, const std::string& output_label_in, bool is_hw_filter) {
        std::string final_output_label = output_label_in;
        if (final_output_label.empty()) {
            final_output_label = "[f" + std::to_string(next_filter_id_++) + "]";
        }

        std::string current_inputs = inputs;
        std::string filter_desc_processed = filter_desc_in;

        // --- Parse Input Labels ---
        // Split the inputs string (e.g., "[0:v][1:v]") into individual labels.
        std::vector<std::string> input_labels_vec;
        size_t start = 0;
        size_t end = current_inputs.find("][");
        while (end != std::string::npos) {
            input_labels_vec.push_back(current_inputs.substr(start, end - start + 1));
            start = end + 1;
            end = current_inputs.find("][", start);
        }
        if (start < current_inputs.length()) {
            input_labels_vec.push_back(current_inputs.substr(start)); // Add the last label
        }

        // --- Determine Current Hardware State of Inputs ---
        bool all_inputs_on_gpu = true;
        bool any_input_on_gpu = false;

        for (const std::string& label : input_labels_vec) {
            if (label.empty()) continue;
            bool is_hw = get_stream_hw_state(label);
            if (is_hw) {
                any_input_on_gpu = true;
            } else {
                all_inputs_on_gpu = false;
            }
        }

        // Default assumption for output state, will be updated based on logic below.
        bool output_on_gpu = any_input_on_gpu;

        // --- Hardware Acceleration Logic ---
        if (hw_type_ != HardwareAccelerationType::None) {
            if (is_hw_filter) {
                // This filter is intended to run on the GPU. Ensure all inputs are on GPU.
                if (!all_inputs_on_gpu) {
                    for (const std::string& label : input_labels_vec) {
                        if (label.empty()) continue;
                        if (!get_stream_hw_state(label)) {
                            // Insert hwupload for CPU inputs
                            FilterNode upload_node;
                            upload_node.inputs = label;
                            upload_node.filter_desc = "hwupload";
                            if (!label.empty() && label.back() == ']') {
                                upload_node.output_label = label.substr(0, label.length() - 1) + "_hw]";
                            } else {
                                upload_node.output_label = label + "_hw";
                            }
                            filter_nodes_.push_back(upload_node);
                            
                            // Replace the original CPU input label with the new GPU output label in current_inputs
                            size_t pos = current_inputs.find(label);
                            if (pos != std::string::npos) {
                                current_inputs.replace(pos, label.length(), upload_node.output_label);
                            }
                            set_stream_hw_state(upload_node.output_label, true);
                        }
                    }
                    output_on_gpu = true; // Output will definitely be on GPU
                }

                // Translate generic filters to hardware equivalents
                if (hw_type_ == HardwareAccelerationType::NVDEC_CUDA) {
                    if (filter_desc_processed.find("overlay") == 0 && filter_desc_processed.find("overlay_cuda") == std::string::npos) {
                        filter_desc_processed.replace(0, 7, "overlay_cuda");
                        
                        // overlay_cuda does not support the 'format' option, so strip it out if present
                        size_t format_pos = filter_desc_processed.find(":format=");
                        if (format_pos != std::string::npos) {
                            size_t next_colon = filter_desc_processed.find(':', format_pos + 1);
                            if (next_colon != std::string::npos) {
                                filter_desc_processed.erase(format_pos, next_colon - format_pos);
                            } else {
                                filter_desc_processed.erase(format_pos);
                            }
                        }
                    } else if (filter_desc_processed.find("scale") == 0 && filter_desc_processed.find("scale_cuda") == std::string::npos) {
                        filter_desc_processed.replace(0, 5, "scale_cuda");
                    }
                }
                
                // Add format=yuv420p to ensure compatible output format if needed, though CUDA overlay might handle it.
                if (filter_desc_processed.find("format=") == std::string::npos && hw_type_ != HardwareAccelerationType::NVDEC_CUDA) {
                    filter_desc_processed += ",format=yuv420p";
                }

            } else {
                // This filter is intended to run on the CPU. Ensure all inputs are on CPU.
                if (any_input_on_gpu) {
                    for (const std::string& label : input_labels_vec) {
                        if (label.empty()) continue;
                        if (get_stream_hw_state(label)) {
                            // Insert hwdownload for GPU inputs
                            FilterNode download_node;
                            download_node.inputs = label;
                            download_node.filter_desc = "hwdownload";
                            std::string dl_out;
                            if (!label.empty() && label.back() == ']') {
                                dl_out = label.substr(0, label.length() - 1) + "_dl]";
                            } else {
                                dl_out = label + "_dl";
                            }
                            download_node.output_label = dl_out;
                            filter_nodes_.push_back(download_node);
                            
                            FilterNode format_node;
                            format_node.inputs = dl_out;
                            format_node.filter_desc = "format=nv12"; // Format often needed after download
                            if (!label.empty() && label.back() == ']') {
                                format_node.output_label = label.substr(0, label.length() - 1) + "_cpu]";
                            } else {
                                format_node.output_label = label + "_cpu";
                            }
                            filter_nodes_.push_back(format_node);
                            
                            size_t pos = current_inputs.find(label);
                            if (pos != std::string::npos) {
                                current_inputs.replace(pos, label.length(), format_node.output_label);
                            }
                            set_stream_hw_state(format_node.output_label, false);
                        }
                    }
                    output_on_gpu = false; // Output will definitely be on CPU
                }
            }
        }

        // --- Add the actual filter node ---
        FilterNode node;
        node.inputs = current_inputs;
        node.filter_desc = filter_desc_processed;
        node.output_label = final_output_label;
        filter_nodes_.push_back(node);

        set_stream_hw_state(final_output_label, output_on_gpu);
        return final_output_label;
    }

    std::string FFmpegFilterGraph::build() const {
        std::ostringstream oss;
        // Ensure correct syntax for filter_complex: "[in1][in2]filter_name=options[out1];"
        for (const auto& node : filter_nodes_) {
            oss << node.inputs << node.filter_desc << node.output_label;
            if (!node.output_label.empty() || !node.inputs.empty() || !node.filter_desc.empty()) {
                oss << ";";
            }
        }
        std::string result = oss.str();
        if (!result.empty()) {
            result.pop_back(); // Remove trailing semicolon
        }
        return result;
    }

    void FFmpegFilterGraph::set_stream_hw_state(const std::string& stream_label, bool is_hw) {
        stream_hw_states_[stream_label] = is_hw;
    }

    bool FFmpegFilterGraph::get_stream_hw_state(const std::string& stream_label) const {
        auto it = stream_hw_states_.find(stream_label);
        if (it != stream_hw_states_.end()) {
            return it->second;
        }
        return false; // Assume CPU if state is unknown
    }

} // namespace cta