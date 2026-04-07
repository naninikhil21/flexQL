#ifndef IO_URING_REACTOR_H
#define IO_URING_REACTOR_H

#include "utils/mpsc_ring_buffer.h"
#include "parsing/simd_parser.h"
#include <liburing.h>
#include <vector>
#include <array>
#include <thread>
#include <netinet/in.h>

constexpr int MAX_CONNECTIONS = 1024;
constexpr int BUFFER_SIZE = 65536; // 64KB

struct Request {
    int fd;
    std::vector<char> buffer;
};

class IOUringReactor {
public:
    IOUringReactor(int port, const std::vector<MPSCRingBuffer<ParsedRow>*>& queues);
    ~IOUringReactor();

    void start();
    void stop();
    bool set_affinity(int core_id);
    uint64_t rows_received() const;
    uint64_t accepted_connections() const;
    uint64_t parse_stalls() const;

private:
    void run();
    void setup_listening_socket(int port);
    void add_accept_request();
    void add_read_request(int client_fd);

    int _port;
    int _listen_fd;
    struct io_uring _ring;
    std::thread _thread;
    std::atomic<bool> _stop{false};
    std::atomic<uint64_t> _rows_received{0};
    std::atomic<uint64_t> _accepted_connections{0};
    std::atomic<uint64_t> _parse_stalls{0};

    std::vector<MPSCRingBuffer<ParsedRow>*> _queues;
    std::vector<Request> _requests;
    std::array<std::array<char, BUFFER_SIZE>, MAX_CONNECTIONS> _buffers{};
};

#endif // IO_URING_REACTOR_H
