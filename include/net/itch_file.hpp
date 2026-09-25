#pragma once

/// Memory-mapped Nasdaq ITCH binary file (the format of the sample files at
/// emi.nasdaq.com, after gunzip): a plain sequence of
/// [u16 big-endian length][message] records.
///
/// Replaces the old PcapReplayer, which (despite the name) read the same
/// format but claimed huge pages it could never get (MAP_HUGETLB fails on
/// regular files) and passed MADV_SEQUENTIAL | MADV_WILLNEED as one bit-OR of
/// two enum values, which is just MADV_WILLNEED.

#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

class ItchFile {
public:
    ItchFile() = default;
    ~ItchFile();
    ItchFile(const ItchFile&)            = delete;
    ItchFile& operator=(const ItchFile&) = delete;

    /// Map `path` read-only. `populate` pre-faults every page (Linux
    /// MAP_POPULATE), so a timed replay never takes a page fault. Returns an
    /// empty string on success, otherwise an error description.
    [[nodiscard]] std::string open(const char* path, bool populate = true);

    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }

private:
    const uint8_t* data_ = nullptr;
    std::size_t    size_ = 0;
};

} // namespace net
