#pragma once

#include "../camera/PercipioCamera.h"
#include "../shm/SharedFrameWriter.h"

#include <atomic>
#include <mutex>
#include <string>

namespace lbgm461 {

struct GetYoloResultRequest {
    std::string path_suffix;
    bool is_only_images = true;
};

struct GetYoloResultResponse {
    std::string status;
    std::string path;
    std::int64_t ticks = 0;
};

class CameraService {
public:
    explicit CameraService(ServiceConfig config);

    bool Initialize();
    bool InitModels(const std::string& model_name);
    GetYoloResultResponse GetYoloResult(const GetYoloResultRequest& request);
    void Shutdown();

    std::string LastError() const;

private:
    std::int64_t NextTicks(std::int64_t candidate);
    void SetError(const std::string& message);

    ServiceConfig config_;
    PercipioCamera camera_;
    SharedFrameWriter writer_;
    std::atomic<std::int64_t> ticks_{0};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

}  // namespace lbgm461
