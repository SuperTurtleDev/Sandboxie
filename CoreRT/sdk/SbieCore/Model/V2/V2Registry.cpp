// Sandboxie-OSS — SbieCore/Model/V2/V2Registry.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2Registry.h 实现。

#include "V2Registry.h"
#include "V2Cache.h"
#include "V2EncBox.h"
#include "../../DriverApi/DriverApi.h"
#include "../../SvcClient/SvcClient.h"
#include "../../Util/Json.h"
#include "../../Util/Status.h"
#include "../../Util/Utf8.h"

#include <windows.h>

namespace sbie::model::v2 {

namespace {

// vendor/api_flags.h 的 CONF_GET_*（值与 drv/api_flags.h 一致；不引驱动头）
constexpr ULONG kConfIndexMask  = 0x00FFFFFF;
constexpr ULONG kConfNoExpand   = 0x20000000;
constexpr ULONG kConfNoTemplates= 0x10000000;
constexpr ULONG kConfNoGlobal   = 0x40000000;

// 裸 QueryConf（驱动缓存读；rc<0 或空值 = 未命中）
bool RawQueryConf(const std::wstring& section, const std::wstring& setting,
                  std::wstring* value)
{
    if (!drv::Loaded() && !drv::LoadSbieDll())
        return false;
    WCHAR buf[2000];
    buf[0] = L'\0';
    LONG rc = drv::ApiP()->SbieApi_QueryConf(section.c_str(), setting.c_str(),
                                             kConfNoGlobal | kConfNoTemplates
                                                 | kConfNoExpand,
                                             buf, (ULONG)sizeof(buf));
    value->assign(buf);
    return rc >= 0 && !value->empty();
}

// alias 索引互斥（RAII）。注意：WaitForSingleObject 超时不持有所有权——
// held() 必须按等待结果判定，否则超时后会无锁读-改-写并在未持有的互斥体
// 上 ReleaseMutex
struct AliasMutex {
    HANDLE h = nullptr;
    bool own = false;
    explicit AliasMutex(DWORD timeoutMs)
    {
        h = CreateMutexW(nullptr, FALSE, L"Local\\SbieOSS_AliasLock");
        if (h) {
            DWORD w = WaitForSingleObject(h, timeoutMs);
            own = (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED);
        }
    }
    ~AliasMutex()
    {
        if (own)
            ReleaseMutex(h);
        if (h)
            CloseHandle(h);
    }
    bool held() const { return own; }
};

std::vector<AliasEntry> LoadAliasesNoLock()
{
    std::vector<AliasEntry> list;
    V2Err e;
    std::wstring text = FileReadAll(AliasIndexPath(), &e);
    if (!e.Ok() || text.empty())
        return list;
    json::JsonValue root;
    SbieStatus je;
    if (!json::Parse(util::WideToUtf8(text), &root, &je) || !root.isObject())
        return list;
    // {"alias": {"box":…,"box_path":…,"added":…}, …}
    // JsonValue 无对象键枚举 → 存储形态改为 {"aliases":[{alias,box,box_path,added}]}
    const json::JsonValue* arr = root.find(L"aliases");
    if (!arr || !arr->isArray())
        return list;
    for (const auto& v : arr->items()) {
        if (!v.isObject())
            continue;
        AliasEntry en;
        if (const json::JsonValue* a = v.find(L"alias"))
            en.alias = a->asString();
        if (const json::JsonValue* b = v.find(L"box"))
            en.box = b->asString();
        if (const json::JsonValue* p = v.find(L"box_path"))
            en.boxPath = p->asString();
        if (const json::JsonValue* d = v.find(L"added"))
            en.added = d->asString();
        if (!en.alias.empty() && !en.box.empty())
            list.push_back(std::move(en));
    }
    return list;
}

V2Err SaveAliasesNoLock(const std::vector<AliasEntry>& list)
{
    json::JsonValue root = json::JsonValue::Object();
    json::JsonValue arr = json::JsonValue::Array();
    for (const auto& en : list) {
        json::JsonValue o = json::JsonValue::Object();
        o.set(L"alias", json::JsonValue(en.alias));
        o.set(L"box", json::JsonValue(en.box));
        o.set(L"box_path", json::JsonValue(en.boxPath));
        o.set(L"added", json::JsonValue(en.added));
        arr.pushBack(std::move(o));
    }
    root.set(L"aliases", std::move(arr));
    if (!EnsureRuntimeDirs())
        return {SbieStatus::GENERIC, L"cannot create runtime dirs"};
    return WriteTextFileAtomic(AliasIndexPath(), json::SerializeUtf8(root));
}

} // namespace

// ---------------------------------------------------------------------------
// 探测
// ---------------------------------------------------------------------------

V2Err ProbeRegistration(const std::wstring& box, RegState* state)
{
    std::wstring v;
    if (!RawQueryConf(box, L"Enabled", &v)) {
        *state = RegState::Absent;
        return {};
    }
    *state = (v[0] == L'y' || v[0] == L'Y') ? RegState::Registered
                                            : RegState::RegisteredDisabled;
    return {};
}

bool CacheBelongsToOtherBox(const std::wstring& box, const std::wstring& boxDir,
                            std::wstring* ownerOut)
{
    if (!CacheBelongsToOtherDir(box, boxDir, ownerOut))
        return false;
    // 声明根可能是加密盒的 <dir>\data——第二次比对豁免
    return CacheBelongsToOtherDir(box, EncBoxFileRoot(boxDir), ownerOut);
}

bool CacheBelongsToOtherDir(const std::wstring& box, const std::wstring& boxDir,
                            std::wstring* ownerOut)
{
    if (!CacheExists(box))
        return false;
    IniFileData ini;
    if (!ParseIniFile(CachePathFor(box), &ini).Ok())
        return false;
    const IniSectionData* sec = ini.Find(box);
    if (!sec)
        return false;
    for (const auto& kv : sec->entries) {
        if (_wcsicmp(kv.key.c_str(), L"FileRootPath") == 0
            && _wcsicmp(kv.value.c_str(), boxDir.c_str()) != 0) {
            if (ownerOut)
                *ownerOut = kv.value;
            return true;
        }
    }
    return false;
}

bool WaitForRegistration(const std::wstring& box, bool wantPresent, DWORD timeoutMs)
{
    DWORD waited = 0, delay = 50;
    for (;;) {
        RegState st;
        ProbeRegistration(box, &st);
        bool present = st != RegState::Absent;
        if (present == wantPresent)
            return true;
        if (waited >= timeoutMs)
            return false;
        Sleep(delay);
        waited += delay;
        if (delay < 200)
            delay = min(delay * 2, 200u);
    }
}

// ---------------------------------------------------------------------------
// 注册 / 注销
// ---------------------------------------------------------------------------

V2Err ReloadDriverConf()
{
    SbieStatus s = drv::ReloadConf(0, false);
    if (s != SbieStatus::OK)
        return {s, L"SbieApi_ReloadConf failed: " + NtStatusText(drv::LastNtStatus())};
    return {};
}

// 决策记录（用户拍板方向）：主 ini 的实际生效位置由驱动/服务知道
// （Conf_Read 三层搜索在内核：conf.c:256-269 IniPath 注册表值 → Home →
// \SystemRoot），用户态零探测。ImportBox= 行只可能来自主 ini 的
// [GlobalSettings] 节（Conf_Import_AllIncludes 只读 Conf_Data 的该节，
// conf.c:1121-1153；IniPath 重定向也只是换个位置读"主 ini"本身）——
// 因此唯一正路 = 经 SbieSvc SBIE_INI 协议写（服务知道 ini 在哪并负责
// 落盘+刷新，这正是它存在的意义；非"server"，不违背删 server）。
// 打点链：SvcClient 写行返回 → 缓存文件在约定目录 → ReloadConf IOCTL
// 返回码 → 驱动 QueryConf/枚举可见。
V2Err EnsureImportBoxLine(const std::wstring& password)
{
    if (!EnsureRuntimeDirs())
        return {SbieStatus::GENERIC, L"cannot create runtime dirs"};
    const std::wstring dir = BoxesDir() + L"\\";   // 尾随 '\' = 通配语义

    // 已有等值行？（经驱动缓存枚举 GlobalSettings\ImportBox 全部值）
    if (drv::Loaded() || drv::LoadSbieDll()) {
        std::vector<std::wstring> vals;
        if (drv::QueryConfList(L"GlobalSettings", L"ImportBox", true, true,
                               &vals)
            == SbieStatus::OK) {
            for (const auto& v : vals)
                if (_wcsicmp(v.c_str(), dir.c_str()) == 0)
                    return {};
        }
    }

    // 经 SbieSvc 官方写通道追加（refresh=true = 服务侧 SaveIni+ReloadConf）
    SbieStatus s = svc::SvcClient::Instance().IniSetSetting(
        L"GlobalSettings", L"ImportBox", dir, svc::SvcClient::SetMode::Append,
        true, password);
    if (s != SbieStatus::OK)
        return {s,
                std::wstring(L"SbieSvc ImportBox write failed (")
                    + StatusName(s)
                    + L"); if an EditPassword is set, add this line manually"
                      L" under [GlobalSettings]: ImportBox=" + dir};

    // N1（docs/11 复测 major）：写入成功 != 驱动可见——服务侧 refresh 的
    // 落盘与 ReloadConf 存在时序窗口（净 ini 首次 exec 3/3 确定性失败）。
    // 写后校验回读：经驱动 QueryConf 枚举 GlobalSettings\ImportBox 直至
    // 我们的目录在列；不可见则退避 + 驱动侧重载（IOCTL），上限 10s。
    // 可见后才允许进入缓存写入/注册阶段。
    {
        auto lineVisible = [&dir]() {
            std::vector<std::wstring> vals;
            if (drv::QueryConfList(L"GlobalSettings", L"ImportBox", true, true,
                                   &vals)
                != SbieStatus::OK)
                return false;
            for (const auto& v : vals)
                if (_wcsicmp(v.c_str(), dir.c_str()) == 0)
                    return true;
            return false;
        };
        DWORD waited = 0, delay = 50;
        int reloads = 0;
        while (!lineVisible()) {
            if (waited >= 10000)
                return {SbieStatus::GENERIC,
                        L"ImportBox line written but not driver-visible within"
                        L" 10s (svc write ok, "
                            + std::to_wstring(reloads) + L" reloads)"};
            if (waited && waited % 1000 < delay && reloads < 8) {
                drv::ReloadConf(0, false);
                ++reloads;
            }
            Sleep(delay);
            waited += delay;
            if (delay < 200)
                delay = min(delay * 2, 200u);
        }
    }
    return {};
}

V2Err RegisterBox(const std::wstring& box, const std::wstring& boxDir,
                  std::vector<std::wstring>* outApplied,
                  const std::wstring& password)
{
    const std::wstring ini = boxDir + L"\\sandbox.ini";
    if (!PathExists(ini))
        return {SbieStatus::NOT_FOUND, L"missing " + ini};

    // 部署前提自愈：主 ini 的 ImportBox 通配行（缺失 = register 放文件
    // 永远不可见——用户实测的 10s 超时根因）
    V2Err de = EnsureImportBoxLine(password);
    if (!de.Ok())
        return de;

    ExpandOutput ex = ExpandBoxConfig(ini, box, boxDir);
    if (!ex.status.Ok())
        return ex.status;

    // 同名碰撞检查以缓存文件为源（缓存文件名=盒名=节名，唯一）：既有缓存
    // 的 FileRootPath 指向别的目录 = 两目录同基名，拒绝覆盖（否则前者盒根
    // 被静默改写）。不用驱动内存侧 Conf 查询——reload 前的内存里可能有
    // 已删除缓存留下的"幽灵节"（用户实测现场），会误报碰撞。
    {
        std::wstring owner;
        if (CacheBelongsToOtherBox(box, boxDir, &owner))
            return {SbieStatus::INVALID,
                    L"box name '" + box + L"' already used by another directory ("
                        + owner
                        + L"); rename the directory or unregister the other box"
                          L" first"};
    }

    // 声明根：sandbox.ini 显式 FileRootPath 优先（加密盒=<dir>\data；
    // 任意盒可声明子目录根）；缺省 = boxDir
    std::wstring expectedRoot = boxDir;
    {
        IniFileData sini;
        if (ParseIniFile(ini, &sini).Ok()) {
            const IniSectionData* sec = sini.Find(box);
            if (!sec)
                sec = sini.Find(L"");
            if (sec)
                for (const auto& kv : sec->entries)
                    if (_wcsicmp(kv.key.c_str(), L"FileRootPath") == 0
                        && !kv.value.empty())
                        expectedRoot = kv.value;
        }
    }
    V2Err e = WriteBoxCache(box, boxDir, ini, ex.kv, ex.applied, expectedRoot);
    if (!e.Ok())
        return e;   // 缓存写/自检失败（docs/10 §9.2 码 12 = CACHE_INVALID）

    e = ReloadDriverConf();
    if (!e.Ok())
        return e;

    if (!WaitForRegistration(box, true, 10000)) {
        // 失败回收：缓存是不可见注册的唯一载体——超时即删，避免残留文件
        // 在后续 reload 被导入成"幽灵盒"与下次注册碰撞（用户实测现场）
        DeleteBoxCache(box);
        // 诊断分解：ImportBox 行在主 ini？
        std::vector<std::wstring> vals;
        bool lineOk = false;
        if (drv::QueryConfList(L"GlobalSettings", L"ImportBox", true, true,
                               &vals)
            == SbieStatus::OK) {
            std::wstring want = BoxesDir() + L"\\";
            for (const auto& v : vals)
                if (_wcsicmp(v.c_str(), want.c_str()) == 0)
                    lineOk = true;
        }
        return {SbieStatus::GENERIC,
                L"registration not visible after reload: ImportBox line "
                    + std::wstring(lineOk ? L"present" : L"MISSING")
                    + L"; cache rolled back (10s)"};
    }
    if (outApplied)
        *outApplied = std::move(ex.applied);
    return {};
}

V2Err UnregisterBox(const std::wstring& box, bool busyCheck)
{
    if (busyCheck) {
        V2Err ce;
        size_t n = BoxProcessCount(box, &ce);
        if (!ce.Ok())
            return ce;
        if (n > 0)
            return {SbieStatus::BOX_BUSY,
                    L"box '" + box + L"' has " + std::to_wstring(n)
                        + L" running process(es)"};
    }
    V2Err e = DeleteBoxCache(box);
    if (!e.Ok())
        return e;
    e = ReloadDriverConf();
    if (!e.Ok())
        return e;
    WaitForRegistration(box, false, 10000);   // 尽力；超时由缓存缺文件兜底
    return {};
}

V2Err SyncBoxConfig(const std::wstring& box, const std::wstring& boxDir,
                    const std::wstring& password)
{
    return RegisterBox(box, boxDir, nullptr, password);   // 同流水线（幂等重展开+reload）
}

// ---------------------------------------------------------------------------
// running.lock
// ---------------------------------------------------------------------------

bool LockExists(const std::wstring& boxDir)
{
    return PathExists(boxDir + L"\\running.lock");
}

LockInfo ReadLock(const std::wstring& boxDir)
{
    LockInfo info;
    IniFileData ini;
    if (!ParseIniFile(boxDir + L"\\running.lock", &ini).Ok())
        return info;
    // ini 格式：散行归入空名节
    const IniSectionData* sec = ini.Find(L"");
    if (!sec && !ini.sections.empty())
        sec = &ini.sections[0];
    if (!sec)
        return info;
    for (const auto& kv : sec->entries) {
        if (_wcsicmp(kv.key.c_str(), L"box") == 0)
            info.box = kv.value;
        else if (_wcsicmp(kv.key.c_str(), L"alias") == 0)
            info.alias = kv.value;
        else if (_wcsicmp(kv.key.c_str(), L"box_path") == 0)
            info.boxPath = kv.value;
        else if (_wcsicmp(kv.key.c_str(), L"cache") == 0)
            info.cache = kv.value;
        else if (_wcsicmp(kv.key.c_str(), L"creator_pid") == 0)
            info.creatorPid = (DWORD)wcstoul(kv.value.c_str(), nullptr, 10);
        else if (_wcsicmp(kv.key.c_str(), L"created") == 0)
            info.created = kv.value;
    }
    return info;
}

V2Err WriteLock(const std::wstring& boxDir, const LockInfo& info)
{
    std::wstring t;
    t += L"v=2\n";
    t += L"box=" + info.box + L"\n";
    t += L"alias=" + info.alias + L"\n";
    t += L"box_path=" + info.boxPath + L"\n";
    t += L"cache=" + info.cache + L"\n";
    t += L"creator_pid=" + std::to_wstring(info.creatorPid) + L"\n";
    t += L"created=" + info.created + L"\n";
    return WriteTextFileAtomic(boxDir + L"\\running.lock", util::WideToUtf8(t));
}

V2Err DeleteLock(const std::wstring& boxDir)
{
    const std::wstring p = boxDir + L"\\running.lock";
    if (!PathExists(p))
        return {};
    if (!DeleteFileW(p.c_str()))
        return {SbieStatus::GENERIC, L"cannot delete running.lock"};
    return {};
}

bool LockCreatorDead(const LockInfo& info)
{
    if (info.creatorPid == 0)
        return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, info.creatorPid);
    if (h) {
        CloseHandle(h);
        return false;
    }
    return GetLastError() == ERROR_INVALID_PARAMETER;   // PID 不复用语义近似
}

// ---------------------------------------------------------------------------
// alias 索引
// ---------------------------------------------------------------------------

bool AliasGet(const std::wstring& alias, AliasEntry* out)
{
    AliasMutex m(3000);
    if (!m.held())
        return false;
    for (const auto& en : LoadAliasesNoLock()) {
        if (_wcsicmp(en.alias.c_str(), alias.c_str()) == 0) {
            *out = en;
            return true;
        }
    }
    return false;
}

V2Err AliasSet(const std::wstring& alias, const std::wstring& box,
               const std::wstring& boxPath)
{
    AliasMutex m(3000);
    if (!m.held())
        return {SbieStatus::GENERIC, L"alias index busy"};
    auto list = LoadAliasesNoLock();
    // 一盒一名：同盒旧别名移除
    for (size_t i = 0; i < list.size();) {
        if (_wcsicmp(list[i].box.c_str(), box.c_str()) == 0
            || _wcsicmp(list[i].alias.c_str(), alias.c_str()) == 0)
            list.erase(list.begin() + (long)i);
        else
            ++i;
    }
    AliasEntry en;
    en.alias = alias;
    en.box = box;
    en.boxPath = boxPath;
    en.added = NowIsoTimestamp();
    list.push_back(std::move(en));
    return SaveAliasesNoLock(list);
}

V2Err AliasRemove(const std::wstring& alias)
{
    AliasMutex m(3000);
    if (!m.held())
        return {SbieStatus::GENERIC, L"alias index busy"};
    auto list = LoadAliasesNoLock();
    for (size_t i = 0; i < list.size();) {
        if (_wcsicmp(list[i].alias.c_str(), alias.c_str()) == 0)
            list.erase(list.begin() + (long)i);
        else
            ++i;
    }
    return SaveAliasesNoLock(list);
}

V2Err AliasRemoveForBox(const std::wstring& box)
{
    AliasMutex m(3000);
    if (!m.held())
        return {SbieStatus::GENERIC, L"alias index busy"};
    auto list = LoadAliasesNoLock();
    for (size_t i = 0; i < list.size();) {
        if (_wcsicmp(list[i].box.c_str(), box.c_str()) == 0)
            list.erase(list.begin() + (long)i);
        else
            ++i;
    }
    return SaveAliasesNoLock(list);
}

std::vector<AliasEntry> AliasList()
{
    AliasMutex m(3000);
    if (!m.held())
        return {};
    return LoadAliasesNoLock();
}

// ---------------------------------------------------------------------------
// 目标解析
// ---------------------------------------------------------------------------

BoxTarget ResolveTarget(const std::wstring& arg)
{
    BoxTarget t;
    if (arg.size() >= 2 && arg[0] == L'*') {
        std::wstring alias = arg.substr(1);
        AliasEntry en;
        if (!AliasGet(alias, &en)) {
            t.status = {SbieStatus::NOT_FOUND, L"unknown alias: " + alias};
            return t;
        }
        t.alias = alias;
        t.box = en.box;
        t.boxDir = en.boxPath;
    } else {
        t.boxDir = NormalizeDirPath(arg);
        if (t.boxDir.empty()) {
            t.status = {SbieStatus::NOT_FOUND, L"box directory not found: " + arg};
            return t;
        }
        t.box = BoxNameFromPath(t.boxDir);
    }
    V2Err e = ValidateBoxName(t.box);
    if (!e.Ok()) {
        t.status = {e.code, e.msg + L" (directory: " + t.boxDir + L")"};
        return t;
    }
    if (!PathExists(t.boxDir + L"\\sandbox.ini")) {
        t.status = {SbieStatus::NOT_FOUND,
                    t.boxDir + L"\\sandbox.ini not found (not a V2 box directory)"};
        return t;
    }
    return t;
}

} // namespace sbie::model::v2
