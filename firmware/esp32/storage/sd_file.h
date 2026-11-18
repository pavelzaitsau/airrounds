#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

namespace airrounds {
namespace storage {

class SdFile {
public:
    SdFile() = default;
    SdFile(const SdFile&) = delete;
    SdFile& operator=(const SdFile&) = delete;

    ~SdFile() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    bool openNew(const char* path) {
        if (descriptor_ >= 0) {
            lastError_ = EBUSY;
            return false;
        }
        if (path == nullptr || *path == '\0') {
            lastError_ = EINVAL;
            return false;
        }

        descriptor_ = ::open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        lastError_ = descriptor_ < 0 ? errno : 0;
        return descriptor_ >= 0;
    }

    explicit operator bool() const { return descriptor_ >= 0; }
    int lastError() const { return lastError_; }

    size_t write(const uint8_t* data, size_t size) {
#if defined(ARDUINO_ARCH_ESP32)
        // ESP-IDF FAT can return bytes written and set errno in the same call.
        errno = 0;
#endif
        const ssize_t written = ::write(descriptor_, data, size);
#if defined(ARDUINO_ARCH_ESP32)
        lastError_ = errno;
#else
        lastError_ = written < 0 ? errno : 0;
#endif
        return written < 0 ? 0 : static_cast<size_t>(written);
    }

private:
    int descriptor_ = -1;
    int lastError_ = 0;
};

}  // namespace storage
}  // namespace airrounds
