#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lbgm461 {

struct CameraIntrinsicsData {
    int width = 0;
    int height = 0;
    double ppx = 0.0;
    double ppy = 0.0;
    double fx = 0.0;
    double fy = 0.0;
    int model = 0;
    std::vector<double> coeffs;

    bool IsValid() const noexcept {
        return width > 0 && height > 0 && fx > 0.0 && fy > 0.0;
    }
};

struct ImageBuffer {
    int width = 0;
    int height = 0;
    int mat_type = 0;
    std::vector<std::uint8_t> data;

    bool Empty() const noexcept {
        return width <= 0 || height <= 0 || data.empty();
    }
};

struct FrameSnapshot {
    std::string ip;
    ImageBuffer color;
    ImageBuffer depth;
    CameraIntrinsicsData intrinsics;
    float depth_scale = 1.0f;
    std::int64_t ticks = 0;
};

struct ServiceConfig {
    std::string service_name = "camera_service";
    std::string listen_address = "0.0.0.0:5111";
    std::string shared_memory_prefix = "/dev/shm/ImageMemoryShareData";
    std::string camera_ip = "169.254.0.10";
    std::string camera_sn;
    std::string sdk_root;
    int frame_width = 640;
    int frame_height = 480;
    int frame_period_ms = 33;
    int grab_timeout_ms = 300;  // 300ms grab + ~50ms write + network < 500ms gRPC deadline
    float depth_scale = 1.0f;
    bool synthetic_camera = false;
};

}  // namespace lbgm461
