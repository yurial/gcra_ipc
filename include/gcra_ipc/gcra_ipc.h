// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace gcra_ipc {

namespace detail {

struct gcra_ipc_shm;

} // namespace detail

////////////////////////////////////////////////////////////////////////////////

//! Error thrown by #shaper::CreateOrThrow and #shaper::Reconfigure.
/*!
 *  Carries the errno value describing the failure: EINVAL for invalid arguments
 *  (empty shm object name, foreign magic, detached shaper) or the system errno
 *  of a failed shm_open/ftruncate/mmap call.
 */
class gcra_ipc_error
    : public std::runtime_error
{
public:
    gcra_ipc_error(int errorCode, const std::string& message)
        : std::runtime_error(message + " (errno " + std::to_string(errorCode) + ")")
        , ErrorCode_(errorCode)
    { }

    //! The errno value of the failure.
    int errorCode() const noexcept
    {
        return ErrorCode_;
    }

private:
    const int ErrorCode_;
};

////////////////////////////////////////////////////////////////////////////////

//! Interprocess rate shaper (GCRA — Generic Cell Rate Algorithm) backed by a POSIX
//! shared-memory object.
/*!
 *  The shaper keeps the whole queue schedule in a single shared word
 *  (the virtual queue position, in nanoseconds) and works without timers, threads,
 *  locks or syscalls on the hot path: #Reserve only performs one CAS.
 *  Semantics is that of a shaper, not a policer: a charge always succeeds and
 *  returns the absolute deadline until which the caller must not perform the
 *  limited action; there is no rejection.
 *
 *  All times are nanoseconds of CLOCK_MONOTONIC. Several instances of |shaper|,
 *  possibly in different processes, attached to the same shm object share one queue.
 *  The mapping lives until the end of the process; there is no detach API.
 *
 *  Thread affinity: any
 */
class shaper
{
public:
    //! Attaches to an existing shm object without creating it.
    /*!
     *  \param shmName non-empty POSIX shm object name with a leading slash.
     *  \return the attached shaper, or null when the object does not exist
     *  (errno ENOENT), its magic is foreign (errno EINVAL) or a system call has failed
     *  (system errno preserved); errno is set whenever null is returned.
     *  No side effects are performed on the object.
     */
    static std::shared_ptr<shaper> Open(const std::string& shmName);

    //! Attaches to an existing or creates a new shm object and configures it.
    /*!
     *  On a freshly created (zero-filled) object the magic is initialized (R18);
     *  on an existing object the call acts as a reconfiguration: position is preserved
     *  and previously issued deadlines stay valid (R16).
     *
     *  \param shmName non-empty POSIX shm object name with a leading slash.
     *  \param ratePerSecond target rate in units per second, 0..UINT64_MAX, stored
     *  as is; 0 disables the limit (fail-open).
     *  \param burstWindowNs burst window in nanoseconds, 0..UINT64_MAX, stored
     *  as is; 0 means a strict spacer without burst credit. The immediate burst
     *  of a request series in units is floor(burstWindowNs x ratePerSecond / 1e9)
     *  plus the cost of the last immediate request.
     *  \return the attached shaper, or null: foreign magic (errno EINVAL) or
     *  a system failure (system errno preserved); errno is set whenever null
     *  is returned.
     */
    static std::shared_ptr<shaper> Create(const std::string& shmName, std::uint64_t ratePerSecond, std::uint64_t burstWindowNs);

    //! Same as #Create, but failures are reported by throwing gcra_ipc_error
    //! with the corresponding errno code (EINVAL or a system errno).
    static std::shared_ptr<shaper> CreateOrThrow(const std::string& shmName, std::uint64_t ratePerSecond, std::uint64_t burstWindowNs);

    //! Reconfigures the rate and the burst window of an attached object in place.
    /*!
     *  Position is neither trimmed nor recalculated: all previously issued deadlines
     *  remain valid, new requests use the new parameters (R16). The parameters are
     *  published atomically, repeated calls simply overwrite them (R14).
     *
     *  Throws gcra_ipc_error (EINVAL) when the shaper is not attached.
     *
     *  \param ratePerSecond allowed range and meaning as in #Create.
     *  \param burstWindowNs allowed range and meaning as in #Create.
     */
    void Reconfigure(std::uint64_t ratePerSecond, std::uint64_t burstWindowNs);

    //! Charges #units against the shared schedule and returns the slot deadline.
    /*!
     *  Always succeeds: the returned value is the absolute moment (nanoseconds of
     *  CLOCK_MONOTONIC) before which the caller must not perform the limited action.
     *  The returned value is the request slot, not the new queue position.
     *
     *  \param nowNs current CLOCK_MONOTONIC time in nanoseconds as read by the caller.
     *  \param units cost of the request, 0..2^24 (detail::units_max); the bound is a usage
     *  contract and is not checked: the increment numerator units x 1e9 fits uint64
     *  up to the theoretical limit 2^34; beyond it the numerator wraps around (a
     *  defined uint64 overflow), yielding both an increment and a deadline smaller
     *  than the exact ones — guarding against this is the caller's responsibility.
     *  |units == 0| is a no-op returning #nowNs.
     *
     *  Safe to call concurrently from any threads of any processes sharing the object.
     */
    std::uint64_t Reserve(std::uint64_t nowNs, std::uint64_t units);

private:
    shaper() = default;

    detail::gcra_ipc_shm* Shm_ = nullptr;
};

using shaper_ptr = std::shared_ptr<shaper>;

////////////////////////////////////////////////////////////////////////////////

} // namespace gcra_ipc

#define GCRA_IPC_INL_H_
#include <gcra_ipc/gcra_ipc-inl.h>
#undef GCRA_IPC_INL_H_
