#ifndef KVSTORE_HPP
#define KVSTORE_HPP

#include <unordered_map>
#include <string>
#include <vector>
#include <span>
#include <optional>
#include <expected>
#include <variant>
#include <deque>
#include <cstdint>

enum class KVError {
    NotFound,
    WrongType,
    ZeroStreamId,
    WrongStreamId
};

enum class ValueType {
    String,
    List,
    Stream
};

struct StreamId {
    uint64_t ms;
    uint64_t seq;

    bool operator<(const StreamId& other) const {
        if (ms != other.ms) return ms < other.ms;
        return seq < other.seq;
    }
    bool operator==(const StreamId& other) const {
        return ms == other.ms && seq == other.seq;
    }
    bool operator<=(const StreamId& other) const {
        return *this < other || *this == other;
    }
};

using Fields = std::vector<std::pair<std::string, std::string>>;

struct StreamEntry {
    StreamId id;
    Fields fields;
};

struct Stream {
    std::deque<StreamEntry> entries;
    StreamId last_id = {0, 0};
};

struct RedisObject {
    ValueType type = ValueType::String;

    std::variant<std::string, std::deque<std::string>, Stream> value;

    std::optional<int64_t> expiry_at_ms = std::nullopt;
};

class KVStore {
public:
    bool set_string(const std::string& key,
                    const std::string& value,
                    std::optional<int64_t> expiry_in_ms = std::nullopt);
    std::expected<std::string, KVError> get_string(const std::string& key);
    
    std::expected<ValueType, KVError> type_of_key(const std::string& key);
                    
    std::expected<size_t, KVError> rpush_list(const std::string& key, std::span<const std::string> elements);
    std::expected<size_t, KVError> lpush_list(const std::string& key, std::span<const std::string> elements);
    std::expected<std::vector<std::string>, KVError> lpop_list(const std::string& key, size_t n_elem = 1);
    std::expected<std::vector<std::string>, KVError> rpop_list(const std::string& key, size_t n_elem = 1);
    std::expected<std::vector<std::string>, KVError> slice_list(const std::string& key, ssize_t start, ssize_t stop);
    std::expected<size_t, KVError> size_list(const std::string& key);

    std::expected<StreamId, KVError> xadd_stream(const std::string& key, const StreamId& id, const Fields& fields);
    StreamId genid_stream(const std::string& key, std::optional<uint64_t> id_ms, std::optional<uint64_t> id_seq);

private:
    std::unordered_map<std::string, RedisObject> data_;
    static int64_t now_millis();
    std::unordered_map<std::string, RedisObject>::iterator find(const std::string& key);
};

#endif // KVSTORE_HPP