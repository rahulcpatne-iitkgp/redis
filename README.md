# Redis-CPP

A high-performance, single-threaded Redis-like key-value store implementation in C++23.

## Architecture Overview

This implementation uses an **event-driven, non-blocking I/O model** with Linux epoll for handling thousands of concurrent clients efficiently in a single thread.

### Core Components

```
┌─────────────┐
│   Client    │
└──────┬──────┘
       │ TCP
┌──────▼──────────────────────────────┐
│   main.cpp (Event Loop)             │
│  - epoll-based I/O multiplexing     │
│  - Non-blocking sockets             │
│  - Connection management            │
│  - Blocking command registry        │
└──────┬──────────────────────────────┘
       │
┌──────▼──────────┐     ┌──────────────┐
│   resp.cpp      │───▶│  command.cpp │
│  RESP Protocol  │     │   Handlers   │
│  - Parser       │     │  - PING/ECHO │
│  - Encoder      │     │  - SET/GET   │
└─────────────────┘     │  - LPUSH/... │
                        │  - BLPOP     │
                        └──────┬───────┘
                               │
                        ┌──────▼───────┐
                        │  kvstore.cpp │
                        │  Data Layer  │
                        │ - String/List│
                        │ - Expiry     │
                        └──────────────┘
```

---

## Module Breakdown

### 1. **resp.hpp / resp.cpp** - RESP Protocol Implementation
**Lines**: 140 + 322 = 462

#### Responsibilities
- Parse incoming RESP (Redis Serialization Protocol) messages
- Encode outgoing responses
- Incremental parsing with `BufferCursor` for streaming data

#### Key Types
```cpp
struct RespValue {
    std::variant<SimpleString, ErrorString, Integer, BulkString, Array> data;
};

struct Command {
    std::string name;
    std::vector<std::string> args;
};

class BufferCursor {
    // Zero-copy parsing with string_view
    // Tracks position in buffer for incremental parsing
};
```

#### RESP Types Supported
- **Simple Strings**: `+OK\r\n`
- **Errors**: `-ERR message\r\n`
- **Integers**: `:123\r\n`
- **Bulk Strings**: `$5\r\nhello\r\n` (supports null: `$-1\r\n`)
- **Arrays**: `*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n` (supports null arrays)

#### Parse Strategy
- **Incremental**: Returns `ParseResult::NeedMoreData` if buffer incomplete
- **Backtracking**: Saves cursor position, restores on incomplete parse
- **Validation**: Strict integer parsing, CRLF enforcement

---

### 2. **kvstore.hpp / kvstore.cpp** - Data Storage Layer
**Lines**: 51 + 168 = 219

#### Responsibilities
- In-memory storage with TTL support
- Type-safe operations via `std::expected<T, KVError>`
- Lazy expiry on key access

#### Data Model
```cpp
enum class ValueType { String, List };

struct RedisObject {
    ValueType type;
    std::variant<std::string, std::deque<std::string>> value;
    std::optional<int64_t> expiry_at_ms;
};

class KVStore {
    std::unordered_map<std::string, RedisObject> data_;
};
```

#### Error Handling
Uses C++23 `std::expected` for type-safe error propagation:
```cpp
std::expected<std::string, KVError> get_string(const std::string& key);
// Returns: value on success, KVError::NotFound or KVError::WrongType on failure
```

#### Implemented Operations

| Category | Commands | Complexity |
|----------|----------|------------|
| **Strings** | `SET`, `GET` | O(1) |
| **Lists** | `LPUSH`, `RPUSH`, `LPOP`, `RPOP` | O(1) |
|  | `LRANGE`, `LLEN` | O(n) for range |
| **Expiry** | Lazy deletion on access | O(1) |

#### Design Decisions
- **deque over vector**: O(1) push/pop from both ends for lists
- **Lazy expiry**: Check on access, not background sweep (saves CPU)
- **No persistence**: Pure in-memory (future: RDB/AOF)

---

### 3. **command.hpp / command.cpp** - Command Handlers
**Lines**: 54 + 279 = 333

#### Responsibilities
- Dispatch commands to appropriate handlers
- Provide context for blocking operations
- Implement Redis command semantics

#### Command Context
```cpp
struct CommandContext {
    KVStore& store;
    std::function<void(const std::string&, int64_t)> block_on_key;
    std::function<void(const std::string&)> notify_key;
};
```

Provides handlers access to:
- Storage layer (`store`)
- Blocking primitives for BLPOP
- Wake-up mechanism for push operations

#### Handler Pattern
```cpp
std::string handle_xxx(const Command& cmd, CommandContext& ctx) {
    // 1. Validate args
    // 2. Execute operation via ctx.store
    // 3. Return RESP-encoded response
    // 4. Empty string = deferred response (BLPOP)
}
```

#### Implemented Commands (11 total)

| Command | Description | Example |
|---------|-------------|---------|
| **PING** | Connectivity test | `PING` → `PONG` |
| **ECHO** | Echo message | `ECHO hello` → `hello` |
| **SET** | Set string with optional TTL | `SET key val PX 1000` |
| **GET** | Get string | `GET key` → `val` |
| **LPUSH** | Push to list head | `LPUSH mylist a b c` |
| **RPUSH** | Push to list tail | `RPUSH mylist x y z` |
| **LPOP** | Pop from list head | `LPOP mylist 2` |
| **RPOP** | Pop from list tail | `RPOP mylist` |
| **LRANGE** | Get list slice | `LRANGE mylist 0 -1` |
| **LLEN** | Get list length | `LLEN mylist` → `5` |
| **BLPOP** | Blocking pop | `BLPOP mylist 10` |

#### SET Options
- `EX seconds` - Expire after N seconds
- `PX milliseconds` - Expire after N milliseconds

---

### 4. **main.cpp** - Event Loop & Networking
**Lines**: 448

#### Responsibilities
- epoll-based event loop
- Non-blocking I/O with edge-triggered semantics
- Connection lifecycle management
- **Blocking command infrastructure** (BLPOP)

#### Event Loop Architecture

```cpp
while (true) {
    timeout = blocking_registry.get_next_timeout_ms();  // Dynamic timeout
    events = epoll_wait(epfd, timeout);
    
    // 1. Handle expired BLPOP waiters
    for (waiter : get_expired_waiters()) {
        send_nil_response(waiter);
    }
    
    // 2. Process socket events
    for (event : events) {
        if (LISTEN) accept_connections();
        if (EPOLLIN) read_and_parse();
        if (EPOLLOUT) flush_output();
    }
}
```

#### Connection State Machine
```cpp
struct Connection {
    int fd;
    std::string inbuf;      // Incoming data buffer
    size_t in_pos;          // Parse position
    std::string outbuf;     // Outgoing response buffer
    size_t out_pos;         // Send position
    bool to_close;          // Graceful shutdown flag
    BlockingWaiter* active_waiter;  // For BLPOP
};
```

#### Blocking Command Implementation

**Problem**: Traditional command handlers return responses immediately. BLPOP needs to defer the response until data arrives or timeout.

**Solution**: Waiter registry + callback pattern

```cpp
struct BlockingWaiter {
    std::string key;
    int64_t expire_at_ms;   // 0 = block forever
    FdInfo_t* fd_info;
    std::list<BlockingWaiter*>::iterator registry_it;
};

class BlockingRegistry {
    // Track waiters per key
    std::unordered_map<std::string, std::list<BlockingWaiter*>> key_waiters_;
    
    // Track all waiters for timeout management
    std::list<BlockingWaiter*> all_waiters_;
};
```

**Flow**:
1. **BLPOP** on empty list → register waiter, return `""` (deferred)
2. **LPUSH** → check for waiters, pop element, send response immediately
3. **Timeout** → send nil response, clean up waiter
4. **Disconnect** → remove active waiter from registry

#### I/O Strategy
- **Non-blocking sockets** (`O_NONBLOCK` + `accept4`)
- **Edge-triggered epoll** (more efficient than level-triggered)
- **Incremental parsing** (BufferCursor saves position between reads)
- **Buffered writes** (accumulate in `outbuf`, flush on `EPOLLOUT`)

#### Buffer Management
- **Input compaction**: When buffer > 64KB and half consumed, shift data
- **Output compaction**: Same strategy for response buffer
- Prevents unbounded growth on long-lived connections

---

## Performance Characteristics

| Aspect | Implementation | Tradeoff |
|--------|---------------|----------|
| **Concurrency** | Single-threaded epoll | Simple, no locks; CPU-bound on one core |
| **Memory** | In-memory only | Fast; no durability |
| **Expiry** | Lazy on access | Low CPU; stale keys linger until accessed |
| **BLPOP timeout** | O(n) linear scan | Simple; slow with 1000+ waiters |
| **Parsing** | Incremental + backtracking | Handles partial packets; some CPU overhead |

---

## Design Patterns Used

### 1. **std::expected for Error Handling**
Replaces exceptions for expected errors:
```cpp
auto result = store.get_string(key);
if (!result) {
    if (result.error() == KVError::WrongType) {
        return encode_error("WRONGTYPE ...");
    }
    return encode_null();
}
return encode_bulk_string(result.value());
```

### 2. **Deferred Response Pattern**
Blocking commands return empty string to signal "no immediate response":
```cpp
std::string handle_blpop(...) {
    if (has_data) return encode_array({key, value});
    ctx.block_on_key(key, timeout_ms);
    return "";  // Event loop will not call enqueue_response()
}
```

### 3. **Callback-based Context**
Avoids circular dependencies between modules:
```cpp
CommandContext ctx{
    .store = kv_store,
    .block_on_key = [&](key, timeout) { /* ... */ },
    .notify_key = [&](key) { /* ... */ }
};
```

### 4. **Type Punning with std::variant**
Store multiple data types in single map:
```cpp
std::variant<std::string, std::deque<std::string>> value;
// Access: std::get<std::string>(obj.value)
```

---

## Limitations & Future Work

### Current Limitations
1. **No persistence** - Data lost on restart
2. **Single-threaded** - Limited to one CPU core
3. **No clustering** - Single instance only
4. **Limited commands** - 11 vs Redis's 200+
5. **BLPOP timeout**: O(n) with many waiters

### Future Enhancements

| Feature | Approach | Complexity |
|---------|----------|------------|
| **Persistence** | Fork-based RDB snapshots | Medium |
| **AOF logging** | Background fsync thread | Medium |
| **Multi-key BLPOP** | `std::vector<std::string> keys` in waiter | Low |
| **BRPOP** | Add `bool pop_from_left` to waiter | Low |
| **Timeout optimization** | std::set<Waiter*> ordered by expiry | Low |
| **Sorted sets** | std::map + skip list hybrid | High |
| **Pub/Sub** | Subscriber registry similar to BlockingRegistry | Medium |
| **Transactions** | MULTI/EXEC queue + rollback | High |

### Optimization Opportunities
1. **jemalloc** - Better memory allocator for small objects
2. **io_uring** - Modern async I/O (Linux 5.1+)
3. **SIMD parsing** - Vectorized RESP parsing (simdjson-style)
4. **Shared-nothing sharding** - Multiple processes, partition keys

---

## Building & Testing

```bash
# Build
mkdir build && cd build
cmake ..
make -j4

# Run server (port 6379)
./redis

# Test with redis-cli
redis-cli PING
redis-cli SET mykey "hello"
redis-cli GET mykey
redis-cli BLPOP mylist 5
```