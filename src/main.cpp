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
#include <unordered_map>
#include <list>
#include <chrono>

#include "resp.hpp"
#include "kvstore.hpp"
#include "command.hpp"

KVStore kv_store;
CommandRegistry cmd_registry;

// Forward declarations
struct FdInfo_t;
struct BlockingRequest;

// Blocking command waiter
struct BlockingRequest {
    std::string key;
    int64_t expire_at_ms;  // 0 = no timeout
    FdInfo_t* fd_info;
    std::list<BlockingRequest*>::iterator registry_it;  // position in all_requests_
};

// Registry for blocking waiters
class BlockingRegistry {
public:
    // Register a waiter for a key. Returns pointer to waiter (owned by registry).
    BlockingRequest* add_waiter(const std::string& key, int64_t timeout_ms, FdInfo_t* fd_info) {
        auto* waiter = new BlockingRequest{};
        waiter->key = key;
        waiter->fd_info = fd_info;
        if (timeout_ms > 0) {
            waiter->expire_at_ms = now_millis() + timeout_ms;
        } else {
            waiter->expire_at_ms = 0;  // block forever
        }
        
        all_requests_.push_back(waiter);
        waiter->registry_it = std::prev(all_requests_.end());
        key_requests_[key].push_back(waiter);
        return waiter;
    }
    
    // Remove a specific waiter
    void remove_waiter(BlockingRequest* waiter) {
        if (!waiter) return;
        
        // Remove from key map
        auto kit = key_requests_.find(waiter->key);
        if (kit != key_requests_.end()) {
            kit->second.remove(waiter);
            if (kit->second.empty()) {
                key_requests_.erase(kit);
            }
        }
        
        // Remove from all_waiters list
        all_requests_.erase(waiter->registry_it);
        delete waiter;
    }
    
    // Get first waiter for a key (FIFO), or nullptr
    BlockingRequest* get_waiter_for_key(const std::string& key) {
        auto it = key_requests_.find(key);
        if (it == key_requests_.end() || it->second.empty()) {
            return nullptr;
        }
        return it->second.front();
    }
    
    // Get all expired waiters
    std::vector<BlockingRequest*> get_expired_waiters() {
        std::vector<BlockingRequest*> expired;
        int64_t now = now_millis();
        for (auto* w : all_requests_) {
            if (w->expire_at_ms > 0 && w->expire_at_ms <= now) {
                expired.push_back(w);
            }
        }
        return expired;
    }
    
    // Get time until next timeout (for epoll_wait), -1 if none
    int get_next_timeout_ms() {
        int64_t now = now_millis();
        int64_t min_timeout = INT64_MAX;
        for (auto* w : all_requests_) {
            if (w->expire_at_ms > 0) {
                int64_t remaining = w->expire_at_ms - now;
                if (remaining <= 0) return 0;  // already expired
                min_timeout = std::min(min_timeout, remaining);
            }
        }
        if (min_timeout == INT64_MAX) return -1;  // no timeouts
        return static_cast<int>(std::min(min_timeout, (int64_t)INT32_MAX));
    }
    
private:
    static int64_t now_millis() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();
    }
    
    std::unordered_map<std::string, std::list<BlockingRequest*>> key_requests_;
    std::list<BlockingRequest*> all_requests_;
};

BlockingRegistry blocking_registry;

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
    BlockingRequest* active_waiter = nullptr;  // if blocked on BLPOP
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
    
    // Clean up any active waiter
    if (fd_info->conn->active_waiter) {
        blocking_registry.remove_waiter(fd_info->conn->active_waiter);
        fd_info->conn->active_waiter = nullptr;
    }
    
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
        // Don't process more commands if connection is blocked
        if (conn->active_waiter) break;
        
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

        // Create context with callbacks
        CommandContext ctx{
            .store = kv_store,
            .block_on_key = [fd_info](const std::string& key, int64_t timeout_ms) {
                auto* waiter = blocking_registry.add_waiter(key, timeout_ms, fd_info);
                fd_info->conn->active_waiter = waiter;
            },
            .notify_key = [epfd](const std::string& key) {
                while (auto* waiter = blocking_registry.get_waiter_for_key(key)) {
                    // Try to pop an element for this waiter
                    auto result = kv_store.lpop_list(key);
                    if (!result || result.value().empty()) {
                        break;  // No more elements
                    }
                    
                    // Send response to waiting client
                    std::string response = encode_array(std::vector<std::string>{key, result.value().front()});
                    enqueue_response(epfd, waiter->fd_info, response);
                    
                    // Clean up waiter
                    waiter->fd_info->conn->active_waiter = nullptr;
                    blocking_registry.remove_waiter(waiter);
                }
            }
        };
        
        std::string response;
        auto handler = cmd_registry.get(cmd.name);
        if (handler.has_value()) {
            response = (*handler)(cmd, ctx);
        } else {
            response = encode_error("ERR unknown command '" + cmd.name + "'");
        }
        
        // Empty response means deferred (blocking command)
        if (!response.empty()) {
            enqueue_response(epfd, fd_info, response);
        }
    }
    compact_inbuf(conn);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // Register all commands
    commands::register_all(cmd_registry);

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
        // Calculate timeout for BLPOP waiters
        int timeout_ms = blocking_registry.get_next_timeout_ms();
        
        ssize_t n = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait failed\n";
            break;
        }
        
        // Handle expired BLPOP waiters
        auto expired = blocking_registry.get_expired_waiters();
        for (auto* waiter : expired) {
            // Send nil response for timeout
            std::string response = encode_array(std::nullopt);
            enqueue_response(epfd, waiter->fd_info, response);
            waiter->fd_info->conn->active_waiter = nullptr;
            blocking_registry.remove_waiter(waiter);
        }

        for (int i = 0; i < n; i++) {
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
            if (keep_alive && (ev & EPOLLOUT)) {
                while(conn->out_pos != conn->outbuf.size()) {
                    char *data = conn->outbuf.data() + conn->out_pos;
                    size_t len = conn->outbuf.size() - conn->out_pos;
                    ssize_t n = send(conn->fd, data, len, 0);
                    if (n <= 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        keep_alive = false;
                        break;
                    }
                    conn->out_pos += n;
                }
                if (conn->out_pos == conn->outbuf.size()) {
                    conn->outbuf.clear();
                    conn->out_pos = 0;
                    if (conn->to_close) {
                        keep_alive = false;
                    }
                } else {
                    compact_outbuf(conn);
                }
            }
            if (!keep_alive) {
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
