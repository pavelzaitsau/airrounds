#pragma once

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace airrounds {
namespace storage {

enum class FilePresence { missing, present, error };

class SdFile {
public:
    SdFile() = default;
    SdFile(const SdFile&) = delete;
    SdFile& operator=(const SdFile&) = delete;

    ~SdFile() { close(); }

    bool openNew(const char* path) { return open(path, O_WRONLY | O_CREAT | O_EXCL); }

    bool openRead(const char* path) {
        if (!open(path, O_RDONLY | O_NONBLOCK)) {
            return false;
        }
        struct stat info;
        if (::fstat(descriptor_, &info) == 0 && S_ISREG(info.st_mode)) {
            return true;
        }
        close();
        return false;
    }

    ssize_t read(uint8_t* data, size_t size) { return ::read(descriptor_, data, size); }

    bool sync() { return ::fsync(descriptor_) == 0; }

    bool close() {
        if (descriptor_ < 0) {
            return true;
        }
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return ::close(descriptor) == 0;
    }

    static FilePresence probe(const char* path) {
        struct stat info;
        if (::stat(path, &info) != 0) {
            return errno == ENOENT ? FilePresence::missing : FilePresence::error;
        }
        return S_ISREG(info.st_mode) ? FilePresence::present : FilePresence::error;
    }

    static bool publish(const char* from, const char* to) {
        return probe(to) == FilePresence::missing && ::rename(from, to) == 0;
    }

    static void remove(const char* path) { ::unlink(path); }

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
    bool open(const char* path, int flags) {
        if (descriptor_ >= 0) {
            lastError_ = EBUSY;
            return false;
        }
        if (path == nullptr || *path == '\0') {
            lastError_ = EINVAL;
            return false;
        }
        descriptor_ = ::open(path, flags, 0600);
        lastError_ = descriptor_ < 0 ? errno : 0;
        return descriptor_ >= 0;
    }

    int descriptor_ = -1;
    int lastError_ = 0;
};

} // namespace storage
} // namespace airrounds
