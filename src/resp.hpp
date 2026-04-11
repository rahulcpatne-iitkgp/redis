#ifndef RESP_HPP
#define RESP_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <cassert>

struct ParseError {
    std::string message;
};

enum class ParseStatus {
    Ok,
    NeedMoreData,
    Error
};

template <typename T>
class [[nodiscard]] ParseResult {
public:
    ParseStatus status = ParseStatus::NeedMoreData;
    std::optional<T> data;
    std::optional<ParseError> error;

    ParseResult() = default;
    ParseResult(T v) : status(ParseStatus::Ok), data(std::move(v)) {}
    ParseResult(ParseError err) : status(ParseStatus::Error), error(std::move(err)) {}

    bool is_ok() const { return status == ParseStatus::Ok; }
    bool need_more_data() const { return status == ParseStatus::NeedMoreData; }
    bool is_error() const { return status == ParseStatus::Error; }

    const T& unwrap() const { assert(is_ok()); return *data; }
    T& unwrap() { assert(is_ok()); return *data; }

    const ParseError& unwrap_error() const { assert(is_error()); return *error; }
};

struct RespValue;

struct SimpleString {
    std::string text;
};

struct ErrorString {
    std::string text;
};

struct Integer {
    int64_t value;
};

struct BulkString {
    std::optional<std::string> text; // null bulk string => std::nullopt
};

struct Array {
    std::optional<std::vector<std::unique_ptr<RespValue>>> items; // null array => std::nullopt
};

struct RespValue {
    std::variant<SimpleString, ErrorString, Integer, BulkString, Array> data;
    std::string encode() const;
};

struct Command {
    std::string name;
    std::vector<std::string> args;
};

class BufferCursor {
private:
    std::string_view buf_;
    size_t pos_ = 0;

public:
    explicit BufferCursor(std::string_view buf, size_t pos = 0) : buf_(buf), pos_(pos) {}

    bool eof() const { return pos_ >= buf_.size(); }
    size_t position() const { return pos_; }
    void set_position(size_t p) { pos_ = p; }

    size_t remaining() const { return buf_.size() - pos_; }

    char peek() const {
        assert(!eof());
        return buf_[pos_];
    }

    char get() {
        assert(!eof());
        return buf_[pos_++];
    }

    std::string_view peek_ahead(size_t length) const {
        assert(!eof());
        if (pos_ + length > buf_.size()) return {};
        return buf_.substr(pos_, length);
    }

    bool can_consume(size_t length) const {
        return pos_ + length <= buf_.size();
    }

    void consume(size_t length) {
        assert(can_consume(length));
        pos_ += length;
    }

    std::optional<size_t> find_crlf() const {
        size_t idx = buf_.find("\r\n", pos_);
        if (idx == std::string_view::npos) return std::nullopt;
        return idx;
    }
};

using RespParseResult = ParseResult<std::unique_ptr<RespValue>>;

RespParseResult parse_simple_string(BufferCursor& cur);
RespParseResult parse_error_string(BufferCursor& cur);
RespParseResult parse_integer(BufferCursor& cur);
RespParseResult parse_bulk_string(BufferCursor& cur);
RespParseResult parse_array(BufferCursor& cur);

RespParseResult parse_value(BufferCursor& cur);
ParseResult<Command> parse_command(BufferCursor& cur);

std::string encode_simple_string(const std::string& s);
std::string encode_error(const std::string& msg);
std::string encode_bulk_string(const std::optional<std::string>& s);
std::string encode_integer(int64_t n);
std::string encode_array(const std::optional<std::vector<std::string>>& items);
std::string encode_null_array();

#endif // RESP_HPP