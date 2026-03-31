#ifndef COMMAND_HPP
#define COMMAND_HPP

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "kvstore.hpp"
#include "resp.hpp"

using CommandHandler = std::function<std::string(const Command&, KVStore&)>;

class CommandRegistry {
public:
    void register_command(const std::string& name, CommandHandler handler);
    std::optional<CommandHandler> get(const std::string& name) const;

private:
    std::unordered_map<std::string, CommandHandler> handlers_;
};

// Built-in command handlers
namespace commands {
    std::string handle_ping(const Command& cmd, KVStore& store);
    std::string handle_echo(const Command& cmd, KVStore& store);
    std::string handle_set(const Command& cmd, KVStore& store);
    std::string handle_get(const Command& cmd, KVStore& store);

    void register_all(CommandRegistry& registry);
}

#endif // COMMAND_HPP
