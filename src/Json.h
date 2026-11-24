#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace Json
{
class Value;

using Array = std::vector<Value>;
using Object = std::vector<std::pair<std::string, Value>>;

class Value
{
public:
    using Data = std::variant<std::monostate, bool, int64_t, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>>;

    Value() = default;

    Value(std::nullptr_t)
    {
    }

    Value(bool value) : m_data(value)
    {
    }

    Value(int value) : m_data(static_cast<int64_t>(value))
    {
    }

    Value(int64_t value) : m_data(value)
    {
    }

    Value(double value) : m_data(value)
    {
    }

    Value(const char* value) : m_data(std::string(value ? value : ""))
    {
    }

    Value(std::string value) : m_data(std::move(value))
    {
    }

    Value(Array value) : m_data(std::make_shared<Array>(std::move(value)))
    {
    }

    Value(Object value) : m_data(std::make_shared<Object>(std::move(value)))
    {
    }

    bool IsNull() const
    {
        return std::holds_alternative<std::monostate>(m_data);
    }

    bool IsBool() const
    {
        return std::holds_alternative<bool>(m_data);
    }

    bool IsInteger() const
    {
        return std::holds_alternative<int64_t>(m_data);
    }

    bool IsNumber() const
    {
        return IsInteger() || std::holds_alternative<double>(m_data);
    }

    bool IsString() const
    {
        return std::holds_alternative<std::string>(m_data);
    }

    bool IsArray() const
    {
        return std::holds_alternative<std::shared_ptr<Array>>(m_data);
    }

    bool IsObject() const
    {
        return std::holds_alternative<std::shared_ptr<Object>>(m_data);
    }

    bool Bool(bool fallback = false) const
    {
        const auto* value = std::get_if<bool>(&m_data);
        return value ? *value : fallback;
    }

    int64_t Integer(int64_t fallback = 0) const;

    double Number(double fallback = 0.0) const;

    std::string String(const std::string& fallback = {}) const;

    const Array& Items() const;

    const Object& Members() const;

    Array& MutableItems();

    Object& MutableMembers();

    const Value* Find(std::string_view key) const;

    const Value& operator[](std::string_view key) const;

    void Set(const std::string& key, Value value);

    void Remove(std::string_view key);

    const Data& GetData() const
    {
        return m_data;
    }

private:
    Data m_data;
};

Value Parse(std::string_view text);

std::string Dump(const Value& value);

std::string SnakeToCamel(std::string_view key);

std::string CamelToSnake(std::string_view key);

Value RenameKeys(const Value& value, bool toCamel);

} // namespace Json
