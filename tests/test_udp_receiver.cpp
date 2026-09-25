#include "net/udp_receiver.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

TEST(UdpReceiver, ReceivesUnicastDatagramsOnLoopback) {
    net::UdpReceiver rx;
    net::UdpReceiver::Options opt;
    opt.port = 0;  // ephemeral
    ASSERT_EQ(rx.open(opt), "");
    ASSERT_NE(rx.bound_port(), 0);

    const int tx = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(tx, 0);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(rx.bound_port());
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 3; ++i) {
        const char payload[4] = {'M', 'O', 'L', static_cast<char>('0' + i)};
        ASSERT_EQ(::sendto(tx, payload, sizeof payload, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to), 4);
    }
    ::close(tx);

    int received = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received < 3 && std::chrono::steady_clock::now() < deadline) {
        const std::size_t n = rx.poll();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& p = rx.packet(i);
            ASSERT_EQ(p.len, 4u);
            EXPECT_EQ(std::memcmp(p.data, "MOL", 3), 0);
            EXPECT_EQ(p.data[3], '0' + received);
            ++received;
        }
        if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(received, 3);
}

TEST(UdpReceiver, RejectsInvalidGroup) {
    net::UdpReceiver rx;
    net::UdpReceiver::Options opt;
    opt.group = "not-an-ip";
    EXPECT_NE(rx.open(opt), "");
}

} // namespace
