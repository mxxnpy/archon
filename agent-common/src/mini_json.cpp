#include "mini_json.hpp"

#include <cmath>
#include <cstdlib>

namespace agent::json {

const Value& Value::operator[](const std::string& key) const {
    static const Value null;
    if (type != Type::Object) return null;
    auto it = obj.find(key);
    return it == obj.end() ? null : it->second;
}

namespace {

class Parser {
public:
    Parser(const std::string& t, const Limits& l) : s_(t), lim_(l) {}

    bool document(Value& out) {
        ws();
        if (!value(out, 0)) return false;
        ws();
        return i_ == s_.size();
    }

private:
    const std::string& s_;
    const Limits& lim_;
    size_t i_ = 0;
    size_t count_ = 0;

    void ws() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_;
    }
    bool lit(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s_.compare(i_, n, word) != 0) return false;
        i_ += n;
        return true;
    }

    bool value(Value& v, size_t depth) {
        if (depth > lim_.max_depth || ++count_ > lim_.max_elements || i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') {
            v.type = Value::Type::String;
            return string(v.str);
        }
        if (c == 't' && lit("true")) {
            v.type = Value::Type::Bool;
            v.b = true;
            return true;
        }
        if (c == 'f' && lit("false")) {
            v.type = Value::Type::Bool;
            v.b = false;
            return true;
        }
        if (c == 'n' && lit("null")) {
            v.type = Value::Type::Null;
            return true;
        }
        return number(v);
    }

    bool object(Value& v, size_t depth) {
        v.type = Value::Type::Object;
        ++i_;  // {
        ws();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        for (;;) {
            ws();
            if (i_ >= s_.size() || s_[i_] != '"') return false;
            std::string key;
            if (!string(key)) return false;
            ws();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            ++i_;
            ws();
            Value child;
            if (!value(child, depth + 1)) return false;
            v.obj[key] = std::move(child);  // a repeated key keeps the last value
            ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return false;
        }
    }

    bool array(Value& v, size_t depth) {
        v.type = Value::Type::Array;
        ++i_;  // [
        ws();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        for (;;) {
            ws();
            Value child;
            if (!value(child, depth + 1)) return false;
            v.arr.push_back(std::move(child));
            ws();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return false;
        }
    }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        ++i_;  // "
        out.clear();
        while (i_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c < 0x20) return false;  // raw control characters are not allowed
            if (c != '\\') {
                out += char(c);
            } else {
                if (i_ >= s_.size()) return false;
                const char e = s_[i_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {  // surrogate pair
                            uint32_t lo = 0;
                            if (i_ + 2 > s_.size() || s_[i_] != '\\' || s_[i_ + 1] != 'u') return false;
                            i_ += 2;
                            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return false;  // lone low surrogate
                        }
                        utf8(out, cp);
                        break;
                    }
                    default:
                        return false;
                }
            }
            if (out.size() > lim_.max_string) return false;
        }
        return false;  // unterminated
    }

    bool number(Value& v) {
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') ++i_;
        if (i_ >= s_.size()) return false;
        if (s_[i_] == '0') {
            ++i_;
        } else if (s_[i_] >= '1' && s_[i_] <= '9') {
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
        } else {
            return false;
        }
        if (i_ < s_.size() && s_[i_] == '.') {
            ++i_;
            const size_t d = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == d) return false;
        }
        if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '+' || s_[i_] == '-')) ++i_;
            const size_t d = i_;
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') ++i_;
            if (i_ == d) return false;
        }
        if (i_ - start > 40) return false;
        v.type = Value::Type::Number;
        v.num = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return std::isfinite(v.num);
    }
};

}  // namespace

bool parse(const std::string& text, Value& out, const Limits& limits) {
    out = Value{};
    Parser p(text, limits);
    if (p.document(out)) return true;
    out = Value{};
    return false;
}

std::string quote(const std::string& s) {
    static const char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace agent::json
