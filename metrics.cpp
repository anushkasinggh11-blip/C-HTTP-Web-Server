#include "metrics.h"

#include <string>
#include <utility>

void ServerMetrics::accepted() {
    accepted_.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::rejected() {
    rejected_.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::queued(std::size_t count) {
    waiting_.store(count, std::memory_order_relaxed);
}

void ServerMetrics::started() {
    active_.fetch_add(1, std::memory_order_relaxed);
}

void ServerMetrics::finished(int status, std::chrono::microseconds elapsed) {
    if (status == 408) {
        timeouts_.fetch_add(1, std::memory_order_relaxed);
    }
    if (status >= 100 && status <= 599) {
        statusFamilies_[static_cast<std::size_t>(status / 100)]
            .fetch_add(1, std::memory_order_relaxed);
    } else {
        withoutResponse_.fetch_add(1, std::memory_order_relaxed);
    }

    const auto micros = static_cast<std::uint64_t>(elapsed.count());
    totalMicros_.fetch_add(micros, std::memory_order_relaxed);
    auto peak = peakMicros_.load(std::memory_order_relaxed);
    while (peak < micros &&
           !peakMicros_.compare_exchange_weak(peak, micros, std::memory_order_relaxed)) {
    }
    finished_.fetch_add(1, std::memory_order_relaxed);
    active_.fetch_sub(1, std::memory_order_relaxed);
}

Response ServerMetrics::response() const {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt_).count();
    const auto count = finished_.load(std::memory_order_relaxed);
    const auto elapsed = totalMicros_.load(std::memory_order_relaxed);
    const auto average = count == 0 ? 0 : elapsed / count;

    std::string output = "{\n";
    output += "  \"uptime_seconds\": " + std::to_string(seconds) + ",\n";
    const auto append = [&output](std::string_view key, std::uint64_t value) {
        output += "  \"";
        output += key;
        output += "\": ";
        output += std::to_string(value);
        output += ",\n";
    };

    append("accepted", accepted_.load(std::memory_order_relaxed));
    append("queue_rejected", rejected_.load(std::memory_order_relaxed));
    append("queued", waiting_.load(std::memory_order_relaxed));
    append("active", active_.load(std::memory_order_relaxed));
    append("finished", count);
    append("without_response", withoutResponse_.load(std::memory_order_relaxed));
    append("timed_out", timeouts_.load(std::memory_order_relaxed));
    append("average_worker_microseconds", average);
    append("peak_worker_microseconds", peakMicros_.load(std::memory_order_relaxed));
    output += "  \"status_families\": {\n";
    for (std::size_t family = 2; family <= 5; ++family) {
        output += "    \"" + std::to_string(family) + "xx\": " +
                  std::to_string(statusFamilies_[family].load(std::memory_order_relaxed));
        output += family == 5 ? "\n" : ",\n";
    }
    output += "  }\n}\n";

    return {"200 OK", "application/json; charset=utf-8", std::move(output),
            "Cache-Control: no-store\r\n"};
}
