#include "PercipioCamera.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <utility>
#include <condition_variable>
#include <mutex>

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "TYApi.h"
#include "TYImageProc.h"
#endif

namespace lbgm461 {

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
#define TYCHECK(expr) do { \
    TY_STATUS _s = (expr); \
    if (_s != TY_STATUS_OK) { \
        std::cerr << "[SDK] " #expr " failed: " << TYErrorString(_s) << std::endl; \
        SetError(std::string(#expr " failed: ") + TYErrorString(_s)); \
        return false; \
    } \
} while(0)
#endif

class PercipioCamera::Impl {
public:
    explicit Impl(ServiceConfig config)
        : config_(std::move(config)),
          initialized_(false),
          connected_(false),
          running_(false),
          stop_requested_(false),
          latest_ticks_(0) {
    }

    ~Impl() {
        Stop();
    }

    bool Init() {
#if !defined(LBGM461_ENABLE_PERCIPIO_SDK)
        if (!config_.synthetic_camera) {
            SetError("synthetic camera is disabled, but Percipio SDK support is not enabled in this build");
            return false;
        }
#endif

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
        if (!config_.synthetic_camera) {
            TY_STATUS status = TYInitLib();
            if (status != TY_STATUS_OK) {
                SetError(std::string("TYInitLib failed: ") + TYErrorString(status));
                return false;
            }
            TY_VERSION_INFO ver;
            TYLibVersion(&ver);
            std::cout << "[SDK] Camport4 lib version: " << ver.major << "." << ver.minor << "." << ver.patch << std::endl;
        }
#endif

        initialized_ = true;
        SetError("");
        return true;
    }

    bool Connect() {
        if (!initialized_) {
            SetError("camera is not initialized");
            return false;
        }

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
        if (!config_.synthetic_camera) {
            // Discover devices
            TYCHECK(TYUpdateInterfaceList());

            uint32_t ifaceCount = 0;
            TYCHECK(TYGetInterfaceNumber(&ifaceCount));
            if (ifaceCount == 0) {
                SetError("No network interfaces found");
                return false;
            }

            std::vector<TY_INTERFACE_INFO> ifaceInfos(ifaceCount);
            uint32_t filled = 0;
            TYCHECK(TYGetInterfaceList(ifaceInfos.data(), ifaceCount, &filled));

            // Try opening device directly with IP
            TY_STATUS status;
            if (!config_.camera_ip.empty()) {
                for (uint32_t i = 0; i < filled; i++) {
                    if (!TYIsNetworkInterface(ifaceInfos[i].type)) continue;
                    TY_INTERFACE_HANDLE hIface = nullptr;
                    status = TYOpenInterface(ifaceInfos[i].id, &hIface);
                    if (status != TY_STATUS_OK) continue;

                    TY_DEV_HANDLE hDev = nullptr;
                    status = TYOpenDeviceWithIP(hIface, config_.camera_ip.c_str(), &hDev);
                    if (status == TY_STATUS_OK) {
                        hDevice_ = hDev;
                        hIface_ = hIface;
                        std::cout << "[SDK] Connected via IP: " << config_.camera_ip << std::endl;
                        break;
                    }
                    TYCloseInterface(hIface);
                }
            }

            // Fallback: scan and open by SN
            if (!hDevice_ && !config_.camera_sn.empty()) {
                for (uint32_t i = 0; i < filled; i++) {
                    TY_INTERFACE_HANDLE hIface = nullptr;
                    status = TYOpenInterface(ifaceInfos[i].id, &hIface);
                    if (status != TY_STATUS_OK) continue;

                    TYUpdateDeviceList(hIface);
                    TY_DEV_HANDLE hDev = nullptr;
                    status = TYOpenDevice(hIface, config_.camera_sn.c_str(), &hDev);
                    if (status == TY_STATUS_OK) {
                        hDevice_ = hDev;
                        hIface_ = hIface;
                        std::cout << "[SDK] Connected via SN: " << config_.camera_sn << std::endl;
                        break;
                    }
                    TYCloseInterface(hIface);
                }
            }

            if (!hDevice_) {
                SetError("Failed to open camera (IP=" + config_.camera_ip + " SN=" + config_.camera_sn + ")");
                return false;
            }

            // Get device info
            TY_DEVICE_BASE_INFO devInfo;
            if (TYGetDeviceInfo(hDevice_, &devInfo) == TY_STATUS_OK) {
                std::cout << "[SDK] Device: " << devInfo.modelName
                          << " SN=" << devInfo.id << std::endl;
            }

            // Enable depth + color components
            TY_COMPONENT_ID allComps;
            TYCHECK(TYGetComponentIDs(hDevice_, &allComps));

            if (allComps & TY_COMPONENT_DEPTH_CAM) {
                TYCHECK(TYEnableComponents(hDevice_, TY_COMPONENT_DEPTH_CAM));
            }
            if (allComps & TY_COMPONENT_RGB_CAM) {
                TYCHECK(TYEnableComponents(hDevice_, TY_COMPONENT_RGB_CAM));
            }

            // Get depth scale unit
            float scaleUnit = 1.0f;
            TYGetFloat(hDevice_, TY_COMPONENT_DEPTH_CAM, TY_FLOAT_SCALE_UNIT, &scaleUnit);
            config_.depth_scale = scaleUnit;
            std::cout << "[SDK] Depth scale unit: " << scaleUnit << std::endl;

            // Get intrinsics
            TY_CAMERA_CALIB_INFO depthCalib = {};
            status = TYGetStruct(hDevice_, TY_COMPONENT_DEPTH_CAM,
                                 TY_STRUCT_CAM_CALIB_DATA, &depthCalib, sizeof(depthCalib));
            if (status == TY_STATUS_OK && depthCalib.intrinsicWidth > 0) {
                intrinsics_.width = depthCalib.intrinsicWidth;
                intrinsics_.height = depthCalib.intrinsicHeight;
                // 3x3 row-major: [fx, 0, cx; 0, fy, cy; 0, 0, 1]
                intrinsics_.fx = static_cast<double>(depthCalib.intrinsic.data[0]);
                intrinsics_.fy = static_cast<double>(depthCalib.intrinsic.data[4]);
                intrinsics_.ppx = static_cast<double>(depthCalib.intrinsic.data[2]);
                intrinsics_.ppy = static_cast<double>(depthCalib.intrinsic.data[5]);
                intrinsics_.model = 0;
                intrinsics_.coeffs.resize(12);
                for (int i = 0; i < 12; i++) {
                    intrinsics_.coeffs[i] = static_cast<double>(depthCalib.distortion.data[i]);
                }
                std::cout << "[SDK] Intrinsics: " << intrinsics_.width << "x" << intrinsics_.height
                          << " fx=" << intrinsics_.fx << " fy=" << intrinsics_.fy
                          << " cx=" << intrinsics_.ppx << " cy=" << intrinsics_.ppy << std::endl;
            } else {
                std::cout << "[SDK] Warning: failed to get intrinsics, using defaults" << std::endl;
            }

            // Allocate frame buffers
            uint32_t frameSize = 0;
            TYCHECK(TYGetFrameBufferSize(hDevice_, &frameSize));
            frameBuffer_[0].resize(frameSize);
            frameBuffer_[1].resize(frameSize);
            TYCHECK(TYEnqueueBuffer(hDevice_, frameBuffer_[0].data(), frameSize));
            TYCHECK(TYEnqueueBuffer(hDevice_, frameBuffer_[1].data(), frameSize));
            std::cout << "[SDK] Frame buffer size: " << frameSize << " bytes" << std::endl;

            // Disable trigger mode (continuous capture)
            bool hasTrigger = false;
            TYHasFeature(hDevice_, TY_COMPONENT_DEVICE, TY_STRUCT_TRIGGER_PARAM_EX, &hasTrigger);
            if (hasTrigger) {
                TY_TRIGGER_PARAM_EX trigger = {};
                trigger.mode = TY_TRIGGER_MODE_OFF;
                TYSetStruct(hDevice_, TY_COMPONENT_DEVICE, TY_STRUCT_TRIGGER_PARAM_EX, &trigger, sizeof(trigger));
            }
        }
#endif

        // Set default intrinsics if not populated from camera
        if (!intrinsics_.IsValid()) {
            intrinsics_.width = config_.frame_width;
            intrinsics_.height = config_.frame_height;
            intrinsics_.fx = 600.0;
            intrinsics_.fy = 600.0;
            intrinsics_.ppx = static_cast<double>(config_.frame_width) / 2.0;
            intrinsics_.ppy = static_cast<double>(config_.frame_height) / 2.0;
            intrinsics_.model = 0;
            intrinsics_.coeffs = {0.0, 0.0, 0.0, 0.0, 0.0};
        }

        connected_ = true;
        SetError("");
        return true;
    }

    bool Start() {
        if (!connected_) {
            SetError("camera is not connected");
            return false;
        }

        if (running_) {
            return true;
        }

        running_ = true;
        stop_requested_ = false;

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
        if (!config_.synthetic_camera) {
            TY_STATUS status = TYStartCapture(hDevice_);
            if (status != TY_STATUS_OK) {
                SetError(std::string("TYStartCapture failed: ") + TYErrorString(status));
                running_ = false;
                return false;
            }
            std::cout << "[SDK] Capture started" << std::endl;
        }
#endif

        worker_ = std::thread(&Impl::WorkerLoop, this);
        return true;
    }

    bool Stop() {
        stop_requested_ = true;
        running_ = false;
        cv_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
        if (!config_.synthetic_camera && hDevice_) {
            TYStopCapture(hDevice_);
            TYCloseDevice(hDevice_);
            hDevice_ = nullptr;
        }
        if (hIface_) {
            TYCloseInterface(hIface_);
            hIface_ = nullptr;
        }
        if (!config_.synthetic_camera) {
            TYDeinitLib();
        }
#endif

        connected_ = false;
        initialized_ = false;
        return true;
    }

    bool Grab(FrameSnapshot& out, int timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);

        // Check if we have a fresh frame (not stale)
        const std::int64_t now = NowTicks();
        const std::int64_t max_age_ms = static_cast<std::int64_t>(timeout_ms) * 4;

        if (latest_ticks_ > 0 && (now - latest_ticks_) < max_age_ms) {
            out = latest_frame_;
            return true;
        }

        if (timeout_ms <= 0) {
            SetError("no frame available");
            return false;
        }

        const bool ready = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, now, max_age_ms] {
            return (latest_ticks_ > 0 && (NowTicks() - latest_ticks_) < max_age_ms) || stop_requested_;
        });

        if (!ready || latest_ticks_ <= 0 || (NowTicks() - latest_ticks_) >= max_age_ms) {
            SetError("frame grab timeout (camera may be disconnected)");
            return false;
        }

        out = latest_frame_;
        return true;
    }

    CameraIntrinsicsData GetIntrinsics() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return intrinsics_;
    }

    bool IsConnected() const noexcept {
        return connected_;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

private:
    void WorkerLoop() {
        int consecutive_errors = 0;
        const int kMaxConsecutiveErrors = 10;  // ~20s at 2s timeout per TYFetchFrame

        while (!stop_requested_) {
#ifdef LBGM461_ENABLE_PERCIPIO_SDK
            if (!config_.synthetic_camera) {
                TY_FRAME_DATA frame = {};
                TY_STATUS status = TYFetchFrame(hDevice_, &frame, 2000);
                if (status == TY_STATUS_OK) {
                    ProcessFrame(frame);
                    TYEnqueueBuffer(hDevice_, frame.userBuffer, frame.bufferSize);
                    consecutive_errors = 0;  // Reset on success
                } else if (status == TY_STATUS_TIMEOUT) {
                    consecutive_errors++;
                } else {
                    consecutive_errors++;
                    std::cerr << "[SDK] TYFetchFrame error (" << consecutive_errors << "/"
                              << kMaxConsecutiveErrors << "): " << TYErrorString(status) << std::endl;
                }

                // Auto-reconnect if too many consecutive failures
                if (consecutive_errors >= kMaxConsecutiveErrors) {
                    std::cerr << "[SDK] Too many errors, attempting reconnect..." << std::endl;
                    if (Reconnect()) {
                        consecutive_errors = 0;
                        std::cout << "[SDK] Reconnect successful" << std::endl;
                    } else {
                        std::cerr << "[SDK] Reconnect failed: " << last_error_
                                  << ", retry in 5s..." << std::endl;
                        consecutive_errors = 0;  // Reset to avoid rapid retry
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                    }
                }
                continue;
            }
#endif
            // Synthetic mode
            const auto ticks = NowTicks();
            FrameSnapshot synth = GenerateSyntheticFrame(ticks);

            {
                std::lock_guard<std::mutex> lock(mutex_);
                latest_frame_ = std::move(synth);
                latest_ticks_ = ticks;
            }
            cv_.notify_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, config_.frame_period_ms)));
        }
    }

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    bool Reconnect() {
        // 1. Stop capture and close device
        if (hDevice_) {
            TYStopCapture(hDevice_);
            TYCloseDevice(hDevice_);
            hDevice_ = nullptr;
        }
        if (hIface_) {
            TYCloseInterface(hIface_);
            hIface_ = nullptr;
        }

        // 2. Brief pause to let camera settle
        std::this_thread::sleep_for(std::chrono::seconds(2));

        if (stop_requested_) return false;

        // 3. Re-open device with IP
        TYCHECK(TYUpdateInterfaceList());

        uint32_t ifaceCount = 0;
        TYCHECK(TYGetInterfaceNumber(&ifaceCount));
        if (ifaceCount == 0) {
            SetError("Reconnect: no network interfaces");
            return false;
        }

        std::vector<TY_INTERFACE_INFO> ifaceInfos(ifaceCount);
        uint32_t filled = 0;
        TYCHECK(TYGetInterfaceList(ifaceInfos.data(), ifaceCount, &filled));

        for (uint32_t i = 0; i < filled; i++) {
            if (!TYIsNetworkInterface(ifaceInfos[i].type)) continue;
            TY_INTERFACE_HANDLE hIface = nullptr;
            TY_STATUS st = TYOpenInterface(ifaceInfos[i].id, &hIface);
            if (st != TY_STATUS_OK) continue;

            TY_DEV_HANDLE hDev = nullptr;
            st = TYOpenDeviceWithIP(hIface, config_.camera_ip.c_str(), &hDev);
            if (st == TY_STATUS_OK) {
                hDevice_ = hDev;
                hIface_ = hIface;
                break;
            }
            TYCloseInterface(hIface);
        }

        if (!hDevice_) {
            SetError("Reconnect: failed to open camera at " + config_.camera_ip);
            return false;
        }

        // 4. Re-enable components
        TY_COMPONENT_ID allComps;
        TYCHECK(TYGetComponentIDs(hDevice_, &allComps));
        if (allComps & TY_COMPONENT_DEPTH_CAM) {
            TYCHECK(TYEnableComponents(hDevice_, TY_COMPONENT_DEPTH_CAM));
        }
        if (allComps & TY_COMPONENT_RGB_CAM) {
            TYCHECK(TYEnableComponents(hDevice_, TY_COMPONENT_RGB_CAM));
        }

        // 5. Re-allocate frame buffers
        uint32_t frameSize = 0;
        TYCHECK(TYGetFrameBufferSize(hDevice_, &frameSize));
        frameBuffer_[0].resize(frameSize);
        frameBuffer_[1].resize(frameSize);
        TYCHECK(TYEnqueueBuffer(hDevice_, frameBuffer_[0].data(), frameSize));
        TYCHECK(TYEnqueueBuffer(hDevice_, frameBuffer_[1].data(), frameSize));

        // 6. Disable trigger mode
        bool hasTrigger = false;
        TYHasFeature(hDevice_, TY_COMPONENT_DEVICE, TY_STRUCT_TRIGGER_PARAM_EX, &hasTrigger);
        if (hasTrigger) {
            TY_TRIGGER_PARAM_EX trigger = {};
            trigger.mode = TY_TRIGGER_MODE_OFF;
            TYSetStruct(hDevice_, TY_COMPONENT_DEVICE, TY_STRUCT_TRIGGER_PARAM_EX, &trigger, sizeof(trigger));
        }

        // 7. Restart capture
        TY_STATUS status = TYStartCapture(hDevice_);
        if (status != TY_STATUS_OK) {
            SetError(std::string("Reconnect: TYStartCapture failed: ") + TYErrorString(status));
            TYCloseDevice(hDevice_);
            hDevice_ = nullptr;
            return false;
        }

        std::cout << "[SDK] Reconnect: capture restarted" << std::endl;
        return true;
    }
#endif

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    void ProcessFrame(const TY_FRAME_DATA& frame) {
        std::int64_t ticks = NowTicks();

        FrameSnapshot snapshot;
        snapshot.ip = config_.camera_ip;
        snapshot.ticks = ticks;
        snapshot.depth_scale = config_.depth_scale;
        snapshot.intrinsics = intrinsics_;

        for (int i = 0; i < frame.validCount; i++) {
            const TY_IMAGE_DATA& img = frame.image[i];
            if (img.status != TY_STATUS_OK) continue;

            if (img.componentID == TY_COMPONENT_DEPTH_CAM) {
                snapshot.depth.width = img.width;
                snapshot.depth.height = img.height;
                snapshot.depth.mat_type = 2;  // CV_16UC1
                std::size_t bytes = static_cast<std::size_t>(img.width) * img.height * 2;
                snapshot.depth.data.assign(
                    static_cast<const uint8_t*>(img.buffer),
                    static_cast<const uint8_t*>(img.buffer) + bytes);
            } else if (img.componentID == TY_COMPONENT_RGB_CAM) {
                snapshot.color.width = img.width;
                snapshot.color.height = img.height;
                // Handle different pixel formats
                if (img.pixelFormat == TY_PIXEL_FORMAT_BGR || img.pixelFormat == TY_PIXEL_FORMAT_RGB) {
                    snapshot.color.mat_type = 16;  // CV_8UC3
                    std::size_t bytes = static_cast<std::size_t>(img.width) * img.height * 3;
                    snapshot.color.data.assign(
                        static_cast<const uint8_t*>(img.buffer),
                        static_cast<const uint8_t*>(img.buffer) + bytes);
                } else {
                    // Use TYDecodeImage for YUYV, Bayer, JPEG, etc.
                    snapshot.color.mat_type = 16;  // CV_8UC3
                    TYImageInfo inputInfo = {};
                    inputInfo.width = img.width;
                    inputInfo.height = img.height;
                    inputInfo.format = static_cast<TYPixFmt>(img.pixelFormat);
                    inputInfo.dataSize = img.size;
                    inputInfo.data = img.buffer;

                    uint32_t outSize = 0;
                    TYGetDecodeBufferSize(&inputInfo, &outSize, TY_OUTPUT_FORMAT_BGR);
                    snapshot.color.data.resize(outSize);

                    TYDecodeResult decResult = {};
                    if (TYDecodeImage(&inputInfo, TY_OUTPUT_FORMAT_BGR,
                                      snapshot.color.data.data(), outSize, &decResult) == TY_DECODE_SUCCESS) {
                        snapshot.color.data.resize(decResult.dataSize);
                    } else {
                        // Last resort: raw copy
                        std::size_t rawBytes = static_cast<std::size_t>(img.size);
                        snapshot.color.data.assign(
                            static_cast<const uint8_t*>(img.buffer),
                            static_cast<const uint8_t*>(img.buffer) + rawBytes);
                    }
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_frame_ = std::move(snapshot);
            latest_ticks_ = ticks;
        }
        cv_.notify_all();
    }
#endif

    FrameSnapshot GenerateSyntheticFrame(std::int64_t ticks) const {
        FrameSnapshot frame;
        frame.ip = config_.camera_ip;
        frame.intrinsics = intrinsics_;
        frame.depth_scale = config_.depth_scale;
        frame.ticks = ticks;

        const int width = std::max(1, config_.frame_width);
        const int height = std::max(1, config_.frame_height);
        frame.color.width = width;
        frame.color.height = height;
        frame.color.mat_type = 16;
        frame.color.data.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);

        frame.depth.width = width;
        frame.depth.height = height;
        frame.depth.mat_type = 2;
        frame.depth.data.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 2U);

        const std::uint8_t tick_low = static_cast<std::uint8_t>(ticks & 0xFF);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x));
                const std::size_t color_offset = index * 3U;
                frame.color.data[color_offset + 0U] = static_cast<std::uint8_t>((x + tick_low) % 256);
                frame.color.data[color_offset + 1U] = static_cast<std::uint8_t>((y + tick_low) % 256);
                frame.color.data[color_offset + 2U] = static_cast<std::uint8_t>(((x + y) + tick_low) % 256);

                const std::uint16_t depth_value = static_cast<std::uint16_t>(500U + ((x + y) % 1000));
                const std::size_t depth_offset = index * 2U;
                frame.depth.data[depth_offset + 0U] = static_cast<std::uint8_t>(depth_value & 0xFFU);
                frame.depth.data[depth_offset + 1U] = static_cast<std::uint8_t>((depth_value >> 8U) & 0xFFU);
            }
        }

        return frame;
    }

    std::int64_t NowTicks() const {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }

    void SetError(const std::string& message) {
        last_error_ = message;
    }

    ServiceConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool initialized_;
    bool connected_;
    bool running_;
    bool stop_requested_;
    std::int64_t latest_ticks_;
    FrameSnapshot latest_frame_;
    CameraIntrinsicsData intrinsics_;
    std::string last_error_;

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    TY_INTERFACE_HANDLE hIface_ = nullptr;
    TY_DEV_HANDLE hDevice_ = nullptr;
    std::vector<char> frameBuffer_[2];
#endif
};

// ===== Public interface delegates =====

PercipioCamera::PercipioCamera(ServiceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

PercipioCamera::~PercipioCamera() = default;

bool PercipioCamera::Init() { return impl_->Init(); }
bool PercipioCamera::Connect() { return impl_->Connect(); }
bool PercipioCamera::Start() { return impl_->Start(); }
bool PercipioCamera::Stop() { return impl_->Stop(); }
bool PercipioCamera::Grab(FrameSnapshot& out, int timeout_ms) { return impl_->Grab(out, timeout_ms); }
CameraIntrinsicsData PercipioCamera::GetIntrinsics() const { return impl_->GetIntrinsics(); }
bool PercipioCamera::IsConnected() const noexcept { return impl_->IsConnected(); }
std::string PercipioCamera::LastError() const { return impl_->LastError(); }

}  // namespace lbgm461
