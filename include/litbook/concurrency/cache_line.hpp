#pragma once

#include <cstddef>

namespace litbook::concurrency {

// The number of bytes we keep between two variables that different threads write, so
// they never share a cache line (false sharing).
//
// When two cores write different variables that happen to sit on the same cache line,
// the line has to move back and forth between the cores on every write, even though
// the threads never touch each other's data. Padding each such variable out to its own
// line stops that.
//
// We picked 128 on purpose instead of asking the compiler. On the Apple M3 Pro that
// litbook is developed on, the Linux VM reports 64 byte lines (getconf and the CTR_EL0
// register), but macOS reports hw.cachelinesize = 128 for the same chip. On x86-64 the
// lines are 64 bytes, but the L2 spatial prefetcher fetches lines in aligned pairs, so
// neighbors 64 bytes apart can still interfere. 128 is safe on all of these and only
// costs a few hundred bytes per queue.
//
// std::hardware_destructive_interference_size was rejected. GCC 15 and Clang 21 both
// say 256 on 64-bit ARM, the value can change with -mtune or the compiler version (so
// two files built differently could disagree about a struct's layout), and GCC warns
// when it is used in a header, which our -Werror turns into a build error. See D-048.
inline constexpr std::size_t kCacheLineSize = 128;

static_assert((kCacheLineSize & (kCacheLineSize - 1)) == 0, "must be a power of two");

}  // namespace litbook::concurrency
