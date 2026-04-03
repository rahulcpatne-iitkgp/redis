#include "kvstore.hpp"
#include <chrono>
#include <cassert>

int64_t KVStore::now_millis() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch())
        .count();
}

std::unordered_map<std::string, RedisObject>::iterator KVStore::find(const std::string& key) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return data_.end();
    }
    if (it->second.expiry_at_ms.has_value()) {
        if (now_millis() >= it->second.expiry_at_ms.value()) {
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
    if (it != data_.end() && it->second.type != ValueType::String) {
        return false;
    }
    if (it == data_.end()) {
        it = data_.insert({key, RedisObject{}}).first;
    }
    RedisObject &obj = it->second;
    obj.type = ValueType::String;
    obj.value = value;
    
    if (expiry_in_ms.has_value()) {
        obj.expiry_at_ms = now_millis() + expiry_in_ms.value();
    } else {
        obj.expiry_at_ms = std::nullopt;
    }
    return true;
}

std::expected<std::string, KVError> KVStore::get_string(const std::string& key) {
    auto it = find(key);
    if (it == data_.end()) {
        return std::unexpected(KVError::NotFound);
    }
    if (it->second.type != ValueType::String) {
        return std::unexpected(KVError::WrongType);
    }
    return std::get<std::string>(it->second.value);
}

std::expected<size_t, KVError> KVStore::rpush_list(const std::string& key, std::span<const std::string> elements) {
    auto it = find(key);
    if (it != data_.end() && it->second.type != ValueType::List) {
        return std::unexpected(KVError::WrongType);
    }
    if (it == data_.end()) {
        it = data_.insert({key, RedisObject{}}).first;
        it->second.value = std::deque<std::string>{};
        it->second.type = ValueType::List;
    }
    RedisObject &obj = it->second;
    auto &dq = std::get<std::deque<std::string>>(obj.value);
    for(const auto& elem : elements) {
        dq.emplace_back(elem);
    }
    return dq.size();
}

std::expected<size_t, KVError> KVStore::lpush_list(const std::string& key, std::span<const std::string> elements) {
    auto it = find(key);
    if (it != data_.end() && it->second.type != ValueType::List) {
        return std::unexpected(KVError::WrongType);
    }
    if (it == data_.end()) {
        it = data_.insert({key, RedisObject{}}).first;
        it->second.value = std::deque<std::string>{};
        it->second.type = ValueType::List;
    }
    RedisObject &obj = it->second;
    auto &dq = std::get<std::deque<std::string>>(obj.value);
    for(const auto& elem : elements) {
        dq.push_front(elem);
    }
    return dq.size();
}

std::expected<std::vector<std::string>, KVError> KVStore::lpop_list(const std::string& key, size_t n_elem) {
    auto it = find(key);
    if (it == data_.end()) {
        return std::vector<std::string>{};
    }
    if (it->second.type != ValueType::List) {
        return std::unexpected(KVError::WrongType);
    }
    auto &dq = std::get<std::deque<std::string>>(it->second.value);
    n_elem = std::min(n_elem, dq.size());
    std::vector<std::string> result;
    result.reserve(n_elem);
    for(size_t i = 0; i < n_elem; i++) {
        result.emplace_back(std::move(dq.front()));
        dq.pop_front();
    }
    return result;
}

std::expected<std::vector<std::string>, KVError> KVStore::rpop_list(const std::string& key, size_t n_elem) {
    auto it = find(key);
    if (it == data_.end()) {
        return std::vector<std::string>{};
    }
    if (it->second.type != ValueType::List) {
        return std::unexpected(KVError::WrongType);
    }
    auto &dq = std::get<std::deque<std::string>>(it->second.value);
    n_elem = std::min(n_elem, dq.size());
    std::vector<std::string> result;
    result.reserve(n_elem);
    for(size_t i = 0; i < n_elem; i++) {
        result.emplace_back(std::move(dq.back()));
        dq.pop_back();
    }
    return result;
}


std::expected<std::vector<std::string>, KVError> KVStore::slice_list(const std::string& key, ssize_t start, ssize_t stop) {
    auto it = find(key);
    if (it == data_.end()) {
        return std::vector<std::string>{};
    }
    if (it->second.type != ValueType::List) {
        return std::unexpected(KVError::WrongType);
    }
    
    auto &dq = std::get<std::deque<std::string>>(it->second.value);
    ssize_t len = static_cast<ssize_t>(dq.size());
    if (start < 0) start = std::max<ssize_t>(0, len + start);
    if (stop < 0) stop = len + stop;
    stop = std::min(stop, len - 1);
    
    std::vector<std::string> result;
    if (start <= stop) {
        result.reserve(stop-start+1);
    }
    for (ssize_t i = start; i <= stop; ++i) {
        result.emplace_back(dq[i]);
    }
    return result;
}

std::expected<size_t, KVError> KVStore::size_list(const std::string& key) {
    auto it = find(key);
    if (it == data_.end()) {
        return std::unexpected(KVError::NotFound);
    }
    if (it->second.type != ValueType::List) {
        return std::unexpected(KVError::WrongType);
    }
    auto &dq = std::get<std::deque<std::string>>(it->second.value);
    return dq.size();
}