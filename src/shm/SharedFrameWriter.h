#pragma once

#include "../common/FrameTypes.h"

#include <mutex>
#include <string>

namespace lbgm461 {

class SharedFrameWriter {
public:
    explicit SharedFrameWriter(std::string prefix = "/dev/shm/ImageMemoryShareData");

    bool Write(const FrameSnapshot& snapshot, const std::string& suffix, std::string& out_path);
    std::string LastError() const;

private:
    std::string ComposePath(const std::string& suffix) const;
    void SetError(const std::string& message);

    std::string prefix_;
    mutable std::mutex mutex_;
    std::string last_error_;
};

}  // namespace lbgm461
