#include "litbook/pipeline/doorbell.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

namespace litbook::pipeline {

Doorbell::Doorbell() : fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (!fd_.valid()) {
        throw std::system_error(errno, std::generic_category(), "eventfd");
    }
}

void Doorbell::ring() noexcept {
    if (state_.exchange(kRung, std::memory_order_acq_rel) != kSleeping) {
        return;  // The consumer is awake and will look at its queue anyway.
    }
    const std::uint64_t one = 1;
    // A write can only fail if the counter is near its limit, and then the eventfd is
    // readable anyway. GCC with glibc's fortify checks refuses a (void) cast here, so the
    // result is checked explicitly.
    if (::write(fd_.get(), &one, sizeof(one)) < 0) {
        return;
    }
    wakeups_sent_.fetch_add(1, std::memory_order_relaxed);
}

bool Doorbell::prepare_to_sleep() noexcept {
    if (state_.exchange(kSleeping, std::memory_order_acq_rel) == kRung) {
        // Something was pushed since we last woke up. We stay awake. A producer that saw
        // Sleeping in the short time before this store wrote the eventfd, which only
        // costs one early wakeup later.
        state_.store(kAwake, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void Doorbell::sleep(int timeout_ms) noexcept {
    pollfd p{};
    p.fd = fd_.get();
    p.events = POLLIN;
    // A signal (EINTR) or a timeout just ends the sleep early, which is harmless because
    // the caller looks at its queue again.
    (void)::poll(&p, 1, timeout_ms);
    woke_up();
}

void Doorbell::woke_up() noexcept {
    state_.store(kAwake, std::memory_order_relaxed);
    // Reading resets the eventfd's counter. It fails with EAGAIN when nothing was
    // written, which is fine.
    std::uint64_t count = 0;
    if (::read(fd_.get(), &count, sizeof(count)) < 0) {
        return;
    }
}

}  // namespace litbook::pipeline
