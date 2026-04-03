#ifndef KVSTORE_HPP
#define KVSTORE_HPP

#include <unordered_map>
#include <string>
#include <vector>
#include <span>
#include <optional>
#include <expected>
#include <variant>

enum class KVError {
    NotFound,
    WrongType
};

enum class ValueType {
    String,
    List
};

struct RedisObject {
    ValueType type = ValueType::String;

    std::variant<std::string, std::vector<std::string>> value;

    std::optional<int64_t> expiry_at_ms = std::nullopt;
};

class KVStore {
public:
    bool set_string(const std::string& key,
                    const std::string& value,
                    std::optional<int64_t> expiry_in_ms = std::nullopt);
    std::expected<std::string, KVError> get_string(const std::string& key);
    
                    
    std::expected<size_t, KVError> push_list(const std::string& key, std::span<const std::string> elements);
    std::expected<std::vector<std::string>, KVError> slice_list(const std::string& key, ssize_t start, ssize_t stop);

private:
    std::unordered_map<std::string, RedisObject> data_;
    static int64_t now_millis();
    std::unordered_map<std::string, RedisObject>::iterator find(const std::string& key);
};

#endif // KVSTORE_HPP