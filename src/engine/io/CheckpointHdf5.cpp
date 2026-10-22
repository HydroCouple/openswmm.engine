// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "CheckpointHdf5.hpp"
#include "Hdf5ObjectCreation.hpp"
#include "Hdf5Mutex.hpp"
#include "../core/FileIO.hpp"
#include <H5FDdevelop.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <cstdint>
#include <iterator>
#include <new>
#include <tuple>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/file.h>
#include <unistd.h>
#endif

namespace openswmm::io {
namespace {
struct File {
    H5FD_t base{};
    int fd = -1;
    haddr_t eoa = 0, eof = 0;
    uint64_t volume = 0, index = 0;
};
constexpr haddr_t maxAddress = INT64_MAX;
#ifdef _WIN32
int closeFd(int fd) {
    return _close(fd);
}
int64_t seekFd(int fd, uint64_t at) {
    return _lseeki64(fd, at, SEEK_SET);
}
#else
int closeFd(int fd) {
    return close(fd);
}
int64_t seekFd(int fd, uint64_t at) {
    return lseek(fd, at, SEEK_SET);
}
#endif
H5FD_t* openFile(const char* name, unsigned flags, hid_t, haddr_t limit) {
    if (!name || !*name || !limit || limit > maxAddress)
        return nullptr;
    auto* f = new (std::nothrow) File;
    if (!f)
        return nullptr;
#ifdef _WIN32
    HANDLE h = INVALID_HANDLE_VALUE;
    try {
        const DWORD create = flags & H5F_ACC_EXCL    ? CREATE_NEW
                             : flags & H5F_ACC_TRUNC ? CREATE_ALWAYS
                             : flags & H5F_ACC_CREAT ? OPEN_ALWAYS
                                                     : OPEN_EXISTING;
        h = CreateFileW(utf8_path(name).c_str(),
                        GENERIC_READ | (flags & H5F_ACC_RDWR ? GENERIC_WRITE : 0),
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, create,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    } catch (...) {
        delete f;
        return nullptr;
    }
    if (h != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};
        LARGE_INTEGER size{};
        if (GetFileInformationByHandle(h, &info) && GetFileSizeEx(h, &size) && size.QuadPart >= 0) {
            f->volume = info.dwVolumeSerialNumber;
            f->index = (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
            f->eof = size.QuadPart;
            f->fd = _open_osfhandle(reinterpret_cast<intptr_t>(h),
                                    _O_BINARY | (flags & H5F_ACC_RDWR ? _O_RDWR : _O_RDONLY));
        }
        if (f->fd < 0)
            CloseHandle(h);
    }
#else
    const int mode = (flags & H5F_ACC_RDWR ? O_RDWR : O_RDONLY) |
                     (flags & H5F_ACC_CREAT ? O_CREAT : 0) | (flags & H5F_ACC_TRUNC ? O_TRUNC : 0) |
                     (flags & H5F_ACC_EXCL ? O_EXCL : 0);
    f->fd = ::open(name, mode, 0666);
    struct stat info{};
    if (f->fd >= 0 && fstat(f->fd, &info) == 0 && info.st_size >= 0) {
        f->volume = info.st_dev;
        f->index = info.st_ino;
        f->eof = info.st_size;
    } else if (f->fd >= 0) {
        closeFd(f->fd);
        f->fd = -1;
    }
#endif
    if (f->fd < 0 || f->eof > maxAddress) {
        if (f->fd >= 0)
            closeFd(f->fd);
        delete f;
        return nullptr;
    }
    return &f->base;
}
herr_t closeFile(H5FD_t* base) {
    auto* f = reinterpret_cast<File*>(base);
    if (closeFd(f->fd) < 0)
        return -1;
    delete f;
    return 0;
}
int compare(const H5FD_t* a, const H5FD_t* b) {
    const auto* x = reinterpret_cast<const File*>(a);
    const auto* y = reinterpret_cast<const File*>(b);
    const auto p = std::tie(x->volume, x->index), q = std::tie(y->volume, y->index);
    return p < q ? -1 : p > q ? 1 : 0;
}
herr_t query(const H5FD_t*, unsigned long* flags) {
    if (flags)
        *flags = H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_ACCUMULATE_METADATA |
                 H5FD_FEAT_DATA_SIEVE | H5FD_FEAT_AGGREGATE_SMALLDATA |
                 H5FD_FEAT_POSIX_COMPAT_HANDLE | H5FD_FEAT_DEFAULT_VFD_COMPATIBLE;
    return 0;
}
haddr_t getEoa(const H5FD_t* f, H5FD_mem_t) {
    return reinterpret_cast<const File*>(f)->eoa;
}
herr_t setEoa(H5FD_t* f, H5FD_mem_t, haddr_t eoa) {
    if (eoa > maxAddress)
        return -1;
    reinterpret_cast<File*>(f)->eoa = eoa;
    return 0;
}
haddr_t getEof(const H5FD_t* f, H5FD_mem_t) {
    const auto* file = reinterpret_cast<const File*>(f);
    return std::max(file->eof, file->eoa);
}
herr_t getHandle(H5FD_t* f, hid_t, void** out) {
    if (!out)
        return -1;
    *out = &reinterpret_cast<File*>(f)->fd;
    return 0;
}
herr_t readFile(H5FD_t* base, H5FD_mem_t, hid_t, haddr_t at, size_t size, void* data) {
    auto* f = reinterpret_cast<File*>(base);
    if (at > f->eoa || size > f->eoa - at || seekFd(f->fd, at) < 0)
        return -1;
    auto* out = static_cast<char*>(data);
    while (size) {
        const auto n = unsigned(std::min(size, size_t(INT_MAX)));
#ifdef _WIN32
        const auto got = _read(f->fd, out, n);
#else
        const auto got = ::read(f->fd, out, n);
#endif
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (!got) {
            std::memset(out, 0, size);
            break;
        }
        out += got;
        size -= got;
    }
    return 0;
}
herr_t writeFile(H5FD_t* base, H5FD_mem_t, hid_t, haddr_t at, size_t size, const void* data) {
    auto* f = reinterpret_cast<File*>(base);
    if (at > f->eoa || size > f->eoa - at || seekFd(f->fd, at) < 0)
        return -1;
    const auto* in = static_cast<const char*>(data);
    const auto end = at + size;
    while (size) {
        const auto n = unsigned(std::min(size, size_t(INT_MAX)));
#ifdef _WIN32
        const auto written = _write(f->fd, in, n);
#else
        const auto written = ::write(f->fd, in, n);
#endif
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        in += written;
        size -= written;
    }
    f->eof = std::max(f->eof, end);
    return 0;
}
herr_t truncateFile(H5FD_t* base, hid_t, bool) {
    auto* f = reinterpret_cast<File*>(base);
    if (f->eoa == f->eof)
        return 0;
#ifdef _WIN32
    const auto rc = _chsize_s(f->fd, f->eoa);
#else
    const auto rc = ftruncate(f->fd, f->eoa);
#endif
    if (rc)
        return -1;
    f->eof = f->eoa;
    return 0;
}
herr_t lockFile(H5FD_t* base, bool write) {
    const auto fd = reinterpret_cast<File*>(base)->fd;
#ifdef _WIN32
    OVERLAPPED offset{};
    return LockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(fd)),
                      LOCKFILE_FAIL_IMMEDIATELY | (write ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0,
                      MAXDWORD, MAXDWORD, &offset)
               ? 0
               : -1;
#else
    return flock(fd, (write ? LOCK_EX : LOCK_SH) | LOCK_NB);
#endif
}
herr_t unlockFile(H5FD_t* base) {
    const auto fd = reinterpret_cast<File*>(base)->fd;
#ifdef _WIN32
    OVERLAPPED offset{};
    return UnlockFileEx(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), 0, MAXDWORD, MAXDWORD,
                        &offset)
               ? 0
               : -1;
#else
    return flock(fd, LOCK_UN);
#endif
}
hid_t driver() {
    static hid_t id = H5I_INVALID_HID;
    if (id < 0 || H5Iget_type(id) != H5I_VFL) {
        H5FD_class_t cls{};
        cls.version = H5FD_CLASS_VERSION;
        cls.value = H5_VFD_RESERVED;
        cls.name = "openswmm-checkpoint";
        cls.maxaddr = maxAddress;
        cls.fc_degree = H5F_CLOSE_WEAK;
        cls.open = openFile;
        cls.close = closeFile;
        cls.cmp = compare;
        cls.query = query;
        cls.get_eoa = getEoa;
        cls.set_eoa = setEoa;
        cls.get_eof = getEof;
        cls.get_handle = getHandle;
        cls.read = readFile;
        cls.write = writeFile;
        cls.truncate = truncateFile;
        cls.lock = lockFile;
        cls.unlock = unlockFile;
        const H5FD_mem_t map[] = H5FD_FLMAP_DICHOTOMY;
        std::copy(std::begin(map), std::end(map), cls.fl_map);
        id = H5FDregister(&cls);
    }
    return id;
}
}
hid_t checkpointHdf5Access() {
    std::lock_guard<std::recursive_mutex> lock(resultHdf5Mutex());
    const auto id = driver();
    if (id < 0)
        return H5I_INVALID_HID;
    const auto props = H5Pcreate(H5P_FILE_ACCESS);
    if (props >= 0 && H5Pset_driver(props, id, nullptr) >= 0)
        return props;
    if (props >= 0)
        H5Pclose(props);
    return H5I_INVALID_HID;
}
hid_t createCheckpointHdf5(const char* path, unsigned flags) {
    std::lock_guard<std::recursive_mutex> lock(resultHdf5Mutex());
#ifdef _WIN32
    const auto access = checkpointHdf5Access();
    if (access < 0)
        return H5I_INVALID_HID;
    const auto file = createHdf5File(path, flags, H5P_DEFAULT, access);
    H5Pclose(access);
    return file;
#else
    return createHdf5File(path, flags, H5P_DEFAULT, H5P_DEFAULT);
#endif
}
hid_t openCheckpointHdf5(const char* path, unsigned flags) {
    std::lock_guard<std::recursive_mutex> lock(resultHdf5Mutex());
#ifdef _WIN32
    const auto access = checkpointHdf5Access();
    if (access < 0)
        return H5I_INVALID_HID;
    const auto file = H5Fopen(path, flags, access);
    H5Pclose(access);
    return file;
#else
    return H5Fopen(path, flags, H5P_DEFAULT);
#endif
}
int checkpointHdf5Descriptor(hid_t file) {
    std::lock_guard<std::recursive_mutex> lock(resultHdf5Mutex());
    const auto properties = H5Fget_access_plist(file);
    if (properties < 0)
        return -1;
    const auto kind = H5Pget_driver(properties);
    H5Pclose(properties);
    void* handle = nullptr;
    if ((kind != H5FD_SEC2 && kind != driver()) ||
        H5Fget_vfd_handle(file, H5P_DEFAULT, &handle) < 0 || !handle)
        return -1;
    return *static_cast<int*>(handle);
}
}
