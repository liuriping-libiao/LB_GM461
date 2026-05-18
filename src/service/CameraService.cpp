#include "CameraService.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace lbgm461 {

CameraService::CameraService(ServiceConfig config)
    : config_(std::move(config)),
      camera_(config_),
      writer_(config_.shared_memory_prefix) {
}

bool CameraService::Initialize() {
    if (!camera_.Init()) {
        SetError(camera_.LastError());
        return false;
    }

    if (!camera_.Connect()) {
        SetError(camera_.LastError());
        return false;
    }

    if (!camera_.Start()) {
        SetError(camera_.LastError());
        return false;
    }

    SetError("");
    return true;
}

bool CameraService::InitModels(const std::string& model_name) {
    (void)model_name;
    return true;
}

GetYoloResultResponse CameraService::GetYoloResult(const GetYoloResultRequest& request) {
    GetYoloResultResponse response;

    FrameSnapshot snapshot;
    if (!camera_.Grab(snapshot, config_.grab_timeout_ms)) {
        response.status = "failed_camera_timeout";
        SetError(camera_.LastError());
        return response;
    }

    snapshot.ticks = NextTicks(snapshot.ticks);

    std::string path;
    if (!writer_.Write(snapshot, request.path_suffix, path)) {
        response.status = "failed_shm_write";
        SetError(writer_.LastError());
        return response;
    }

    response.status = "successful";
    response.path = path;
    response.ticks = snapshot.ticks;
    SetError("");
    return response;
}

bool CameraService::GrabFrame(FrameSnapshot& out) {
    if (!camera_.Grab(out, config_.grab_timeout_ms)) {
        SetError(camera_.LastError());
        return false;
    }
    return true;
}

void CameraService::Shutdown() {
    camera_.Stop();
}

std::string CameraService::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

std::int64_t CameraService::NextTicks(std::int64_t candidate) {
    const std::int64_t current = ticks_.load();
    const std::int64_t next = std::max(current + 1, candidate);
    ticks_.store(next);
    return next;
}

void CameraService::SetError(const std::string& message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = message;
}

}  // namespace lbgm461
