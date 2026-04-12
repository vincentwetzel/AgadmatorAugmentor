#include "FramePrefetcher.h"
#include "GPUAccelerator.h"
#include <iostream>

namespace aa {

FramePrefetcher::FramePrefetcher(const std::string& video_path)
    : video_path_(video_path) {}

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
        request_timestamp_ = timestamp_seconds;
        request_fps_ = fps;
        request_pending_ = true;
        result_ready_ = false;
    }
    cv_.notify_one();
}

FramePrefetcher::FrameData FramePrefetcher::get_result() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return result_ready_ || stop_requested_; });

    return result_;
}

void FramePrefetcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_requested_) return;
        stop_requested_ = true;
        request_pending_ = false;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void FramePrefetcher::worker_loop() {
    cv::VideoCapture cap(video_path_, cv::CAP_ANY, {
        cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
    });
    if (!cap.isOpened()) {
        std::cerr << "FramePrefetcher: Cannot open video: " << video_path_ << "\n";
        return;
    }

    double total_frames = cap.get(cv::CAP_PROP_FRAME_COUNT);
    int current_frame = 0;

    while (true) {
        // Wait for a request
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return request_pending_ || stop_requested_; });

        if (stop_requested_) return;

        double target_ts = request_timestamp_;
        double fps = request_fps_;
        request_pending_ = false;

        int target_frame = static_cast<int>(target_ts * fps);
        if (target_frame < 0) target_frame = 0;
        if (target_frame >= static_cast<int>(total_frames)) target_frame = static_cast<int>(total_frames) - 1;

        lock.unlock();

        // Seek and read frame
        cv::Mat frame;
        // cap.set() is extremely slow due to keyframe seeking. For FAST mode polling 
        // (e.g. 5.0s jumps), sequential cap.grab() is dramatically faster.
        if (target_frame > current_frame && target_frame - current_frame <= fps * 15) {
            for (int i = 0; i < target_frame - current_frame; ++i) {
                cap.grab();
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
            GPUAccelerator::cvtColor_BGR2GRAY(data.board_bgr, data.board_gray);
        }

        // Return result
        {
            std::lock_guard<std::mutex> lock2(mutex_);
            result_ = std::move(data);
            result_ready_ = true;
        }
        cv_.notify_one();
    }
}

} // namespace aa
