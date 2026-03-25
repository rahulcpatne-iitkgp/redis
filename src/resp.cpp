#include "resp.hpp"

#include <cassert>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace {

ParseResult<long long> parse_strict_ll(const std::string& s) {
    size_t idx = 0;
    try {
        long long v = std::stoll(s, &idx, 10);
        if (idx != s.size()) {
            return ParseError{"Invalid integer format"};
        }
        return v;
    } catch (...) {
        return ParseError{"Invalid integer format"};
    }
}

} // namespace

ParseResult<std::string> parse_line(BufferCursor& cur) {
    auto crlf_pos = cur.find_crlf();
    if (!crlf_pos) {
        return ParseResult<std::string>();
    }

    size_t len = *crlf_pos - cur.position();
    std::string line(cur.peek_ahead(len));
    cur.set_position(*crlf_pos + 2); // skip \r\n
    return line;
}

RespParseResult parse_simple_string(BufferCursor& cur) {
    // +<string>\r\n
    assert(!cur.eof());
    assert(cur.peek() == '+');

    size_t saved = cur.position();
    cur.get(); // consume '+'

    auto resp_line = parse_line(cur);
    if (resp_line.need_more_data()) {
        cur.set_position(saved);
        return RespParseResult();
    }

    auto value = std::make_unique<RespValue>();
    value->data = SimpleString{std::move(resp_line.unwrap())};
    return value;
}

RespParseResult parse_error_string(BufferCursor& cur) {
    // -<string>\r\n
    assert(!cur.eof());
    assert(cur.peek() == '-');

    size_t saved = cur.position();
    cur.get(); // consume '-'

    auto resp_line = parse_line(cur);
    if (resp_line.need_more_data()) {
        cur.set_position(saved);
        return RespParseResult();
    }

    auto value = std::make_unique<RespValue>();
    value->data = ErrorString{std::move(resp_line.unwrap())};
    return value;
}

RespParseResult parse_integer(BufferCursor& cur) {
    // :[<+|->]<value>\r\n
    assert(!cur.eof());
    assert(cur.peek() == ':');

    size_t saved = cur.position();
    cur.get(); // consume ':'

    auto resp_line = parse_line(cur);
    if (resp_line.need_more_data()) {
        cur.set_position(saved);
        return RespParseResult();
    }

    auto parsed = parse_strict_ll(resp_line.unwrap());
    if (parsed.is_error()) {
        cur.set_position(saved);
        return ParseError{"Invalid integer format"};
    }

    auto value = std::make_unique<RespValue>();
    value->data = Integer{parsed.unwrap()};
    return value;
}

RespParseResult parse_bulk_string(BufferCursor& cur) {
    // $<length>\r\n<data>\r\n
    assert(!cur.eof());
    assert(cur.peek() == '$');

    size_t saved = cur.position();
    cur.get(); // consume '$'

    auto resp_line = parse_line(cur);
    if (resp_line.need_more_data()) {
        cur.set_position(saved);
        return RespParseResult();
    }

    auto len_res = parse_strict_ll(resp_line.unwrap());
    if (len_res.is_error()) {
        cur.set_position(saved);
        return ParseError{"Invalid length"};
    }

    long long length_ll = len_res.unwrap();

    auto value = std::make_unique<RespValue>();

    if (length_ll == -1) {
        value->data = BulkString{std::nullopt};
        return value;
    }

    if (length_ll < 0) {
        cur.set_position(saved);
        return ParseError{"Invalid length"};
    }

    size_t length = static_cast<size_t>(length_ll);

    if (cur.remaining() < length + 2) {
        cur.set_position(saved);
        return RespParseResult();
    }

    std::string data(cur.peek_ahead(length));
    cur.consume(length + 2); // data + \r\n

    value->data = BulkString{std::move(data)};
    return value;
}

RespParseResult parse_array(BufferCursor& cur) {
    // *<count>\r\n<value1><value2>...<valueN>
    assert(!cur.eof());
    assert(cur.peek() == '*');

    size_t saved = cur.position();
    cur.get(); // consume '*'

    auto resp_line = parse_line(cur);
    if (resp_line.need_more_data()) {
        cur.set_position(saved);
        return RespParseResult();
    }

    auto count_res = parse_strict_ll(resp_line.unwrap());
    if (count_res.is_error()) {
        cur.set_position(saved);
        return ParseError{"Invalid array length"};
    }

    long long count_ll = count_res.unwrap();
    auto value = std::make_unique<RespValue>();

    if (count_ll == -1) {
        value->data = Array{std::nullopt};
        return value;
    }

    if (count_ll < 0) {
        cur.set_position(saved);
        return ParseError{"Invalid array length"};
    }

    size_t count = static_cast<size_t>(count_ll);

    std::vector<std::unique_ptr<RespValue>> items;
    items.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        auto item_result = parse_value(cur);

        if (item_result.need_more_data()) {
            cur.set_position(saved);
            return RespParseResult();
        }

        if (item_result.is_error()) {
            cur.set_position(saved);
            return ParseError{"Failed to parse array item: " + item_result.unwrap_error().message};
        }

        items.push_back(std::move(item_result.unwrap()));
    }

    value->data = Array{std::move(items)};
    return value;
}

RespParseResult parse_value(BufferCursor& cur) {
    if (cur.eof()) {
        return RespParseResult();
    }

    char next = cur.peek();
    switch (next) {
        case '+': return parse_simple_string(cur);
        case '-': return parse_error_string(cur);
        case ':': return parse_integer(cur);
        case '$': return parse_bulk_string(cur);
        case '*': return parse_array(cur);
        default:  return ParseError{"Invalid RESP type"};
    }
}