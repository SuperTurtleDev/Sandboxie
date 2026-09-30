// Sandboxie-OSS — SbieCore/Util/Json.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors

#include "Json.h"
#include "Utf8.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sbie::json {

void JsonValue::set(const std::wstring& key, JsonValue v)
{
    for (auto& kv : obj_) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    obj_.emplace_back(key, std::move(v));
}

const JsonValue* JsonValue::find(const wchar_t* key) const
{
    for (auto& kv : obj_)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

//---------------------------------------------------------------------------
// 序列化
//---------------------------------------------------------------------------

static void AppendEscaped(std::string& out, const std::wstring& s)
{
    out += '"';
    for (wchar_t c : s) {
        switch (c) {
        case L'"':  out += "\\\""; break;
        case L'\\': out += "\\\\"; break;
        case L'\b': out += "\\b";  break;
        case L'\f': out += "\\f";  break;
        case L'\n': out += "\\n";  break;
        case L'\r': out += "\\r";  break;
        case L'\t': out += "\\t";  break;
        default:
            if ((unsigned)c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04X", (unsigned)c);
                out += buf;
            } else {
                // 其余字符按 UTF-8 原样输出（含 BMP 之外字符：wstring 中的
                // 代理对经 WideToUtf8 正确编码）
                out += util::WideToUtf8(std::wstring(1, c));
            }
        }
    }
    out += '"';
}

std::string EscapeStringUtf8(const std::wstring& s)
{
    std::string out;
    out.reserve(s.size() * 2 + 2);
    AppendEscaped(out, s);
    return out;
}

static void SerializeTo(std::string& out, const JsonValue& v)
{
    switch (v.type()) {
    case JsonValue::Type::Null:   out += "null"; break;
    case JsonValue::Type::Bool:   out += v.asBool() ? "true" : "false"; break;
    case JsonValue::Type::Int: {
        // int64 整数直出，不经 double（04 §7.3 约束 3）
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", v.asInt());
        out += buf;
        break;
    }
    case JsonValue::Type::Double: {
        double d = v.asDouble();
        if (std::isnan(d) || std::isinf(d)) {
            out += "null"; // JSON 无 NaN/Inf
            break;
        }
        char buf[64];
        // 最短往返表示；无前导 +、无多余精度
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        double back = std::strtod(buf, nullptr);
        if (back != d)
            std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
        break;
    }
    case JsonValue::Type::String:
        AppendEscaped(out, v.asString());
        break;
    case JsonValue::Type::Array: {
        out += '[';
        bool first = true;
        for (auto& item : v.items()) {
            if (!first) out += ',';
            first = false;
            SerializeTo(out, item);
        }
        out += ']';
        break;
    }
    case JsonValue::Type::Object: {
        out += '{';
        bool first = true;
        for (auto& kv : v.members()) {
            if (!first) out += ',';
            first = false;
            AppendEscaped(out, kv.first);
            out += ':';
            SerializeTo(out, kv.second);
        }
        out += '}';
        break;
    }
    }
}

std::string SerializeUtf8(const JsonValue& v)
{
    std::string out;
    out.reserve(256);
    SerializeTo(out, v);
    return out;
}

//---------------------------------------------------------------------------
// 解析（递归下降；server 管道请求用）
//---------------------------------------------------------------------------

namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;
    SbieStatus err = SbieStatus::OK;

    bool fail() { if (err == SbieStatus::OK) err = SbieStatus::ERR_JSON; return false; }

    void SkipWs()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
            ++p;
    }

    bool ParseValue(JsonValue* out)
    {
        if (++depth > 32) // 04 §7.3 约束 4
            return fail();
        SkipWs();
        if (p >= end)
            return fail();
        bool ok = false;
        switch (*p) {
        case '{': ok = ParseObject(out); break;
        case '[': ok = ParseArray(out); break;
        case '"': {
            std::wstring s;
            ok = ParseString(&s);
            if (ok) *out = JsonValue(std::move(s));
            break;
        }
        case 't': ok = ParseLit("true", 4);  if (ok) *out = JsonValue(true);  break;
        case 'f': ok = ParseLit("false", 5); if (ok) *out = JsonValue(false); break;
        case 'n': ok = ParseLit("null", 4);  if (ok) *out = JsonValue(nullptr); break;
        default:  ok = ParseNumber(out); break;
        }
        --depth;
        return ok;
    }

    bool ParseLit(const char* lit, size_t n)
    {
        if ((size_t)(end - p) < n || std::memcmp(p, lit, n) != 0)
            return fail();
        p += n;
        return true;
    }

    bool ParseNumber(JsonValue* out)
    {
        const char* start = p;
        if (p < end && *p == '-')
            ++p;
        // 整数部分：0 或 [1-9][0-9]*（禁止前导 0；禁止前导 +）
        if (p >= end || !(*p >= '0' && *p <= '9'))
            return fail();
        if (*p == '0')
            ++p;
        else
            while (p < end && *p >= '0' && *p <= '9')
                ++p;
        bool isDouble = false;
        if (p < end && *p == '.') {
            isDouble = true;
            ++p;
            if (p >= end || !(*p >= '0' && *p <= '9'))
                return fail();
            while (p < end && *p >= '0' && *p <= '9')
                ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            isDouble = true;
            ++p;
            if (p < end && (*p == '+' || *p == '-'))
                ++p;
            if (p >= end || !(*p >= '0' && *p <= '9'))
                return fail();
            while (p < end && *p >= '0' && *p <= '9')
                ++p;
        }
        std::string tok(start, (size_t)(p - start));
        if (!isDouble) {
            errno = 0;
            char* endp = nullptr;
            long long v = std::strtoll(tok.c_str(), &endp, 10);
            if (errno == 0 && endp && *endp == '\0') {
                *out = JsonValue(v);
                return true;
            }
            // 溢出 int64：按 double 继续（合法 JSON，仅精度受限）
        }
        char* endp = nullptr;
        double d = std::strtod(tok.c_str(), &endp);
        if (!endp || *endp != '\0')
            return fail();
        *out = JsonValue(d);
        return true;
    }

    bool ParseHex4(unsigned* out)
    {
        if (end - p < 4)
            return fail();
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9')      v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return fail();
        }
        *out = v;
        return true;
    }

    bool ParseString(std::wstring* out)
    {
        if (p >= end || *p != '"')
            return fail();
        ++p;
        std::string utf8;
        while (true) {
            if (p >= end)
                return fail();
            unsigned char c = (unsigned char)*p;
            if (c == '"') { ++p; break; }
            if (c == '\\') {
                ++p;
                if (p >= end)
                    return fail();
                char e = *p++;
                switch (e) {
                case '"':  utf8 += '"';  break;
                case '\\': utf8 += '\\'; break;
                case '/':  utf8 += '/';  break;
                case 'b':  utf8 += '\b'; break;
                case 'f':  utf8 += '\f'; break;
                case 'n':  utf8 += '\n'; break;
                case 'r':  utf8 += '\r'; break;
                case 't':  utf8 += '\t'; break;
                case 'u': {
                    unsigned hi;
                    if (!ParseHex4(&hi))
                        return false;
                    if (hi >= 0xD800 && hi <= 0xDBFF) {
                        // 高代理必须紧跟 \uDC00-\uDFFF（代理对正确合成 UTF-16）
                        if (end - p < 6 || p[0] != '\\' || p[1] != 'u')
                            return fail();
                        p += 2;
                        unsigned lo;
                        if (!ParseHex4(&lo))
                            return false;
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            return fail();
                        wchar_t pair[2] = { (wchar_t)hi, (wchar_t)lo };
                        utf8 += util::WideToUtf8(std::wstring(pair, 2));
                    } else if (hi >= 0xDC00 && hi <= 0xDFFF) {
                        return fail(); // 孤立低代理
                    } else {
                        wchar_t one = (wchar_t)hi;
                        utf8 += util::WideToUtf8(std::wstring(1, one));
                    }
                    break;
                }
                default:
                    return fail(); // 未知转义（04 §7.3 约束）
                }
            } else if (c < 0x20) {
                return fail(); // 裸控制字符
            } else {
                utf8 += (char)c;
                ++p;
            }
        }
        *out = util::Utf8ToWide(utf8);
        return true;
    }

    bool ParseArray(JsonValue* out)
    {
        ++p; // '['
        *out = JsonValue::Array();
        SkipWs();
        if (p < end && *p == ']') { ++p; return true; }
        while (true) {
            JsonValue item;
            if (!ParseValue(&item))
                return false;
            out->pushBack(std::move(item));
            SkipWs();
            if (p >= end)
                return fail();
            if (*p == ',') { ++p; continue; } // 尾逗号会在下一轮 ParseValue 处失败
            if (*p == ']') { ++p; return true; }
            return fail();
        }
    }

    bool ParseObject(JsonValue* out)
    {
        ++p; // '{'
        *out = JsonValue::Object();
        SkipWs();
        if (p < end && *p == '}') { ++p; return true; }
        while (true) {
            SkipWs();
            std::wstring key;
            if (!ParseString(&key))
                return false;
            SkipWs();
            if (p >= end || *p != ':')
                return fail();
            ++p;
            JsonValue val;
            if (!ParseValue(&val))
                return false;
            out->set(std::move(key), std::move(val));
            SkipWs();
            if (p >= end)
                return fail();
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            return fail();
        }
    }
};

} // namespace

bool Parse(std::string_view utf8, JsonValue* out, SbieStatus* err)
{
    Parser ps{ utf8.data(), utf8.data() + utf8.size() };
    bool ok = ps.ParseValue(out);
    if (ok) {
        ps.SkipWs();
        if (ps.p != ps.end)
            ok = false, ps.err = SbieStatus::ERR_JSON; // 尾随内容
    }
    if (err)
        *err = ok ? SbieStatus::OK : SbieStatus::ERR_JSON;
    return ok;
}

} // namespace sbie::json
