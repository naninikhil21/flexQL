// flexql_client — Interactive REPL client for FlexQL
// Usage: ./flexql_client <host> <port>
//        e.g. ./flexql_client 127.0.0.1 9000

#include "flexql.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

static int print_callback(void* /*arg*/, int argc, char** argv, char** col_names) {
    for (int i = 0; i < argc; ++i) {
        if (i > 0) {
            std::cout << " | ";
        }
        std::cout << (col_names[i] ? col_names[i] : "?") << " = "
                  << (argv[i] ? argv[i] : "NULL");
    }
    std::cout << '\n';
    return 0; // continue processing
}

int main(int argc, char* argv[]) {
    const char* host = "127.0.0.1";
    int port = 9000;

    if (argc >= 3) {
        host = argv[1];
        port = std::atoi(argv[2]);
    } else if (argc == 2) {
        host = argv[1];
    }

    FlexQL* db = nullptr;
    int rc = flexql_open(host, port, &db);
    if (rc != FLEXQL_OK) {
        std::cerr << "Error: Could not connect to " << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "Connected to FlexQL server at " << host << ":" << port << "\n";
    std::cout << "Type SQL queries ending with ';'. Type '.exit' or 'quit' to exit.\n\n";

    std::string line;
    std::string accumulated;

    while (std::cout << (accumulated.empty() ? "flexql> " : "   ...> ") && std::getline(std::cin, line)) {
        // Trim leading/trailing whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) {
            if (accumulated.empty()) {
                continue;
            }
            // Empty line in middle of multi-line — keep accumulating
            continue;
        }
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);

        if (line.empty()) {
            continue;
        }

        // Quit commands
        if (accumulated.empty() &&
            (line == "quit" || line == "exit" || line == "\\q" || line == ".exit")) {
            break;
        }

        // Accumulate lines until we see a semicolon
        if (!accumulated.empty()) {
            accumulated += " ";
        }
        accumulated += line;

        // Check if the accumulated query contains a semicolon
        if (accumulated.find(';') == std::string::npos) {
            continue; // keep reading lines
        }

        char* errmsg = nullptr;
        rc = flexql_exec(db, accumulated.c_str(), print_callback, nullptr, &errmsg);
        if (rc != FLEXQL_OK) {
            if (errmsg) {
                std::cerr << errmsg << "\n";
                flexql_free(errmsg);
            } else {
                std::cerr << "Error: query failed\n";
            }
        } else {
            std::cout << "OK\n";
        }
        accumulated.clear();
    }

    std::cout << "\nBye!\n";
    flexql_close(db);
    return 0;
}
