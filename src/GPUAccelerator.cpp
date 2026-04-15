// Extracted from cpp directory
#include "GPUAccelerator.h"
#include "BoardLocalizer.h"

#ifdef HAVE_SYSTEM_CUDA
#include <cuda_runtime.h>
#include <nppi.h>
#include <nppdefs.h>
#include <windows.h>
#include <delayimp.h>
#endif

#include <iostream>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>

namespace aa {

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
    other.width_ = other.height_ = 0;
    other.channels_ = other.type_ = 0;
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
        other.width_ = other.height_ = 0;
        other.channels_ = other.type_ = 0;
        other.step_ = 0;
    }
    return *this;
}

void GPUMat::release() {
#ifdef HAVE_SYSTEM_CUDA
    if (data_) {
        cudaFree(data_);
        data_ = nullptr;
    }
#else
    data_ = nullptr;
#endif
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

    if (capacity_ >= req_size && type_ == type && width_ >= width && height_ >= height) {
        width_ = width; height_ = height;
        channels_ = ch; type_ = type; step_ = aligned_step;
        return;
    }

#ifdef HAVE_SYSTEM_CUDA
    if (data_) cudaFree(data_);
    cudaMalloc(&data_, req_size);
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
                wchar_t cuda_path[MAX_PATH];
                GetEnvironmentVariableW(L"CUDA_PATH", cuda_path, MAX_PATH);
                wchar_t full_path[MAX_PATH];
                swprintf(full_path, MAX_PATH, L"%ls\\bin\\%ls", cuda_path, dll);
                if (GetFileAttributesW(full_path) == INVALID_FILE_ATTRIBUTES) {
                    std::wcout << L"  [GPU] NPP runtime DLL not found (" << dll << L"), using CPU\n";
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

void GPUAccelerator::resize(const cv::Mat& src, cv::Mat& dst, cv::Size dsize,
                             double fx, double fy, int interp) {
    if (!initialized_) init();
#ifdef HAVE_SYSTEM_CUDA
    if (available_ && src.depth() == CV_8U && src.channels() <= 4) {
        cv::Mat c_src = src.isContinuous() ? src : src.clone();
        cv::Mat c_out;
        c_out.create(dsize, c_src.type());
        NppiSize src_roi = { c_src.cols, c_src.rows };
        NppiSize dst_roi = { c_out.cols, c_out.rows };
        NppiRect src_rect = { 0, 0, c_src.cols, c_src.rows };
        NppiRect dst_rect = { 0, 0, c_out.cols, c_out.rows };
        NppStreamContext ctx = make_stream_ctx();
        NppiInterpolationMode mode = NPPI_INTER_LINEAR;
        if (interp == cv::INTER_AREA || interp == cv::INTER_LANCZOS4)
            mode = NPPI_INTER_LANCZOS;
        else if (interp == cv::INTER_CUBIC)
            mode = NPPI_INTER_CUBIC;
        NppStatus st;
        double x_factor = static_cast<double>(c_out.cols) / c_src.cols;
        double y_factor = static_cast<double>(c_out.rows) / c_src.rows;
        Npp8u *d_src = static_cast<Npp8u*>(tls.a.get(c_src.total() * c_src.elemSize()));
        Npp8u *d_dst = static_cast<Npp8u*>(tls.b.get(c_out.total() * c_out.elemSize()));
        cudaMemcpy(d_src, c_src.data, c_src.total() * c_src.elemSize(), cudaMemcpyHostToDevice);
        if (c_src.channels() == 1) {
            st = nppiResizeSqrPixel_8u_C1R_Ctx(d_src, src_roi, c_src.cols * 1, src_rect,
                d_dst, c_out.cols * 1, dst_rect, x_factor, y_factor, 0, 0, mode, ctx);
        } else if (c_src.channels() == 3) {
            st = nppiResizeSqrPixel_8u_C3R_Ctx(d_src, src_roi, c_src.cols * 3, src_rect,
                d_dst, c_out.cols * 3, dst_rect, x_factor, y_factor, 0, 0, mode, ctx);
        } else {
            st = nppiResizeSqrPixel_8u_C4R_Ctx(d_src, src_roi, c_src.cols * 4, src_rect,
                d_dst, c_out.cols * 4, dst_rect, x_factor, y_factor, 0, 0, mode, ctx);
        }
        if (st == NPP_SUCCESS) {
            cudaDeviceSynchronize();
            if (cudaGetLastError() == cudaSuccess) {
                cudaMemcpy(c_out.data, d_dst, c_out.total() * c_out.elemSize(), cudaMemcpyDeviceToHost);
                c_out.copyTo(dst);
                return;
            }
        }
    }
#endif
    cv::resize(src, dst, dsize, fx, fy, interp);
}

void GPUAccelerator::absdiff(const cv::Mat& a, const cv::Mat& b, cv::Mat& out) {
    if (!initialized_) init();
#ifdef HAVE_SYSTEM_CUDA
    if (available_ && a.size() == b.size() && a.type() == b.type() && a.depth() == CV_8U) {
        cv::Mat c_a = a.isContinuous() ? a : a.clone();
        cv::Mat c_b = b.isContinuous() ? b : b.clone();
        cv::Mat c_out;
        c_out.create(c_a.size(), c_a.type());
        NppiSize roi = { c_a.cols, c_a.rows };
        NppStreamContext ctx = make_stream_ctx();
        NppStatus st;
        Npp8u *d_a = static_cast<Npp8u*>(tls.a.get(c_a.total() * c_a.elemSize()));
        Npp8u *d_b = static_cast<Npp8u*>(tls.b.get(c_b.total() * c_b.elemSize()));
        Npp8u *d_out = static_cast<Npp8u*>(tls.c.get(c_out.total() * c_out.elemSize()));
        cudaMemcpy(d_a, c_a.data, c_a.total() * c_a.elemSize(), cudaMemcpyHostToDevice);
        cudaMemcpy(d_b, c_b.data, c_b.total() * c_b.elemSize(), cudaMemcpyHostToDevice);
        if (c_a.channels() == 1) {
            st = nppiAbsDiff_8u_C1R_Ctx(d_a, c_a.cols * 1, d_b, c_b.cols * 1,
                d_out, c_out.cols * 1, roi, ctx);
        } else if (c_a.channels() == 3) {
            st = nppiAbsDiff_8u_C3R_Ctx(d_a, c_a.cols * 3, d_b, c_b.cols * 3,
                d_out, c_out.cols * 3, roi, ctx);
        } else {
            st = nppiAbsDiff_8u_C4R_Ctx(d_a, c_a.cols * 4, d_b, c_b.cols * 4,
                d_out, c_out.cols * 4, roi, ctx);
        }
        if (st == NPP_SUCCESS) {
            cudaDeviceSynchronize();
            if (cudaGetLastError() == cudaSuccess) {
                cudaMemcpy(c_out.data, d_out, c_out.total() * c_out.elemSize(), cudaMemcpyDeviceToHost);
                c_out.copyTo(out);
                return;
            }
        }
    }
#endif
    cv::absdiff(a, b, out);
}

void GPUAccelerator::cvtColor_BGR2GRAY(const cv::Mat& src, cv::Mat& dst) {
    if (!initialized_) init();
#ifdef HAVE_SYSTEM_CUDA
    if (available_ && src.type() == CV_8UC3) {
        cv::Mat c_src = src.isContinuous() ? src : src.clone();
        cv::Mat c_dst;
        c_dst.create(c_src.size(), CV_8UC1);
        NppiSize roi = { c_src.cols, c_src.rows };
        NppStreamContext ctx = make_stream_ctx();
        Npp8u *d_src = static_cast<Npp8u*>(tls.a.get(c_src.total() * c_src.elemSize()));
        Npp8u *d_dst = static_cast<Npp8u*>(tls.b.get(c_dst.total() * c_dst.elemSize()));
        cudaMemcpy(d_src, c_src.data, c_src.total() * c_src.elemSize(), cudaMemcpyHostToDevice);
        NppStatus st = nppiRGBToGray_8u_C3C1R_Ctx(d_src, c_src.cols * 3,
            d_dst, c_dst.cols * 1, roi, ctx);
        if (st == NPP_SUCCESS) {
            cudaDeviceSynchronize();
            if (cudaGetLastError() == cudaSuccess) {
                cudaMemcpy(c_dst.data, d_dst, c_dst.total() * c_dst.elemSize(), cudaMemcpyDeviceToHost);
                c_dst.copyTo(dst);
                return;
            }
        }
    }
#endif
    cv::cvtColor(src, dst, cv::COLOR_BGR2GRAY);
}

void GPUAccelerator::matchTemplate(const cv::Mat& image, const cv::Mat& templ,
                                    cv::Mat& result, int method) {
    if (!initialized_) init();
#ifdef HAVE_SYSTEM_CUDA
    if (available_ && method == cv::TM_CCOEFF_NORMED &&
        image.depth() == CV_8U && image.channels() == 1) {
        int result_w = image.cols - templ.cols + 1;
        int result_h = image.rows - templ.rows + 1;
        if (result_w > 0 && result_h > 0) {
            cv::Mat result_32f(result_h, result_w, CV_32FC1);
            std::vector<float> h_image_f(image.total());
            std::vector<float> h_templ_f(templ.total());
            for (int i = 0; i < static_cast<int>(image.total()); ++i)
                h_image_f[i] = image.data[i];
            for (int i = 0; i < static_cast<int>(templ.total()); ++i)
                h_templ_f[i] = templ.data[i];
            float *d_image = static_cast<float*>(tls.a.get(h_image_f.size() * sizeof(float)));
            float *d_templ = static_cast<float*>(tls.b.get(h_templ_f.size() * sizeof(float)));
            float *d_result = static_cast<float*>(tls.c.get(result_32f.total() * sizeof(float)));
            cudaMemcpy(d_image, h_image_f.data(), h_image_f.size() * sizeof(float), cudaMemcpyHostToDevice);
            cudaMemcpy(d_templ, h_templ_f.data(), h_templ_f.size() * sizeof(float), cudaMemcpyHostToDevice);
            NppStreamContext ctx = make_stream_ctx();
            NppStatus st = nppiCrossCorrFull_Norm_32f_C1R_Ctx(
                d_image, static_cast<int>(image.cols * sizeof(float)),
                {image.cols, image.rows},
                d_templ, static_cast<int>(templ.cols * sizeof(float)),
                {templ.cols, templ.rows},
                d_result, static_cast<int>(result_w * sizeof(float)),
                ctx);
            if (st == NPP_SUCCESS) {
                cudaMemcpy(result_32f.data, d_result, result_32f.total() * sizeof(float), cudaMemcpyDeviceToHost);
                result_32f.copyTo(result);
                return;
            }
        }
    }
#endif
    cv::matchTemplate(image, templ, result, method);
}

void GPUAccelerator::threshold(const cv::Mat& src, cv::Mat& dst, double thresh,
                                double maxval, int type) {
    if (!initialized_) init();
#ifdef HAVE_SYSTEM_CUDA
    // CPU fallback — NPP threshold semantics differ and memory transfer overhead negates benefit.
#endif
    cv::threshold(src, dst, thresh, maxval, type);
}

void GPUAccelerator::inRange(const cv::Mat& src, cv::Scalar lower,
                              cv::Scalar upper, cv::Mat& dst) {
    cv::inRange(src, lower, upper, dst);
}

void GPUAccelerator::integral(const cv::Mat& src, cv::Mat& sum, int sdepth) {
    cv::integral(src, sum, sdepth);
}

// ─────────────────────────────────────────────────────────────────────────────
// GPUPipeline Implementation (must be after make_stream_ctx)
// ─────────────────────────────────────────────────────────────────────────────

void GPUPipeline::init() {
    GPUAccelerator::init();
    available_ = GPUAccelerator::is_available();
}

void GPUPipeline::update_current(const cv::Mat& host_gray) {
    if (!available_) return;
    GPUMat temp;
    temp.upload(host_gray);
    prev_gray_ = std::move(curr_gray_);
    curr_gray_ = std::move(temp);
}

std::vector<double> GPUPipeline::compute_square_diff_means(const BoardGeometry& geo,
                                                              int margin_h, int margin_w) {
    if (!available_ || prev_gray_.empty() || curr_gray_.empty()) {
        return {};
    }

#ifdef HAVE_SYSTEM_CUDA
    int w = curr_gray_.width();
    int h = curr_gray_.height();
    diff_gray_.ensure_capacity(w, h, CV_8UC1);

    NppiSize roi = { w, h };
    NppStatus st = nppiAbsDiff_8u_C1R_Ctx(
        static_cast<const Npp8u*>(prev_gray_.ptr()), prev_gray_.step(),
        static_cast<const Npp8u*>(curr_gray_.ptr()), curr_gray_.step(),
        static_cast<Npp8u*>(diff_gray_.ptr()), diff_gray_.step(),
        roi, make_stream_ctx());
    if (st != NPP_SUCCESS) return {};

    int int_w = w + 1, int_h = h + 1;
    integral_.ensure_capacity(int_w, int_h, CV_32FC1);
    st = nppiIntegral_8u32f_C1R_Ctx(
        static_cast<const Npp8u*>(diff_gray_.ptr()), diff_gray_.step(),
        static_cast<Npp32f*>(integral_.ptr()), integral_.step(),
        roi, 0.0f, make_stream_ctx());
    if (st != NPP_SUCCESS) return {};

    cv::Mat int_mat;
    integral_.download(int_mat);

    // Pre-compute corner coordinates on host
    struct SquareRect { int x1, y1, x2, y2; double area_inv; };
    std::vector<SquareRect> rects(64);
    for (int row = 0; row < 8; ++row) {
        for (int col = 0; col < 8; ++col) {
            int y1 = static_cast<int>(row * geo.sq_h) + margin_h;
            int y2 = static_cast<int>((row + 1) * geo.sq_h) - margin_h;
            int x1 = static_cast<int>(col * geo.sq_w) + margin_w;
            int x2 = static_cast<int>((col + 1) * geo.sq_w) - margin_w;
            y1 = (std::max)(0, (std::min)(y1, h - 1));
            y2 = (std::max)(y1 + 1, (std::min)(y2, h));
            x1 = (std::max)(0, (std::min)(x1, w - 1));
            x2 = (std::max)(x1 + 1, (std::min)(x2, w));
            int area = (y2 - y1) * (x2 - x1);
            rects[row * 8 + col] = { x1, y1, x2, y2, 1.0 / area };
        }
    }

    const auto* I = int_mat.ptr<float>();
    int int_step = int_mat.step1();
    std::vector<double> means(64);
    for (int i = 0; i < 64; ++i) {
        auto& r = rects[i];
        double sum = I[r.y2 * int_step + r.x2]
                   - I[r.y1 * int_step + r.x2]
                   - I[r.y2 * int_step + r.x1]
                   + I[r.y1 * int_step + r.x1];
        means[i] = sum * r.area_inv;
    }
    return means;
#else
    return {};
#endif
}

void GPUPipeline::download_current(cv::Mat& host) const {
    curr_gray_.download(host);
}

void GPUPipeline::download_previous(cv::Mat& host) const {
    prev_gray_.download(host);
}

void GPUPipeline::download_diff(cv::Mat& host) const {
    diff_gray_.download(host);
}

} // namespace aa
