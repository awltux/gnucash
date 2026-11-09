/********************************************************************\
 * json.hpp -- the small JSON reader the report designer needs       *
 *                                                                   *
 * gnc-webd has no JSON dependency (glib has no parser, and adding   *
 * one would be a new dependency just for the designer).  This is a  *
 * strict RFC 8259 reader for the value types a report spec uses:    *
 * null, booleans, numbers, strings, arrays and objects.  It rejects *
 * trailing garbage, duplicate keys, bad escapes and out-of-range    *
 * numbers, and every failure says where in the body it happened.    *
 *                                                                   *
 * This program is free software; you can redistribute it and/or     *
 * modify it under the terms of the GNU General Public License as    *
 * published by the Free Software Foundation; either version 2 of    *
 * the License, or (at your option) any later version.               *
\********************************************************************/

#ifndef GNC_WEB_JSON_HPP
#define GNC_WEB_JSON_HPP

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace web
{
namespace json
{

enum class Type
{
    Null,
    Bool,
    Number,
    String,
    Array,
    Object,
};

struct Value
{
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    bool is_null () const { return type == Type::Null; }
    bool is_bool () const { return type == Type::Bool; }
    bool is_number () const { return type == Type::Number; }
    bool is_string () const { return type == Type::String; }
    bool is_array () const { return type == Type::Array; }
    bool is_object () const { return type == Type::Object; }

    const Value* find (const std::string& key) const
    {
        if (type != Type::Object)
            return nullptr;
        const auto found = object.find (key);
        return found == object.end () ? nullptr : &found->second;
    }
};

namespace detail
{

inline void
append_utf8 (std::string& out, unsigned int code)
{
    if (code <= 0x7f)
        out += static_cast<char> (code);
    else if (code <= 0x7ff)
    {
        out += static_cast<char> (0xc0 | (code >> 6));
        out += static_cast<char> (0x80 | (code & 0x3f));
    }
    else if (code <= 0xffff)
    {
        out += static_cast<char> (0xe0 | (code >> 12));
        out += static_cast<char> (0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char> (0x80 | (code & 0x3f));
    }
    else
    {
        out += static_cast<char> (0xf0 | (code >> 18));
        out += static_cast<char> (0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char> (0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char> (0x80 | (code & 0x3f));
    }
}

class Parser
{
public:
    Parser (const std::string& text) : text_ (text) {}

    bool parse (Value& out, std::string& error)
    {
        if (!parse_value (out, 0))
        {
            error = error_;
            return false;
        }
        skip_space ();
        if (pos_ != text_.size ())
        {
            error = fail ("trailing data after the JSON value");
            return false;
        }
        return true;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;
    std::string error_;

    std::string fail (const std::string& message)
    {
        error_ = "at byte " + std::to_string (pos_) + ": " + message;
        return error_;
    }

    void skip_space ()
    {
        while (pos_ < text_.size ())
        {
            const char c = text_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++pos_;
        }
    }

    bool parse_value (Value& out, int depth)
    {
        if (depth > 64)
        {
            fail ("JSON is nested too deeply");
            return false;
        }
        skip_space ();
        if (pos_ >= text_.size ())
        {
            fail ("unexpected end of JSON");
            return false;
        }

        const char c = text_[pos_];
        if (c == '{')
            return parse_object (out, depth);
        if (c == '[')
            return parse_array (out, depth);
        if (c == '"')
        {
            out.type = Type::String;
            return parse_string (out.string);
        }
        if (c == 't' && text_.compare (pos_, 4, "true") == 0)
        {
            pos_ += 4;
            out.type = Type::Bool;
            out.boolean = true;
            return true;
        }
        if (c == 'f' && text_.compare (pos_, 5, "false") == 0)
        {
            pos_ += 5;
            out.type = Type::Bool;
            out.boolean = false;
            return true;
        }
        if (c == 'n' && text_.compare (pos_, 4, "null") == 0)
        {
            pos_ += 4;
            out.type = Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9'))
            return parse_number (out);
        fail (std::string ("unexpected character '") + c + "'");
        return false;
    }

    bool parse_object (Value& out, int depth)
    {
        ++pos_; /* { */
        out.type = Type::Object;
        out.object.clear ();
        skip_space ();
        if (pos_ < text_.size () && text_[pos_] == '}')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            skip_space ();
            if (pos_ >= text_.size () || text_[pos_] != '"')
            {
                fail ("expected a quoted field name");
                return false;
            }
            std::string name;
            if (!parse_string (name))
                return false;
            if (out.object.count (name) != 0)
            {
                fail ("duplicate field '" + name + "'");
                return false;
            }
            skip_space ();
            if (pos_ >= text_.size () || text_[pos_] != ':')
            {
                fail ("expected ':' after field '" + name + "'");
                return false;
            }
            ++pos_;
            Value value;
            if (!parse_value (value, depth + 1))
                return false;
            out.object.emplace (std::move (name), std::move (value));
            skip_space ();
            if (pos_ >= text_.size ())
            {
                fail ("unterminated object");
                return false;
            }
            if (text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}')
            {
                ++pos_;
                return true;
            }
            fail ("expected ',' or '}' in object");
            return false;
        }
    }

    bool parse_array (Value& out, int depth)
    {
        ++pos_; /* [ */
        out.type = Type::Array;
        out.array.clear ();
        skip_space ();
        if (pos_ < text_.size () && text_[pos_] == ']')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            Value value;
            if (!parse_value (value, depth + 1))
                return false;
            out.array.push_back (std::move (value));
            skip_space ();
            if (pos_ >= text_.size ())
            {
                fail ("unterminated array");
                return false;
            }
            if (text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']')
            {
                ++pos_;
                return true;
            }
            fail ("expected ',' or ']' in array");
            return false;
        }
    }

    bool parse_hex4 (unsigned int& value)
    {
        if (pos_ + 4 > text_.size ())
        {
            fail ("short \\u escape");
            return false;
        }
        value = 0;
        for (int index = 0; index < 4; ++index)
        {
            const char c = text_[pos_ + index];
            unsigned int digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned int> (c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned int> (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned int> (c - 'A' + 10);
            else
            {
                fail ("invalid \\u escape");
                return false;
            }
            value = value * 16 + digit;
        }
        pos_ += 4;
        return true;
    }

    bool parse_string (std::string& out)
    {
        ++pos_; /* " */
        out.clear ();
        while (pos_ < text_.size ())
        {
            const unsigned char c = static_cast<unsigned char> (text_[pos_]);
            if (c == '"')
            {
                ++pos_;
                return true;
            }
            if (c < 0x20)
            {
                fail ("control character in string");
                return false;
            }
            if (c != '\\')
            {
                out += static_cast<char> (c);
                ++pos_;
                continue;
            }
            ++pos_;
            if (pos_ >= text_.size ())
            {
                fail ("unterminated escape");
                return false;
            }
            const char escape = text_[pos_++];
            switch (escape)
            {
            case '"':  out += '"'; break;
            case '\\': out += '\\'; break;
            case '/':  out += '/'; break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u':
            {
                unsigned int code = 0;
                if (!parse_hex4 (code))
                    return false;
                if (code >= 0xd800 && code <= 0xdbff)
                {
                    if (pos_ + 2 > text_.size () || text_[pos_] != '\\' ||
                        text_[pos_ + 1] != 'u')
                    {
                        fail ("high surrogate without a low surrogate");
                        return false;
                    }
                    pos_ += 2;
                    unsigned int low = 0;
                    if (!parse_hex4 (low))
                        return false;
                    if (low < 0xdc00 || low > 0xdfff)
                    {
                        fail ("invalid low surrogate");
                        return false;
                    }
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                }
                else if (code >= 0xdc00 && code <= 0xdfff)
                {
                    fail ("unexpected low surrogate");
                    return false;
                }
                append_utf8 (out, code);
                break;
            }
            default:
                fail (std::string ("invalid escape '\\") + escape + "'");
                return false;
            }
        }
        fail ("unterminated string");
        return false;
    }

    bool parse_number (Value& out)
    {
        const size_t start = pos_;
        if (text_[pos_] == '-')
            ++pos_;
        if (pos_ >= text_.size ())
        {
            fail ("expected a digit");
            return false;
        }
        if (text_[pos_] == '0')
        {
            ++pos_;
            if (pos_ < text_.size () && text_[pos_] >= '0' &&
                text_[pos_] <= '9')
            {
                fail ("leading zero in a number");
                return false;
            }
        }
        else if (text_[pos_] >= '1' && text_[pos_] <= '9')
        {
            while (pos_ < text_.size () && text_[pos_] >= '0' &&
                   text_[pos_] <= '9')
                ++pos_;
        }
        else
        {
            fail ("expected a digit");
            return false;
        }
        if (pos_ < text_.size () && text_[pos_] == '.')
        {
            ++pos_;
            if (pos_ >= text_.size () || text_[pos_] < '0' ||
                text_[pos_] > '9')
            {
                fail ("expected a digit after '.'");
                return false;
            }
            while (pos_ < text_.size () && text_[pos_] >= '0' &&
                   text_[pos_] <= '9')
                ++pos_;
        }
        if (pos_ < text_.size () && (text_[pos_] == 'e' || text_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < text_.size () &&
                (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (pos_ >= text_.size () || text_[pos_] < '0' ||
                text_[pos_] > '9')
            {
                fail ("expected a digit in the exponent");
                return false;
            }
            while (pos_ < text_.size () && text_[pos_] >= '0' &&
                   text_[pos_] <= '9')
                ++pos_;
        }

        const std::string number = text_.substr (start, pos_ - start);
        char* end = nullptr;
        const double value = std::strtod (number.c_str (), &end);
        if (end == nullptr || *end != '\0' || !std::isfinite (value))
        {
            fail ("number is out of range");
            return false;
        }
        out.type = Type::Number;
        out.number = value;
        return true;
    }
};

} // namespace detail

/** Parse one complete JSON value.  On failure `error` names the byte offset
 *  and what was expected. */
inline bool
parse (const std::string& text, Value& out, std::string& error)
{
    detail::Parser parser (text);
    return parser.parse (out, error);
}

/** A JSON string literal, quotes included. */
inline std::string
quote (const std::string& text)
{
    std::string out = "\"";
    for (const unsigned char c : text)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char escape[8];
                std::snprintf (escape, sizeof escape, "\\u%04x", c);
                out += escape;
            }
            else
                out += static_cast<char> (c);
        }
    }
    out += "\"";
    return out;
}

} // namespace json
} // namespace web

#endif
