#include "gamebattle/term.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <sstream>

namespace gamebattle::term {
namespace {

constexpr std::uint8_t kVersion = 131;
constexpr std::uint8_t kNewFloat = 70;
constexpr std::uint8_t kSmallInteger = 97;
constexpr std::uint8_t kInteger = 98;
constexpr std::uint8_t kAtom = 100;
constexpr std::uint8_t kSmallTuple = 104;
constexpr std::uint8_t kLargeTuple = 105;
constexpr std::uint8_t kNil = 106;
constexpr std::uint8_t kString = 107;
constexpr std::uint8_t kList = 108;
constexpr std::uint8_t kBinary = 109;
constexpr std::uint8_t kSmallBig = 110;
constexpr std::uint8_t kLargeBig = 111;
constexpr std::uint8_t kSmallAtom = 115;
constexpr std::uint8_t kMap = 116;
constexpr std::uint8_t kAtomUtf8 = 118;
constexpr std::uint8_t kSmallAtomUtf8 = 119;
constexpr std::size_t kMaxDepth = 128;
constexpr std::size_t kMaxContainerItems = 1'000'000;

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    Value read_document() {
        if (u8() != kVersion) {
            throw DecodeError("ETF version byte 131 is missing");
        }
        Value value = read_value(0);
        if (offset_ != bytes_.size()) {
            throw DecodeError("trailing bytes after ETF value");
        }
        return value;
    }

private:
    std::uint8_t u8() {
        require(1);
        return bytes_[offset_++];
    }

    std::uint16_t u16() {
        require(2);
        const auto value = static_cast<std::uint16_t>((bytes_[offset_] << 8U) | bytes_[offset_ + 1]);
        offset_ += 2;
        return value;
    }

    std::uint32_t u32() {
        require(4);
        const auto value = (static_cast<std::uint32_t>(bytes_[offset_]) << 24U) |
                           (static_cast<std::uint32_t>(bytes_[offset_ + 1]) << 16U) |
                           (static_cast<std::uint32_t>(bytes_[offset_ + 2]) << 8U) |
                           static_cast<std::uint32_t>(bytes_[offset_ + 3]);
        offset_ += 4;
        return value;
    }

    std::uint64_t u64() {
        const auto high = static_cast<std::uint64_t>(u32());
        const auto low = static_cast<std::uint64_t>(u32());
        return (high << 32U) | low;
    }

    std::string text(std::size_t length) {
        require(length);
        std::string result(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
        offset_ += length;
        return result;
    }

    void require(std::size_t count) const {
        if (count > bytes_.size() - offset_) {
            throw DecodeError("truncated ETF value");
        }
    }

    static void check_count(std::uint32_t count) {
        if (count > kMaxContainerItems) {
            throw DecodeError("ETF container is too large");
        }
    }

    Value read_value(std::size_t depth) {
        if (depth > kMaxDepth) {
            throw DecodeError("ETF nesting is too deep");
        }

        const auto tag = u8();
        switch (tag) {
        case kSmallInteger:
            return Value(static_cast<std::int64_t>(u8()));
        case kInteger:
            return Value(static_cast<std::int64_t>(static_cast<std::int32_t>(u32())));
        case kSmallBig:
            return read_big(u8());
        case kLargeBig:
            return read_big(u32());
        case kNewFloat: {
            const auto bits = u64();
            return Value(std::bit_cast<double>(bits));
        }
        case kAtom:
        case kAtomUtf8:
            return Value(Atom{text(u16())});
        case kSmallAtom:
        case kSmallAtomUtf8:
            return Value(Atom{text(u8())});
        case kBinary: {
            const auto length = u32();
            check_count(length);
            require(length);
            Binary binary;
            binary.value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
            offset_ += length;
            return Value(std::move(binary));
        }
        case kString: {
            const auto length = u16();
            Value::List list;
            list.reserve(length);
            for (std::uint16_t index = 0; index < length; ++index) {
                list.emplace_back(static_cast<std::int64_t>(u8()));
            }
            return Value::list(std::move(list));
        }
        case kNil:
            return Value::list({});
        case kList: {
            const auto count = u32();
            check_count(count);
            Value::List list;
            list.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                list.push_back(read_value(depth + 1));
            }
            const Value tail = read_value(depth + 1);
            const auto* tail_list = std::get_if<Value::ListValue>(&tail.data);
            if (tail_list == nullptr || !tail_list->value.empty()) {
                throw DecodeError("improper ETF lists are not supported");
            }
            return Value::list(std::move(list));
        }
        case kSmallTuple:
            return read_tuple(u8(), depth);
        case kLargeTuple:
            return read_tuple(u32(), depth);
        case kMap: {
            const auto count = u32();
            check_count(count);
            Value::Object object;
            object.reserve(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                Value key = read_value(depth + 1);
                std::string key_text;
                if (const auto* atom = std::get_if<Atom>(&key.data)) {
                    key_text = atom->value;
                } else if (const auto* binary = std::get_if<Binary>(&key.data)) {
                    key_text.assign(binary->value.begin(), binary->value.end());
                } else {
                    throw DecodeError("ETF map keys must be atoms or binaries");
                }
                object.emplace_back(std::move(key_text), read_value(depth + 1));
            }
            return Value::object(std::move(object));
        }
        default:
            throw DecodeError("unsupported ETF tag: " + std::to_string(tag));
        }
    }

    Value read_tuple(std::uint32_t count, std::size_t depth) {
        check_count(count);
        Value::Tuple tuple;
        tuple.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            tuple.push_back(read_value(depth + 1));
        }
        return Value::tuple(std::move(tuple));
    }

    Value read_big(std::uint32_t count) {
        if (count > sizeof(std::uint64_t)) {
            throw DecodeError("integer does not fit into 64 bits");
        }
        const auto sign = u8();
        std::uint64_t magnitude = 0;
        for (std::uint32_t index = 0; index < count; ++index) {
            magnitude |= static_cast<std::uint64_t>(u8()) << (index * 8U);
        }
        if (sign == 0) {
            if (magnitude > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                throw DecodeError("positive integer does not fit into int64");
            }
            return Value(static_cast<std::int64_t>(magnitude));
        }
        if (sign != 1 || magnitude > (std::uint64_t{1} << 63U)) {
            throw DecodeError("negative integer does not fit into int64");
        }
        if (magnitude == (std::uint64_t{1} << 63U)) {
            return Value(std::numeric_limits<std::int64_t>::min());
        }
        return Value(-static_cast<std::int64_t>(magnitude));
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t offset_{0};
};

class Writer {
public:
    std::vector<std::uint8_t> write_document(const Value& value) {
        out_.push_back(kVersion);
        write_value(value, 0);
        return std::move(out_);
    }

private:
    void u16(std::uint16_t value) {
        out_.push_back(static_cast<std::uint8_t>(value >> 8U));
        out_.push_back(static_cast<std::uint8_t>(value));
    }

    void u32(std::uint32_t value) {
        out_.push_back(static_cast<std::uint8_t>(value >> 24U));
        out_.push_back(static_cast<std::uint8_t>(value >> 16U));
        out_.push_back(static_cast<std::uint8_t>(value >> 8U));
        out_.push_back(static_cast<std::uint8_t>(value));
    }

    void bytes(std::string_view value) {
        out_.insert(out_.end(), value.begin(), value.end());
    }

    void write_value(const Value& value, std::size_t depth) {
        if (depth > kMaxDepth) {
            throw std::runtime_error("term nesting is too deep");
        }
        std::visit([&](const auto& item) { write_item(item, depth); }, value.data);
    }

    void write_item(std::int64_t value, std::size_t) {
        if (value >= 0 && value <= 255) {
            out_.push_back(kSmallInteger);
            out_.push_back(static_cast<std::uint8_t>(value));
            return;
        }
        if (value >= std::numeric_limits<std::int32_t>::min() &&
            value <= std::numeric_limits<std::int32_t>::max()) {
            out_.push_back(kInteger);
            u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
            return;
        }

        out_.push_back(kSmallBig);
        std::uint64_t magnitude;
        if (value < 0) {
            magnitude = value == std::numeric_limits<std::int64_t>::min()
                            ? (std::uint64_t{1} << 63U)
                            : static_cast<std::uint64_t>(-value);
        } else {
            magnitude = static_cast<std::uint64_t>(value);
        }
        std::uint8_t count = 0;
        for (auto copy = magnitude; copy != 0; copy >>= 8U) {
            ++count;
        }
        out_.push_back(count);
        out_.push_back(value < 0 ? 1 : 0);
        for (std::uint8_t index = 0; index < count; ++index) {
            out_.push_back(static_cast<std::uint8_t>(magnitude >> (index * 8U)));
        }
    }

    void write_item(double value, std::size_t) {
        out_.push_back(kNewFloat);
        const auto bits = std::bit_cast<std::uint64_t>(value);
        u32(static_cast<std::uint32_t>(bits >> 32U));
        u32(static_cast<std::uint32_t>(bits));
    }

    void write_item(const Atom& atom, std::size_t) {
        if (atom.value.size() <= 255) {
            out_.push_back(kSmallAtomUtf8);
            out_.push_back(static_cast<std::uint8_t>(atom.value.size()));
        } else if (atom.value.size() <= 65535) {
            out_.push_back(kAtomUtf8);
            u16(static_cast<std::uint16_t>(atom.value.size()));
        } else {
            throw std::runtime_error("atom is too long");
        }
        bytes(atom.value);
    }

    void write_item(const Binary& binary, std::size_t) {
        if (binary.value.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("binary is too large");
        }
        out_.push_back(kBinary);
        u32(static_cast<std::uint32_t>(binary.value.size()));
        out_.insert(out_.end(), binary.value.begin(), binary.value.end());
    }

    void write_item(const Value::ListValue& list, std::size_t depth) {
        if (list.value.empty()) {
            out_.push_back(kNil);
            return;
        }
        out_.push_back(kList);
        u32(static_cast<std::uint32_t>(list.value.size()));
        for (const auto& item : list.value) {
            write_value(item, depth + 1);
        }
        out_.push_back(kNil);
    }

    void write_item(const Value::TupleValue& tuple, std::size_t depth) {
        if (tuple.value.size() <= 255) {
            out_.push_back(kSmallTuple);
            out_.push_back(static_cast<std::uint8_t>(tuple.value.size()));
        } else {
            out_.push_back(kLargeTuple);
            u32(static_cast<std::uint32_t>(tuple.value.size()));
        }
        for (const auto& item : tuple.value) {
            write_value(item, depth + 1);
        }
    }

    void write_item(const Value::ObjectValue& object, std::size_t depth) {
        out_.push_back(kMap);
        u32(static_cast<std::uint32_t>(object.value.size()));
        for (const auto& [key, item] : object.value) {
            write_item(Atom{key}, depth + 1);
            write_value(item, depth + 1);
        }
    }

    std::vector<std::uint8_t> out_;
};

[[noreturn]] void type_error(std::string_view path, std::string_view expected) {
    throw DecodeError(std::string(path) + " must be " + std::string(expected));
}

} // namespace

Value Value::atom(std::string value) { return Value(Atom{std::move(value)}); }

Value Value::binary(std::string_view value) {
    Binary result;
    result.value.assign(value.begin(), value.end());
    return Value(std::move(result));
}

Value Value::list(List value) { return Value(ListValue{std::move(value)}); }
Value Value::tuple(Tuple value) { return Value(TupleValue{std::move(value)}); }
Value Value::object(Object value) { return Value(ObjectValue{std::move(value)}); }

Value decode(std::span<const std::uint8_t> bytes) { return Reader(bytes).read_document(); }
std::vector<std::uint8_t> encode(const Value& value) { return Writer().write_document(value); }

const Value* find(const Value& object, std::string_view key) {
    const auto* data = std::get_if<Value::ObjectValue>(&object.data);
    if (data == nullptr) {
        return nullptr;
    }
    const auto iterator = std::find_if(data->value.begin(), data->value.end(),
                                       [&](const auto& entry) { return entry.first == key; });
    return iterator == data->value.end() ? nullptr : &iterator->second;
}

const Value::List& as_list(const Value& value, std::string_view path) {
    if (const auto* list = std::get_if<Value::ListValue>(&value.data)) {
        return list->value;
    }
    type_error(path, "a list");
}

const Value::Object& as_object(const Value& value, std::string_view path) {
    if (const auto* object = std::get_if<Value::ObjectValue>(&value.data)) {
        return object->value;
    }
    type_error(path, "a map");
}

std::string as_string(const Value& value, std::string_view path) {
    if (const auto* atom = std::get_if<Atom>(&value.data)) {
        return atom->value;
    }
    if (const auto* binary = std::get_if<Binary>(&value.data)) {
        return std::string(binary->value.begin(), binary->value.end());
    }
    type_error(path, "an atom or binary");
}

std::int64_t as_int(const Value& value, std::string_view path) {
    if (const auto* integer = std::get_if<std::int64_t>(&value.data)) {
        return *integer;
    }
    type_error(path, "an integer");
}

bool as_bool(const Value& value, std::string_view path) {
    if (const auto* atom = std::get_if<Atom>(&value.data)) {
        if (atom->value == "true") {
            return true;
        }
        if (atom->value == "false") {
            return false;
        }
    }
    type_error(path, "true or false");
}

std::string get_string(const Value& object, std::string_view key, std::string default_value) {
    const auto* value = find(object, key);
    return value == nullptr ? std::move(default_value) : as_string(*value, key);
}

std::int64_t get_int(const Value& object, std::string_view key, std::int64_t default_value) {
    const auto* value = find(object, key);
    return value == nullptr ? default_value : as_int(*value, key);
}

bool get_bool(const Value& object, std::string_view key, bool default_value) {
    const auto* value = find(object, key);
    return value == nullptr ? default_value : as_bool(*value, key);
}

} // namespace gamebattle::term
