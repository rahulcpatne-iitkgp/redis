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
#include <sys/epoll.h>
#include <fcntl.h>

#include "resp.hpp"
#include "kvstore.hpp"

KVStore kv_store;

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    return true;
}

struct Connection {
    int fd;
    std::string inbuf;
    size_t in_pos = 0;
    std::string outbuf;
    size_t out_pos = 0;
    bool to_close = false;
};

enum class FdType {
    Listen,
    Connection
};

struct FdInfo_t {
    FdType type;
    Connection* conn = nullptr;
};

static void close_connection(int epfd, FdInfo_t* fd_info) {
    assert(fd_info->type == FdType::Connection);
    int conn_fd = fd_info->conn->fd;
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, conn_fd, nullptr) < 0) {
        std::cerr << "epoll_ctl DEL conn failed: " << strerror(errno) << "\n";
        return;
    }
    close(conn_fd);
    delete fd_info->conn;
    delete fd_info;
}

static void update_epoll_interest(int epfd, FdInfo_t* fd_info) {
    assert(fd_info->type == FdType::Connection);
    Connection* conn = fd_info->conn;
    int conn_fd = conn->fd;

    epoll_event conn_ev{};
    conn_ev.data.ptr = fd_info;
    conn_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;
    if (!conn->outbuf.empty()) {
        conn_ev.events |= EPOLLOUT;
    }
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, conn_fd, &conn_ev) < 0) {
        std::cerr << "epoll_ctl MOD conn failed: " << strerror(errno) << "\n";
    }
}

static void enqueue_response(int epfd, FdInfo_t *fd_info, const std::string &response) {
    fd_info->conn->outbuf.append(response);
    update_epoll_interest(epfd, fd_info);
}

static void compact_inbuf(Connection* conn) {
    if (conn->in_pos >= 65536 && conn->in_pos > conn->inbuf.size() / 2) {
        conn->inbuf.erase(0, conn->in_pos);
        conn->in_pos = 0;
    }
}

static void compact_outbuf(Connection* conn) {
    if (conn->out_pos >= 65536 && conn->out_pos > conn->outbuf.size() / 2) {
        conn->outbuf.erase(0, conn->out_pos);
        conn->out_pos = 0;
    }
}

static void parse_and_handle(int epfd, FdInfo_t *fd_info) {
    Connection *conn = fd_info->conn;
    BufferCursor cursor(conn->inbuf, conn->in_pos);
    while (true) {
        auto parse_res = parse_command(cursor);
        if (parse_res.need_more_data()) break;
        if (parse_res.is_error()) {
            std::string err = encode_error(parse_res.error.value().message);
            enqueue_response(epfd, fd_info, err);
            conn->to_close = true;
            break;
        }

        Command cmd = std::move(parse_res.data.value());
        conn->in_pos = cursor.position();

        std::string response;
        if (cmd.name == "PING") {
            response = encode_simple_string("PONG");
        } else if (cmd.name == "ECHO") {
            if (!cmd.args.empty()) {
                response = encode_bulk_string(cmd.args[0]);
            } else {
                response = encode_bulk_string(std::nullopt);
            }
        } else if (cmd.name == "SET") {
            if(cmd.args.size() < 2) {
                response = encode_error("invalid command usage SET");
                enqueue_response(epfd, fd_info, response);
                continue;
            }
            std::string key = std::move(cmd.args[0]);
            std::string value = std::move(cmd.args[1]);
            int64_t expiry_in_ms = 0;
            if(cmd.args.size() >= 4) {
                if (cmd.args[2] == "PX") {
                    try {
                        expiry_in_ms = std::stoll(cmd.args[3]);
                    }
                    catch(...) {
                        response = encode_error("invalid command usage SET");
                        enqueue_response(epfd, fd_info, response);
                        continue;
                    }
                } else if (cmd.args[2] == "EX") {
                    try {
                        expiry_in_ms = std::stoll(cmd.args[3]) * 1000LL;
                    }
                    catch(...) {
                        response = encode_error("invalid command usage SET");
                        enqueue_response(epfd, fd_info, response);
                        continue;
                    }
                } else {
                    response = encode_error("invalid command usage SET");
                    enqueue_response(epfd, fd_info, response);
                    continue;
                }
                kv_store.set_string(key, value, expiry_in_ms);
            } else {
                kv_store.set_string(key, value);
            }
            response = encode_simple_string("OK");
        } else if (cmd.name == "GET") {
            if(cmd.args.size() < 1) {
                response = encode_error("invalid command usage GET");
                enqueue_response(epfd, fd_info, response);
                continue;
            }
            std::string key = std::move(cmd.args[0]);
            std::optional<std::string> value = kv_store.get_string(key);
            response = encode_bulk_string(value);
        } else {
            response = encode_error("unknown command");
        }
        enqueue_response(epfd, fd_info, response);
    }
    compact_inbuf(conn);
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

    if (!set_nonblocking(listen_fd)) {
        std::cerr << "Failed to set non-blocking mode\n";
        return 1;
    }

    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(6379); // redis port

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0)
    {
        std::cerr << "Failed to bind to port 6379\n";
        return 1;
    }

    int connection_backlog = 128;
    if (listen(listen_fd, connection_backlog) != 0)
    {
        std::cerr << "listen failed\n";
        return 1;
    }
    std::cout << "Waiting for a client to connect...\n";
    std::cout << "Logs from your program will appear here!\n";

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        std::cerr << "epoll create failed\n";
        return 1;
    }
    epoll_event listen_ev;
    listen_ev.events = EPOLLIN;
    listen_ev.data.ptr = new FdInfo_t{FdType::Listen, nullptr};
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &listen_ev) < 0) {
        std::cerr << "epoll_ctl failed\n";
        return 1;
    }

    constexpr int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];
    
    while (true) {
        ssize_t n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait failed\n";
            break;
        }

        for(int i = 0; i < n; i++) {
            FdInfo_t* fd_info = static_cast<FdInfo_t*>(events[i].data.ptr);
            uint32_t ev = events[i].events;
            if (fd_info->type == FdType::Listen) {
                while (true) {
                    sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int conn_fd = accept4(listen_fd, (sockaddr *) &client_addr, &client_len, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (conn_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        std::cerr << "accept4 failed\n";
                        break;
                    }
                    
                    epoll_event client_ev{};
                    Connection* conn_ptr = new Connection{};
                    conn_ptr->fd = conn_fd;
                    client_ev.data.ptr = new FdInfo_t{FdType::Connection, conn_ptr};
                    client_ev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR;

                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_fd, &client_ev) < 0) {
                        std::cerr << "epoll_ctl ADD client failed\n";
                        close(conn_fd);
                        delete conn_ptr;
                        delete static_cast<FdInfo_t*>(client_ev.data.ptr);
                        break;
                    }
                    std::cout << "Client connected: " << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) << "\n";
                }
                continue;
            }
            Connection* conn = fd_info->conn;
            if (ev & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                // Client disconnected or error occurred
                close_connection(epfd, fd_info);
                continue;
            }
            bool keep_alive = true;
            if (ev & EPOLLIN) {
                char readbuf[4096];
                while (true) {
                    ssize_t n = recv(conn->fd, readbuf, sizeof(readbuf), 0);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        std::cerr << "recv failed: " << strerror(errno) << "\n";
                        keep_alive = false;
                        break;
                    } else if (n == 0) {
                        keep_alive = false;
                        break;
                    }
                    conn->inbuf.append(readbuf, static_cast<size_t>(n));
                }
                if (keep_alive) parse_and_handle(epfd, fd_info);
            }
            if(keep_alive && (ev & EPOLLOUT)) {
                while(conn->out_pos != conn->outbuf.size()) {
                    char *data = conn->outbuf.data() + conn->out_pos;
                    size_t len = conn->outbuf.size() - conn->out_pos;
                    ssize_t n = send(conn->fd, data, len, 0);
                    if(n <= 0) {
                        if(errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        keep_alive = false;
                        break;
                    }
                    conn->out_pos += n;
                }
                if(conn->out_pos == conn->outbuf.size()) {
                    conn->outbuf.clear();
                    conn->out_pos = 0;
                    if(conn->to_close) {
                        keep_alive = false;
                    }
                } else {
                    compact_outbuf(conn);
                }
            }
            if(!keep_alive) {
                close_connection(epfd, fd_info);
                continue;
            }
            update_epoll_interest(epfd, fd_info);
        }
    }

    close(epfd);
    close(listen_fd);
    return 0;
}
