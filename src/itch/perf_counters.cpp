#include "itch/perf_counters.hpp"

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#  include <linux/perf_event.h>
#  include <sys/ioctl.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#endif

namespace itch {

#if defined(__linux__)

namespace {

int open_counter(uint64_t config, int group_fd) {
    perf_event_attr attr{};
    attr.size           = sizeof attr;
    attr.type           = PERF_TYPE_HARDWARE;
    attr.config         = config;
    attr.disabled       = group_fd == -1 ? 1 : 0;  // the leader starts the group
    attr.exclude_kernel = 1;                       // works with perf_event_paranoid <= 2
    attr.exclude_hv     = 1;
    attr.read_format    = PERF_FORMAT_GROUP;
    return static_cast<int>(::syscall(SYS_perf_event_open, &attr, 0 /* this thread */, -1 /* any cpu */,
                                      group_fd, 0UL));
}

} // namespace

PerfCounters::PerfCounters() {
    leader_ = open_counter(PERF_COUNT_HW_CPU_CYCLES, -1);
    if (leader_ < 0) {
        error_ = std::string("perf_event_open(cycles): ") + std::strerror(errno) +
                 (errno == EACCES || errno == EPERM ? " (check /proc/sys/kernel/perf_event_paranoid)" : "") +
                 (errno == ENOENT || errno == ENODEV || errno == EOPNOTSUPP ? " (no hardware PMU: VM or container?)" : "");
        return;
    }
    instr_ = open_counter(PERF_COUNT_HW_INSTRUCTIONS, leader_);
    if (instr_ < 0) {
        error_ = std::string("perf_event_open(instructions): ") + std::strerror(errno);
        close_all();
        return;
    }
    // Optional: some kernels/CPUs (notably some AMD setups) don't map
    // ref-cycles. Keep cycles + instructions (IPC) without it.
    ref_ = open_counter(PERF_COUNT_HW_REF_CPU_CYCLES, leader_);
    if (ref_ < 0) error_ = "ref-cycles unavailable: no frequency ratio";
    ::ioctl(leader_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ::ioctl(leader_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}

PerfCounters::~PerfCounters() { close_all(); }

void PerfCounters::close_all() noexcept {
    for (int* fd : {&instr_, &ref_, &leader_}) {
        if (*fd >= 0) ::close(*fd);
        *fd = -1;
    }
}

PerfCounters::Sample PerfCounters::read() const noexcept {
    if (leader_ < 0) return {};
    // PERF_FORMAT_GROUP: { nr, value[nr] } in the order the counters were
    // opened: cycles, instructions, then ref-cycles if it was available.
    uint64_t buf[1 + 3] = {};
    const ssize_t want = static_cast<ssize_t>(sizeof(uint64_t) * (ref_ >= 0 ? 4 : 3));
    if (::read(leader_, buf, sizeof buf) != want) return {};
    return {buf[1], ref_ >= 0 ? buf[3] : 0, buf[2]};
}

#else  // !__linux__

PerfCounters::PerfCounters() : error_("hardware counters need Linux perf_event_open") {}
PerfCounters::~PerfCounters() = default;
void PerfCounters::close_all() noexcept { leader_ = ref_ = instr_ = -1; }
PerfCounters::Sample PerfCounters::read() const noexcept { return {}; }

#endif

} // namespace itch
