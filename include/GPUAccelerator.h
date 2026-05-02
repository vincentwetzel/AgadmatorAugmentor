#pragma once

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

namespace cta {

// Forward declaration
struct BoardGeometry;

// ─────────────────────────────────────────────────────────────────────────────
// GPUMat — Device-side memory wrapper (RAII, move-only)
// ─────────────────────────────────────────────────────────────────────────────

/// Device-side memory wrapper for GPU-resident image data.
/// Manages CUDA memory with RAII semantics and provides upload/download helpers.
/// Move-only (no copy) to prevent accidental device memory duplication.
class GPUMat {
public:
    GPUMat() = default;
    ~GPUMat();

    // Move-only
    GPUMat(GPUMat&& other) noexcept;
    GPUMat& operator=(GPUMat&& other) noexcept;
    GPUMat(const GPUMat&) = delete;
    GPUMat& operator=(const GPUMat&) = delete;

    /// Uploads a host cv::Mat to device memory. Resizes device buffer if needed.
    void upload(const cv::Mat& host);

    /// Downloads device memory to a host cv::Mat. Creates/resizes as needed.
    void download(cv::Mat& host) const;

    /// Ensures device buffer is large enough for width×height×elemSize(type).
    /// Preserves existing data if buffer is already sufficient.
    void ensure_capacity(int width, int height, int type);

    /// Releases device memory.
    void release();

    /// Returns true if device memory is allocated.
    bool empty() const { return data_ == nullptr; }

    /// Raw device pointer — use with NPP functions.
    void* ptr() const { return data_; }
    template<typename T> T* ptr_as() const { return static_cast<T*>(data_); }

    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    int type() const { return type_; }
    size_t step() const { return step_; }
    size_t capacity() const { return capacity_; }

private:
    void* data_ = nullptr;
    size_t capacity_ = 0;
    int width_ = 0, height_ = 0, channels_ = 0, type_ = 0;
    size_t step_ = 0; // bytes per row
};

// ─────────────────────────────────────────────────────────────────────────────
// GPUPipeline — Zero-copy GPU pipeline for frame diff + square means
// ─────────────────────────────────────────────────────────────────────────────

/// Zero-copy GPU pipeline: keeps grayscale frames on the GPU to eliminate
/// per-frame Host↔Device memory copies during absdiff + integral operations.
///
/// Traditional per-frame flow:
///   host_gray_a → [H→D] → GPU absdiff → [D→H] → diff_mat → CPU integral → means
///   host_gray_b → [H→D] ↗                          (3 memory copies)
///
/// Zero-copy pipeline flow:
///   host_gray → upload once → swap prev/curr pointers → GPU absdiff →
///   GPU integral → GPU extract 64 means → [D→H] 64 doubles (1 copy)
class GPUPipeline {
public:
    /// Initialize GPU pipeline (must be called after GPUAccelerator::init()).
    void init();

    /// Returns true if GPU pipeline is available.
    bool is_available() const { return available_; }

    /// Upload a host grayscale image to the "current" GPU buffer.
    /// Swaps internal buffers so "prev" becomes the old "current".
    void update_current(const cv::Mat& host_gray);

    /// Compute absolute difference between prev and current on GPU,
    /// then compute square means entirely on GPU. Only 64 doubles
    /// are downloaded back to the host.
    /// @return Vector of 64 mean square diff values (row-major: index = row*8 + col).
    ///         Returns empty vector if GPU pipeline unavailable.
    std::vector<double> compute_square_diff_means(const BoardGeometry& geo,
                                                    int margin_h, int margin_w);

    /// Download the current GPU grayscale buffer to host.
    /// Used for debug screenshots and clock extraction.
    void download_current(cv::Mat& host) const;

    /// Download the previous GPU grayscale buffer to host.
    /// Used for revert detection history matching.
    void download_previous(cv::Mat& host) const;

    /// Download the GPU absdiff result to host.
    /// Used when the CPU diff image is needed for move scoring or debug output.
    void download_diff(cv::Mat& host) const;

private:
    bool available_ = false;
    GPUMat prev_gray_;   // Previous board state (GPU-resident)
    GPUMat curr_gray_;   // Current board state (GPU-resident)
    GPUMat diff_gray_;   // absdiff result (GPU-resident)
    GPUMat integral_;    // Integral image (GPU-resident, 32F)
    GPUMat d_means_;     // Device-side 64-double output buffer
};

// ─────────────────────────────────────────────────────────────────────────────
// GPUAccelerator — Individual operation wrappers (legacy API)
// ─────────────────────────────────────────────────────────────────────────────

/// GPU Acceleration using system CUDA SDK (NPP) directly.
///
/// Does NOT depend on OpenCV being built with CUDA support.
/// Uses NVIDIA NPP (NVIDIA Performance Primitives) from the CUDA toolkit.
/// Falls back to CPU gracefully if CUDA is not available.
///
/// Operations accelerated:
///   - resize       → nppiResize
///   - absdiff      → nppiAbsDiff
///   - cvtColor     → nppiColorTwist / nppiBGRToGray
///   - matchTemplate → nppiCrossCorrNorm
///   - threshold    → nppiThreshold
///
/// Setup (CMakeLists.txt):
///   - Detects CUDA 13.2 at: C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2
///   - Links: cudart.lib + npp*.lib
///   - Defines: HAVE_SYSTEM_CUDA

class GPUAccelerator {
public:
    /// Initialize and detect GPU. Safe to call multiple times.
    /// Must be called before any GPU operation.
    static void init();

    /// Returns true if a CUDA-capable GPU was detected.
    static bool is_available();

    /// Returns device name or "CPU" if unavailable.
    static std::string device_name();

    /// ── GPU-accelerated operations (CPU fallback if GPU unavailable) ──

    /// absdiff(out = |a - b|)
    static void absdiff(const cv::Mat& a, const cv::Mat& b, cv::Mat& out);

    /// cvtColor BGR -> GRAY
    static void cvtColor_BGR2GRAY(const cv::Mat& src, cv::Mat& dst);

    /// matchTemplate (TM_CCOEFF_NORMED)
    static void matchTemplate(const cv::Mat& image, const cv::Mat& templ,
                              cv::Mat& result, int method = cv::TM_CCOEFF_NORMED);

    /// resize
    static void resize(const cv::Mat& src, cv::Mat& dst, cv::Size dsize,
                       double fx = 0, double fy = 0,
                       int interp = cv::INTER_LINEAR);

    /// integral (prefix sum) — not GPU-accelerated, always uses CPU cv::integral
    static void integral(const cv::Mat& src, cv::Mat& sum, int sdepth = CV_64F);

    /// inRange
    static void inRange(const cv::Mat& src, cv::Scalar lower, cv::Scalar upper,
                        cv::Mat& dst);

    /// threshold
    static void threshold(const cv::Mat& src, cv::Mat& dst, double thresh,
                          double maxval, int type);

private:
    static bool available_;
    static bool initialized_;
    static std::string device_name_;
};

} // namespace cta
