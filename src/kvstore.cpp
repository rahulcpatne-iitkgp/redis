#include "kvstore.hpp"
#include <chrono>

int64_t KVStore::now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

void KVStore::set_string(const std::string &key,
                         const std::string &value,
                         std::optional<int64_t> expiry_in_ms) {
    RedisObject &obj = data_[key];
    obj.type = ValueType::String;
    obj.value = value;
    
    if(expiry_in_ms.has_value()) {
        obj.expiry_at_ms = now_millis() + expiry_in_ms.value();
    } else {
        obj.expiry_at_ms = std::nullopt;
    }
}

std::optional<std::string> KVStore::get_string(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    RedisObject& obj = it->second;
    if (obj.expiry_at_ms.has_value() && now_millis() >= obj.expiry_at_ms.value()) {
        data_.erase(it);
        return std::nullopt;
    }
    return std::get<std::string>(obj.value);
}