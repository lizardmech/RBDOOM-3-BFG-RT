#include "PathTraceAccelCpuAllocation.h"

#include <atomic>
#include <new>
#include <stdexcept>

namespace
{
std::atomic<int> g_accelCpuReserveFailureOrdinal{-1};
std::atomic<int> g_accelCpuReserveCounter{0};
std::atomic<bool> g_accelCpuReserveLengthError{false};
}

void PathTraceAccelCpuPackSetAllocationFailureForTest(
    int reserveOrdinal, bool lengthError)
{
    g_accelCpuReserveFailureOrdinal.store(reserveOrdinal,
        std::memory_order_release);
    g_accelCpuReserveCounter.store(0, std::memory_order_release);
    g_accelCpuReserveLengthError.store(lengthError,
        std::memory_order_release);
}

void PathTraceAccelCpuPackMaybeInjectAllocationFailure()
{
    const int target = g_accelCpuReserveFailureOrdinal.load(
        std::memory_order_acquire);
    if (target < 0) return;
    const int ordinal = g_accelCpuReserveCounter.fetch_add(
        1, std::memory_order_acq_rel);
    if (ordinal != target) return;
    g_accelCpuReserveFailureOrdinal.store(-1, std::memory_order_release);
    if (g_accelCpuReserveLengthError.load(std::memory_order_acquire))
        throw std::length_error("injected AccelCpuPack reserve failure");
    throw std::bad_alloc();
}
