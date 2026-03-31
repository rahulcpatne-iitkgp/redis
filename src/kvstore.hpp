#ifndef KVSTORE_HPP
#define KVSTORE_HPP

#include <unordered_map>
#include <string>
#include <vector>
#include <optional>
#include <variant>

enum class ValueType {
    String
};

struct RedisObject {
    ValueType type = ValueType::String;

    std::variant<std::string, std::vector<std::string>> value;

    std::optional<int64_t> expiry_at_ms = std::nullopt;
};

class KVStore {
public:
    void set_string(const std::string& key,
                    const std::string& value,
                    std::optional<int64_t> expiry_in_ms = std::nullopt);

    std::optional<std::string> get_string(const std::string& key);

private:
    std::unordered_map<std::string, RedisObject> data_;

    static int64_t now_millis();
};

#endif // KVSTORE_HPP