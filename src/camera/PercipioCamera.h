#pragma once

#include "../common/FrameTypes.h"

#include <memory>

namespace lbgm461 {

class PercipioCamera {
public:
    explicit PercipioCamera(ServiceConfig config);
    ~PercipioCamera();

    bool Init();
    bool Connect();
    bool Start();
    bool Stop();
    bool Grab(FrameSnapshot& out, int timeout_ms);
    CameraIntrinsicsData GetIntrinsics() const;

    bool IsConnected() const noexcept;
    std::string LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lbgm461
