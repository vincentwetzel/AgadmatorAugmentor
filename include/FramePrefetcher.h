#pragma once

#include <opencv2/core/mat.hpp>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <string>
#include <queue>
#include "BoardLocalizer.h"

namespace aa {

/// Pre-decodes video frames in a background thread, pre-computing grayscale
/// and board crop so the main extraction loop never blocks on FFmpeg I/O.
///
/// Async pattern:
///   prefetcher.request_next(t, fps);   // non-blocking: starts background decode
///   FrameData fd = prefetcher.get_result();  // blocks until ready
///   // ... process fd ...
///   prefetcher.request_next(next_t, fps);  // kick off next while processing current
class FramePrefetcher {
public:
    struct FrameData {
        cv::Mat bgr;         // Full BGR frame
        cv::Mat board_bgr;   // Board region (BGR)
        cv::Mat board_gray; // Board region (grayscale)
        double timestamp = 0.0;
        int frame_index = 0;
        bool valid = false;
    };

    explicit FramePrefetcher(const std::string& video_path);
    ~FramePrefetcher();

    /// Initialize with board geometry. Must be called after locate_board().
    void init(const BoardGeometry& geo);

    /// Non-blocking: queues a request to decode the frame at `timestamp`.
    void request_next(double timestamp_seconds, double fps);

    /// Blocking: wait for and return the most recent decoded frame.
    /// Blocks only if the worker hasn't finished yet.
    FrameData get_result();

    /// Signal the background thread to stop and join it.
    void stop();

    /// Clears all pending requests and unread results from the queues.
    void clear_queues();

    /// Returns true if the prefetcher has been initialized.
    bool is_initialized() const { return initialized_; }

private:
    void worker_loop();

    std::string video_path_;
    BoardGeometry geo_;
    bool initialized_ = false;

    // Worker thread
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;

    // Requests from main thread
    struct Request {
        double timestamp;
        double fps;
    };
    std::queue<Request> request_queue_;
    const size_t max_queue_size_ = 3;

    // Results from worker thread
    std::queue<FrameData> result_queue_;

    // Control
    bool stop_requested_ = false;
};

} // namespace aa
