#include "net/itch_file.hpp"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace net {

ItchFile::~ItchFile() {
    if (data_ != nullptr) ::munmap(const_cast<uint8_t*>(data_), size_);
}

std::string ItchFile::open(const char* path, bool populate) {
    if (data_ != nullptr) {
        ::munmap(const_cast<uint8_t*>(data_), size_);
        data_ = nullptr;
        size_ = 0;
    }
    const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::string("open: ") + std::strerror(errno);

    struct ::stat st {};
    if (::fstat(fd, &st) != 0) {
        const int e = errno;
        ::close(fd);
        return std::string("fstat: ") + std::strerror(e);
    }
    if (st.st_size <= 0) {
        ::close(fd);
        return "file is empty";
    }
    const auto len = static_cast<std::size_t>(st.st_size);

    int flags = MAP_PRIVATE;
#if defined(MAP_POPULATE)
    if (populate) flags |= MAP_POPULATE;
#else
    (void)populate;
#endif
    void* p = ::mmap(nullptr, len, PROT_READ, flags, fd, 0);
    const int map_errno = errno;
    ::close(fd);  // the mapping keeps its own reference to the file
    if (p == MAP_FAILED) return std::string("mmap: ") + std::strerror(map_errno);

    // Two separate calls: the MADV_* values are enumerators, not bit flags.
    (void)::madvise(p, len, MADV_SEQUENTIAL);
    (void)::madvise(p, len, MADV_WILLNEED);

    data_ = static_cast<const uint8_t*>(p);
    size_ = len;
    return {};
}

} // namespace net
