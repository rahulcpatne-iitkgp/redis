#ifndef COMMAND_HPP
#define COMMAND_HPP

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "kvstore.hpp"
#include "resp.hpp"

// CommandContext provides handlers access to store and blocking functionality
struct CommandContext {
    KVStore& store;
    
    // Callback for BLPOP to register as waiting
    std::function<void(const std::string& key, int64_t timeout_ms)> block_on_key;
    
    // Callback for LPUSH/RPUSH to wake up blocked clients
    std::function<void(const std::string& key)> notify_key;
};

// Handler returns response string, or empty string for deferred (blocking) response
using CommandHandler = std::function<std::string(const Command&, CommandContext&)>;

class CommandRegistry {
public:
    void register_command(const std::string& name, CommandHandler handler);
    std::optional<CommandHandler> get(const std::string& name) const;

private:
    std::unordered_map<std::string, CommandHandler> handlers_;
};

// Built-in command handlers
namespace commands {
    std::string handle_ping(const Command& cmd, CommandContext& ctx);
    std::string handle_echo(const Command& cmd, CommandContext& ctx);
    std::string handle_set(const Command& cmd, CommandContext& ctx);
    std::string handle_get(const Command& cmd, CommandContext& ctx);
    std::string handle_type(const Command& cmd, CommandContext& ctx);

    std::string handle_rpush(const Command& cmd, CommandContext& ctx);
    std::string handle_lpush(const Command& cmd, CommandContext& ctx);
    std::string handle_lpop(const Command& cmd, CommandContext& ctx);
    std::string handle_rpop(const Command& cmd, CommandContext& ctx);
    std::string handle_lrange(const Command& cmd, CommandContext& ctx);
    std::string handle_llen(const Command& cmd, CommandContext& ctx);
    
    std::string handle_blpop(const Command& cmd, CommandContext& ctx);

    void register_all(CommandRegistry& registry);
}

#endif // COMMAND_HPP
