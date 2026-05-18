#include "common/FrameTypes.h"
#include "service/CameraService.h"
#include "service/GrpcServer.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

lbgm461::ServiceConfig ParseArguments(int argc, char** argv) {
    lbgm461::ServiceConfig config;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto consume_value = [&](std::string& target) {
            if (index + 1 < argc) {
                target = argv[++index];
            }
        };

        if (argument == "--listen" || argument == "-l") {
            consume_value(config.listen_address);
        } else if (argument == "--camera-ip") {
            consume_value(config.camera_ip);
        } else if (argument == "--camera-sn") {
            consume_value(config.camera_sn);
        } else if (argument == "--sdk-root") {
            consume_value(config.sdk_root);
        } else if (argument == "--shared-prefix") {
            consume_value(config.shared_memory_prefix);
        } else if (argument == "--width") {
            if (index + 1 < argc) {
                config.frame_width = std::stoi(argv[++index]);
            }
        } else if (argument == "--height") {
            if (index + 1 < argc) {
                config.frame_height = std::stoi(argv[++index]);
            }
        } else if (argument == "--period-ms") {
            if (index + 1 < argc) {
                config.frame_period_ms = std::stoi(argv[++index]);
            }
        } else if (argument == "--grab-timeout-ms") {
            if (index + 1 < argc) {
                config.grab_timeout_ms = std::stoi(argv[++index]);
            }
        } else if (argument == "--depth-scale") {
            if (index + 1 < argc) {
                config.depth_scale = std::stof(argv[++index]);
            }
        } else if (argument == "--synthetic") {
            config.synthetic_camera = true;
        } else if (argument == "--no-synthetic") {
            config.synthetic_camera = false;
        }
    }

    return config;
}

void PrintConfig(const lbgm461::ServiceConfig& config) {
    std::cout << "service: " << config.service_name << '\n';
    std::cout << "listen: " << config.listen_address << '\n';
    std::cout << "camera ip: " << config.camera_ip << '\n';
    std::cout << "camera sn: " << config.camera_sn << '\n';
    std::cout << "sdk root: " << config.sdk_root << '\n';
    std::cout << "shared prefix: " << config.shared_memory_prefix << '\n';
    std::cout << "frame: " << config.frame_width << 'x' << config.frame_height << '\n';
    std::cout << "period(ms): " << config.frame_period_ms << '\n';
    std::cout << "grab timeout(ms): " << config.grab_timeout_ms << '\n';
    std::cout << "depth scale: " << config.depth_scale << '\n';
    std::cout << "synthetic camera: " << (config.synthetic_camera ? "true" : "false") << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    const lbgm461::ServiceConfig config = ParseArguments(argc, argv);
    PrintConfig(config);

    lbgm461::CameraService service(config);
    if (!service.Initialize()) {
        std::cerr << "failed to initialize service: " << service.LastError() << '\n';
        return 1;
    }

    lbgm461::GrpcServer grpc_server(service);
    if (grpc_server.Start(config.listen_address)) {
        std::cout << "gRPC server started at: " << config.listen_address << '\n';
        std::cout << "service is running; press Ctrl+C to stop." << '\n';
    } else {
        std::cout << "gRPC server is not available in this build: " << grpc_server.LastError() << '\n';
        std::cout << "fallback to local smoke mode." << '\n';

        const lbgm461::GetYoloResultRequest smoke_request{"_smoke", true};
        const lbgm461::GetYoloResultResponse smoke_response = service.GetYoloResult(smoke_request);
        if (smoke_response.status != "successful") {
            std::cerr << "smoke request failed: " << service.LastError() << '\n';
            service.Shutdown();
            return 2;
        }

        std::cout << "smoke frame path: " << smoke_response.path << '\n';
        std::cout << "smoke ticks: " << smoke_response.ticks << '\n';
        std::cout << "service is running; press Ctrl+C to stop." << '\n';
    }

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}
