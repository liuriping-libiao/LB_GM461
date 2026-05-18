#pragma once

#include "CameraService.h"

#include <memory>
#include <string>

namespace lbgm461 {

class GrpcServer {
public:
    explicit GrpcServer(CameraService& camera_service);
    ~GrpcServer();

    bool Start(const std::string& listen_address);
    void Stop();
    bool IsRunning() const noexcept;
    std::string LastError() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lbgm461
