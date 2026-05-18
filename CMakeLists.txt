cmake_minimum_required(VERSION 3.20)

project(LBGM461 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LBGM461_ENABLE_SYNTHETIC_CAMERA "Use synthetic frames when the SDK is not linked yet" ON)
option(LBGM461_ENABLE_GRPC "Build gRPC server when protobuf/gRPC dependencies are available" ON)
option(LBGM461_ENABLE_PERCIPIO_SDK "Enable Percipio SDK integration" OFF)

set(LBGM461_PERCIPIO_SDK_ROOT "" CACHE PATH "Percipio SDK root path")

set(LBGM461_GRPC_ENABLED OFF)

if (LBGM461_ENABLE_GRPC)
    find_package(Protobuf CONFIG QUIET)
    find_package(gRPC CONFIG QUIET)

    if (Protobuf_FOUND AND gRPC_FOUND)
        set(LBGM461_GRPC_ENABLED ON)
        set(_GRPC_USE_TARGETS ON)
    else()
        # Fallback: use pkg-config (system packages on Ubuntu/Debian)
        find_package(PkgConfig QUIET)
        if (PkgConfig_FOUND)
            pkg_check_modules(GRPCPP QUIET grpc++)
            pkg_check_modules(PROTOBUF QUIET protobuf)
        endif()
        find_program(_PROTOC protoc)
        find_program(_GRPC_CPP_PLUGIN grpc_cpp_plugin)
        if (GRPCPP_FOUND AND PROTOBUF_FOUND AND _PROTOC AND _GRPC_CPP_PLUGIN)
            set(LBGM461_GRPC_ENABLED ON)
            set(_GRPC_USE_TARGETS OFF)
            message(STATUS "Using system gRPC via pkg-config (protoc=${_PROTOC}, plugin=${_GRPC_CPP_PLUGIN})")
        endif()
    endif()

    if (LBGM461_GRPC_ENABLED)
        set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/CaptureMachine.proto)
        get_filename_component(PROTO_DIR ${PROTO_FILE} DIRECTORY)
        set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
        file(MAKE_DIRECTORY ${GENERATED_DIR})

        get_filename_component(PROTO_BASENAME ${PROTO_FILE} NAME_WE)
        set(PROTO_SRCS ${GENERATED_DIR}/${PROTO_BASENAME}.pb.cc)
        set(PROTO_HDRS ${GENERATED_DIR}/${PROTO_BASENAME}.pb.h)
        set(GRPC_SRCS ${GENERATED_DIR}/${PROTO_BASENAME}.grpc.pb.cc)
        set(GRPC_HDRS ${GENERATED_DIR}/${PROTO_BASENAME}.grpc.pb.h)

        if (_GRPC_USE_TARGETS)
            add_custom_command(
                OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
                COMMAND protobuf::protoc
                ARGS
                    --proto_path=${PROTO_DIR}
                    --cpp_out=${GENERATED_DIR}
                    --grpc_out=${GENERATED_DIR}
                    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
                    ${PROTO_FILE}
                DEPENDS ${PROTO_FILE}
                COMMENT "Generating protobuf and gRPC sources from ${PROTO_FILE}"
                VERBATIM
            )
        else()
            add_custom_command(
                OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
                COMMAND ${_PROTOC}
                ARGS
                    --proto_path=${PROTO_DIR}
                    --cpp_out=${GENERATED_DIR}
                    --grpc_out=${GENERATED_DIR}
                    --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
                    ${PROTO_FILE}
                DEPENDS ${PROTO_FILE}
                COMMENT "Generating protobuf and gRPC sources from ${PROTO_FILE}"
                VERBATIM
            )
        endif()

        add_library(capturemachine_proto STATIC
            ${PROTO_SRCS}
            ${GRPC_SRCS}
        )

        if (_GRPC_USE_TARGETS)
            target_link_libraries(capturemachine_proto
                PUBLIC
                    protobuf::libprotobuf
                    gRPC::grpc++
            )
        else()
            target_include_directories(capturemachine_proto PUBLIC ${PROTOBUF_INCLUDE_DIRS} ${GRPCPP_INCLUDE_DIRS})
            target_link_libraries(capturemachine_proto PUBLIC ${PROTOBUF_LIBRARIES} ${GRPCPP_LIBRARIES})
        endif()

        target_include_directories(capturemachine_proto
            PUBLIC
                ${GENERATED_DIR}
        )
    else()
        message(WARNING "LBGM461_ENABLE_GRPC is ON, but protobuf/gRPC dependencies were not found. Build will continue without network gRPC server.")
    endif()
endif()

add_executable(camera_service
    src/main.cpp
    src/camera/PercipioCamera.cpp
    src/service/CameraService.cpp
    src/service/GrpcServer.cpp
    src/shm/SharedFrameWriter.cpp
)

# glibc 2.35 兼容层 (目标设备 Ubuntu 22.04 缺少 arc4random_buf)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(camera_service PRIVATE src/common/glibc_compat.c)
endif()

target_include_directories(camera_service PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (LBGM461_GRPC_ENABLED)
    target_compile_definitions(camera_service PRIVATE LBGM461_HAS_GRPC=1)
    target_link_libraries(camera_service PRIVATE capturemachine_proto)
endif()

if (MSVC)
    target_compile_options(camera_service PRIVATE /W4 /permissive-)
else()
    target_compile_options(camera_service PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (LBGM461_ENABLE_SYNTHETIC_CAMERA)
    target_compile_definitions(camera_service PRIVATE LBGM461_ENABLE_SYNTHETIC_CAMERA=1)
endif()

if (LBGM461_ENABLE_PERCIPIO_SDK)
    target_compile_definitions(camera_service PRIVATE LBGM461_ENABLE_PERCIPIO_SDK=1)

    # Default SDK root to project directory (in-tree headers + libs)
    if (NOT LBGM461_PERCIPIO_SDK_ROOT)
        set(LBGM461_PERCIPIO_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    target_include_directories(camera_service PRIVATE ${LBGM461_PERCIPIO_SDK_ROOT}/include)

    if (WIN32)
        set(_SDK_LIB_DIR "${LBGM461_PERCIPIO_SDK_ROOT}/lib/win/x64")
        set(_SDK_BIN_DIR "${LBGM461_PERCIPIO_SDK_ROOT}/lib/win/x64")
    else()
        # Detect architecture for Linux
        if (CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64")
            set(_SDK_LIB_DIR "${LBGM461_PERCIPIO_SDK_ROOT}/lib/linux/aarch64")
        else()
            set(_SDK_LIB_DIR "${LBGM461_PERCIPIO_SDK_ROOT}/lib/linux/x64")
        endif()
    endif()

    target_link_directories(camera_service PRIVATE ${_SDK_LIB_DIR})
    target_link_libraries(camera_service PRIVATE tycam tyimgproc)

    if (WIN32)
        add_custom_command(TARGET camera_service POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_SDK_BIN_DIR}/tycam.dll"
                "${_SDK_BIN_DIR}/tyimgproc.dll"
                $<TARGET_FILE_DIR:camera_service>
            COMMENT "Copying Camport4 SDK DLLs"
        )
    endif()

    # Linux: set RPATH so the executable can find .so files
    if (UNIX)
        set_target_properties(camera_service PROPERTIES
            INSTALL_RPATH "$ORIGIN:$ORIGIN/../lib"
            BUILD_WITH_INSTALL_RPATH TRUE
        )
    endif()
endif()
