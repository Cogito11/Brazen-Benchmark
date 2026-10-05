#pragma once

// Best-effort pinning of the calling thread to a specific logical CPU
// core. This matters for benchmark consistency: an unpinned thread can
// get migrated between cores mid-run by the OS scheduler, which resets
// per-core cache state and can land on a core with a different
// turbo/boost clock than the one it started on -- both of which add
// run-to-run noise that has nothing to do with what's being measured.
//
// Two things make "just pin thread i to core i" wrong in practice, and
// this header handles both:
//
//  1. The process may not be *allowed* to use every core the machine has:
//     containers (Docker --cpuset-cpus, Kubernetes), `taskset`, `start
//     /affinity`, or a job object can restrict it. Pinning to a core
//     outside the allowed set always fails, and starting more worker
//     threads than allowed cores oversubscribes the ones that remain.
//     AllowedCores() reports what the process can really use.
//  2. Windows machines with more than 64 logical processors split them
//     into "processor groups", and a plain affinity mask can only address
//     one group (and shifting by 64 or more bits is undefined behavior).
//     Cores are therefore identified by a global index and mapped to
//     (group, bit) when pinning.

#include <algorithm>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach/mach.h>
    #include <mach/thread_policy.h>
    #include <pthread.h>
#else
    #include <pthread.h>
    #include <sched.h>
#endif

namespace brazen {

namespace affinity_detail {

inline std::vector<unsigned> FallbackCores() {
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 1;
    std::vector<unsigned> cores(n);
    for (unsigned i = 0; i < n; ++i) cores[i] = i;
    return cores;
}

#if defined(_WIN32)
// Number of logical processors in each active processor group.
inline std::vector<unsigned> GroupSizes() {
    std::vector<unsigned> sizes;
    WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < groups; ++g) sizes.push_back(GetActiveProcessorCount(g));
    return sizes;
}
#endif

inline std::vector<unsigned> DetectAllowedCores() {
    std::vector<unsigned> cores;
#if defined(_WIN32)
    std::vector<unsigned> groups = GroupSizes();
    if (groups.size() <= 1) {
        // One processor group: honor the process affinity mask (it can be
        // narrower than the machine if the user started us with
        // `start /affinity` or from a job object).
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask) && processMask != 0) {
            for (unsigned bit = 0; bit < sizeof(DWORD_PTR) * 8; ++bit)
                if (processMask & (static_cast<DWORD_PTR>(1) << bit)) cores.push_back(bit);
        }
    } else {
        // Several groups: every logical processor, indexed globally
        // (group 0's processors first, then group 1's, ...).
        unsigned global = 0;
        for (unsigned size : groups)
            for (unsigned i = 0; i < size; ++i) cores.push_back(global++);
    }
#elif defined(__APPLE__)
    // macOS doesn't expose a restricted CPU set or hard pinning.
    cores = FallbackCores();
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        for (int i = 0; i < CPU_SETSIZE; ++i)
            if (CPU_ISSET(i, &set)) cores.push_back(static_cast<unsigned>(i));
    }
#endif
    if (cores.empty()) cores = FallbackCores(); // detection failed: best guess
    std::sort(cores.begin(), cores.end());
    cores.erase(std::unique(cores.begin(), cores.end()), cores.end());
    return cores;
}

} // namespace affinity_detail

// The logical CPUs this process is allowed to run on, ascending. Never
// empty. Detected once; the identifiers are what PinCurrentThreadToCore()
// expects (OS CPU numbers on Linux, global processor indexes on Windows).
inline const std::vector<unsigned>& AllowedCores() {
    static const std::vector<unsigned> cores = affinity_detail::DetectAllowedCores();
    return cores;
}

// Returns true if the OS accepted the pin request.
//
// - Linux / Windows: this is a hard pin -- the scheduler will not run
//   the thread on any other core afterward.
// - macOS: the kernel only exposes an affinity *tag* (grouping hint via
//   thread_policy_set), not a guaranteed pin like Linux/Windows offer,
//   so treat a `true` return there as "requested" rather than "enforced".
//
// An out-of-range core index simply returns false.
inline bool PinCurrentThreadToCore(unsigned coreIndex) {
#if defined(_WIN32)
    std::vector<unsigned> groups = affinity_detail::GroupSizes();
    if (groups.size() <= 1) {
        if (coreIndex >= sizeof(DWORD_PTR) * 8) return false; // shifting by >= width is undefined
        DWORD_PTR mask = static_cast<DWORD_PTR>(1) << coreIndex;
        return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
    }
    unsigned remaining = coreIndex;
    for (size_t g = 0; g < groups.size(); ++g) {
        if (remaining < groups[g]) {
            GROUP_AFFINITY ga = {};
            ga.Group = static_cast<WORD>(g);
            ga.Mask = static_cast<KAFFINITY>(1) << remaining;
            return SetThreadGroupAffinity(GetCurrentThread(), &ga, nullptr) != 0;
        }
        remaining -= groups[g];
    }
    return false;
#elif defined(__APPLE__)
    // The affinity "tag" is an arbitrary grouping hint, so the kernel happily
    // accepts nonsense values (and 0xFFFFFFFF + 1 wraps to the "no affinity"
    // tag). Validate the index ourselves to honor the out-of-range contract.
    if (coreIndex >= AllowedCores().size()) return false;
    thread_affinity_policy_data_t policy = { static_cast<integer_t>(coreIndex + 1) };
    thread_port_t thread = pthread_mach_thread_np(pthread_self());
    return thread_policy_set(thread, THREAD_AFFINITY_POLICY,
                              (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
    if (coreIndex >= static_cast<unsigned>(CPU_SETSIZE)) return false;
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(coreIndex, &cpuSet);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuSet) == 0;
#endif
}

} // namespace brazen
