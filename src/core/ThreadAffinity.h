#pragma once

// Best-effort pinning of the calling thread to a specific logical CPU
// core. This matters for benchmark consistency: an unpinned thread can
// get migrated between cores mid-run by the OS scheduler, which resets
// per-core cache state and can land on a core with a different
// turbo/boost clock than the one it started on -- both of which add
// run-to-run noise that has nothing to do with what's being measured.

#if defined(_WIN32)
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

// Returns true if the OS accepted the pin request.
//
// - Linux / Windows: this is a hard pin -- the scheduler will not run
//   the thread on any other core afterward.
// - macOS: the kernel only exposes an affinity *tag* (grouping hint via
//   thread_policy_set), not a guaranteed pin like Linux/Windows offer,
//   so treat a `true` return there as "requested" rather than "enforced".
inline bool PinCurrentThreadToCore(unsigned coreIndex) {
#if defined(_WIN32)
    DWORD_PTR mask = (DWORD_PTR)1 << coreIndex;
    return SetThreadAffinityMask(GetCurrentThread(), mask) != 0;
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy = { static_cast<integer_t>(coreIndex + 1) };
    thread_port_t thread = pthread_mach_thread_np(pthread_self());
    return thread_policy_set(thread, THREAD_AFFINITY_POLICY,
                              (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
#else
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    CPU_SET(coreIndex, &cpuSet);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuSet) == 0;
#endif
}

} // namespace brazen
