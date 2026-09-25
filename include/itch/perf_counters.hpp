#pragma once

/// Hardware counters for the calling thread via Linux perf_event_open:
/// core cycles, reference cycles and retired instructions, read as one group.
///
///   frequency ratio = cycles / ref_cycles
///
/// Reference cycles tick at the nominal (TSC) rate while the core is unhalted,
/// so the ratio is the effective frequency relative to nominal: above 1 means
/// turbo, a drop during a burst means the core was downclocked (e.g. an AVX
/// frequency license). These are generic perf events (no model-specific
/// encodings), unlike the license events that tools/license_check.sh reads
/// through perf(1).
///
/// Unavailable (available() == false, error() says why) on non-Linux systems,
/// with perf_event_paranoid > 2, or in VMs/containers without a virtual PMU.
/// ref-cycles is optional: where the kernel doesn't provide it, cycles and
/// instructions (IPC) still work, ref_cycles reads as 0, frequency_ratio()
/// returns 0 and error() says so.

#include <cstdint>
#include <string>

namespace itch {

class PerfCounters {
public:
    struct Sample {
        uint64_t cycles       = 0;
        uint64_t ref_cycles   = 0;
        uint64_t instructions = 0;

        [[nodiscard]] double ipc() const noexcept {
            return cycles ? static_cast<double>(instructions) / static_cast<double>(cycles) : 0.0;
        }
        [[nodiscard]] double frequency_ratio() const noexcept {
            return ref_cycles ? static_cast<double>(cycles) / static_cast<double>(ref_cycles) : 0.0;
        }
        Sample operator-(const Sample& o) const noexcept {
            return {cycles - o.cycles, ref_cycles - o.ref_cycles, instructions - o.instructions};
        }
    };

    PerfCounters();   ///< opens and enables the counter group (user space only)
    ~PerfCounters();
    PerfCounters(const PerfCounters&)            = delete;
    PerfCounters& operator=(const PerfCounters&) = delete;

    [[nodiscard]] bool available() const noexcept { return leader_ >= 0; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Current cumulative counts; subtract two samples for an interval.
    [[nodiscard]] Sample read() const noexcept;

private:
    void close_all() noexcept;

    int leader_ = -1;
    int ref_    = -1;
    int instr_  = -1;
    std::string error_;
};

} // namespace itch
