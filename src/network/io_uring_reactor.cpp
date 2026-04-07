// Legacy reactor path retained for reference; SqlCompatServer is the active server path.
#include "network/io_uring_reactor.h"

#ifdef HAVE_LIBURING

#include "utils/thread_affinity.h"
#include <stdexcept>
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>

namespace {
constexpr uint64_t ACCEPT_TAG = UINT64_MAX;
constexpr size_t MAX_PARSED_ROWS_PER_READ = 8192;
}

IOUringReactor::IOUringReactor(int port, const std::vector<MPSCRingBuffer<ParsedRow>*>& queues)
    : _port(port), _queues(queues) {
    if (_queues.empty()) {
        throw std::runtime_error("At least one queue is required");
    }

    if (io_uring_queue_init(2048, &_ring, 0) < 0) {
        throw std::runtime_error("io_uring_queue_init failed");
    }
    setup_listening_socket(port);
}

IOUringReactor::~IOUringReactor() {
    stop();
    io_uring_queue_exit(&_ring);
}

void IOUringReactor::start() {
    _thread = std::thread(&IOUringReactor::run, this);
}

void IOUringReactor::stop() {
    _stop.store(true);
    if (_thread.joinable()) {
        _thread.join();
    }
}

bool IOUringReactor::set_affinity(int core_id) {
    if (!_thread.joinable()) {
        return false;
    }
    return ThreadAffinity::set_thread_affinity(_thread, core_id);
}

uint64_t IOUringReactor::rows_received() const {
    return _rows_received.load(std::memory_order_relaxed);
}

uint64_t IOUringReactor::accepted_connections() const {
    return _accepted_connections.load(std::memory_order_relaxed);
}

uint64_t IOUringReactor::parse_stalls() const {
    return _parse_stalls.load(std::memory_order_relaxed);
}

void IOUringReactor::setup_listening_socket(int port) {
    _listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_listen_fd < 0) {
        throw std::runtime_error("Failed to create listening socket");
    }

    const int val = 1;
    setsockopt(_listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    serv_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(_listen_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        throw std::runtime_error("Failed to bind socket");
    }

    if (listen(_listen_fd, 10) < 0) {
        throw std::runtime_error("Failed to listen on socket");
    }
}

void IOUringReactor::add_accept_request() {
    io_uring_sqe* sqe = io_uring_get_sqe(&_ring);
    io_uring_prep_accept(sqe, _listen_fd, nullptr, nullptr, 0);
    io_uring_sqe_set_data64(sqe, ACCEPT_TAG);
}

void IOUringReactor::add_read_request(int client_fd) {
    if (client_fd < 0 || client_fd >= MAX_CONNECTIONS) {
        close(client_fd);
        return;
    }

    io_uring_sqe* sqe = io_uring_get_sqe(&_ring);
    io_uring_prep_read(sqe, client_fd, _buffers[client_fd].data(), BUFFER_SIZE, 0);
    io_uring_sqe_set_data64(sqe, static_cast<uint64_t>(client_fd));
}

void IOUringReactor::run() {
    add_accept_request();
    io_uring_submit(&_ring);

    while (!_stop) {
        io_uring_cqe* cqe;
        int ret = io_uring_wait_cqe(&_ring, &cqe);
        if (ret < 0) {
            perror("io_uring_wait_cqe");
            continue;
        }

        uint64_t user_data = io_uring_cqe_get_data64(cqe);

        if (user_data == ACCEPT_TAG) { // Accept
            int client_fd = cqe->res;
            if (client_fd >= 0) {
                _accepted_connections.fetch_add(1, std::memory_order_relaxed);
                add_read_request(client_fd);
            }
            add_accept_request();
        } else { // Read
            int client_fd = static_cast<int>(user_data);
            if (cqe->res <= 0) { // Connection closed or error
                close(client_fd);
            } else {
                int bytes_read = cqe->res;
                std::string_view remaining(_buffers[client_fd].data(), bytes_read);

                while (!remaining.empty()) {
                    ParsedRow parsed_rows[MAX_PARSED_ROWS_PER_READ];
                    size_t consumed = 0;
                    size_t parsed_count = SIMDParser::parse_batch_into(remaining, parsed_rows, MAX_PARSED_ROWS_PER_READ, &consumed);

                    if (parsed_count == 0 || consumed == 0) {
                        _parse_stalls.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }

                    _rows_received.fetch_add(parsed_count, std::memory_order_relaxed);

                    for (size_t i = 0; i < parsed_count; ++i) {
                        ParsedRow row = parsed_rows[i];
                        size_t qidx = static_cast<size_t>(row.key % _queues.size());
                        MPSCRingBuffer<ParsedRow>* q = _queues[qidx];

                        while (!q->try_push(std::move(row))) {
                            // Spin while backpressured.
                        }
                    }

                    remaining.remove_prefix(consumed);
                }
                add_read_request(client_fd);
            }
        }
        io_uring_cqe_seen(&_ring, cqe);
        io_uring_submit(&_ring);
    }
}

#endif // HAVE_LIBURING
