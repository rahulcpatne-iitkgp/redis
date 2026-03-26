#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <thread>
#include <algorithm>
#include <vector>
#include <string_view>

#include "resp.hpp"

static std::string to_upper_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return out;
}

// Handle a new connection
static void handle_connection(int conn_fd) {
    std::string buffer;
    char read_buf[1024];
    char write_buf[1024];
    size_t offset = 0;

    while(true) {
        ssize_t n = recv(conn_fd, read_buf, sizeof(read_buf), 0);
        if(n <= 0) break;    // client disconencted

        buffer.append(read_buf, n);
        while(true) {
            BufferCursor cur(buffer, offset);
            auto parse_res = parse_command(cur);
            if(parse_res.need_more_data()) break;
            if(parse_res.is_error()) {
                std::string err = encodeError(parse_res.error.value().message);
                strncpy(write_buf, err.c_str(), sizeof(write_buf));
                ssize_t n = send(conn_fd, write_buf, err.length(), MSG_NOSIGNAL);
            }
            Command cmd = std::move(parse_res.data.value());
            offset = cur.position();

            std::string response;
            std::string upper_name = to_upper_str(cmd.name);
            if (upper_name == "PING") {
                response = encodeSimpleString("PONG");
            } else if (upper_name == "ECHO") {
                if(!cmd.args.empty()) {
                    response = encodeBulkString(cmd.args[0]);
                } else {
                    response = encodeNullBulkString();
                }
            } else {
                response = encodeError("unknown command");
            }
            if(send(conn_fd, response.data(), response.size(), MSG_NOSIGNAL) < 0) {
                std::cerr << "Error sending response\n";
                break;  // if client disconnected, next recv will fail
            }
        }
    }
    close(conn_fd);
}

int main(int argc, char **argv)
{
    // Flush after every std::cout / std::cerr
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        std::cerr << "Failed to create server socket\n";
        return 1;
    }

    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        std::cerr << "setsockopt failed\n";
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(6379); // redis port

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0)
    {
        std::cerr << "Failed to bind to port 6379\n";
        return 1;
    }

    int connection_backlog = 5;
    if (listen(listen_fd, connection_backlog) != 0)
    {
        std::cerr << "listen failed\n";
        return 1;
    }

    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);
    std::cout << "Waiting for a client to connect...\n";

    std::cout << "Logs from your program will appear here!\n";

    
    while(true) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int conn_fd = accept(listen_fd, (struct sockaddr *) &client_addr, (socklen_t *) &client_addr_len);
        if(conn_fd < 0) {
            std::cerr << "Error accepting connection\n";
        }
        std::cout << "Client connected\n";
        std::thread t(handle_connection, conn_fd);
        t.detach();
    }

    close(listen_fd);
    return 0;
}
