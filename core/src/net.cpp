#include "archon/net.hpp"

#include <chrono>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace arc::net {

#if defined(_WIN32)
const socket_t kInvalidSocket = INVALID_SOCKET;
static int last_error() { return WSAGetLastError(); }
static bool would_block(int e) { return e == WSAEWOULDBLOCK; }
#else
const socket_t kInvalidSocket = -1;
static int last_error() { return errno; }
static bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS; }
#endif

bool init() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
#if defined(_WIN32)
        WSADATA d;
        ok = WSAStartup(MAKEWORD(2, 2), &d) == 0;
#else
        signal(SIGPIPE, SIG_IGN);
        ok = true;
#endif
    });
    return ok;
}

void close_socket(socket_t s) {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    closesocket(s);
#else
    ::close(s);
#endif
}

void shutdown_send(socket_t s) {
    if (s == kInvalidSocket) return;
#if defined(_WIN32)
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
}

static bool set_nonblocking(socket_t s, bool on) {
#if defined(_WIN32)
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

static void tune_stream(socket_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
#if defined(__APPLE__)
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

static std::string describe_error(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(last_error()) + ")";
}

socket_t listen_tcp(uint16_t port, bool loopback_only, uint16_t* bound_port, std::string* err) {
    init();
    // Prefer an IPv6 dual-stack socket so IPv4 and IPv6 controllers both work.
    // Loopback-only binds 127.0.0.1: "::1" would not accept IPv4 loopback.
    socket_t s = loopback_only ? kInvalidSocket : ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    bool v6 = s != kInvalidSocket;
    if (v6) {
        int off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
    } else {
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == kInvalidSocket) {
            if (err) *err = describe_error("socket");
            return kInvalidSocket;
        }
    }
#if defined(_WIN32)
    int excl = 1;  // no port hijacking by other processes
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
#else
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
    int rc;
    if (v6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_port = htons(port);
        a.sin6_addr = loopback_only ? in6addr_loopback : in6addr_any;
        rc = ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    } else {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(loopback_only ? INADDR_LOOPBACK : INADDR_ANY);
        rc = ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    if (rc != 0 || ::listen(s, 16) != 0) {
        if (err) *err = describe_error("bind/listen");
        close_socket(s);
        return kInvalidSocket;
    }
    if (bound_port) {
        sockaddr_storage ss{};
        socklen_t len = sizeof ss;
        getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len);
        *bound_port = ss.ss_family == AF_INET6
                          ? ntohs(reinterpret_cast<sockaddr_in6*>(&ss)->sin6_port)
                          : ntohs(reinterpret_cast<sockaddr_in*>(&ss)->sin_port);
    }
    set_nonblocking(s, true);
    return s;
}

socket_t accept_tcp(socket_t listener, std::string* peer) {
    sockaddr_storage ss{};
    socklen_t len = sizeof ss;
    socket_t c = ::accept(listener, reinterpret_cast<sockaddr*>(&ss), &len);
    if (c == kInvalidSocket) return kInvalidSocket;
    set_nonblocking(c, true);
    tune_stream(c);
    if (peer) {
        char host[NI_MAXHOST] = {0};
        if (getnameinfo(reinterpret_cast<sockaddr*>(&ss), len, host, sizeof host, nullptr, 0,
                        NI_NUMERICHOST) == 0)
            *peer = host;
    }
    return c;
}

socket_t connect_tcp(const std::string& host, uint16_t port, int timeout_ms, std::string* err) {
    init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    const std::string port_s = std::to_string(port);
    // An interface suffix ("192.168.0.5%en0", as some platform APIs print
    // addresses) is meaningless for IPv4 and makes getaddrinfo treat the
    // literal as a host name. Keep it for IPv6, where it is the scope id.
    std::string target = host;
    if (const size_t pct = target.find('%'); pct != std::string::npos) {
        in_addr v4{};
        if (inet_pton(AF_INET, target.substr(0, pct).c_str(), &v4) == 1) target.resize(pct);
    }
    if (getaddrinfo(target.c_str(), port_s.c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "cannot resolve " + host;
        return kInvalidSocket;
    }
    socket_t result = kInvalidSocket;
    for (addrinfo* ai = res; ai && result == kInvalidSocket; ai = ai->ai_next) {
        socket_t s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == kInvalidSocket) continue;
        set_nonblocking(s, true);
        int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool connected = rc == 0;
        if (!connected && would_block(last_error())) {
            std::vector<PollItem> items{{s, false, true}};
            if (poll(items, timeout_ms) > 0 && items[0].writable && !items[0].failed) {
                int so_err = 0;
                socklen_t l = sizeof so_err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &l);
                connected = so_err == 0;
            }
        }
        if (connected) {
            tune_stream(s);
            result = s;
        } else {
            close_socket(s);
        }
    }
    freeaddrinfo(res);
    if (result == kInvalidSocket && err) *err = "cannot connect to " + host + ":" + port_s;
    return result;
}

long send_some(socket_t s, const uint8_t* p, size_t n) {
#if defined(_WIN32)
    int r = ::send(s, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
#elif defined(MSG_NOSIGNAL)
    ssize_t r = ::send(s, p, n, MSG_NOSIGNAL);
#else
    ssize_t r = ::send(s, p, n, 0);
#endif
    if (r >= 0) return long(r);
    return would_block(last_error()) ? kWouldBlock : kIoError;
}

long recv_some(socket_t s, uint8_t* p, size_t n) {
#if defined(_WIN32)
    int r = ::recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
#else
    ssize_t r = ::recv(s, p, n, 0);
#endif
    if (r >= 0) return long(r);
    int e = last_error();
#if !defined(_WIN32)
    if (e == EINTR) return kWouldBlock;
#endif
    return would_block(e) ? kWouldBlock : kIoError;
}

int poll(std::vector<PollItem>& items, int timeout_ms) {
#if defined(_WIN32)
    std::vector<WSAPOLLFD> fds(items.size());
#else
    std::vector<pollfd> fds(items.size());
#endif
    for (size_t i = 0; i < items.size(); ++i) {
        fds[i].fd = items[i].fd;
        fds[i].events = short((items[i].want_read ? POLLIN : 0) | (items[i].want_write ? POLLOUT : 0));
        fds[i].revents = 0;
    }
#if defined(_WIN32)
    int rc = WSAPoll(fds.data(), ULONG(fds.size()), timeout_ms);
#else
    int rc = ::poll(fds.data(), nfds_t(fds.size()), timeout_ms);
    if (rc < 0 && errno == EINTR) rc = 0;
#endif
    for (size_t i = 0; i < items.size(); ++i) {
        const short re = fds[i].revents;
        items[i].readable = (re & (POLLIN | POLLHUP)) != 0;
        items[i].writable = (re & POLLOUT) != 0;
        items[i].failed = (re & (POLLERR | POLLNVAL)) != 0;
        // A hang-up with no data still has to be observed through recv() == 0.
    }
    return rc;
}

Waker::Waker() : rx_(kInvalidSocket), tx_(kInvalidSocket) {
    init();
    rx_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    tx_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx_ == kInvalidSocket || tx_ == kInvalidSocket) {
        close_socket(rx_);
        close_socket(tx_);
        rx_ = tx_ = kInvalidSocket;
        return;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t len = sizeof a;
    if (::bind(rx_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        getsockname(rx_, reinterpret_cast<sockaddr*>(&a), &len) != 0 ||
        ::connect(tx_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        close_socket(rx_);
        close_socket(tx_);
        rx_ = tx_ = kInvalidSocket;
        return;
    }
    set_nonblocking(rx_, true);
    set_nonblocking(tx_, true);
}

Waker::~Waker() {
    close_socket(rx_);
    close_socket(tx_);
}

void Waker::wake() {
    if (tx_ == kInvalidSocket) return;
    const char b = 1;
    ::send(tx_, &b, 1, 0);
}

void Waker::drain() {
    char buf[64];
    while (::recv(rx_, buf, sizeof buf, 0) > 0) {
    }
}

bool is_local_network_address(const std::string& numeric_host) {
    std::string h = numeric_host;
    const auto pct = h.find('%');
    if (pct != std::string::npos) h.resize(pct);  // drop IPv6 zone id
    if (h.rfind("::ffff:", 0) == 0 && h.find('.') != std::string::npos) h = h.substr(7);
    in_addr a4{};
    if (inet_pton(AF_INET, h.c_str(), &a4) == 1) {
        const uint32_t ip = ntohl(a4.s_addr);
        const auto in = [&](uint32_t net, int bits) { return (ip >> (32 - bits)) == (net >> (32 - bits)); };
        return in(0x0A000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) || in(0xA9FE0000, 16) ||
               in(0x7F000000, 8) || in(0x64400000, 10);
    }
    in6_addr a6{};
    if (inet_pton(AF_INET6, h.c_str(), &a6) == 1) {
        const uint8_t* b = a6.s6_addr;
        bool loopback = b[15] == 1;
        for (int i = 0; i < 15 && loopback; ++i) loopback = b[i] == 0;
        return loopback || (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) || (b[0] & 0xFE) == 0xFC;
    }
    return false;
}

std::string host_name() {
    init();
    char buf[256] = {0};
    if (gethostname(buf, sizeof buf - 1) != 0) return "Computer";
    return buf;
}

}  // namespace arc::net
