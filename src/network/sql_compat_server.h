#ifndef SQL_COMPAT_SERVER_H
#define SQL_COMPAT_SERVER_H

class SqlCompatServer {
public:
    explicit SqlCompatServer(int port);
    int run();

private:
    int _port;
};

#endif // SQL_COMPAT_SERVER_H
