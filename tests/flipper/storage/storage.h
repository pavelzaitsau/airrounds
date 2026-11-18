#pragma once
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#define APP_DATA_PATH(path) "/data/" path
enum { FSAM_READ, FSAM_WRITE, FSOM_OPEN_EXISTING, FSOM_CREATE_ALWAYS };
enum FS_Error { FSE_OK, FSE_NOT_EXIST, FSE_INTERNAL };
struct Storage {
    std::map<std::string, std::vector<unsigned char>> files;
    std::string unreadablePath;
    bool shortWrite = false;
    bool failSync = false;
    bool failRemoval = false;
    bool failRead = false;
    bool shortRead = false;
    bool failSize = false;
    bool failClose = false;
};
struct File {
    Storage* store;
    std::string path;
    FS_Error error;
    bool opened;
};
inline File* storage_file_alloc(Storage* store) {
    return new File{store, "", FSE_OK, false};
}
inline void storage_file_free(File* file) {
    delete file;
}
inline bool storage_file_open(File* f, const char* path, int, int flags) {
    f->path = path;
    if (flags == FSOM_CREATE_ALWAYS) {
        f->store->files[path].clear();
    } else if (f->store->failRead || f->store->unreadablePath == path) {
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
    return wasOpen && !f->store->failClose;
}
inline FS_Error storage_file_get_error(File* f) {
    return f->error;
}
inline bool storage_file_sync(File* f) {
    return !f->store->failSync;
}
inline std::size_t storage_file_size(File* f) {
    if (f->store->failSize) {
        f->error = FSE_INTERNAL;
        return 0;
    }
    f->error = FSE_OK;
    return f->store->files.at(f->path).size();
}
inline std::size_t storage_file_write(File* f, const unsigned char* b, std::size_t n) {
    n -= f->store->shortWrite ? 1U : 0U;
    f->store->files.at(f->path).assign(b, b + n);
    return n;
}
inline std::size_t storage_file_read(File* f, unsigned char* b, std::size_t n) {
    n -= f->store->shortRead ? 1U : 0U;
    std::memcpy(b, f->store->files.at(f->path).data(), n);
    return n;
}
inline FS_Error storage_common_remove(Storage* s, const char* path) {
    if (s->failRemoval) {
        return FSE_INTERNAL;
    }
    return s->files.erase(path) == 0 ? FSE_NOT_EXIST : FSE_OK;
}
