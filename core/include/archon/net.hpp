#pragma once
// Thin portable TCP layer (Winsock / BSD sockets / lwIP).

#include <cstdint>
#include <string>
#include <vector>

namespace arc::net {

#if defined(_WIN32)
using socket_t = uintptr_t;
#else
using socket_t = int;
#endif
extern const socket_t kInvalidSocket;

bool init();  // idempotent; WSAStartup on Windows, SIGPIPE handling on POSIX
void close_socket(socket_t s);
// Half-close: send FIN after the last queued bytes so the peer reads them
// before the socket goes away (avoids RST discarding a final error frame).
void shutdown_send(socket_t s);

// Listens on all interfaces (IPv6 dual-stack when available, else IPv4).
// port 0 picks an ephemeral port; the bound port is returned in *bound_port.
socket_t listen_tcp(uint16_t port, bool loopback_only, uint16_t* bound_port, std::string* err);
socket_t accept_tcp(socket_t listener, std::string* peer);
// Blocking connect with timeout over every resolved address.
socket_t connect_tcp(const std::string& host, uint16_t port, int timeout_ms, std::string* err);

constexpr long kWouldBlock = -2;
constexpr long kIoError = -1;
// >0 bytes, kWouldBlock, kIoError. recv also returns 0 on orderly close.
long send_some(socket_t s, const uint8_t* p, size_t n);
long recv_some(socket_t s, uint8_t* p, size_t n);

struct PollItem {
    socket_t fd;
    bool want_read;
    bool want_write;
    bool readable = false;
    bool writable = false;
    bool failed = false;
};
// Returns number of ready items, 0 on timeout, -1 on error.
int poll(std::vector<PollItem>& items, int timeout_ms);

// Wakes a thread blocked in poll() from another thread (UDP loopback pair).
class Waker {
public:
    Waker();
    ~Waker();
    Waker(const Waker&) = delete;
    Waker& operator=(const Waker&) = delete;
    bool ok() const { return rx_ != kInvalidSocket; }
    socket_t fd() const { return rx_; }
    void wake();
    void drain();

private:
    socket_t rx_;
    socket_t tx_;
};

std::string host_name();

// True for loopback, private (RFC 1918), link-local, CGNAT (100.64/10) and
// IPv6 ULA/link-local addresses, including IPv4-mapped forms. The agent only
// serves the local network: anything else is refused at accept time.
bool is_local_network_address(const std::string& numeric_host);

}  // namespace arc::net
