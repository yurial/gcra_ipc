// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <gcra_ipc/gcra_ipc.h>
#include <gcra_ipc/gcra_ipc_detail.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <random>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace gcra_ipc {
namespace {

////////////////////////////////////////////////////////////////////////////////

using detail::gcra_ipc_shm;

constexpr std::uint64_t Ms = 1'000'000;
constexpr std::uint64_t Second = 1'000'000'000;

// The golden configuration (A13): rate 10 units/s, burst window 200 ms.
constexpr std::uint64_t GoldenRatePerSecond = 10;
constexpr std::uint64_t GoldenBurstWindowNs = 200'000'000; // 200 ms: burst budget floor(2e8 x 10 / 1e9) = 2 units

std::string MakeShmName()
{
    std::random_device rd;
    std::uint64_t value = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    char buffer[64];
    ::snprintf(buffer, sizeof(buffer), "/gcra_ipc_ut_%d_%016llx", ::getpid(), static_cast<unsigned long long>(value));
    return buffer;
}

// Removes the shm object name at scope exit; mappings of attached shapers outlive it.
class ShmUnlinkGuard
{
public:
    explicit ShmUnlinkGuard(std::string name)
        : Name_(std::move(name))
    { }

    ShmUnlinkGuard(const ShmUnlinkGuard&) = delete;
    ShmUnlinkGuard& operator=(const ShmUnlinkGuard&) = delete;

    ~ShmUnlinkGuard()
    {
        ::shm_unlink(Name_.c_str());
    }

private:
    const std::string Name_;
};

// Creates the golden configuration through the factory API (A13, T1): the layout
// is configured exclusively by Create(), no direct shm writes.
shaper_ptr CreateGoldenShaper(const std::string& shmName)
{
    auto shaper = shaper::Create(shmName, /*ratePerSecond*/ GoldenRatePerSecond, /*burstWindowNs*/ GoldenBurstWindowNs);
    EXPECT_TRUE(shaper);
    return shaper;
}

// Direct shm mapping used to observe state behind shaper and to prepare objects
// that the factories must not create themselves.
class RawShm
{
public:
    ~RawShm()
    {
        if (Shm_) {
            ::munmap(Shm_, sizeof(gcra_ipc_shm));
        }
        if (Fd_ >= 0) {
            ::close(Fd_);
        }
        if (MustUnlink_) {
            ::shm_unlink(Name_.c_str());
        }
    }

    void Open(const std::string& name, bool create)
    {
        Name_ = name;
        Fd_ = ::shm_open(name.c_str(), O_RDWR | (create ? O_CREAT : 0), 0600);
        ASSERT_GE(Fd_, 0);
        ASSERT_EQ(::ftruncate(Fd_, sizeof(gcra_ipc_shm)), 0);
        auto* mapped = ::mmap(
            nullptr,
            sizeof(gcra_ipc_shm),
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            Fd_,
            0);
        ASSERT_NE(mapped, MAP_FAILED);
        MustUnlink_ = create;
        Shm_ = static_cast<gcra_ipc_shm*>(mapped);
    }

    gcra_ipc_shm* Get() const
    {
        return Shm_;
    }

private:
    std::string Name_;
    int Fd_ = -1;
    gcra_ipc_shm* Shm_ = nullptr;
    bool MustUnlink_ = false;
};

////////////////////////////////////////////////////////////////////////////////

TEST(ShaperTest, GoldenSchedule)
{
    auto shmName = MakeShmName();
    ShmUnlinkGuard unlinkGuard(shmName);

    auto shaper = CreateGoldenShaper(shmName);
    ASSERT_TRUE(shaper);

    RawShm observer;
    observer.Open(shmName, /*create*/ false);
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), GoldenRatePerSecond);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), GoldenBurstWindowNs);

    struct Row
    {
        const char* Name;
        std::uint64_t Units;
        std::uint64_t Now;
        std::uint64_t ExpectedSlot;
        std::uint64_t ExpectedPositionAfter;
    };

    const Row rows[] = {
        // Golden table T1: burst budget B = 2 plus the cost of the last immediate
        // request gives 3 immediate unit slots, 100 ms cadence per unit.
        {"A", 1, 0, 0, 100 * Ms},
        {"B", 1, 0, 0, 200 * Ms},
        {"C", 1, 0, 0, 300 * Ms},
        {"D", 1, 0, 100 * Ms, 400 * Ms},
        // Idle period: burst credit has not accumulated (A8).
        {"E", 1, 10 * Second, 10 * Second, 10 * Second + 100 * Ms},
        {"F", 1, 10 * Second, 10 * Second, 10 * Second + 200 * Ms},
        {"G", 1, 10 * Second, 10 * Second, 10 * Second + 300 * Ms},
        {"H", 1, 10 * Second, 10 * Second + 100 * Ms, 10 * Second + 400 * Ms},
        // Cost scales the position step (A5).
        {"I", 2, 10 * Second + 400 * Ms, 10 * Second + 400 * Ms, 10 * Second + 600 * Ms},
        {"J", 1, 10 * Second + 400 * Ms, 10 * Second + 400 * Ms, 10 * Second + 700 * Ms},
        {"K", 1, 10 * Second + 400 * Ms, 10 * Second + 500 * Ms, 10 * Second + 800 * Ms},
        // Idle period; cost-2 requests are immediate exactly twice (A9).
        {"L", 2, 20 * Second, 20 * Second, 20 * Second + 200 * Ms},
        {"M", 2, 20 * Second, 20 * Second, 20 * Second + 400 * Ms},
        {"N", 2, 20 * Second, 20 * Second + 200 * Ms, 20 * Second + 600 * Ms},
        // units == 0 is a no-op (A2).
        {"O", 0, 20 * Second + 300 * Ms, 20 * Second + 300 * Ms, 20 * Second + 600 * Ms},
        // High cost honestly receives a far slot (A6).
        {"P", 5, 20 * Second + 300 * Ms, 20 * Second + 400 * Ms, 21 * Second + 100 * Ms},
    };

    for (const auto& row : rows) {
        auto slot = shaper->Reserve(row.Now, row.Units);
        EXPECT_EQ(slot, row.ExpectedSlot) << row.Name;
        EXPECT_EQ(observer.Get()->state.position_ns.load(), row.ExpectedPositionAfter) << row.Name;
    }
}

TEST(ShaperTest, DisabledLimit)
{
    auto shmName = MakeShmName();
    ShmUnlinkGuard unlinkGuard(shmName);

    auto shaper = CreateGoldenShaper(shmName);
    ASSERT_TRUE(shaper);

    RawShm observer;
    observer.Open(shmName, /*create*/ false);

    shaper->Reconfigure(/*ratePerSecond*/ 0, /*burstWindowNs*/ GoldenBurstWindowNs);
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), 0ull);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), GoldenBurstWindowNs);

    // Any charge returns its now without touching position (R3, A3).
    EXPECT_EQ(shaper->Reserve(20 * Second + 300 * Ms, 5), 20 * Second + 300 * Ms);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 0ull);
    EXPECT_EQ(shaper->Reserve(0, 1), 0);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 0ull);

    // Re-enabling keeps the schedule intact; 1000 units/s -> 1 ms per unit.
    shaper->Reconfigure(/*ratePerSecond*/ 1000, /*burstWindowNs*/ 2 * Ms);
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), 1'000ull);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), 2 * Ms);
    EXPECT_EQ(shaper->Reserve(0, 1), 0);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 1 * Ms);
}

TEST(ShaperTest, UnitsZeroIsNoOp)
{
    auto shmName = MakeShmName();
    ShmUnlinkGuard unlinkGuard(shmName);

    auto shaper = CreateGoldenShaper(shmName);
    ASSERT_TRUE(shaper);

    RawShm observer;
    observer.Open(shmName, /*create*/ false);

    EXPECT_EQ(shaper->Reserve(999 * Ms, 0), 999 * Ms);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 0ull);
    EXPECT_EQ(shaper->Reserve(999 * Ms, 0), 999 * Ms);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 0ull);
}

TEST(ShaperTest, UnderflowProtection)
{
    auto shmName = MakeShmName();
    ShmUnlinkGuard unlinkGuard(shmName);

    auto shaper = CreateGoldenShaper(shmName);
    ASSERT_TRUE(shaper);

    RawShm observer;
    observer.Open(shmName, /*create*/ false);

    // position below the burst window: slots clamp to now (R4, A4).
    EXPECT_EQ(shaper->Reserve(0, 1), 0);
    EXPECT_EQ(shaper->Reserve(0, 1), 0);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 200 * Ms);
}

TEST(ShaperTest, ConfigureValidation)
{
    auto shmName = MakeShmName();
    ShmUnlinkGuard unlinkGuard(shmName);

    // No parameter requires rejection (R15): boundary configurations are accepted
    // and stored as is (R13, T3).
    auto shaper = shaper::Create(shmName, /*ratePerSecond*/ 1, /*burstWindowNs*/ 0);
    ASSERT_TRUE(shaper);

    RawShm observer;
    observer.Open(shmName, /*create*/ false);
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), 1ull);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), 0ull);

    // Window 0 is a strict spacer: no burst credit, the second unit request waits
    // a full second (R13, R9).
    EXPECT_EQ(shaper->Reserve(0, 1), 0);
    EXPECT_EQ(shaper->Reserve(0, 1), Second);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 2 * Second);

    // Window UINT64_MAX is accepted; creation success and boundary storage are all
    // we check on the reconfigured object.
    errno = 0;
    auto wide = shaper::Create(shmName, std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(wide);
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), std::numeric_limits<std::uint64_t>::max());

    // The same boundary on a fresh object works on the hot path without waiting:
    // the window dwarfs the position, so the first unit charge at now == 0 is
    // immediate (slot clamps to now) and advances the position by the minimal
    // increment ceil(1e9 / rate) == 1 ns.
    auto wideShmName = MakeShmName();
    ShmUnlinkGuard wideUnlinkGuard(wideShmName);
    auto freshWide = shaper::Create(wideShmName, std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(freshWide);

    RawShm wideObserver;
    wideObserver.Open(wideShmName, /*create*/ false);
    EXPECT_EQ(freshWide->Reserve(/*nowNs*/ 0, /*units*/ 1), 0);
    EXPECT_EQ(wideObserver.Get()->state.position_ns.load(), 1ull);

    // Reconfigure accepts the same boundaries and stores them as is (R13, R16).
    EXPECT_NO_THROW(shaper->Reconfigure(/*ratePerSecond*/ 0, /*burstWindowNs*/ 0));
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), 0ull);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), 0ull);
    EXPECT_NO_THROW(shaper->Reconfigure(std::numeric_limits<std::uint64_t>::max(), std::numeric_limits<std::uint64_t>::max()));
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_NO_THROW(shaper->Reconfigure(/*ratePerSecond*/ 1000, /*burstWindowNs*/ 2 * Ms));
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), 1'000ull);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), 2 * Ms);
}

TEST(ShaperTest, ReconfigureKeepsPosition)
{
    auto shmName = MakeShmName();
    ShmUnlinkGuard unlinkGuard(shmName);

    auto shaper = CreateGoldenShaper(shmName);
    ASSERT_TRUE(shaper);

    RawShm observer;
    observer.Open(shmName, /*create*/ false);

    // Move position to 21.1 s.
    EXPECT_EQ(shaper->Reserve(0, 211), 0);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 21 * Second + 100 * Ms);

    // A16: rate halved to 5 units/s and the window doubled to 400 ms through the
    // public API; position is preserved (R16).
    shaper->Reconfigure(/*ratePerSecond*/ 5, /*burstWindowNs*/ 400'000'000);
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), 5ull);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), 400 * Ms);

    // The slot is computed from the preserved position by the new rate.
    EXPECT_EQ(shaper->Reserve(20 * Second + 300 * Ms, 1), 20 * Second + 700 * Ms);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 21 * Second + 300 * Ms);
}

TEST(ShaperTest, OpenAttachSemantics)
{
    auto shmName = MakeShmName();

    // Missing object: ENOENT, no side effects (R25).
    errno = 0;
    EXPECT_EQ(shaper::Open(shmName), nullptr);
    EXPECT_EQ(errno, ENOENT);

    // Empty name is rejected (EINVAL).
    errno = 0;
    EXPECT_EQ(shaper::Open(std::string()), nullptr);
    EXPECT_EQ(errno, EINVAL);
    errno = 0;
    EXPECT_EQ(shaper::Create(std::string(), 1000, 2 * Ms), nullptr);
    EXPECT_EQ(errno, EINVAL);
    bool thrownOnEmptyName = false;
    try {
        shaper::CreateOrThrow(std::string(), 1000, 2 * Ms);
    } catch (const gcra_ipc_error& ex) {
        thrownOnEmptyName = true;
        EXPECT_EQ(ex.errorCode(), EINVAL);
    }
    EXPECT_TRUE(thrownOnEmptyName);

    ShmUnlinkGuard unlinkGuard(shmName);
    auto a = CreateGoldenShaper(shmName);
    ASSERT_TRUE(a);

    auto b = shaper::Open(shmName);
    ASSERT_TRUE(b);

    // Two attachments share one queue: slots follow the golden cadence across instances.
    EXPECT_EQ(a->Reserve(0, 1), 0);
    EXPECT_EQ(b->Reserve(0, 1), 0);
    EXPECT_EQ(b->Reserve(0, 1), 0);
    EXPECT_EQ(a->Reserve(0, 1), 100 * Ms);

    // Repeated Create on an existing object reconfigures it without trimming
    // position (R16); 1000 units/s with a zero window is a strict 1 ms spacer.
    auto c = shaper::Create(shmName, /*ratePerSecond*/ 1000, /*burstWindowNs*/ 0);
    ASSERT_TRUE(c);

    RawShm observer;
    observer.Open(shmName, /*create*/ false);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 400 * Ms);
    EXPECT_EQ(observer.Get()->state.rate_per_second.load(), 1'000ull);
    EXPECT_EQ(observer.Get()->state.burst_window_ns.load(), 0ull);

    EXPECT_EQ(a->Reserve(0, 1), 400 * Ms);
    EXPECT_EQ(observer.Get()->state.position_ns.load(), 401 * Ms);
}

TEST(ShaperTest, ForeignMagicIsRejected)
{
    auto shmName = MakeShmName();

    RawShm foreign;
    foreign.Open(shmName, /*create*/ true);
    constexpr std::uint32_t ForeignMagic = 0x12345678u;
    foreign.Get()->magic.store(ForeignMagic);

    errno = 0;
    EXPECT_EQ(shaper::Open(shmName), nullptr);
    EXPECT_EQ(errno, EINVAL);

    errno = 0;
    EXPECT_EQ(shaper::Create(shmName, 1000, 2 * Ms), nullptr);
    EXPECT_EQ(errno, EINVAL);

    bool thrownOnForeignMagic = false;
    try {
        shaper::CreateOrThrow(shmName, 1000, 2 * Ms);
    } catch (const gcra_ipc_error& ex) {
        thrownOnForeignMagic = true;
        EXPECT_EQ(ex.errorCode(), EINVAL);
    }
    EXPECT_TRUE(thrownOnForeignMagic);

    // The foreign object was not touched.
    EXPECT_EQ(foreign.Get()->magic.load(), ForeignMagic);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace gcra_ipc
