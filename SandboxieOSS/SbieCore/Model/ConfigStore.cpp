// Sandboxie-OSS — SbieCore/Model/ConfigStore.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// M2 写路径登记（坑见 docs/04-modules.md §8）：
//   本文件绕开 SvcClient::IniSetSetting 便捷层、经 SvcClient::Call 原始组包，
//   原因：便捷层对空 value（value_len==0，即 0x1814 删值/删节与"置空"）的
//   请求长度按 offsetof(SBIE_INI_SETTING_REQ, value) 计算，比服务端
//   CheckRequest 的 sizeof 下限少 4 字节，必被 STATUS_INVALID_PARAMETER 拒。
//   原始组包按 QSbieAPI 尺寸规则（SbieAPI.cpp:1275）：
//     reqLen = max(sizeof(SBIE_INI_SETTING_REQ),
//                  offsetof(value) + (value_len + 1) * sizeof(WCHAR))，
//   value[value_len] 恒为 NUL（calloc 零化满足 CheckRequest 尾零校验）。

#include "ConfigStore.h"
#include "../DriverApi/DriverApi.h"
#include "../SvcClient/SvcClient.h"

// vendor 标志位（CONF_GET_PROPERTY/CONF_GET_NO_TEMPLS/CONF_GET_NO_EXPAND）
// 与协议头（MSGID_*、SBIE_INI_SETTING_REQ）
#include "api_flags.h"
#include "msgids.h"
#include "sbieiniwire.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace sbie::model {

namespace {

// 原始 SET/ADD/INS/DEL 组包（尺寸规则见文件头注释）。
// valueLenChars=0 表示空值（删值语义）；setting 可为 "*"（删节）/ ""（整节替换，
// 此时 value 为 "\n" 分隔的 Key=Value 文本——服务端 replace-section 分支）。
SbieStatus IniSetRaw(const std::wstring& section, const std::wstring& setting,
                     const wchar_t* value, ULONG valueLenChars,
                     svc::SvcClient::SetMode mode, bool refresh,
                     const std::wstring& password)
{
    if (section.size() > 64 || setting.size() > 64)
        return SbieStatus::INVALID;
    if (password.size() > 64)   // password[66] 定长（03 §8.6）
        return SbieStatus::INVALID;

    static const ULONG msgids[] = {
        MSGID_SBIE_INI_SET_SETTING, MSGID_SBIE_INI_ADD_SETTING,
        MSGID_SBIE_INI_INS_SETTING, MSGID_SBIE_INI_DEL_SETTING,
    };

    const size_t fixedLen = offsetof(SBIE_INI_SETTING_REQ, value);
    size_t reqLen = fixedLen + ((size_t)valueLenChars + 1) * sizeof(WCHAR);
    if (reqLen < sizeof(SBIE_INI_SETTING_REQ))
        reqLen = sizeof(SBIE_INI_SETTING_REQ);

    SBIE_INI_SETTING_REQ* req =
        (SBIE_INI_SETTING_REQ*)calloc(1, reqLen);
    if (!req)
        return SbieStatus::GENERIC;
    req->h.msgid = msgids[(size_t)mode];
    req->h.length = (ULONG)reqLen;
    wcsncpy_s(req->password, password.c_str(), 64);
    req->refresh = refresh ? TRUE : FALSE;
    wcsncpy_s(req->section, section.c_str(), _TRUNCATE);
    wcsncpy_s(req->setting, setting.c_str(), _TRUNCATE);
    req->value_len = valueLenChars;
    if (valueLenChars)
        memcpy(req->value, value, valueLenChars * sizeof(WCHAR));
    // value[valueLenChars] 已由 calloc 零化（CheckRequest 尾零校验）

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = svc::SvcClient::Instance().Call(req, reqLen, &rpl, &rplLen);
    free(req);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

} // namespace

std::optional<std::wstring> ConfigStore::Get(const std::wstring& section,
                                             const std::wstring& setting,
                                             ULONG index, bool noExpand,
                                             bool noTemplates)
{
    std::wstring v;
    SbieStatus st = drv::QueryConfText(section, setting, index, noExpand,
                                       noTemplates, &v);
    if (st != SbieStatus::OK)
        return std::nullopt;
    if (v.empty())
        return std::nullopt;
    return v;
}

std::vector<std::wstring> ConfigStore::GetList(const std::wstring& section,
                                               const std::wstring& setting,
                                               bool noExpand, bool noTemplates)
{
    std::vector<std::wstring> out;
    drv::QueryConfList(section, setting, noExpand, noTemplates, &out);
    return out;
}

std::vector<std::wstring> ConfigStore::ListSettings(const std::wstring& section)
{
    // 枚举 setting 名：QueryConf(section, NULL, index|NO_TEMPLS|NO_EXPAND)。
    // 驱动端 Conf_GetEx（drv/conf.c:1555）：have_section && !have_setting →
    // Conf_Get_Setting_Name（index 低 24 位 = 序号）。**不可** OR
    // CONF_GET_PROPERTY——那是节属性查询（IsVirtual/IniLocation，
    // drv/conf.c:1533-1537，且仅 index==0 时有效），M1 误用导致恒空（坑 §8.16）。
    std::vector<std::wstring> out;
    if (!drv::Loaded() && !drv::LoadSbieDll())
        return out;
    if (section.size() > 64)
        return out;
    for (ULONG i = 0; i < 4096; ++i) {
        WCHAR buf[130] = L"";
        LONG rc = drv::ApiP()->SbieApi_QueryConf(
            section.c_str(), nullptr,
            i | CONF_GET_NO_TEMPLS | CONF_GET_NO_EXPAND,
            buf, sizeof(buf));
        (void)rc;
        if (buf[0] == L'\0')
            break;
        out.push_back(buf);
    }
    return out;
}

SbieStatus ConfigStore::Set(const std::wstring& section,
                            const std::wstring& setting,
                            const std::wstring& value, bool refresh,
                            const std::wstring& password)
{
    // Update：替换该 setting 全部实例为单值；value 空 = 移除全部实例
    return IniSetRaw(section, setting, value.c_str(),
                     (ULONG)value.size(), svc::SvcClient::SetMode::Update,
                     refresh, password);
}

SbieStatus ConfigStore::SetAppend(const std::wstring& section,
                                  const std::wstring& setting,
                                  const std::wstring& value, bool refresh,
                                  const std::wstring& password)
{
    return IniSetRaw(section, setting, value.c_str(),
                     (ULONG)value.size(), svc::SvcClient::SetMode::Append,
                     refresh, password);
}

SbieStatus ConfigStore::SetInsert(const std::wstring& section,
                                  const std::wstring& setting,
                                  const std::wstring& value, bool refresh,
                                  const std::wstring& password)
{
    return IniSetRaw(section, setting, value.c_str(),
                     (ULONG)value.size(), svc::SvcClient::SetMode::Insert,
                     refresh, password);
}

SbieStatus ConfigStore::Delete(const std::wstring& section,
                               const std::wstring& setting,
                               const std::optional<ULONG>& index, bool refresh,
                               const std::wstring& password)
{
    if (!index.has_value())
        return IniSetRaw(section, setting, nullptr, 0,
                         svc::SvcClient::SetMode::Delete, refresh, password);
    // 删单个 index：先读全列表→重放（Update 首值 + Append 其余）——
    // 0x1814 协议仅支持"整 setting 删"，索引级删除在客户端组合
    std::vector<std::wstring> vals = GetList(section, setting);
    if (*index >= vals.size())
        return SbieStatus::NOT_FOUND;
    std::vector<std::wstring> keep;
    for (size_t i = 0; i < vals.size(); ++i)
        if (i != *index)
            keep.push_back(vals[i]);
    SbieStatus st = SbieStatus::OK;
    if (keep.empty())
        return IniSetRaw(section, setting, nullptr, 0,
                         svc::SvcClient::SetMode::Delete, refresh, password);
    for (size_t i = 0; i < keep.size() && st == SbieStatus::OK; ++i)
        st = IniSetRaw(section, setting, keep[i].c_str(), (ULONG)keep[i].size(),
                       i == 0 ? svc::SvcClient::SetMode::Update
                              : svc::SvcClient::SetMode::Append,
                       refresh && i + 1 == keep.size(), password);
    return st;
}

SbieStatus ConfigStore::Reload(bool reconfigureDrv)
{
    return drv::ReloadConf(0, reconfigureDrv);
}

SbieStatus ConfigStore::Path(std::wstring* out, bool* isHome)
{
    return svc::SvcClient::Instance().IniGetPath(out, isHome);
}

bool ConfigStore::Locked()
{
    // QSbieAPI IsConfigLocked 同款：EditPassword 已设（且本端无密码）即锁
    auto pw = Get(L"GlobalSettings", L"EditPassword", 0, true, true);
    return pw.has_value() && !pw->empty();
}

} // namespace sbie::model
