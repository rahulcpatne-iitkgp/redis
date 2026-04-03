#include "command.hpp"

#include <algorithm>
#include <cctype>
#include <span>

void CommandRegistry::register_command(const std::string& name, CommandHandler handler) {
    handlers_[name] = std::move(handler);
}

std::optional<CommandHandler> CommandRegistry::get(const std::string& name) const {
    auto it = handlers_.find(name);
    if (it == handlers_.end()) {
        return std::nullopt;
    }
    return it->second;
}

namespace commands {
    static std::string to_upper_str(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return std::toupper(c); });
        return out;
    }

    std::string handle_ping(const Command& cmd, KVStore& store) {
        (void)store;
        if (!cmd.args.empty()) {
            return encode_bulk_string(cmd.args[0]);
        }
        return encode_simple_string("PONG");
    }

    std::string handle_echo(const Command& cmd, KVStore& store) {
        (void)store;
        if (cmd.args.empty()) {
            return encode_error("ERR wrong number of arguments for 'echo' command");
        }
        return encode_bulk_string(cmd.args[0]);
    }

    std::string handle_set(const Command& cmd, KVStore& store) {
        if (cmd.args.size() < 2) {
            return encode_error("ERR wrong number of arguments for 'set' command");
        }

        const std::string& key = cmd.args[0];
        const std::string& value = cmd.args[1];
        std::optional<int64_t> expiry_ms;

        // Parse options: EX seconds | PX milliseconds
        size_t i = 2;
        while (i < cmd.args.size()) {
            std::string opt = to_upper_str(cmd.args[i]);

            if (opt == "EX" && i + 1 < cmd.args.size() && !expiry_ms.has_value()) {
                try {
                    int64_t seconds = std::stoll(cmd.args[i + 1]);
                    if (seconds <= 0) {
                        return encode_error("ERR invalid expire time in 'set' command");
                    }
                    expiry_ms = seconds * 1000;
                    i += 2;
                } catch (...) {
                    return encode_error("ERR value is not an integer or out of range");
                }
            } else if (opt == "PX" && i + 1 < cmd.args.size() && !expiry_ms.has_value()) {
                try {
                    int64_t ms = std::stoll(cmd.args[i + 1]);
                    if (ms <= 0) {
                        return encode_error("ERR invalid expire time in 'set' command");
                    }
                    expiry_ms = ms;
                    i += 2;
                } catch (...) {
                    return encode_error("ERR value is not an integer or out of range");
                }
            } else {
                return encode_error("ERR syntax error");
            }
        }

        if(!store.set_string(key, value, expiry_ms)) {
            return encode_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        return encode_simple_string("OK");
    }

    std::string handle_get(const Command& cmd, KVStore& store) {
        if (cmd.args.empty()) {
            return encode_error("ERR wrong number of arguments for 'get' command");
        }
        auto result = store.get_string(cmd.args[0]);
        if (!result) {
            if (result.error() == KVError::WrongType) {
                return encode_error("WRONGTYPE Operation against a key holding the wrong kind of value");
            }
            return encode_bulk_string(std::nullopt);
        }
        return encode_bulk_string(result.value());
    }

    std::string handle_rpush(const Command& cmd, KVStore& store) {
        if(cmd.args.size() < 2) {
            return encode_error("ERR wrong number of arguments for 'rpush' command");
        }
        const std::string& key = cmd.args[0];
        std::span<const std::string> elements(cmd.args.data() + 1, cmd.args.size() - 1);
        auto result = store.push_list(key, elements);
        if(!result) {
            return encode_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        return encode_integer(result.value());
    }

    std::string handle_lrange(const Command& cmd, KVStore& store) {
        if (cmd.args.size() < 3) {
            return encode_error("ERR wrong number of arguments for 'lrange' command");
        }
        const std::string& key = cmd.args[0];
        ssize_t start, stop;
        try {
            start = std::stoll(cmd.args[1]);
            stop = std::stoll(cmd.args[2]);
        } catch (...) {
            return encode_error("ERR value is not an integer or out of range");
        }
        auto result = store.slice_list(key, start, stop);
        if (!result) {
            return encode_error("WRONGTYPE Operation against a key holding the wrong kind of value");
        }
        return encode_array(result.value());
    }

    void register_all(CommandRegistry& registry) {
        registry.register_command("PING", handle_ping);
        registry.register_command("ECHO", handle_echo);
        registry.register_command("SET", handle_set);
        registry.register_command("GET", handle_get);
        registry.register_command("RPUSH", handle_rpush);
        registry.register_command("LRANGE", handle_lrange);
    }
} // namespace commands
