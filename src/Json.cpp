#include "Json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace Json
{
namespace
{
const Value& NullValue()
{
    static const Value null;
    return null;
}

const Array& EmptyArray()
{
    static const Array empty;
    return empty;
}

const Object& EmptyObject()
{
    static const Object empty;
    return empty;
}

class Parser
{
public:
    explicit Parser(std::string_view text) : m_text(text)
    {
    }

    Value Document()
    {
        Value value = Parse(0);
        Space();

        if (m_position != m_text.size())
        {
            Fail("Trailing data after JSON document");
        }

        return value;
    }

private:
    [[noreturn]] void Fail(const char* message) const
    {
        throw std::invalid_argument(std::string(message) + " at offset " + std::to_string(m_position));
    }

    void Space()
    {
        while (m_position < m_text.size())
        {
            const char character = m_text[m_position];

            if (character != ' ' && character != '\t' && character != '\n' && character != '\r')
            {
                break;
            }

            ++m_position;
        }
    }

    bool Consume(std::string_view word)
    {
        if (m_text.substr(m_position, word.size()) == word)
        {
            m_position += word.size();
            return true;
        }

        return false;
    }

    Value Parse(int depth)
    {
        if (depth > 64)
        {
            Fail("JSON nesting too deep");
        }

        Space();

        if (m_position >= m_text.size())
        {
            Fail("Unexpected end of JSON");
        }

        const char character = m_text[m_position];

        if (character == '{')
        {
            return ParseObject(depth);
        }

        if (character == '[')
        {
            return ParseArray(depth);
        }

        if (character == '"')
        {
            return Value(ParseString());
        }

        if (Consume("true"))
        {
            return Value(true);
        }

        if (Consume("false"))
        {
            return Value(false);
        }

        if (Consume("null"))
        {
            return Value();
        }

        return ParseNumber();
    }

    Value ParseObject(int depth)
    {
        ++m_position;
        Object object;
        Space();

        if (m_position < m_text.size() && m_text[m_position] == '}')
        {
            ++m_position;
            return Value(std::move(object));
        }

        while (true)
        {
            Space();

            if (m_position >= m_text.size() || m_text[m_position] != '"')
            {
                Fail("Expected object key");
            }

            auto key = ParseString();
            Space();

            if (m_position >= m_text.size() || m_text[m_position] != ':')
            {
                Fail("Expected ':'");
            }

            ++m_position;
            object.emplace_back(std::move(key), Parse(depth + 1));
            Space();

            if (m_position < m_text.size() && m_text[m_position] == ',')
            {
                ++m_position;
                continue;
            }

            if (m_position < m_text.size() && m_text[m_position] == '}')
            {
                ++m_position;
                return Value(std::move(object));
            }

            Fail("Expected ',' or '}'");
        }
    }

    Value ParseArray(int depth)
    {
        ++m_position;
        Array array;
        Space();

        if (m_position < m_text.size() && m_text[m_position] == ']')
        {
            ++m_position;
            return Value(std::move(array));
        }

        while (true)
        {
            array.push_back(Parse(depth + 1));
            Space();

            if (m_position < m_text.size() && m_text[m_position] == ',')
            {
                ++m_position;
                continue;
            }

            if (m_position < m_text.size() && m_text[m_position] == ']')
            {
                ++m_position;
                return Value(std::move(array));
            }

            Fail("Expected ',' or ']'");
        }
    }

    static void AppendUtf8(std::string& output, uint32_t codepoint)
    {
        if (codepoint < 0x80)
        {
            output.push_back(static_cast<char>(codepoint));
        }
        else if (codepoint < 0x800)
        {
            output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else if (codepoint < 0x10000)
        {
            output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
        else
        {
            output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }

    uint32_t Hex4()
    {
        if (m_position + 4 > m_text.size())
        {
            Fail("Truncated unicode escape");
        }

        uint32_t value = 0;

        for (int index = 0; index < 4; ++index)
        {
            const char character = m_text[m_position++];
            value <<= 4;

            if (character >= '0' && character <= '9')
            {
                value |= static_cast<uint32_t>(character - '0');
            }
            else if (character >= 'a' && character <= 'f')
            {
                value |= static_cast<uint32_t>(character - 'a' + 10);
            }
            else if (character >= 'A' && character <= 'F')
            {
                value |= static_cast<uint32_t>(character - 'A' + 10);
            }
            else
            {
                Fail("Invalid unicode escape");
            }
        }

        return value;
    }

    std::string ParseString()
    {
        ++m_position;
        std::string output;

        while (true)
        {
            if (m_position >= m_text.size())
            {
                Fail("Unterminated string");
            }

            const char character = m_text[m_position++];

            if (character == '"')
            {
                return output;
            }

            if (character != '\\')
            {
                output.push_back(character);
                continue;
            }

            if (m_position >= m_text.size())
            {
                Fail("Unterminated escape");
            }

            const char escape = m_text[m_position++];

            switch (escape)
            {
            case '"':
                output.push_back('"');
                break;
            case '\\':
                output.push_back('\\');
                break;
            case '/':
                output.push_back('/');
                break;
            case 'b':
                output.push_back('\b');
                break;
            case 'f':
                output.push_back('\f');
                break;
            case 'n':
                output.push_back('\n');
                break;
            case 'r':
                output.push_back('\r');
                break;
            case 't':
                output.push_back('\t');
                break;
            case 'u':
            {
                uint32_t codepoint = Hex4();

                if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
                {
                    if (Consume("\\u"))
                    {
                        const uint32_t low = Hex4();

                        if (low >= 0xDC00 && low <= 0xDFFF)
                        {
                            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        }
                        else
                        {
                            codepoint = 0xFFFD;
                        }
                    }
                    else
                    {
                        codepoint = 0xFFFD;
                    }
                }
                else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
                {
                    codepoint = 0xFFFD;
                }

                AppendUtf8(output, codepoint);
                break;
            }
            default:
                Fail("Invalid escape");
            }
        }
    }

    Value ParseNumber()
    {
        const size_t start = m_position;

        if (m_position < m_text.size() && m_text[m_position] == '-')
        {
            ++m_position;
        }

        bool fractional = false;

        while (m_position < m_text.size())
        {
            const char character = m_text[m_position];

            if (character >= '0' && character <= '9')
            {
                ++m_position;
            }
            else if (character == '.' || character == 'e' || character == 'E' || character == '+' || character == '-')
            {
                fractional = true;
                ++m_position;
            }
            else
            {
                break;
            }
        }

        const std::string text(m_text.substr(start, m_position - start));

        if (text.empty() || text == "-")
        {
            Fail("Invalid JSON value");
        }

        if (!fractional)
        {
            errno = 0;
            char* end = nullptr;
            const long long integer = std::strtoll(text.c_str(), &end, 10);

            if (errno == 0 && end && *end == '\0')
            {
                return Value(static_cast<int64_t>(integer));
            }
        }

        char* end = nullptr;
        const double number = std::strtod(text.c_str(), &end);

        if (!end || *end != '\0' || !std::isfinite(number))
        {
            Fail("Invalid JSON number");
        }

        return Value(number);
    }

    std::string_view m_text;
    size_t m_position = 0;
};

void DumpString(std::string& output, const std::string& text)
{
    output.push_back('"');

    for (const char raw : text)
    {
        const auto character = static_cast<unsigned char>(raw);

        switch (character)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        default:
            if (character < 0x20)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", character);
                output += buffer;
            }
            else
            {
                output.push_back(raw);
            }
        }
    }

    output.push_back('"');
}

void DumpValue(std::string& output, const Value& value)
{
    std::visit([&output](const auto& item)
    {
        using Type = std::decay_t<decltype(item)>;

        if constexpr (std::is_same_v<Type, std::monostate>)
        {
            output += "null";
        }
        else if constexpr (std::is_same_v<Type, bool>)
        {
            output += item ? "true" : "false";
        }
        else if constexpr (std::is_same_v<Type, int64_t>)
        {
            output += std::to_string(item);
        }
        else if constexpr (std::is_same_v<Type, double>)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%.17g", item);
            output += buffer;
        }
        else if constexpr (std::is_same_v<Type, std::string>)
        {
            DumpString(output, item);
        }
        else if constexpr (std::is_same_v<Type, std::shared_ptr<Array>>)
        {
            output.push_back('[');
            bool first = true;

            for (const auto& entry : *item)
            {
                if (!first)
                {
                    output.push_back(',');
                }

                first = false;
                DumpValue(output, entry);
            }

            output.push_back(']');
        }
        else
        {
            output.push_back('{');
            bool first = true;

            for (const auto& entry : *item)
            {
                if (!first)
                {
                    output.push_back(',');
                }

                first = false;
                DumpString(output, entry.first);
                output.push_back(':');
                DumpValue(output, entry.second);
            }

            output.push_back('}');
        }
    }, value.GetData());
}

} // namespace

int64_t Value::Integer(int64_t fallback) const
{
    if (const auto* integer = std::get_if<int64_t>(&m_data))
    {
        return *integer;
    }

    if (const auto* number = std::get_if<double>(&m_data))
    {
        return static_cast<int64_t>(*number);
    }

    if (const auto* text = std::get_if<std::string>(&m_data))
    {
        char* end = nullptr;
        const long long parsed = std::strtoll(text->c_str(), &end, 10);

        if (end && *end == '\0' && !text->empty())
        {
            return parsed;
        }
    }

    return fallback;
}

double Value::Number(double fallback) const
{
    if (const auto* integer = std::get_if<int64_t>(&m_data))
    {
        return static_cast<double>(*integer);
    }

    if (const auto* number = std::get_if<double>(&m_data))
    {
        return *number;
    }

    return fallback;
}

std::string Value::String(const std::string& fallback) const
{
    if (const auto* text = std::get_if<std::string>(&m_data))
    {
        return *text;
    }

    if (const auto* integer = std::get_if<int64_t>(&m_data))
    {
        return std::to_string(*integer);
    }

    return fallback;
}

const Array& Value::Items() const
{
    const auto* array = std::get_if<std::shared_ptr<Array>>(&m_data);
    return array && *array ? **array : EmptyArray();
}

const Object& Value::Members() const
{
    const auto* object = std::get_if<std::shared_ptr<Object>>(&m_data);
    return object && *object ? **object : EmptyObject();
}

Array& Value::MutableItems()
{
    auto* array = std::get_if<std::shared_ptr<Array>>(&m_data);

    if (!array)
    {
        m_data = std::make_shared<Array>();
        array = std::get_if<std::shared_ptr<Array>>(&m_data);
    }
    else if (array->use_count() > 1)
    {
        *array = std::make_shared<Array>(**array);
    }

    return **array;
}

Object& Value::MutableMembers()
{
    auto* object = std::get_if<std::shared_ptr<Object>>(&m_data);

    if (!object)
    {
        m_data = std::make_shared<Object>();
        object = std::get_if<std::shared_ptr<Object>>(&m_data);
    }
    else if (object->use_count() > 1)
    {
        *object = std::make_shared<Object>(**object);
    }

    return **object;
}

const Value* Value::Find(std::string_view key) const
{
    for (const auto& entry : Members())
    {
        if (entry.first == key)
        {
            return &entry.second;
        }
    }

    return nullptr;
}

const Value& Value::operator[](std::string_view key) const
{
    const auto* found = Find(key);
    return found ? *found : NullValue();
}

void Value::Set(const std::string& key, Value value)
{
    auto& members = MutableMembers();

    for (auto& entry : members)
    {
        if (entry.first == key)
        {
            entry.second = std::move(value);
            return;
        }
    }

    members.emplace_back(key, std::move(value));
}

void Value::Remove(std::string_view key)
{
    if (!IsObject())
    {
        return;
    }

    auto& members = MutableMembers();

    for (auto entry = members.begin(); entry != members.end(); ++entry)
    {
        if (entry->first == key)
        {
            members.erase(entry);
            return;
        }
    }
}

Value Parse(std::string_view text)
{
    return Parser(text).Document();
}

std::string Dump(const Value& value)
{
    std::string output;
    DumpValue(output, value);
    return output;
}

std::string SnakeToCamel(std::string_view key)
{
    std::string output;
    output.reserve(key.size());
    bool upper = false;

    for (size_t index = 0; index < key.size(); ++index)
    {
        const char character = key[index];

        if (character == '_' && index > 0 && index + 1 < key.size())
        {
            upper = true;
            continue;
        }

        if (upper && character >= 'a' && character <= 'z')
        {
            output.push_back(static_cast<char>(character - 'a' + 'A'));
        }
        else
        {
            output.push_back(character);
        }

        upper = false;
    }

    return output;
}

std::string CamelToSnake(std::string_view key)
{
    std::string output;
    output.reserve(key.size() + 4);

    for (size_t index = 0; index < key.size(); ++index)
    {
        const char character = key[index];

        if (character >= 'A' && character <= 'Z')
        {
            if (index > 0)
            {
                output.push_back('_');
            }

            output.push_back(static_cast<char>(character - 'A' + 'a'));
        }
        else
        {
            output.push_back(character);
        }
    }

    return output;
}

Value RenameKeys(const Value& value, bool toCamel)
{
    if (value.IsArray())
    {
        Array array;
        array.reserve(value.Items().size());

        for (const auto& item : value.Items())
        {
            array.push_back(RenameKeys(item, toCamel));
        }

        return Value(std::move(array));
    }

    if (value.IsObject())
    {
        Object object;
        object.reserve(value.Members().size());

        for (const auto& entry : value.Members())
        {
            object.emplace_back(toCamel ? SnakeToCamel(entry.first) : CamelToSnake(entry.first), RenameKeys(entry.second, toCamel));
        }

        return Value(std::move(object));
    }

    return value;
}

} // namespace Json
