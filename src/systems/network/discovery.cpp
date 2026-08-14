#include <corona/systems/network/discovery.h>
#include <corona/kernel/core/i_logger.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")
using socklen_t = int;
static int last_error() { return WSAGetLastError(); }
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <ifaddrs.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR   (-1)
using SOCKET = int;
static int last_error() { return errno; }
#endif

#include <algorithm>
#include <cstring>
#include <vector>
#include <thread>

namespace Corona::Network {

struct Discovery::Impl {
    SOCKET sock = INVALID_SOCKET;
    uint16_t port = kDefaultPort;
    uint64_t project_id = 0;
    DiscoveryPacket outgoing_packet;
    OnPeerDiscovered callback;

    std::atomic<bool> running{false};
    std::thread broadcast_thread;
    std::vector<struct sockaddr_in> broadcast_addrs;
    struct sockaddr_in listen_addr{};

#ifdef _WIN32
    bool wsa_initialized = false;
#endif

    ~Impl() {
        stop();
    }

    bool init_sockets() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_initialized = true;
#endif
        return true;
    }

    void cleanup_sockets() {
#ifdef _WIN32
        if (wsa_initialized) {
            WSACleanup();
            wsa_initialized = false;
        }
#endif
    }

    bool create_socket() {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            CFW_LOG_ERROR("Discovery: socket() failed, errno={}", last_error());
            return false;
        }

        // Allow multiple instances on the same port (SO_REUSEADDR)
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        // Enable broadcast
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&broadcast), sizeof(broadcast));

        // Non-blocking
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
#else
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

        // Bind
        std::memset(&listen_addr, 0, sizeof(listen_addr));
        listen_addr.sin_family = AF_INET;
        listen_addr.sin_port = htons(port);
        listen_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock, reinterpret_cast<struct sockaddr*>(&listen_addr),
                 sizeof(listen_addr)) == SOCKET_ERROR) {
            int err = last_error();
            CFW_LOG_ERROR("Discovery: bind(port={}) failed, errno={}", port, err);
            return false;
        }

        broadcast_addrs.clear();
        const auto add_broadcast = [&](uint32_t address) {
            if (std::any_of(broadcast_addrs.begin(), broadcast_addrs.end(),
                            [address](const sockaddr_in& existing) {
                                return existing.sin_addr.s_addr == address;
                            })) {
                return;
            }
            sockaddr_in target{};
            target.sin_family = AF_INET;
            target.sin_port = htons(port);
            target.sin_addr.s_addr = address;
            broadcast_addrs.push_back(target);
        };

        // Keep limited broadcast as a fallback, then add directed broadcasts
        // for active interfaces so Wi-Fi/AP isolation and routed LANs still
        // receive discovery traffic.
        add_broadcast(INADDR_BROADCAST);
#ifdef _WIN32
        ULONG size = 16 * 1024;
        std::vector<uint8_t> buffer(size);
        ULONG result = GetAdaptersAddresses(
            AF_INET, 0, nullptr,
            reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
        if (result == ERROR_BUFFER_OVERFLOW) {
            buffer.resize(size);
            result = GetAdaptersAddresses(
                AF_INET, 0, nullptr,
                reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
        }
        if (result == NO_ERROR) {
            for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
                 adapter; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp ||
                    adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                    continue;
                }
                for (auto* unicast = adapter->FirstUnicastAddress;
                     unicast; unicast = unicast->Next) {
                    if (!unicast->Address.lpSockaddr ||
                        unicast->Address.lpSockaddr->sa_family != AF_INET) {
                        continue;
                    }
                    const auto* address = reinterpret_cast<const sockaddr_in*>(
                        unicast->Address.lpSockaddr);
                    const auto host = ntohl(address->sin_addr.s_addr);
                    const auto prefix = std::min<ULONG>(unicast->OnLinkPrefixLength, 32);
                    const uint32_t mask = prefix == 0 ? 0u : 0xffffffffu << (32 - prefix);
                    add_broadcast(htonl(host | ~mask));
                }
            }
        }
#else
        ifaddrs* interfaces = nullptr;
        if (getifaddrs(&interfaces) == 0) {
            for (auto* it = interfaces; it; it = it->ifa_next) {
                if (!it->ifa_addr || !it->ifa_netmask ||
                    it->ifa_addr->sa_family != AF_INET) {
                    continue;
                }
                const auto* address = reinterpret_cast<const sockaddr_in*>(it->ifa_addr);
                const auto* mask = reinterpret_cast<const sockaddr_in*>(it->ifa_netmask);
                add_broadcast(address->sin_addr.s_addr | ~mask->sin_addr.s_addr);
            }
            freeifaddrs(interfaces);
        }
#endif

        return true;
    }

    void close_socket() {
        if (sock != INVALID_SOCKET) {
#ifdef _WIN32
            closesocket(sock);
#else
            ::close(sock);
#endif
            sock = INVALID_SOCKET;
        }
    }

    bool stop() {
        bool expected = true;
        if (!running.compare_exchange_strong(expected, false)) return false;

        // Shutdown the socket first so broadcast thread's sendto() unblocks
        close_socket();

        if (broadcast_thread.joinable()) {
            broadcast_thread.join();
        }
        cleanup_sockets();
        return true;
    }
};

Discovery::Discovery() : impl_(std::make_unique<Impl>()) {}
Discovery::~Discovery() { impl_->stop(); }

bool Discovery::start(uint16_t port, const std::string& instance_name,
                      uint64_t project_id, uint16_t listen_port,
                      uint8_t session_role) {
    if (impl_->running.load()) return true; // already running

    if (!impl_->init_sockets()) {
        CFW_LOG_ERROR("Discovery: Failed to initialize sockets");
        return false;
    }

    impl_->port = port;
    impl_->project_id = project_id;

    if (!impl_->create_socket()) {
        CFW_LOG_ERROR("Discovery: Failed to create UDP socket on port {}", port);
        impl_->cleanup_sockets();
        return false;
    }

    // Fill outgoing discovery packet
    impl_->outgoing_packet = DiscoveryPacket{};
    impl_->outgoing_packet.protocol_version = kDiscoveryProtocolVersion;
    impl_->outgoing_packet.project_id = project_id;
    impl_->outgoing_packet.listen_port = listen_port;
    impl_->outgoing_packet.session_role = session_role;
    std::strncpy(impl_->outgoing_packet.instance_name, instance_name.c_str(),
                 sizeof(impl_->outgoing_packet.instance_name) - 1);

    impl_->running.store(true);

    // Start broadcast thread
    impl_->broadcast_thread = std::thread([this]() {
        int elapsed = 0;
        while (impl_->running.load()) {
            for (const auto& target : impl_->broadcast_addrs) {
                const auto sent = sendto(
                    impl_->sock,
                    reinterpret_cast<const char*>(&impl_->outgoing_packet),
                    sizeof(DiscoveryPacket), 0,
                    reinterpret_cast<const struct sockaddr*>(&target),
                    sizeof(target));
                if (sent == SOCKET_ERROR) {
                    CFW_LOG_DEBUG("Discovery: broadcast send failed, errno={}", last_error());
                }
            }

            // Sleep in short chunks so stop() joins quickly instead of
            // waiting up to a full kDiscoveryIntervalMs.
            elapsed = 0;
            while (elapsed < kDiscoveryIntervalMs && impl_->running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                elapsed += 50;
            }
        }
    });
    return true;
}

void Discovery::stop() {
    impl_->stop();
}

void Discovery::set_on_peer_discovered(OnPeerDiscovered cb) {
    impl_->callback = std::move(cb);
}

void Discovery::poll() {
    if (impl_->sock == INVALID_SOCKET || !impl_->running.load()) return;

    DiscoveryPacket incoming;
    struct sockaddr_in sender_addr{};
    socklen_t sender_len = sizeof(sender_addr);

    // Drain all pending broadcast packets
    for (;;) {
        int recvd = recvfrom(
            impl_->sock,
            reinterpret_cast<char*>(&incoming), sizeof(incoming), 0,
            reinterpret_cast<struct sockaddr*>(&sender_addr), &sender_len);

        if (recvd <= 0) break; // No more data or error

        if (static_cast<size_t>(recvd) < sizeof(DiscoveryPacket)) continue;

        // Validate magic
        if (std::strncmp(incoming.magic, "CORONA", 6) != 0) continue;

        if (incoming.protocol_version != kDiscoveryProtocolVersion ||
            incoming.listen_port == 0) continue;

        // Filter: only same project
        if (incoming.project_id != impl_->project_id) continue;

        // Filter: ignore own broadcast
        // (Our own broadcast is also received; skip payloads that match our instance name)
        if (std::strncmp(incoming.instance_name,
                         impl_->outgoing_packet.instance_name,
                         sizeof(incoming.instance_name)) == 0) {
            continue;
        }

        // Get sender IP
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));

        std::string name(incoming.instance_name,
            strnlen(incoming.instance_name, sizeof(incoming.instance_name)));

        if (impl_->callback) {
            impl_->callback(ip_str, name, incoming.project_id,
                            incoming.listen_port, incoming.session_role);
        }
    }
}

}  // namespace Corona::Network
