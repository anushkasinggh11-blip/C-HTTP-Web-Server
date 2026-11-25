#ifndef METRICS_H
#define METRICS_H

#include "http.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

class ServerMetrics {
public:
    void accepted();
    void rejected();
    void queued(std::size_t count);
    void started();
    void finished(int status, std::chrono::microseconds elapsed);
    Response response() const;

private:
    const std::chrono::steady_clock::time_point startedAt_ =
        std::chrono::steady_clock::now();
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> waiting_{0};
    std::atomic<std::uint64_t> active_{0};
    std::atomic<std::uint64_t> finished_{0};
    std::atomic<std::uint64_t> withoutResponse_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> totalMicros_{0};
    std::atomic<std::uint64_t> peakMicros_{0};
    std::array<std::atomic<std::uint64_t>, 6> statusFamilies_{
        {0, 0, 0, 0, 0, 0}
    };
};

#endif
