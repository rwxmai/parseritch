#include "itch/memory.hpp"

#include <new>

#include <sys/mman.h>
#include <unistd.h>

namespace itch {

LargeBuffer::LargeBuffer(std::size_t bytes, bool prefault) : bytes_(bytes) {
    if (bytes == 0) return;
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
#if defined(MADV_HUGEPAGE)
    // Advisory only: ignore failure (THP may be disabled system-wide). Must
    // come before the pages are touched, or they are faulted in as 4 KiB pages
    // (the reason not to use MAP_POPULATE here).
    (void)::madvise(p, bytes, MADV_HUGEPAGE);
#endif
    if (prefault) {
        bool populated = false;
#if defined(MADV_POPULATE_WRITE)  // Linux 5.14+
        populated = ::madvise(p, bytes, MADV_POPULATE_WRITE) == 0;
#endif
        if (!populated) {
            const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            auto* b = static_cast<volatile unsigned char*>(p);
            for (std::size_t off = 0; off < bytes; off += page) b[off] = 0;
        }
    }
    ptr_ = p;
}

LargeBuffer::~LargeBuffer() { release(); }

void LargeBuffer::release() noexcept {
    if (ptr_ != nullptr) {
        ::munmap(ptr_, bytes_);
        ptr_   = nullptr;
        bytes_ = 0;
    }
}

} // namespace itch
