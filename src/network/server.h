#ifndef SERVER_H
#define SERVER_H

#include "network/io_uring_reactor.h"
#include "engine/worker.h"
#include "storage/wal.h"
#include <vector>
#include <memory>

class Server {
public:
    Server(int port, int num_workers);
    void run();

private:
    int _port;
    int _num_workers;
    WriteAheadLog _wal;
    std::vector<std::unique_ptr<MPSCRingBuffer<ParsedRow>>> _queues;
    std::vector<std::unique_ptr<IOUringReactor>> _reactors;
    std::vector<std::unique_ptr<Worker>> _workers;
};

#endif // SERVER_H
