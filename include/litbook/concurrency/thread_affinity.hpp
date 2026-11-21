#pragma once

#include <vector>

namespace litbook::concurrency {

// Helpers for pinning a thread to one CPU, so the scheduler stops moving it between
// cores (each move costs a cold cache). These are Linux functions.
//
// Inside the Docker Desktop VM a "CPU" is a virtual CPU. Pinning keeps a thread on one
// vCPU, but macOS still decides which physical core runs that vCPU, and it may be a
// performance core one moment and an efficiency core the next. Benchmarks that pin
// threads say so for that reason.

// Pins the calling thread to one CPU. Returns 0 on success, or the error number from
// the system (EINVAL for a CPU that is negative, too large, or not allowed for this
// process).
[[nodiscard]] int pin_current_thread(int cpu) noexcept;

// The CPUs this process is allowed to run on, in increasing order. Allocates, so call it
// at startup.
[[nodiscard]] std::vector<int> allowed_cpus();

// The CPU the calling thread is running on right now, or -1 if the system cannot say.
[[nodiscard]] int current_cpu() noexcept;

}  // namespace litbook::concurrency
