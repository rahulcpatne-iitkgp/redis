#include "kvstore.hpp"
#include <chrono>

int64_t KVStore::now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

std::unordered_map<std::string, RedisObject>::iterator KVStore::find(const std::string& key) {
    auto it = data_.find(key);
    if(it == data_.end()) {
        return data_.end();
    }
    if(it->second.expiry_at_ms.has_value()) {
        if(now_millis() >= it->second.expiry_at_ms.value()) {
            data_.erase(it);
            return data_.end();
        }
    }
    return it;
}

bool KVStore::set_string(const std::string &key,
                         const std::string &value,
                         std::optional<int64_t> expiry_in_ms) {
    auto it = find(key);
    if(it != data_.end() && it->second.type != ValueType::String) {
        return false;
    }
    if(it == data_.end()) {
        it = data_.insert({key, RedisObject{}}).first;
    }
    RedisObject &obj = it->second;
    obj.type = ValueType::String;
    obj.value = value;
    
    if(expiry_in_ms.has_value()) {
        obj.expiry_at_ms = now_millis() + expiry_in_ms.value();
    } else {
        obj.expiry_at_ms = std::nullopt;
    }
    return true;
}

size_t KVStore::push_list(const std::string &key, const std::string &element) {
    auto it = find(key);
    if(it != data_.end() && it->second.type != ValueType::List) {
        return 0;
    }
    if(it == data_.end()) {
        it = data_.insert({key, RedisObject{}}).first;
        it->second.value = std::vector<std::string>{};
        it->second.type = ValueType::List;
    }
    RedisObject &obj = it->second;
    auto &v = std::get<std::vector<std::string>>(obj.value);
    v.push_back(element);
    return v.size();
}

std::optional<std::string> KVStore::get_string(const std::string& key) {
    auto it = find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return std::get<std::string>(it->second.value);
}