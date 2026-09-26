// Sandboxie-OSS — SbieCore/Util/Json.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 手写最小 JSON 实现（04-modules.md §7.3 契约，禁止第三方库）：
//   - 值类型 JsonValue（variant 风格；字符串为 std::wstring）
//   - 序列化：SerializeUtf8() 输出 UTF-8 字节流
//   - 解析：Parse() 递归下降（server 管道请求用），拒绝未知转义/注释/尾逗号，
//     递归深度 > 32 拒绝（防恶意管道 payload）
//
// 支持：null / true / false / 十进制整数(int64) / double / 字符串 / 数组 / 对象。
// 字符串转义：`"` `\` `\b \f \n \r \t` + `\uXXXX`（代理对正确合成 UTF-16）；
// 控制字符 (<0x20) 序列化为 `\u00XX`。

#pragma once

#include "Status.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sbie::json {

class JsonValue {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    JsonValue() : type_(Type::Null) {}
    JsonValue(std::nullptr_t) : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Bool), bool_(b) {}
    JsonValue(long long v) : type_(Type::Int), int_(v) {}
    JsonValue(int v) : type_(Type::Int), int_(v) {}
    JsonValue(unsigned long long v) : type_(Type::Int), int_((long long)v) {}
    JsonValue(unsigned v) : type_(Type::Int), int_((long long)v) {}
    JsonValue(double v) : type_(Type::Double), dbl_(v) {}
    JsonValue(const wchar_t* s) : type_(Type::String), str_(s) {}
    JsonValue(const std::wstring& s) : type_(Type::String), str_(s) {}

    static JsonValue Array()  { JsonValue v; v.type_ = Type::Array;  return v; }
    static JsonValue Object() { JsonValue v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull()   const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray()  const { return type_ == Type::Array; }
    bool isString() const { return type_ == Type::String; }
    bool isInt()    const { return type_ == Type::Int; }

    bool        asBool()   const { return bool_; }
    long long   asInt()    const { return int_; }
    double      asDouble() const { return type_ == Type::Int ? (double)int_ : dbl_; }
    const std::wstring& asString() const { return str_; }

    // 数组
    void pushBack(JsonValue v) { arr_.push_back(std::move(v)); }
    const std::vector<JsonValue>& items() const { return arr_; }

    // 对象（保序插入；重复键追加为两个成员，序列化端自己保证不重复）
    void set(const std::wstring& key, JsonValue v);
    const std::vector<std::pair<std::wstring, JsonValue>>& members() const { return obj_; }
    // 查找（首个匹配键），无则 nullptr
    const JsonValue* find(const wchar_t* key) const;

    // 便捷构造数组
    template <typename It>
    static JsonValue arrayOf(It begin, It end) {
        JsonValue a = Array();
        for (It it = begin; it != end; ++it)
            a.pushBack(*it);
        return a;
    }

private:
    Type type_;
    bool bool_ = false;
    long long int_ = 0;
    double dbl_ = 0.0;
    std::wstring str_;
    std::vector<JsonValue> arr_;
    std::vector<std::pair<std::wstring, JsonValue>> obj_;
};

// 序列化为 UTF-8 字节流。int64 按整数输出（不丢精度），double 用最短往返形式。
std::string SerializeUtf8(const JsonValue& v);

// 单字符串转义（含成对引号），UTF-8 输出；供 Output/协议层复用。
std::string EscapeStringUtf8(const std::wstring& s);

// 解析 UTF-8 JSON 文本。失败返回 false 并置 *err（ERR_JSON）。空白仅限
// space/\t/\n/\r（JSON 规范），无注释、无尾逗号、拒绝未知转义。
bool Parse(std::string_view utf8, JsonValue* out, SbieStatus* err);

} // namespace sbie::json
