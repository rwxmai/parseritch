#pragma once

/// Page-aligned, zero-filled, move-only buffers for large hot tables.
///
/// Backed by anonymous mmap. On Linux the region is marked MADV_HUGEPAGE so
/// transparent huge pages can back it: a random-access table of hundreds of MB
/// then needs ~100x fewer TLB entries than with 4 KiB pages, which matters more
/// for hash-table lookup latency than any instruction-level trick.

#include <cstddef>
#include <utility>

namespace itch {

class LargeBuffer {
public:
    LargeBuffer() noexcept = default;
    /// Throws std::bad_alloc. With `prefault`, every page is faulted in now
    /// (after the huge-page advice), so the hot path never takes a first-touch
    /// page fault; that fault shows up directly in latency tails.
    explicit LargeBuffer(std::size_t bytes, bool prefault = true);
    ~LargeBuffer();

    LargeBuffer(LargeBuffer&& o) noexcept
        : ptr_(std::exchange(o.ptr_, nullptr)), bytes_(std::exchange(o.bytes_, 0)) {}
    LargeBuffer& operator=(LargeBuffer&& o) noexcept {
        if (this != &o) {
            release();
            ptr_   = std::exchange(o.ptr_, nullptr);
            bytes_ = std::exchange(o.bytes_, 0);
        }
        return *this;
    }
    LargeBuffer(const LargeBuffer&)            = delete;
    LargeBuffer& operator=(const LargeBuffer&) = delete;

    [[nodiscard]] void* data() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_; }

    template <class T>
    [[nodiscard]] T* as() const noexcept { return static_cast<T*>(ptr_); }

private:
    void release() noexcept;

    void*       ptr_   = nullptr;
    std::size_t bytes_ = 0;
};

} // namespace itch
