#include "SharedFrameWriter.h"

#include "../common/ProtobufWire.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

namespace lbgm461 {

SharedFrameWriter::SharedFrameWriter(std::string prefix)
    : prefix_(std::move(prefix)) {
}

bool SharedFrameWriter::Write(const FrameSnapshot& snapshot, const std::string& suffix, std::string& out_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string path = ComposePath(suffix);
    const std::vector<std::uint8_t> payload = wire::EncodeShareData(snapshot);

    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        SetError("payload is too large");
        return false;
    }

    std::filesystem::path fs_path(path);
    std::error_code ec;
    std::filesystem::create_directories(fs_path.parent_path(), ec);

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        SetError("failed to open shared memory file");
        return false;
    }

    const std::int32_t length = static_cast<std::int32_t>(payload.size());
    stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
    if (!payload.empty()) {
        stream.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    }

    if (!stream.good()) {
        SetError("failed to write shared memory payload");
        return false;
    }

    out_path = path;
    SetError("");
    return true;
}

std::string SharedFrameWriter::LastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::string SharedFrameWriter::ComposePath(const std::string& suffix) const {
    if (suffix.empty()) {
        return prefix_;
    }

    if (suffix.front() == '_') {
        return prefix_ + suffix;
    }

    return prefix_ + '_' + suffix;
}

void SharedFrameWriter::SetError(const std::string& message) {
    last_error_ = message;
}

}  // namespace lbgm461
