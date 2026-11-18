// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
/**
 * @file CheckpointHdf5.cpp
 * @brief Native-descriptor VFD and platform checkpoint HDF5 access wrappers.
 * @ingroup engine_io
 * @author Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license Apache-2.0
 */
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
/**
 * @brief Own one native file descriptor behind the HDF5 VFD callback object.
 *
 * base is first for callback reinterpretation. eoa/eof are logical allocation/known physical length
 * in bytes; volume/index identify the underlying filesystem object for equality. Callback
 * invocation is serialized by callers through the shared result HDF5 mutex; this struct has no
 * independent locking or RAII destructor.
 */
struct File {
    H5FD_t base{};
    int fd = -1;
    haddr_t eoa = 0, eof = 0;
    uint64_t volume = 0, index = 0;
};
constexpr haddr_t maxAddress = INT64_MAX;
#ifdef _WIN32
/**
 * @brief Close a native descriptor through the platform runtime.
 *
 * No retry or ownership bookkeeping is performed; an error does not establish whether the OS
 * already released the descriptor.
 *
 * @param[in] fd Open descriptor being released.
 * @return Platform close status: zero on success, negative on error.
 */
int closeFd(int fd) {
    return _close(fd);
}
/**
 * @brief Set a native descriptor position to an absolute byte offset.
 *
 * Mutates shared descriptor position. Caller serializes seek plus following IO; no descriptor
 * ownership transfer occurs.
 *
 * @param[in] fd Borrowed seekable descriptor.
 * @param[in] at Byte offset representable by the native signed offset type.
 * @return Resulting absolute offset, or negative on seek failure.
 */
int64_t seekFd(int fd, uint64_t at) {
    return _lseeki64(fd, at, SEEK_SET);
}
#else
/**
 * @brief Close a native descriptor through the platform runtime.
 *
 * No retry or ownership bookkeeping is performed; an error does not establish whether the OS
 * already released the descriptor.
 *
 * @param[in] fd Open descriptor being released.
 * @return Platform close status: zero on success, negative on error.
 */
int closeFd(int fd) {
    return close(fd);
}
/**
 * @brief Set a native descriptor position to an absolute byte offset.
 *
 * Mutates shared descriptor position. Caller serializes seek plus following IO; no descriptor
 * ownership transfer occurs.
 *
 * @param[in] fd Borrowed seekable descriptor.
 * @param[in] at Byte offset representable by the native signed offset type.
 * @return Resulting absolute offset, or negative on seek failure.
 */
int64_t seekFd(int fd, uint64_t at) {
    return lseek(fd, at, SEEK_SET);
}
#endif
/**
 * @brief Open a checkpoint VFD native file and capture its identity/size.
 *
 * The unnamed access-property argument is ignored. Uses nonthrowing File allocation and Windows
 * read/write/delete sharing; catches Windows path-conversion errors. POSIX create mode is 0666
 * subject to umask. Closes acquired native resources on setup failure. Does not acquire
 * resultHdf5Mutex itself; HDF5 caller must serialize callbacks.
 *
 * @param[in] name Non-null nonempty filename, UTF-8 on Windows; borrowed only for opening.
 * @param[in] flags HDF5 native access/create/truncate/exclusive flags mapped to platform open
 *     modes.
 * @param[in] limit Nonzero maximum address no greater than INT64_MAX.
 * @return Owned File base pointer for HDF5 closeFile, or nullptr on invalid
 *     input/allocation/open/stat/size failure.
 */
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
/**
 * @brief Close the descriptor and release a checkpoint VFD callback object.
 *
 * On close failure the File allocation is retained and not deleted, although native descriptor
 * state after close error is platform-dependent. Caller must not assume retry is safe. No extra
 * flush is performed.
 *
 * @param[in,out] base Non-null live base pointer returned by this VFD open callback; ownership ends
 *     on success.
 * @return Zero on successful close/delete; -1 when native close fails.
 */
herr_t closeFile(H5FD_t* base) {
    auto* f = reinterpret_cast<File*>(base);
    if (closeFd(f->fd) < 0)
        return -1;
    delete f;
    return 0;
}
/**
 * @brief Order VFD files by captured filesystem volume and file identity.
 *
 * Paths are not compared, allowing two opens of the same native object to compare equal. No IO or
 * pointer retention occurs.
 *
 * @param[in] a Non-null borrowed File base pointer from this driver.
 * @param[in] b Non-null borrowed File base pointer from this driver.
 * @return -1, zero or one for lexicographic volume/index ordering.
 */
int compare(const H5FD_t* a, const H5FD_t* b) {
    const auto* x = reinterpret_cast<const File*>(a);
    const auto* y = reinterpret_cast<const File*>(b);
    const auto p = std::tie(x->volume, x->index), q = std::tie(y->volume, y->index);
    return p < q ? -1 : p > q ? 1 : 0;
}
/**
 * @brief Report metadata aggregation and native-handle VFD capability flags.
 *
 * The unnamed file argument is ignored, so class-level capability queries need no file object.
 * Advertises metadata/small-data aggregation, accumulation, data sieve, POSIX-compatible handle and
 * default-VFD compatibility.
 *
 * @param[out] flags Optional writable flag slot; null requests no stored output.
 * @return Zero unconditionally.
 */
herr_t query(const H5FD_t*, unsigned long* flags) {
    if (flags)
        *flags = H5FD_FEAT_AGGREGATE_METADATA | H5FD_FEAT_ACCUMULATE_METADATA |
                 H5FD_FEAT_DATA_SIEVE | H5FD_FEAT_AGGREGATE_SMALLDATA |
                 H5FD_FEAT_POSIX_COMPAT_HANDLE | H5FD_FEAT_DEFAULT_VFD_COMPATIBLE;
    return 0;
}
/**
 * @brief Read the logical end-of-allocation byte offset.
 *
 * The unnamed memory-type argument is ignored; all allocation types share one address space. Does
 * not inspect physical file size.
 *
 * @param[in] f Non-null borrowed File base pointer.
 * @return Current logical eoa in bytes.
 */
haddr_t getEoa(const H5FD_t* f, H5FD_mem_t) {
    return reinterpret_cast<const File*>(f)->eoa;
}
/**
 * @brief Update the logical end-of-allocation byte offset.
 *
 * The unnamed memory-type argument is ignored. Does not truncate or extend physical storage;
 * truncateFile supplies that later operation.
 *
 * @param[in,out] f Non-null live File base pointer.
 * @param[in] eoa Requested logical allocation endpoint in bytes, at most INT64_MAX.
 * @return Zero on assignment, -1 above the maximum address with eoa unchanged.
 */
herr_t setEoa(H5FD_t* f, H5FD_mem_t, haddr_t eoa) {
    if (eoa > maxAddress)
        return -1;
    reinterpret_cast<File*>(f)->eoa = eoa;
    return 0;
}
/**
 * @brief Return the larger known physical/logical file endpoint.
 *
 * The unnamed memory type is ignored. Does not refresh native file size, so external file
 * modifications are outside this cached view.
 *
 * @param[in] f Non-null borrowed File base pointer.
 * @return max(eof,eoa) in bytes.
 */
haddr_t getEof(const H5FD_t* f, H5FD_mem_t) {
    const auto* file = reinterpret_cast<const File*>(f);
    return std::max(file->eof, file->eoa);
}
/**
 * @brief Expose the address of the VFD-owned descriptor slot to HDF5.
 *
 * The unnamed access-property identifier is ignored. Returned storage/descriptor belong to the VFD
 * and expire at file close; caller must neither free storage nor independently close/change the
 * descriptor.
 *
 * @param[in] f Non-null live File base pointer.
 * @param[out] out Writable pointer slot; receives a borrowed int* into File when non-null.
 * @return Zero on assignment, -1 when out is null.
 */
herr_t getHandle(H5FD_t* f, hid_t, void** out) {
    if (!out)
        return -1;
    *out = &reinterpret_cast<File*>(f)->fd;
    return 0;
}
/**
 * @brief Read bytes, zero-filling an unwritten tail within logical allocation.
 *
 * Unnamed memory-type and transfer-property arguments are ignored. Seeks once, transfers at most
 * INT_MAX bytes per syscall and retries EINTR. A native EOF fills all remaining requested bytes
 * with zero; errors after partial IO leave a partially filled destination. Caller serializes the
 * whole seek/IO sequence; no independent mutex is acquired.
 *
 * @param[in,out] base Non-null live File base; shared native seek position is mutated.
 * @param[in] at Absolute start byte offset within [0,eoa].
 * @param[in] size Byte count no greater than eoa-at; bounded before address arithmetic.
 * @param[out] data Writable buffer of at least size bytes; non-null for nonzero size, borrowed only
 *     for the call.
 * @return Zero on complete operation, -1 on allocation-bound/seek/native IO failure.
 */
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
/**
 * @brief Write bytes within the VFD logical allocation and update known EOF.
 *
 * Unnamed memory-type and transfer-property arguments are ignored. Seeks once, transfers at most
 * INT_MAX bytes per syscall and retries EINTR. Errors after partial IO leave disk changes in place
 * while cached eof is updated only after complete success; no rollback or durability flush occurs.
 * Caller serializes the whole seek/IO sequence; no independent mutex is acquired.
 *
 * @param[in,out] base Non-null live File base; shared native seek position is mutated.
 * @param[in] at Absolute start byte offset within [0,eoa].
 * @param[in] size Byte count no greater than eoa-at; bounded before address arithmetic.
 * @param[in] data Readable buffer of at least size bytes; non-null for nonzero size, borrowed only
 *     for the call.
 * @return Zero on complete operation, -1 on allocation-bound/seek/native IO failure.
 */
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
/**
 * @brief Resize physical storage to match the logical allocation endpoint.
 *
 * Unnamed transfer properties and closing flag are ignored. On success eof is replaced by eoa;
 * failure leaves cached eof unchanged. May shrink or extend the file; does not fsync or acquire a
 * mutex itself.
 *
 * @param[in,out] base Non-null live File base whose descriptor is writable when a size change is
 *     needed.
 * @return Zero when sizes already match or native resize succeeds; -1 on resize failure.
 */
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
/**
 * @brief Attempt a nonblocking shared or exclusive native file lock.
 *
 * Uses flock with LOCK_NB on POSIX, LockFileEx with FAIL_IMMEDIATELY over the MAXDWORD/MAXDWORD
 * range on Windows. Lock lifetime follows explicit unlock/native handle close; caller remains
 * responsible for process-local HDF5 serialization.
 *
 * @param[in] base Non-null borrowed live File base pointer.
 * @param[in] write True requests exclusive lock, false shared lock.
 * @return Zero on lock acquisition, negative on contention/native failure.
 */
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
/**
 * @brief Release the native file lock for a checkpoint VFD object.
 *
 * Uses flock LOCK_UN or matching Windows UnlockFileEx range; does not close the file, change
 * descriptor ownership or release the separate process-local mutex.
 *
 * @param[in] base Non-null borrowed live File base pointer.
 * @return Native unlock status, zero on success and negative on failure.
 */
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
/**
 * @brief Register or reuse the process-local checkpoint VFD class.
 *
 * Caller holds resultHdf5Mutex. Re-registers if cached id is invalid or no longer H5I_VFL, installs
 * native IO/locking callbacks, weak close degree, INT64_MAX address limit and dichotomy memory map.
 * HDF5 owns registration state; does not increment a reference for each returned use or
 * independently unregister.
 * @return Borrowed registered driver identifier, possibly negative when H5FDregister rejects.
 */
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
