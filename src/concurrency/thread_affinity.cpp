#include "litbook/concurrency/thread_affinity.hpp"

#include <pthread.h>
#include <sched.h>

#include <cerrno>
#include <cstddef>
#include <vector>

namespace litbook::concurrency {

int pin_current_thread(int cpu) noexcept {
    // CPU_SET writes outside the set for a CPU number past CPU_SETSIZE, so we refuse
    // those ourselves instead of letting the macro do it.
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return EINVAL;
    }
    // The CPU_SET macros store the CPU number in a size_t, so we convert it here, where
    // we already know it is not negative.
    const auto index = static_cast<std::size_t>(cpu);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(index, &set);
    // pthread_setaffinity_np returns the error number instead of setting errno.
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

std::vector<int> allowed_cpus() {
    cpu_set_t set;
    CPU_ZERO(&set);
    std::vector<int> cpus;
    if (sched_getaffinity(0, sizeof(set), &set) != 0) {
        return cpus;
    }
    for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &set)) {
            cpus.push_back(static_cast<int>(cpu));
        }
    }
    return cpus;
}

int current_cpu() noexcept {
    return sched_getcpu();
}

}  // namespace litbook::concurrency
