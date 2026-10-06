#include <corona/systems/network/discovery.h>

#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void expect_true(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_two_discovery_instances_exchange_endpoint_metadata() {
    Corona::Network::Discovery host;
    Corona::Network::Discovery client;
    struct Seen {
        std::string ip;
        std::string name;
        uint64_t project_id = 0;
        uint16_t port = 0;
        uint8_t role = 0;
    };
    std::vector<Seen> seen;
    host.set_on_peer_discovered([&](const std::string& ip, const std::string& name,
                                    uint64_t project_id, uint16_t port, uint8_t role) {
        seen.push_back({ip, name, project_id, port, role});
    });

    constexpr uint16_t discovery_port = 39191;
    constexpr uint64_t project_id = 0xCAFE;
    expect_true(host.start(discovery_port, "discovery-host", project_id, 39201, 1),
                "host discovery starts");
    expect_true(client.start(discovery_port, "discovery-client", project_id, 39202, 2),
                "client discovery starts");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && seen.empty()) {
        host.poll();
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    expect_true(!seen.empty(), "discovery receives a peer advertisement");
    if (!seen.empty()) {
        expect_true(!seen.front().ip.empty(), "discovery includes sender IP");
        expect_true(seen.front().name == "discovery-client",
                    "discovery includes instance name");
        expect_true(seen.front().project_id == project_id,
                    "discovery includes project id");
        expect_true(seen.front().port == 39202,
                    "discovery includes ENet listen port");
        expect_true(seen.front().role == 2,
                    "discovery includes session role");
    }

    client.stop();
    host.stop();
}

}  // namespace

int main() {
    test_two_discovery_instances_exchange_endpoint_metadata();
    return failures == 0 ? 0 : 1;
}
