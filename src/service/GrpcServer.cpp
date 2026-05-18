#include "GrpcServer.h"

#include <iostream>
#include <mutex>
#include <utility>

#ifdef LBGM461_HAS_GRPC
#include "CaptureMachine.grpc.pb.h"

#include <grpcpp/grpcpp.h>

namespace {

class ImageServiceImpl final : public MyGrpcService::ImageService::Service {
public:
    explicit ImageServiceImpl(lbgm461::CameraService& service)
        : service_(service) {
    }

    grpc::Status ClientTest(
        grpc::ServerContext* /*context*/,
        const MyGrpcService::SendRequest* /*request*/,
        MyGrpcService::SendResponse* response) override {
        response->set_status("successful");
        return grpc::Status::OK;
    }

    grpc::Status InitModels(
        grpc::ServerContext* /*context*/,
        const MyGrpcService::ProtoModelInfos* request,
        MyGrpcService::SendResponse* response) override {
        std::string model_name;
        if (request != nullptr && request->models_size() > 0) {
            model_name = request->models(0).name();
        }
        const bool ok = service_.InitModels(model_name);
        response->set_status(ok ? "successful" : "failed");
        return grpc::Status::OK;
    }

    grpc::Status GetYoloResult(
        grpc::ServerContext* /*context*/,
        const MyGrpcService::ProtoYoloRequest* request,
        MyGrpcService::ProtoYoloResponse* response) override {
        try {
            lbgm461::GetYoloResultRequest local_request;
            if (request != nullptr) {
                local_request.path_suffix = request->path_suffix();
                local_request.is_only_images = request->is_only_images();
            }

            const lbgm461::GetYoloResultResponse local_response = service_.GetYoloResult(local_request);
            response->set_status(local_response.status);
            response->set_path(local_response.path);
            response->set_ticks(local_response.ticks);
            return grpc::Status::OK;
        } catch (const std::exception& ex) {
            std::cerr << "[GetYoloResult] exception: " << ex.what() << std::endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, std::string("exception: ") + ex.what());
        } catch (...) {
            std::cerr << "[GetYoloResult] unknown exception" << std::endl;
            return grpc::Status(grpc::StatusCode::INTERNAL, "unknown exception");
        }
    }

private:
    lbgm461::CameraService& service_;
};

}  // namespace

#endif

namespace lbgm461 {

class GrpcServer::Impl {
public:
    explicit Impl(CameraService& camera_service)
        : camera_service_(camera_service) {
    }

    bool Start(const std::string& listen_address) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return true;
        }

#ifdef LBGM461_HAS_GRPC
        service_impl_ = std::make_unique<ImageServiceImpl>(camera_service_);

        grpc::ServerBuilder builder;
        builder.AddListeningPort(listen_address, grpc::InsecureServerCredentials());
        builder.RegisterService(service_impl_.get());
        server_ = builder.BuildAndStart();
        if (!server_) {
            last_error_ = "failed to start gRPC server";
            service_impl_.reset();
            return false;
        }

        running_ = true;
        last_error_.clear();
        return true;
#else
        (void)listen_address;
        last_error_ = "gRPC dependencies are not enabled in this build";
        return false;
#endif
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return;
        }

#ifdef LBGM461_HAS_GRPC
        if (server_) {
            server_->Shutdown();
            server_.reset();
        }
        service_impl_.reset();
#endif

        running_ = false;
    }

    bool IsRunning() const noexcept {
        return running_;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

private:
    CameraService& camera_service_;
    mutable std::mutex mutex_;
    bool running_ = false;
    std::string last_error_;

#ifdef LBGM461_HAS_GRPC
    std::unique_ptr<ImageServiceImpl> service_impl_;
    std::unique_ptr<grpc::Server> server_;
#endif
};

GrpcServer::GrpcServer(CameraService& camera_service)
    : impl_(std::make_unique<Impl>(camera_service)) {
}

GrpcServer::~GrpcServer() {
    Stop();
}

bool GrpcServer::Start(const std::string& listen_address) {
    return impl_->Start(listen_address);
}

void GrpcServer::Stop() {
    impl_->Stop();
}

bool GrpcServer::IsRunning() const noexcept {
    return impl_->IsRunning();
}

std::string GrpcServer::LastError() const {
    return impl_->LastError();
}

}  // namespace lbgm461
