// Legacy server path retained for reference; SqlCompatServer is the active server path.
#include "network/server.h"
#include <iostream>
#include <chrono>
#include <thread>

namespace {
int safe_hw_threads() {
    unsigned int n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : static_cast<int>(n);
}
}

Server::Server(int port, int num_workers)
    : _port(port), _num_workers(num_workers), _wal("flexql.wal") {
    std::vector<MPSCRingBuffer<ParsedRow>*> queue_ptrs;
    queue_ptrs.reserve(static_cast<size_t>(num_workers));

    for (int i = 0; i < num_workers; ++i) {
        _queues.push_back(std::make_unique<MPSCRingBuffer<ParsedRow>>(131072));
        _workers.push_back(std::make_unique<Worker>(i, *_queues.back(), _wal));
        queue_ptrs.push_back(_queues.back().get());
    }

    // One reactor dispatches incoming rows across all partition-owner queues.
    _reactors.push_back(std::make_unique<IOUringReactor>(port, queue_ptrs));
}

void Server::run() {
    std::cout << "Starting server on port " << _port << " with " << _num_workers << " workers." << std::endl;

    for (auto& worker : _workers) {
        worker->start();
    }

    for (auto& reactor : _reactors) {
        reactor->start();
    }

    const int hw = safe_hw_threads();

    for (size_t i = 0; i < _reactors.size(); ++i) {
        int core = static_cast<int>(i % static_cast<size_t>(hw));
        bool ok = _reactors[i]->set_affinity(core);
        std::cout << "reactor[" << i << "] core=" << core << " affinity=" << (ok ? "ok" : "fail") << std::endl;
    }

    for (size_t i = 0; i < _workers.size(); ++i) {
        int core = static_cast<int>((i + _reactors.size()) % static_cast<size_t>(hw));
        bool ok = _workers[i]->set_affinity(core);
        std::cout << "worker[" << i << "] core=" << core << " affinity=" << (ok ? "ok" : "fail") << std::endl;
    }

    uint64_t prev_rows_total = 0;
    uint64_t prev_rx_total = 0;
    uint64_t prev_accept_total = 0;

    // Keep server alive; shutdown is process-signal driven in this baseline.
    for (;;) {
        uint64_t rows_total = 0;
        for (const auto& worker : _workers) {
            rows_total += worker->rows_inserted();
        }

        uint64_t rx_total = 0;
        uint64_t accept_total = 0;
        uint64_t stalls_total = 0;
        for (const auto& reactor : _reactors) {
            rx_total += reactor->rows_received();
            accept_total += reactor->accepted_connections();
            stalls_total += reactor->parse_stalls();
        }

        uint64_t rows_delta = rows_total - prev_rows_total;
        uint64_t rx_delta = rx_total - prev_rx_total;
        uint64_t accept_delta = accept_total - prev_accept_total;

        prev_rows_total = rows_total;
        prev_rx_total = rx_total;
        prev_accept_total = accept_total;

        std::cout
            << "metrics/s: rx_rows=" << rx_delta
            << " inserted_rows=" << rows_delta
            << " new_conn=" << accept_delta
            << " parse_stalls_total=" << stalls_total
            << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
