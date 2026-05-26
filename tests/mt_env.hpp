#pragma once
// Shared knob for the multithreaded stress tests. They default to heavy runs
// (wide race windows for local + TSan), but CI can shrink them via env vars:
//
//   RETINA_MT_FRAMES     frames the producer publishes   (default per test)
//   RETINA_MT_BYTES      bytes per frame                 (default per test)
//   RETINA_MT_CONSUMERS  consumer threads (latest_value) (default per test)
//
// Keeping these runtime (not compile-time) means one build serves both a
// thorough local run and a fast CI run — no separate configuration.

#include <cstdint>
#include <cstdlib>

namespace retina_test {

// Read an unsigned env var, falling back to `fallback` if unset/empty/non-numeric.
inline uint64_t env_u64(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) return fallback;   // not a number → keep the default
    return static_cast<uint64_t>(parsed);
}

}  // namespace retina_test
