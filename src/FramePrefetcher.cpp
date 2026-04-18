// Extracted from cpp directory
#include "FramePrefetcher.h"
#include "GPUAccelerator.h"
#include <iostream>

namespace aa {

FramePrefetcher::FramePrefetcher(const std::string& video_path, int memory_limit_mb)
    : video_path_(video_path), memory_limit_mb_(memory_limit_mb) {}

FramePrefetcher::~FramePrefetcher() {
    stop();
}

void FramePrefetcher::init(const BoardGeometry& geo) {
    geo_ = geo;
    initialized_ = true;

    // Start the worker thread
    worker_ = std::thread(&FramePrefetcher::worker_loop, this);
}

void FramePrefetcher::request_next(double timestamp_seconds, double fps) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_queue_.push({timestamp_seconds, fps});
    }
    cv_.notify_one();
}

FramePrefetcher::FrameData FramePrefetcher::get_result() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !result_queue_.empty() || stop_requested_; });

    if (stop_requested_ && result_queue_.empty()) {
        return FrameData{};
    }

    FrameData data = std::move(result_queue_.front());
    result_queue_.pop();
    cv_.notify_one(); // Notify worker that space has freed up
    return data;
}

void FramePrefetcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) return;
        stop_requested_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void FramePrefetcher::clear_queues() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Efficiently clear queues and deallocate memory
    std::queue<Request>().swap(request_queue_);
    std::queue<FrameData>().swap(result_queue_);
    
    // Wake up the worker if it was blocked by max_queue_size_
    cv_.notify_all();
}

void FramePrefetcher::worker_loop() {
    cv::VideoCapture cap(video_path_, cv::CAP_ANY, {
        cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
    });
    if (!cap.isOpened()) {
        std::cerr << "FramePrefetcher: Cannot open video: " << video_path_ << "\n";
        // Signal failure to main thread so it doesn't block forever
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result_queue_.push(FrameData{});
        }
        cv_.notify_one();
        return;
    }

    if (memory_limit_mb_ > 0) {
        double width = cap.get(cv::CAP_PROP_FRAME_WIDTH);
        double height = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
        
        // Dynamic calculation of queue cap based on resolution
        size_t frame_bytes = static_cast<size_t>(width * height * 3); // BGR raw frame
        size_t board_bytes = geo_.bw * geo_.bh * 4;                   // Board BGR + Grayscale
        
        size_t total_mb_per_frame = (frame_bytes + board_bytes) / (1024 * 1024);
        if (total_mb_per_frame == 0) total_mb_per_frame = 1;
        
        // Apply the cap, ensuring we at least have a minimum queue of 2 for double-buffering
        max_queue_size_ = std::max<size_t>(2, memory_limit_mb_ / total_mb_per_frame);
        
        std::cout << "FramePrefetcher: Memory limit " << memory_limit_mb_ << "MB applied. Queue capped at " << max_queue_size_ << " frames.\n";
    }

    double total_frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    int current_frame = 0;

    while (true) {
        // Wait for a request
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { 
            return (!request_queue_.empty() && result_queue_.size() < max_queue_size_) || stop_requested_; 
        });

        if (stop_requested_) return;

        auto req = request_queue_.front();
        request_queue_.pop();
        double target_ts = req.timestamp;
        double fps = req.fps;

        // std::round prevents float precision errors from mistakenly triggering backward cap.set() jumps
        int target_frame = static_cast<int>(std::round(target_ts * fps));
        if (target_frame < 0) target_frame = 0;
        if (target_frame >= static_cast<int>(total_frames)) target_frame = static_cast<int>(total_frames) - 1;

        lock.unlock();

        // Seek and read frame
        cv::Mat frame;
        // cap.set() is extremely slow due to keyframe seeking. For FAST mode polling 
        // (e.g. 5.0s jumps), sequential cap.grab() is dramatically faster.
        if (target_frame > current_frame && target_frame - current_frame <= fps * 6) {
            for (int i = 0; i < target_frame - current_frame; ++i) {
                if (!cap.grab()) {
                    break; // Prevent dead-looping if EOF is reached unexpectedly
                }
            }
            cap >> frame;
            current_frame = target_frame + 1;
        } else if (target_frame == current_frame) {
            cap >> frame;
            ++current_frame;
        } else {
            cap.set(cv::CAP_PROP_POS_FRAMES, target_frame);
            cap >> frame;
            current_frame = target_frame + 1;
        }

        // Process frame: crop board region + convert to grayscale
        FrameData data;
        if (!frame.empty()) {
            data.bgr = frame;
            data.timestamp = target_ts;
            data.frame_index = target_frame;
            data.valid = true;

            // Crop board region
            data.board_bgr = frame(cv::Rect(geo_.bx, geo_.by, geo_.bw, geo_.bh));
            // Convert to grayscale
            cv::cvtColor(data.board_bgr, data.board_gray, cv::COLOR_BGR2GRAY); // CPU is faster than H->D + D->H PCIe latency
        }

        // Return result
        {
            std::lock_guard<std::mutex> lock2(mutex_);
            result_queue_.push(std::move(data));
        }
        cv_.notify_one();
    }
}

} // namespace aa
