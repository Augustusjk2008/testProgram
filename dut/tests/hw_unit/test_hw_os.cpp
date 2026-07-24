#include <gtest/gtest.h>

#include "MB_DDF_HW/Os/Fd.h"
#include "MB_DDF_HW/Os/MmapRegion.h"
#include "MB_DDF_HW/Os/Poll.h"

#include <unistd.h>

using namespace MB_DDF::HW;

TEST(HwOsFd, DefaultFdIsInvalid) {
    Os::Fd fd;
    EXPECT_FALSE(fd.valid());
    EXPECT_EQ(fd.get(), -1);
}

TEST(HwOsFd, MoveTransfersOwnership) {
    int pipe_fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(pipe_fds), 0);

    Os::Fd read_fd(pipe_fds[0]);
    Os::Fd moved(std::move(read_fd));
    ::close(pipe_fds[1]);

    EXPECT_FALSE(read_fd.valid());
    EXPECT_TRUE(moved.valid());
}

TEST(HwOsMmapRegion, InvalidFdMapFails) {
    auto region = Os::MmapRegion::map(-1, 4096, 0);
    EXPECT_FALSE(region);
    EXPECT_EQ(region.status().code, StatusCode::MapFailed);
}

TEST(HwOsPoll, InvalidFdReturnsError) {
    auto result = Os::Poll::wait_readable(-1, Timeout::poll());
    EXPECT_FALSE(result);
    EXPECT_EQ(result.status().code, StatusCode::IoError);
}
