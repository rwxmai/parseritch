#include "itch/platform.hpp"

#include <benchmark/benchmark.h>

#include <cstdio>

int main(int argc, char** argv) {
    itch::check_cpu_or_exit();
    benchmark::AddCustomContext("itch_simd_level", itch::simd_level_name());
    if (itch::running_under_rosetta()) {
        benchmark::AddCustomContext("WARNING", "running under Rosetta 2 translation: timings are NOT representative");
        std::fprintf(stderr,
                     "\n*** Running under Rosetta 2: x86 code is being translated to ARM.\n"
                     "*** Use these numbers for smoke-testing only; benchmark on native x86-64.\n\n");
    }
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
