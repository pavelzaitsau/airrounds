#pragma once
#include <cstddef>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
#define APP_DATA_PATH(path) "/data/" path
enum { FSAM_READ, FSAM_WRITE, FSOM_OPEN_EXISTING, FSOM_CREATE_ALWAYS };
enum FS_Error { FSE_OK, FSE_NOT_READY, FSE_NOT_EXIST, FSE_INTERNAL };
struct Storage {
    std::map<std::string, std::vector<unsigned char>> files;
    // Opening this path for reading fails with an internal error.
    std::string unreadablePath;
    // Writes store one byte less than requested.
    bool shortWrite = false;
    bool failSync = false;
    bool failRemoval = false;
    // Every open for reading fails with FSE_NOT_READY, as when the card is missing.
    bool failRead = false;
    // Reads return one byte less than requested.
    bool shortRead = false;
    // storage_file_size fails. The SD backend in 1.5.1 cannot fail here (storage_ext.c,
    // storage_ext_file_size), so this checks only the generic Storage API contract.
    bool failSize = false;
    // Every close fails.
    bool failClose = false;
    // Opening a file for writing fails and leaves any old file in place.
    bool failWriteOpen = false;
    // Closing a written file fails to flush it.
    bool failWriteClose = false;
    // A slow card: each sync blocks the caller this long on the app's clock.
    unsigned syncMs = 0;
    unsigned* clockMs = nullptr;
    // The card stores other bytes than the app wrote, as a failing card can.
    std::function<void(std::vector<unsigned char>&)> damageWrite;
};
// Files support single-call I/O only: a write replaces the whole file and a read starts at byte 0.
// That matches the app, which writes and reads each record in one call.
struct File {
    Storage* store;
    std::string path;
    FS_Error error;
    bool opened;
    bool writing;
};
inline File* storage_file_alloc(Storage* store) {
    return new File{store, "", FSE_OK, false, false};
}
inline void storage_file_free(File* file) {
    delete file;
}
inline bool storage_file_open(File* f, const char* path, int, int flags) {
    f->path = path;
    f->writing = flags == FSOM_CREATE_ALWAYS;
    if (f->writing) {
        if (f->store->failWriteOpen) {
            f->error = FSE_INTERNAL;
            return false;
        }
        f->store->files[path].clear();
    } else if (f->store->failRead) {
        f->error = FSE_NOT_READY;
        return false;
    } else if (f->store->unreadablePath == path) {
        f->error = FSE_INTERNAL;
        return false;
    } else if (f->store->files.count(path) == 0) {
        f->error = FSE_NOT_EXIST;
        return false;
    }
    f->opened = true;
    f->error = FSE_OK;
    return true;
}
inline bool storage_file_close(File* f) {
    const bool wasOpen = f->opened;
    f->opened = false;
    return wasOpen && !f->store->failClose && !(f->writing && f->store->failWriteClose);
}
inline FS_Error storage_file_get_error(File* f) {
    return f->error;
}
// The SDK refuses file operations on a file that is not open.
inline bool storage_file_sync(File* f) {
    if (f->store->clockMs != nullptr) {
        *f->store->clockMs += f->store->syncMs;
    }
    return f->opened && !f->store->failSync;
}
inline std::size_t storage_file_size(File* f) {
    if (!f->opened || f->store->failSize) {
        f->error = FSE_INTERNAL;
        return 0;
    }
    f->error = FSE_OK;
    return f->store->files.at(f->path).size();
}
inline std::size_t storage_file_write(File* f, const unsigned char* b, std::size_t n) {
    if (!f->opened || !f->writing) {
        return 0;
    }
    n -= f->store->shortWrite ? 1U : 0U;
    auto& stored = f->store->files.at(f->path);
    stored.assign(b, b + n);
    if (f->store->damageWrite) {
        f->store->damageWrite(stored);
    }
    return n;
}
inline std::size_t storage_file_read(File* f, unsigned char* b, std::size_t n) {
    if (!f->opened || f->writing) {
        return 0;
    }
    const auto& stored = f->store->files.at(f->path);
    n = n < stored.size() ? n : stored.size();
    n -= f->store->shortRead && n != 0 ? 1U : 0U;
    std::memcpy(b, stored.data(), n);
    return n;
}
inline FS_Error storage_common_remove(Storage* s, const char* path) {
    if (s->failRemoval) {
        return FSE_INTERNAL;
    }
    return s->files.erase(path) == 0 ? FSE_NOT_EXIST : FSE_OK;
}
