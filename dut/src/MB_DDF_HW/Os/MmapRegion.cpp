#include "MB_DDF_HW/Os/MmapRegion.h"

#include <cerrno>
#include <sys/mman.h>

namespace MB_DDF::HW::Os {

MmapRegion::MmapRegion(void* data, size_t size) noexcept : data_(data), size_(size) {}

MmapRegion::~MmapRegion() {
    reset();
}

MmapRegion::MmapRegion(MmapRegion&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

MmapRegion& MmapRegion::operator=(MmapRegion&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

Result<MmapRegion> MmapRegion::map(int fd, size_t length, off_t offset) {
    if (length == 0) {
        return Status::error(StatusCode::InvalidArgument, 0, "mmap length must be non-zero");
    }

    void* mapped = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (mapped == MAP_FAILED) {
        return Status::error(StatusCode::MapFailed, errno, "mmap failed");
    }

    return MmapRegion(mapped, length);
}

bool MmapRegion::valid() const noexcept {
    return data_ != nullptr;
}

void* MmapRegion::data() noexcept {
    return data_;
}

const void* MmapRegion::data() const noexcept {
    return data_;
}

size_t MmapRegion::size() const noexcept {
    return size_;
}

void MmapRegion::reset() noexcept {
    if (data_ != nullptr) {
        ::munmap(data_, size_);
    }
    data_ = nullptr;
    size_ = 0;
}

} // namespace MB_DDF::HW::Os
