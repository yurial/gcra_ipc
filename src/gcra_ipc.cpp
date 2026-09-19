// SPDX-License-Identifier: MIT
#include <gcra_ipc/gcra_ipc.h>

#include <gcra_ipc/gcra_ipc_detail.h>

#include <cerrno>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

namespace gcra_ipc {

////////////////////////////////////////////////////////////////////////////////

namespace {

// shm_open -> ftruncate -> mmap -> close chain shared by Open and Create (R17, R26).
// Returns nullptr with errno set on failure.
detail::gcra_ipc_shm* TryMapShmObject(const std::string& shmName, bool create)
{
    int fd = ::shm_open(
        shmName.c_str(),
        O_RDWR | (create ? O_CREAT : 0),
        0600);
    if (fd < 0) {
        return nullptr;
    }
    if (::ftruncate(fd, sizeof(detail::gcra_ipc_shm)) != 0) {
        auto error = errno;
        ::close(fd);
        errno = error;
        return nullptr;
    }
    void* mapped = ::mmap(
        nullptr,
        sizeof(detail::gcra_ipc_shm),
        PROT_READ | PROT_WRITE,
        MAP_SHARED | MAP_POPULATE,
        fd,
        0);
    if (mapped == MAP_FAILED) {
        auto error = errno;
        ::close(fd);
        errno = error;
        return nullptr;
    }
    ::close(fd);
    return static_cast<detail::gcra_ipc_shm*>(mapped);
}

// Publishes the parameters in the window -> rate order, the rate with release, so
// the only transitionally observable mixed pair is "old rate + new window": a
// fail-open shape, never a transient strict spacer. There is no strictly consistent
// read of the pair (the parameters are loaded relaxed), and transient CAS attempts
// carrying such a mixed pair are allowed (spec Constraints, R14).
void PublishGcraIpcParams(detail::gcra_ipc_shm* shm, std::uint64_t ratePerSecond, std::uint64_t burstWindowNs)
{
    shm->state.burst_window_ns.store(burstWindowNs, std::memory_order_relaxed);
    shm->state.rate_per_second.store(ratePerSecond, std::memory_order_release);
}

std::string FormatGcraIpcCreateError(const std::string& shmName, std::uint64_t ratePerSecond, std::uint64_t burstWindowNs)
{
    return "Error creating GCRA IPC shaper (ShmName: " + shmName
        + ", RatePerSecond: " + std::to_string(ratePerSecond)
        + ", BurstWindowNs: " + std::to_string(burstWindowNs) + ")";
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

shaper_ptr shaper::Open(const std::string& shmName)
{
    if (shmName.empty()) {
        errno = EINVAL;
        return nullptr;
    }

    auto* shm = TryMapShmObject(shmName, /*create*/ false);
    if (!shm) {
        return nullptr;
    }

    auto magic = shm->magic.load(std::memory_order_acquire);
    if (magic != 0 && magic != detail::magic) {
        ::munmap(shm, sizeof(detail::gcra_ipc_shm));
        errno = EINVAL;
        return nullptr;
    }

    auto result = shaper_ptr(new shaper());
    result->Shm_ = shm;
    return result;
}

shaper_ptr shaper::Create(const std::string& shmName, std::uint64_t ratePerSecond, std::uint64_t burstWindowNs)
{
    if (shmName.empty()) {
        errno = EINVAL;
        return nullptr;
    }

    auto* shm = TryMapShmObject(shmName, /*create*/ true);
    if (!shm) {
        return nullptr;
    }

    auto magic = shm->magic.load(std::memory_order_acquire);
    if (magic != 0 && magic != detail::magic) {
        ::munmap(shm, sizeof(detail::gcra_ipc_shm));
        errno = EINVAL;
        return nullptr;
    }
    if (magic == 0) {
        // Fresh zero-filled shm: publish the layout marker, zero state is valid (R18).
        shm->version = detail::layout_version;
        shm->magic.store(detail::magic, std::memory_order_release);
    }

    PublishGcraIpcParams(shm, ratePerSecond, burstWindowNs);

    auto result = shaper_ptr(new shaper());
    result->Shm_ = shm;
    return result;
}

shaper_ptr shaper::CreateOrThrow(const std::string& shmName, std::uint64_t ratePerSecond, std::uint64_t burstWindowNs)
{
    if (auto result = Create(shmName, ratePerSecond, burstWindowNs)) {
        return result;
    }
    auto savedErrno = errno;
    throw gcra_ipc_error(savedErrno, FormatGcraIpcCreateError(shmName, ratePerSecond, burstWindowNs));
}

void shaper::Reconfigure(std::uint64_t ratePerSecond, std::uint64_t burstWindowNs)
{
    if (Shm_ == nullptr) {
        throw gcra_ipc_error(EINVAL, "GCRA IPC shaper is not attached");
    }
    PublishGcraIpcParams(Shm_, ratePerSecond, burstWindowNs);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace gcra_ipc
