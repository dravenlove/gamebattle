#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace gamebattle::term {

struct Atom {
    std::string value;
    bool operator==(const Atom&) const = default;
};

struct Binary {
    std::vector<std::uint8_t> value;
    bool operator==(const Binary&) const = default;
};

struct Value {
    using List = std::vector<Value>;
    using Tuple = std::vector<Value>;
    using Object = std::vector<std::pair<std::string, Value>>;

    struct ListValue { List value; };
    struct TupleValue { Tuple value; };
    struct ObjectValue { Object value; };

    using Storage = std::variant<std::int64_t, double, Atom, Binary, ListValue, TupleValue, ObjectValue>;
    Storage data;

    Value() : data(Atom{"undefined"}) {}
    explicit Value(std::int64_t value) : data(value) {}
    explicit Value(double value) : data(value) {}
    explicit Value(Atom value) : data(std::move(value)) {}
    explicit Value(Binary value) : data(std::move(value)) {}
    explicit Value(ListValue value) : data(std::move(value)) {}
    explicit Value(TupleValue value) : data(std::move(value)) {}
    explicit Value(ObjectValue value) : data(std::move(value)) {}

    static Value atom(std::string value);
    static Value binary(std::string_view value);
    static Value list(List value);
    static Value tuple(Tuple value);
    static Value object(Object value);
};

class DecodeError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

Value decode(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encode(const Value& value);

const Value* find(const Value& object, std::string_view key);
const Value::List& as_list(const Value& value, std::string_view path);
const Value::Object& as_object(const Value& value, std::string_view path);
std::string as_string(const Value& value, std::string_view path);
std::int64_t as_int(const Value& value, std::string_view path);
bool as_bool(const Value& value, std::string_view path);

std::string get_string(const Value& object, std::string_view key, std::string default_value = {});
std::int64_t get_int(const Value& object, std::string_view key, std::int64_t default_value = 0);
bool get_bool(const Value& object, std::string_view key, bool default_value = false);

} // namespace gamebattle::term
