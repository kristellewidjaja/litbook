#pragma once

namespace litbook::concurrency {

// Tells the CPU that the calling thread is spinning in a wait loop. On ARM this is the
// `yield` instruction and on x86 it is `pause`. Both let the core save power and give a
// sibling hardware thread more room, and `pause` also stops the x86 core from guessing
// wrongly about the loop's memory reads when it finally exits. It does not enter the
// kernel and does not give the CPU to another thread.
inline void cpu_relax() noexcept {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

}  // namespace litbook::concurrency
