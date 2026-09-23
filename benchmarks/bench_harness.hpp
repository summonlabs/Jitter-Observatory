// Jitter Observatory - benchmark harness.
// Copyright 2026 Summon Software Labs.
#pragma once

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace jitter::bench {

struct Measurement {
    std::string name;
    std::uint64_t iterations = 0;
    std::uint64_t items = 0;
    double elapsed_ns = 0.0;
    double ns_per_item = 0.0;
    double items_per_second = 0.0;
    std::uint64_t checksum = 0;
};

inline std::vector<Measurement>& measurements() {
    static std::vector<Measurement> instance;
    return instance;
}

// Runs fn(iteration) exactly iterations times, measures the elapsed wall time and
// records how many work items were actually completed.
template <class Fn>
void run(const std::string& name, std::uint64_t iterations, std::uint64_t items_per_iteration,
         Fn&& fn) {
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        checksum = (checksum * 1000003ull) ^ fn(i);
    }
    const auto finish = std::chrono::steady_clock::now();
    const double elapsed_ns =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());

    Measurement measurement;
    measurement.name = name;
    measurement.iterations = iterations;
    measurement.items = iterations * items_per_iteration;
    measurement.elapsed_ns = elapsed_ns;
    measurement.ns_per_item =
        measurement.items == 0 ? 0.0 : elapsed_ns / static_cast<double>(measurement.items);
    measurement.items_per_second =
        elapsed_ns <= 0.0 ? 0.0 : static_cast<double>(measurement.items) * 1e9 / elapsed_ns;
    measurement.checksum = checksum;
    measurements().push_back(measurement);
}

inline void report() {
    std::cout << std::left << std::setw(38) << "benchmark" << std::right << std::setw(12)
              << "iterations" << std::setw(14) << "items" << std::setw(14) << "ns/item"
              << std::setw(16) << "items/second" << std::setw(20) << "checksum" << "\n";
    for (const Measurement& measurement : measurements()) {
        std::cout << std::left << std::setw(38) << measurement.name << std::right << std::setw(12)
                  << measurement.iterations << std::setw(14) << measurement.items << std::setw(14)
                  << std::fixed << std::setprecision(2) << measurement.ns_per_item << std::setw(16)
                  << std::defaultfloat << std::setprecision(4) << measurement.items_per_second
                  << std::setw(20) << measurement.checksum << "\n";
    }
}

}  // namespace jitter::bench
