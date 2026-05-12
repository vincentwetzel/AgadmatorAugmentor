// Extracted from cpp directory
#include "GPUAccelerator.h"
#include "BoardLocalizer.h"

#ifdef HAVE_SYSTEM_CUDA
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <cuda_runtime.h>
#include <nppi.h>
#include <nppdefs.h>
#include <nppi_geometry_transforms.h> // For nppiResizeSqrPixel
#include <nppi_filtering_functions.h> // For nppiCrossCorrNorm
#include <nppi_statistics_functions.h> // Also for nppiCrossCorrNorm if categorized here
#include <nppi_arithmetic_and_logical_operations.h>
#include <windows.h>
#include <delayimp.h>

#undef min
#undef max

// Force CPU fallback for NPP symbols that may be missing in local CUDA toolkit
#define nppiCrossCorrValid_NormLevelGetBufferSize_8u32f_C1R(...) ((NppStatus)-1)
#endif

#include <iostream>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>

namespace cta {

// ─────────────────────────────────────────────────────────────────────────────
// GPUMat Implementation
// ─────────────────────────────────────────────────────────────────────────────

GPUMat::~GPUMat() {
    release();
}

GPUMat::GPUMat(GPUMat&& other) noexcept
    : data_(other.data_), capacity_(other.capacity_),
      width_(other.width_), height_(other.height_),
      channels_(other.channels_), type_(other.type_), step_(other.step_)
{
    other.data_ = nullptr;
    other.capacity_ = 0;
    other.width_ = other.height_ = other.channels_ = other.type_ = 0;
    other.step_ = 0;
}

GPUMat& GPUMat::operator=(GPUMat&& other) noexcept {
    if (this != &other) {
        release();
        data_ = other.data_;
        capacity_ = other.capacity_;
        width_ = other.width_; height_ = other.height_;
        channels_ = other.channels_; type_ = other.type_;
        step_ = other.step_;
        other.data_ = nullptr;
        other.capacity_ = 0;
        other.width_ = other.height_ = other.channels_ = other.type_ = 0;
        other.step_ = 0;
    }
    return *this;
}

void GPUMat::release() {
#ifdef HAVE_SYSTEM_CUDA
    if (data_) {
        cudaFree(data_);
    }
#else
    if (data_) {
        free(data_);
    }
#endif
    data_ = nullptr;
    capacity_ = 0;
    width_ = height_ = channels_ = type_ = 0;
    step_ = 0;
}

void GPUMat::ensure_capacity(int width, int height, int type) {
    int ch = CV_MAT_CN(type);
    size_t elem_size = CV_ELEM_SIZE(type);
    size_t req_step = width * elem_size;
    size_t aligned_step = (req_step + 15) & ~15;
    size_t req_size = aligned_step * height;

    // Re-check capacity: must be strictly greater or equal for requested size
    if (capacity_ >= req_size) {
        width_ = width; height_ = height;
        channels_ = ch; type_ = type; step_ = aligned_step;
        return;
    }

#ifdef HAVE_SYSTEM_CUDA
    if (data_) cudaFree(data_);
    if (cudaMalloc(&data_, req_size) != cudaSuccess) {
        data_ = nullptr;
    }
#else
    if (data_) free(data_);
    data_ = malloc(req_size);
#endif
    capacity_ = req_size;
    width_ = width; height_ = height;
    channels_ = ch; type_ = type; step_ = aligned_step;
}

void GPUMat::upload(const cv::Mat& host) {
    CV_Assert(host.isContinuous());
    int type = host.type();
    ensure_capacity(host.cols, host.rows, type);

    size_t host_step = host.step;
    size_t copy_size = host.cols * CV_ELEM_SIZE(type);

#ifdef HAVE_SYSTEM_CUDA
    if (host_step == copy_size) {
        cudaMemcpy2D(data_, step_, host.data, host_step,
                     copy_size, host.rows, cudaMemcpyHostToDevice);
    } else {
        for (int r = 0; r < host.rows; ++r) {
            cudaMemcpy(static_cast<char*>(data_) + r * step_,
                       host.ptr<char>(r), copy_size, cudaMemcpyHostToDevice);
        }
    }
#else
    if (host_step == copy_size) {
        std::memcpy(data_, host.data, host_step * host.rows);
    }
#endif
}

void GPUMat::download(cv::Mat& host) const {
    if (!data_ || width_ == 0 || height_ == 0) return;

    host.create(height_, width_, type_);
    size_t host_step = host.step;
    size_t copy_size = width_ * CV_ELEM_SIZE(type_);

#ifdef HAVE_SYSTEM_CUDA
    if (host_step == copy_size && step_ == copy_size) {
        cudaMemcpy(host.data, data_, copy_size * height_, cudaMemcpyDeviceToHost);
    } else {
        cudaMemcpy2D(host.data, host_step, data_, step_,
                     copy_size, height_, cudaMemcpyDeviceToHost);
    }
#else
    if (host_step == copy_size && step_ == copy_size) {
        std::memcpy(host.data, data_, copy_size * height_);
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// GPUAccelerator Static Members
// ─────────────────────────────────────────────────────────────────────────────

bool GPUAccelerator::available_ = false;
bool GPUAccelerator::initialized_ = false;
std::string GPUAccelerator::device_name_ = "CPU";
static std::mutex init_mutex;

#ifdef HAVE_SYSTEM_CUDA
static NppStreamContext make_stream_ctx() {
    static NppStreamContext ctx = {};
    static bool init = false;
    if (!init) {
        ctx.hStream = 0;
        cudaGetDevice(&ctx.nCudaDeviceId);
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, ctx.nCudaDeviceId);
        ctx.nMultiProcessorCount = prop.multiProcessorCount;
        ctx.nMaxThreadsPerMultiProcessor = prop.maxThreadsPerMultiProcessor;
        ctx.nMaxThreadsPerBlock = prop.maxThreadsPerBlock;
        ctx.nSharedMemPerBlock = prop.sharedMemPerBlock;
        ctx.nCudaDevAttrComputeCapabilityMajor = prop.major;
        ctx.nCudaDevAttrComputeCapabilityMinor = prop.minor;
        ctx.nStreamFlags = 0;
        ctx.nReserved0 = 0;
        init = true;
    }
    return ctx;
}

struct CudaBuffer {
    void* ptr = nullptr;
    size_t size = 0;
    ~CudaBuffer() { if (ptr) cudaFree(ptr); }
    void* get(size_t req_size) {
        if (size < req_size) {
            if (ptr) cudaFree(ptr);
            size_t new_size = size == 0 ? req_size : size;
            while (new_size < req_size) new_size *= 2;
            cudaMalloc(&ptr, new_size);
            size = new_size;
        }
        return ptr;
    }
};

struct TlsScratch {
    CudaBuffer a, b, c;
};
thread_local TlsScratch tls;

__declspec(noinline) static int try_detect_gpu_impl(char* out_name, int* out_major, int* out_minor) {
    int device_count = 0;
    cudaError_t err = cudaGetDeviceCount(&device_count);
    if (err == cudaSuccess && device_count > 0) {
        cudaDeviceProp prop;
        cudaGetDeviceProperties(&prop, 0);
        *out_major = prop.major;
        *out_minor = prop.minor;
        strncpy(out_name, prop.name, 255);
        out_name[255] = '\0';
        return 1;
    }
    return 0;
}

static int try_detect_gpu(char* out_name, int* out_major, int* out_minor) {
    int result = -1;
    __try {
        result = try_detect_gpu_impl(out_name, out_major, out_minor);
    }
    __except (GetExceptionCode() == STATUS_DLL_NOT_FOUND ? EXCEPTION_EXECUTE_HANDLER :
                                                           EXCEPTION_CONTINUE_SEARCH) {
        result = -1;
    }
    return result;
}
#endif

void GPUAccelerator::init() {
    if (initialized_) return;
    {
        std::lock_guard<std::mutex> lock(init_mutex);
        if (initialized_) return;
        initialized_ = true;

#ifdef HAVE_SYSTEM_CUDA
        static const wchar_t* npp_dlls[] = {
            L"nppial64_13.dll", L"nppicc64_13.dll",
            L"nppig64_13.dll", L"nppist64_13.dll"
        };
        for (const wchar_t* dll : npp_dlls) {
            wchar_t path_buf[MAX_PATH];
            if (!SearchPathW(nullptr, dll, nullptr, MAX_PATH, path_buf, nullptr)) {
                wchar_t cuda_path[MAX_PATH] = {0};
                if (GetEnvironmentVariableW(L"CUDA_PATH", cuda_path, MAX_PATH) > 0) {
                    wchar_t full_path[MAX_PATH];
                    swprintf(full_path, MAX_PATH, L"%ls\\bin\\%ls", cuda_path, dll);
                    if (GetFileAttributesW(full_path) == INVALID_FILE_ATTRIBUTES) {
                        std::wcout << L"  [GPU] NPP runtime DLL not found (" << dll << L"), using CPU\n";
                        available_ = false;
                        return;
                    }
                } else {
                    std::wcout << L"  [GPU] CUDA_PATH not set and NPP DLL not found (" << dll << L"), using CPU\n";
                    available_ = false;
                    return;
                }
            }
        }
        char gpu_name[256] = {};
        int major = 0, minor = 0;
        int detect_result = try_detect_gpu(gpu_name, &major, &minor);
        if (detect_result > 0) {
            device_name_ = gpu_name;
            available_ = true;
            std::cout << "  [GPU] CUDA GPU detected via NPP: " << device_name_ << "\n";
            std::cout << "  [GPU] Compute capability: " << major << "." << minor << "\n";

            // Pre-load libraries to avoid race conditions and debug-heap assertions
            // during concurrent MSVC delay-loading from multiple worker threads.
            LoadLibraryW(L"nppial64_13.dll");
            LoadLibraryW(L"nppicc64_13.dll");
            LoadLibraryW(L"nppig64_13.dll");
            LoadLibraryW(L"nppist64_13.dll");
        } else {
            std::cout << "  [GPU] No CUDA devices found, using CPU\n";
            available_ = false;
        }
#else
        std::cout << "  [GPU] CUDA SDK not linked, using CPU\n";
        available_ = false;
#endif
    }
}

bool GPUAccelerator::is_available() {
    if (!initialized_) init();
    return available_;
}

std::string GPUAccelerator::device_name() {
    if (!initialized_) init();
    return device_name_;
}

// ── GPU Operations ───────────────────────────────────────────────────────────

static void fallback_resize_cpu(const GPUMat& src, GPUMat& dst, cv::Size dsize, int interp) {
    cv::Mat h_src, h_dst;
    src.download(h_src);
    cv::resize(h_src, h_dst, dsize, 0, 0, interp);
    dst.upload(h_dst);
}

void GPUAccelerator::resize(const cv::Mat& src, cv::Mat& dst, cv::Size dsize,
                             double fx, double fy, int interp) {
    // Keep host-image resize on OpenCV. The direct NPP resize path delay-loads
    // nppig64_13.dll during board localization and has triggered debug-heap
    // assertions in the GUI process.
    dst.create(dsize, src.type());
    cv::resize(src, dst, dsize, fx, fy, interp);
}

void GPUAccelerator::resize_gpu(const GPUMat& src, GPUMat& dst, cv::Size dsize,
                                 int interp) {
    if (!initialized_) init();
#ifdef HAVE_SYSTEM_CUDA
    // NPPI Resize currently only supports 8-bit, single-channel (grayscale) inputs
    if (available_ && src.type() == CV_8UC1) { 
        dst.ensure_capacity(dsize.width, dsize.height, src.type());
        NppiSize src_size = { src.width(), src.height() }; 
        NppiRect src_rect = { 0, 0, src.width(), src.height() };
        NppiRect dst_rect = { 0, 0, dsize.width, dsize.height };
        NppStreamContext ctx = make_stream_ctx();
        NppiInterpolationMode mode = NPPI_INTER_LINEAR;
        if (interp == cv::INTER_AREA || interp == cv::INTER_LANCZOS4)
            mode = NPPI_INTER_LANCZOS;
        else if (interp == cv::INTER_CUBIC)
            mode = NPPI_INTER_CUBIC;
        NppStatus st = nppiResizeSqrPixel_8u_C1R_Ctx(static_cast<const Npp8u*>(src.ptr()), src_size, static_cast<int>(src.step()), src_rect, // Line 332
                                                     static_cast<Npp8u*>(dst.ptr()), static_cast<int>(dst.step()), dst_rect, // Line 333
                                                     static_cast<double>(dsize.width) / src.width(), // Line 334
                                                     static_cast<double>(dsize.height) / src.height(), // Line 335
                                                     0, 0, mode, ctx);
        if (st == NPP_SUCCESS) {
            cudaDeviceSynchronize(); // Synchronize to ensure operation completes before returning
            return;
        }
        // If NPP fails (e.g. invalid arguments or context), drop down to CPU
    }
#endif
    // CPU Fallback: Seamlessly download from GPU, resize using OpenCV on CPU, and upload back
    fallback_resize_cpu(src, dst, dsize, interp);
}

// ─────────────────────────────────────────────────────────────────────────────
// GPUPipeline Implementation
// ─────────────────────────────────────────────────────────────────────────────

struct ThreadLocalPipeline {
    GPUMat prev_gray;
    GPUMat curr_gray;
    GPUMat diff;
    int width = 0;
    int height = 0;
    bool initialized = false;
};
static thread_local ThreadLocalPipeline tl_pipe;

void GPUPipeline::init() {
    if (GPUAccelerator::is_available()) {
        tl_pipe.initialized = true;
        available_ = true;
    } else {
        available_ = false;
    }
}

void GPUPipeline::update_current(const cv::Mat& host_gray) {
    if (!available_ || !tl_pipe.initialized) return;
    
    tl_pipe.width = host_gray.cols;
    tl_pipe.height = host_gray.rows;

    // Swap avoids device reallocation and safely shifts current to previous
    GPUMat temp = std::move(tl_pipe.prev_gray);
    tl_pipe.prev_gray = std::move(tl_pipe.curr_gray);
    tl_pipe.curr_gray = std::move(temp);
    
    tl_pipe.curr_gray.upload(host_gray);
}

std::vector<double> GPUPipeline::compute_square_diff_means(const BoardGeometry& geo, int margin_h, int margin_w) {
    (void)geo;
    (void)margin_h;
    (void)margin_w;
    // The experimental NPP square-diff path depends on CUDA/NPP symbols that
    // vary across toolkit versions. Return empty so callers use the strict CPU
    // scoring path.
    return {};
}

void GPUPipeline::download_current(cv::Mat& host) const {
    if (tl_pipe.initialized) tl_pipe.curr_gray.download(host);
}

void GPUPipeline::download_previous(cv::Mat& host) const {
    if (tl_pipe.initialized) tl_pipe.prev_gray.download(host);
}

void GPUPipeline::download_diff(cv::Mat& host) const {
    if (tl_pipe.initialized) tl_pipe.diff.download(host);
}


void GPUAccelerator::absdiff(const cv::Mat& a, const cv::Mat& b, cv::Mat& out) {
#ifdef HAVE_SYSTEM_CUDA
    if (!initialized_) init();
    if (available_ && a.type() == CV_8UC1 && b.type() == CV_8UC1 &&
        a.size() == b.size() && !a.empty() && !b.empty()) {
        GPUMat d_a;
        GPUMat d_b;
        GPUMat d_out;
        d_a.upload(a.isContinuous() ? a : a.clone());
        d_b.upload(b.isContinuous() ? b : b.clone());
        d_out.ensure_capacity(a.cols, a.rows, a.type());

        NppiSize roi = {a.cols, a.rows};
        NppStreamContext ctx = make_stream_ctx();
        NppStatus st = nppiAbsDiff_8u_C1R_Ctx(
            static_cast<const Npp8u*>(d_a.ptr()), static_cast<int>(d_a.step()),
            static_cast<const Npp8u*>(d_b.ptr()), static_cast<int>(d_b.step()),
            static_cast<Npp8u*>(d_out.ptr()), static_cast<int>(d_out.step()),
            roi, ctx);

        if (st == NPP_SUCCESS && cudaDeviceSynchronize() == cudaSuccess) {
            d_out.download(out);
            return;
        }
    }
#endif
    cv::absdiff(a, b, out);
}

void GPUAccelerator::matchTemplate(const cv::Mat& image, const cv::Mat& templ, cv::Mat& result, int method) {
    if (image.cols >= templ.cols && image.rows >= templ.rows) {
        result.create(image.rows - templ.rows + 1, image.cols - templ.cols + 1, CV_32F);
    }
    cv::matchTemplate(image, templ, result, method);
}

void GPUAccelerator::cvtColor_BGR2GRAY(const cv::Mat& src, cv::Mat& dst) {
    cv::cvtColor(src, dst, cv::COLOR_BGR2GRAY);
}

void GPUAccelerator::integral(const cv::Mat& src, cv::Mat& sum, int sdepth) {
    cv::integral(src, sum, sdepth);
}

void GPUAccelerator::inRange(const cv::Mat& src, cv::Scalar lower, cv::Scalar upper, cv::Mat& dst) {
    cv::inRange(src, lower, upper, dst);
}

void GPUAccelerator::threshold(const cv::Mat& src, cv::Mat& dst, double thresh, double maxval, int type) {
    cv::threshold(src, dst, thresh, maxval, type);
}
} // namespace cta
