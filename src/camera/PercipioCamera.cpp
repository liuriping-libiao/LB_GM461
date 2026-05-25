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
#include "TYCoordinateMapper.h"
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

            // 枚举深度相机支持的图像模式（近/远视场）
            uint32_t depthModeCount = 0;
            TYGetEnumEntryCount(hDevice_, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE, &depthModeCount);
            if (depthModeCount > 0) {
                std::vector<TY_ENUM_ENTRY> depthModes(depthModeCount);
                TYGetEnumEntryInfo(hDevice_, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE,
                                   depthModes.data(), depthModeCount, &depthModeCount);
                std::cout << "[SDK] 深度相机支持的图像模式 (" << depthModeCount << " 种):" << std::endl;
                for (uint32_t i = 0; i < depthModeCount; i++) {
                    std::cout << "  [" << i << "] " << depthModes[i].description
                              << " (value=0x" << std::hex << depthModes[i].value << std::dec << ")" << std::endl;
                }

                uint32_t currentDepthMode = 0;
                TYGetEnum(hDevice_, TY_COMPONENT_DEPTH_CAM, TY_ENUM_IMAGE_MODE, &currentDepthMode);
                std::cout << "[SDK] 深度相机当前模式: 0x" << std::hex << currentDepthMode << std::dec << std::endl;
            }

            // 枚举彩色相机支持的图像模式
            uint32_t colorModeCount = 0;
            TYGetEnumEntryCount(hDevice_, TY_COMPONENT_RGB_CAM, TY_ENUM_IMAGE_MODE, &colorModeCount);
            if (colorModeCount > 0) {
                std::vector<TY_ENUM_ENTRY> colorModes(colorModeCount);
                TYGetEnumEntryInfo(hDevice_, TY_COMPONENT_RGB_CAM, TY_ENUM_IMAGE_MODE,
                                   colorModes.data(), colorModeCount, &colorModeCount);
                std::cout << "[SDK] 彩色相机支持的图像模式 (" << colorModeCount << " 种):" << std::endl;
                for (uint32_t i = 0; i < colorModeCount; i++) {
                    std::cout << "  [" << i << "] " << colorModes[i].description
                              << " (value=0x" << std::hex << colorModes[i].value << std::dec << ")" << std::endl;
                }

                uint32_t currentColorMode = 0;
                TYGetEnum(hDevice_, TY_COMPONENT_RGB_CAM, TY_ENUM_IMAGE_MODE, &currentColorMode);
                std::cout << "[SDK] 彩色相机当前模式: 0x" << std::hex << currentColorMode << std::dec << std::endl;
            }

            // Get depth scale unit
            float scaleUnit = 1.0f;
            TYGetFloat(hDevice_, TY_COMPONENT_DEPTH_CAM, TY_FLOAT_SCALE_UNIT, &scaleUnit);
            config_.depth_scale = scaleUnit;
            std::cout << "[SDK] 深度缩放单位: " << scaleUnit << std::endl;

            // 获取深度相机标定数据
            depthCalib_ = {};
            status = TYGetStruct(hDevice_, TY_COMPONENT_DEPTH_CAM,
                                 TY_STRUCT_CAM_CALIB_DATA, &depthCalib_, sizeof(depthCalib_));
            if (status == TY_STATUS_OK && depthCalib_.intrinsicWidth > 0) {
                intrinsics_.width = depthCalib_.intrinsicWidth;
                intrinsics_.height = depthCalib_.intrinsicHeight;
                // 3x3 row-major: [fx, 0, cx; 0, fy, cy; 0, 0, 1]
                intrinsics_.fx = static_cast<double>(depthCalib_.intrinsic.data[0]);
                intrinsics_.fy = static_cast<double>(depthCalib_.intrinsic.data[4]);
                intrinsics_.ppx = static_cast<double>(depthCalib_.intrinsic.data[2]);
                intrinsics_.ppy = static_cast<double>(depthCalib_.intrinsic.data[5]);
                intrinsics_.model = 0;
                intrinsics_.coeffs.resize(12);
                for (int i = 0; i < 12; i++) {
                    intrinsics_.coeffs[i] = static_cast<double>(depthCalib_.distortion.data[i]);
                }
                std::cout << "[SDK] 深度相机内参: " << intrinsics_.width << "x" << intrinsics_.height
                          << " fx=" << intrinsics_.fx << " fy=" << intrinsics_.fy
                          << " cx=" << intrinsics_.ppx << " cy=" << intrinsics_.ppy << std::endl;
            } else {
                std::cout << "[SDK] 警告: 获取深度相机内参失败，使用默认值" << std::endl;
            }

            // 获取彩色相机标定数据（用于坐标对齐映射）
            colorCalib_ = {};
            status = TYGetStruct(hDevice_, TY_COMPONENT_RGB_CAM,
                                 TY_STRUCT_CAM_CALIB_DATA, &colorCalib_, sizeof(colorCalib_));
            if (status == TY_STATUS_OK && colorCalib_.intrinsicWidth > 0) {
                std::cout << "[SDK] 彩色相机内参: " << colorCalib_.intrinsicWidth << "x" << colorCalib_.intrinsicHeight
                          << " fx=" << colorCalib_.intrinsic.data[0] << " fy=" << colorCalib_.intrinsic.data[4]
                          << " cx=" << colorCalib_.intrinsic.data[2] << " cy=" << colorCalib_.intrinsic.data[5] << std::endl;
                hasColorCalib_ = true;
            } else {
                std::cout << "[SDK] 警告: 获取彩色相机内参失败，将跳过坐标对齐" << std::endl;
                hasColorCalib_ = false;
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

        static bool logged_once = false;
        if (!logged_once) {
            std::cout << "[SDK] 首帧数据(对齐前): 彩色图=" << snapshot.color.width << "x" << snapshot.color.height
                      << " (" << snapshot.color.data.size() << " 字节)"
                      << ", 深度图=" << snapshot.depth.width << "x" << snapshot.depth.height
                      << " (" << snapshot.depth.data.size() << " 字节)" << std::endl;
        }

        // 将深度图映射到彩色图坐标系，保留原彩色图质量。
        if (hasColorCalib_ && !snapshot.depth.Empty() && !snapshot.color.Empty()) {
            const int depthW = snapshot.depth.width;
            const int depthH = snapshot.depth.height;
            const int colorW = snapshot.color.width;
            const int colorH = snapshot.color.height;
            const int outputW = std::min(depthW, colorW);
            const int outputH = std::min(depthH, colorH);
            const std::size_t depthPixelCount = static_cast<std::size_t>(depthW) * depthH;
            const std::size_t expectedDepthBytes = depthPixelCount * sizeof(uint16_t);

            if (snapshot.depth.data.size() < expectedDepthBytes) {
                if (!logged_once) {
                    std::cout << "[SDK] 警告: 深度数据大小不足, 跳过深度→彩色坐标映射" << std::endl;
                }
            } else {
                std::vector<uint16_t> sourceDepth(depthPixelCount, 0);
                std::memcpy(sourceDepth.data(), snapshot.depth.data.data(), expectedDepthBytes);
                std::vector<uint16_t> mappedDepth(static_cast<std::size_t>(colorW) * colorH, 0);

                TY_STATUS mapStatus = TYMapDepthImageToColorCoordinate(
                    &depthCalib_, static_cast<uint32_t>(depthW), static_cast<uint32_t>(depthH),
                    sourceDepth.data(),
                    &colorCalib_, static_cast<uint32_t>(colorW), static_cast<uint32_t>(colorH),
                    mappedDepth.data(),
                    config_.depth_scale);

                if (mapStatus == TY_STATUS_OK) {
                    const int cropX = CenterCropOffset(colorW, outputW);
                    const int cropY = CenterCropOffset(colorH, outputH);

                    snapshot.color = CropBgrImage(snapshot.color, outputW, outputH);
                    snapshot.depth = CropDepthImage(mappedDepth, colorW, colorH, outputW, outputH);
                    snapshot.intrinsics = BuildColorAlignedIntrinsics(colorW, colorH, cropX, cropY, outputW, outputH);

                    if (!logged_once) {
                        const int validDepthPercent = CalculateValidDepthPercent(snapshot.depth);
                        std::cout << "[SDK] 首帧数据(对齐后): 彩色图=" << snapshot.color.width << "x" << snapshot.color.height
                                  << " (" << snapshot.color.data.size() << " 字节), 深度图="
                                  << snapshot.depth.width << "x" << snapshot.depth.height
                                  << " (" << snapshot.depth.data.size() << " 字节), 深度已对齐到彩色图"
                                  << ", 有效深度=" << validDepthPercent << "%" << std::endl;
                    }
                } else {
                    if (!logged_once) {
                        std::cout << "[SDK] 警告: 深度→彩色坐标映射失败, 保留原始帧" << std::endl;
                    }
                }
            }
        }

        if (!logged_once) {
            logged_once = true;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            latest_frame_ = std::move(snapshot);
            latest_ticks_ = ticks;
        }
        cv_.notify_all();
    }
#endif

#ifdef LBGM461_ENABLE_PERCIPIO_SDK
    static int CenterCropOffset(int source_size, int target_size) {
        return std::max(0, (source_size - target_size) / 2);
    }

    static ImageBuffer CropBgrImage(const ImageBuffer& source, int target_width, int target_height) {
        ImageBuffer cropped;
        cropped.width = std::min(source.width, target_width);
        cropped.height = std::min(source.height, target_height);
        cropped.mat_type = source.mat_type;
        cropped.data.resize(static_cast<std::size_t>(cropped.width) * cropped.height * 3U);

        const int crop_x = CenterCropOffset(source.width, cropped.width);
        const int crop_y = CenterCropOffset(source.height, cropped.height);
        const std::size_t target_stride = static_cast<std::size_t>(cropped.width) * 3U;

        for (int row = 0; row < cropped.height; ++row) {
            const std::size_t source_offset = (static_cast<std::size_t>(row + crop_y) * source.width + crop_x) * 3U;
            std::copy_n(source.data.data() + source_offset, target_stride, cropped.data.data() + row * target_stride);
        }

        return cropped;
    }

    static ImageBuffer CropDepthImage(const std::vector<uint16_t>& source,
                                      int source_width,
                                      int source_height,
                                      int target_width,
                                      int target_height) {
        ImageBuffer cropped;
        cropped.width = std::min(source_width, target_width);
        cropped.height = std::min(source_height, target_height);
        cropped.mat_type = 2;  // CV_16UC1
        cropped.data.resize(static_cast<std::size_t>(cropped.width) * cropped.height * 2U);

        const int crop_x = CenterCropOffset(source_width, cropped.width);
        const int crop_y = CenterCropOffset(source_height, cropped.height);
        const std::size_t target_stride = static_cast<std::size_t>(cropped.width) * sizeof(uint16_t);

        for (int row = 0; row < cropped.height; ++row) {
            const std::size_t source_offset = (static_cast<std::size_t>(row + crop_y) * source_width + crop_x) * sizeof(uint16_t);
            std::memcpy(cropped.data.data() + row * target_stride,
                        reinterpret_cast<const uint8_t*>(source.data()) + source_offset,
                        target_stride);
        }

        return cropped;
    }

    static int CalculateValidDepthPercent(const ImageBuffer& depth) {
        const std::size_t pixel_count = static_cast<std::size_t>(depth.width) * depth.height;
        if (pixel_count == 0 || depth.data.size() < pixel_count * 2U) {
            return 0;
        }

        std::size_t valid_count = 0;
        for (std::size_t index = 0; index < pixel_count; ++index) {
            if (depth.data[index * 2U] != 0 || depth.data[index * 2U + 1U] != 0) {
                ++valid_count;
            }
        }

        return static_cast<int>((valid_count * 100U + pixel_count / 2U) / pixel_count);
    }

    CameraIntrinsicsData BuildColorAlignedIntrinsics(int color_width,
                                                     int color_height,
                                                     int crop_x,
                                                     int crop_y,
                                                     int output_width,
                                                     int output_height) const {
        CameraIntrinsicsData aligned;
        aligned.width = output_width;
        aligned.height = output_height;

        const double scale_x = colorCalib_.intrinsicWidth > 0
            ? static_cast<double>(color_width) / static_cast<double>(colorCalib_.intrinsicWidth)
            : 1.0;
        const double scale_y = colorCalib_.intrinsicHeight > 0
            ? static_cast<double>(color_height) / static_cast<double>(colorCalib_.intrinsicHeight)
            : 1.0;

        aligned.fx = static_cast<double>(colorCalib_.intrinsic.data[0]) * scale_x;
        aligned.fy = static_cast<double>(colorCalib_.intrinsic.data[4]) * scale_y;
        aligned.ppx = static_cast<double>(colorCalib_.intrinsic.data[2]) * scale_x - static_cast<double>(crop_x);
        aligned.ppy = static_cast<double>(colorCalib_.intrinsic.data[5]) * scale_y - static_cast<double>(crop_y);
        aligned.model = 0;
        aligned.coeffs.resize(12);
        for (int i = 0; i < 12; ++i) {
            aligned.coeffs[i] = static_cast<double>(colorCalib_.distortion.data[i]);
        }

        return aligned;
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
    TY_CAMERA_CALIB_INFO depthCalib_ = {};
    TY_CAMERA_CALIB_INFO colorCalib_ = {};
    bool hasColorCalib_ = false;
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
