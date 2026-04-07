#ifndef THREAD_AFFINITY_H
#define THREAD_AFFINITY_H

#include <thread>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace ThreadAffinity {

inline bool set_current_thread_affinity(int core_id) {
#ifdef __linux__
    if (core_id < 0) {
        return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)core_id;
    return false;
#endif
}

inline bool set_thread_affinity(std::thread& t, int core_id) {
#ifdef __linux__
    if (core_id < 0) {
        return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    return pthread_setaffinity_np(t.native_handle(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)t;
    (void)core_id;
    return false;
#endif
}

} // namespace ThreadAffinity

#endif // THREAD_AFFINITY_H
