#include "network/sql_compat_server.h"
#include <iostream>
#include <thread>

int main(int argc, char* argv[]) {
    int port = 9000;

    if (argc > 1) {
        port = std::stoi(argv[1]);
    }

    try {
        SqlCompatServer server(port);
        return server.run();
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
