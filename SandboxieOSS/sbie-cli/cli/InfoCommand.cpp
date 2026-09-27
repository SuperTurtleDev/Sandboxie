// Sandboxie-OSS — sbie-cli/cli/InfoCommand.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// `sbie-cli info` — 系统概览（R4）：
//   * CLI 版本 / 驱动（版本/abi/alive）/ SbieSvc（版本/连接态）
//   * 主 ini 生效路径：经 SvcClient IniGetPath 查询（服务知道 ini 在哪，
//     用户态零三层探测——docs/10 附录 3 决策）
//   * ImportBox 行状态（驱动缓存枚举 GlobalSettings\ImportBox，标注我们的
//     boxes 目录是否在列）
//   * 注册中的 V2 盒表：盒名/别名/路径/用户进程数/锁/任务/monitor 心跳
//   * monitor 状态：心跳文件 pid + 新鲜度（B2 判据同款）
// 表格 + --json 双轨。

#include "Cli.h"
#include "Commands.h"
#include "Output.h"
#include "../../SbieCore/DriverApi/DriverApi.h"
#include "../../SbieCore/Model/V2/V2Cache.h"
#include "../../SbieCore/Model/V2/V2Common.h"
#include "../../SbieCore/Model/V2/V2Registry.h"
#include "../../SbieCore/Model/V2/V2Task.h"
#include "../../SbieCore/SvcClient/SvcClient.h"
#include "../../SbieCore/Util/Status.h"
#include "../../SbieCore/Util/Utf8.h"
#include "../../SbieCore/Util/Version.h"

#include <windows.h>

#include <string>
#include <vector>

namespace sbie::cli {

using namespace sbie::model::v2;

int CmdInfo(const CommandContext& ctx)
{
    const GlobalOptions& opts = ctx.opts;
    json::JsonValue j = json::JsonValue::Object();

    // ---- CLI / 驱动 ----
    std::wstring cliVer = sbie::kCliVersionW;
    j.set(L"cli_version", json::JsonValue(cliVer));

    std::wstring drvLine = L"unavailable";
    bool driverAlive = false;
    if (drv::LoadSbieDll(opts.sbieDllPath)) {
        drv::VersionInfo vi = drv::GetVersion();
        driverAlive = drv::DriverAlive();
        wchar_t abi[16];
        swprintf_s(abi, L"%X", vi.abi);
        drvLine = vi.version + L" (abi 0x" + abi + L", "
                  + (driverAlive ? L"alive" : L"absent") + L")";
        json::JsonValue d = json::JsonValue::Object();
        d.set(L"version", json::JsonValue(vi.version));
        d.set(L"abi", json::JsonValue((long long)vi.abi));
        d.set(L"alive", json::JsonValue(driverAlive));
        j.set(L"driver", std::move(d));
    } else {
        j.set(L"driver", json::JsonValue(nullptr));
    }

    // ---- SbieSvc：版本 + 连接 + 主 ini 生效路径（零用户态探测）----
    std::wstring svcLine = L"disconnected";
    std::wstring iniPath = L"unknown";
    bool isHome = false;
    ULONG svcAbi = 0;
    std::wstring svcVer;
    {
        sbie::svc::SvcClient& svc = sbie::svc::SvcClient::Instance();
        if (svc.Connected()
            && svc.IniGetVersion(&svcVer, &svcAbi) == SbieStatus::OK) {
            wchar_t abi[16];
            swprintf_s(abi, L"%X", svcAbi);
            svcLine = svcVer + L" (abi 0x" + abi + L", connected)";
            std::wstring p;
            bool home = false;
            if (svc.IniGetPath(&p, &home) == SbieStatus::OK) {
                iniPath = p + (home ? L"  (home dir)" : L"");
                isHome = home;
            }
        }
        json::JsonValue sv = json::JsonValue::Object();
        if (!svcVer.empty()) {
            sv.set(L"version", json::JsonValue(svcVer));
            sv.set(L"abi", json::JsonValue((long long)svcAbi));
            sv.set(L"connected", json::JsonValue(true));
        } else {
            sv.set(L"connected", json::JsonValue(false));
        }
        sv.set(L"ini_path", json::JsonValue(iniPath));
        sv.set(L"ini_is_home", json::JsonValue(isHome));
        j.set(L"svc", std::move(sv));
    }

    // ---- ImportBox 行状态 ----
    bool importLineOk = false;
    {
        std::vector<std::wstring> vals;
        if (drv::Loaded()
            && drv::QueryConfList(L"GlobalSettings", L"ImportBox", true, true,
                                  &vals)
                   == SbieStatus::OK) {
            std::wstring want = BoxesDir() + L"\\";
            for (const auto& v : vals)
                if (_wcsicmp(v.c_str(), want.c_str()) == 0)
                    importLineOk = true;
        }
        json::JsonValue ib = json::JsonValue::Object();
        ib.set(L"line_present", json::JsonValue(importLineOk));
        ib.set(L"expected", json::JsonValue(BoxesDir() + L"\\"));
        j.set(L"import_box", std::move(ib));
    }

    // ---- monitor 心跳（B2 判据同款）----
    std::wstring monLine = L"not running";
    DWORD monPid = 0;
    {
        // 先探互斥体：monitor 退出后 status 文件残留（终态 mtime 恒陈旧）
        DWORD sid = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sid);
        HANDLE mMon = OpenMutexW(SYNCHRONIZE, FALSE,
                                 (L"Local\\SbieOSS_Monitor_S"
                                  + std::to_wstring(sid)).c_str());
        bool monRunning = mMon != nullptr;
        if (mMon)
            CloseHandle(mMon);
        WIN32_FILE_ATTRIBUTE_DATA fad;
        const std::wstring st = MonitorsDir() + L"\\monitor.status";
        if (monRunning && GetFileAttributesExW(st.c_str(), GetFileExInfoStandard,
                                               &fad)) {
            FILETIME nowFt;
            GetSystemTimeAsFileTime(&nowFt);
            ULONGLONG now = ((ULONGLONG)nowFt.dwHighDateTime << 32)
                            + nowFt.dwLowDateTime;
            ULONGLONG then = ((ULONGLONG)fad.ftLastWriteTime.dwHighDateTime << 32)
                             + fad.ftLastWriteTime.dwLowDateTime;
            bool fresh = now > then && (now - then) < 3ull * 10000000ull;
            V2Err e;
            std::wstring txt = FileReadAll(st, &e);
            size_t p = txt.find(L"pid=");
            if (p != std::wstring::npos)
                monPid = (DWORD)wcstoul(txt.c_str() + p + 4, nullptr, 10);
            monLine = std::to_wstring(monPid) + L" ("
                      + (fresh ? L"healthy" : L"STALE heartbeat") + L")";
            json::JsonValue m = json::JsonValue::Object();
            m.set(L"pid", json::JsonValue((long long)monPid));
            m.set(L"heartbeat_fresh", json::JsonValue(fresh));
            j.set(L"monitor", std::move(m));
        } else {
            j.set(L"monitor", json::JsonValue(nullptr));
        }
    }

    // ---- V2 盒表 ----
    std::vector<std::pair<std::wstring, std::wstring>> kv;
    kv.emplace_back(L"cli", cliVer);
    kv.emplace_back(L"driver", drvLine);
    kv.emplace_back(L"sbiesvc", svcLine);
    kv.emplace_back(L"main ini", iniPath);
    kv.emplace_back(L"ImportBox line", importLineOk ? L"present" : L"MISSING");
    kv.emplace_back(L"monitor", monLine);

    util::TablePrinter tab;
    tab.AddColumn(L"BOX");
    tab.AddColumn(L"ALIAS");
    tab.AddColumn(L"DIRECTORY");
    tab.AddColumn(L"USER PROCS", true);
    tab.AddColumn(L"LOCK");
    tab.AddColumn(L"TASK");
    json::JsonValue boxes = json::JsonValue::Array();

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((BoxesDir() + L"\\*.ini").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            std::wstring box(fd.cFileName);
            size_t dot = box.rfind(L'.');
            if (dot != std::wstring::npos)
                box.resize(dot);
            if (!ValidateBoxName(box).Ok())
                continue;
            std::wstring alias, dir;
            {
                IniFileData ini;
                if (ParseIniFile(CachePathFor(box), &ini).Ok()) {
                    if (const IniSectionData* sec = ini.Find(box)) {
                        for (const auto& e2 : sec->entries) {
                            if (_wcsicmp(e2.key.c_str(), L"FileRootPath") == 0)
                                dir = e2.value;
                        }
                    }
                }
            }
            for (const auto& a : AliasList())
                if (_wcsicmp(a.box.c_str(), box.c_str()) == 0)
                    alias = a.alias;
            V2Err ce;
            size_t procs = BoxUserProcessCount(box, &ce);
            bool lock = !dir.empty() && PathExists(dir + L"\\running.lock");
            bool task = TaskExists(box);
            tab.AddRow({box, alias.empty() ? L"-" : L"*" + alias,
                        dir.empty() ? L"-" : dir,
                        ce.Ok() ? std::to_wstring(procs) : L"?",
                        lock ? L"yes" : L"-", task ? L"yes" : L"-"});
            json::JsonValue r = json::JsonValue::Object();
            r.set(L"box", json::JsonValue(box));
            if (!alias.empty())
                r.set(L"alias", json::JsonValue(alias));
            if (!dir.empty())
                r.set(L"box_path", json::JsonValue(dir));
            r.set(L"user_procs",
                  json::JsonValue((long long)(ce.Ok() ? (long long)procs : -1)));
            r.set(L"lock", json::JsonValue(lock));
            r.set(L"task", json::JsonValue(task));
            boxes.pushBack(std::move(r));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    j.set(L"boxes", std::move(boxes));

    if (opts.json) {
        EmitJsonOk(opts, j);
        return 0;
    }
    EmitKv(opts, kv, j);
    if (!opts.quiet)
        util::PrintLineUtf8("");
    EmitRows(opts, tab, json::JsonValue::Array(), L"no registered v2 boxes");
    return 0;
}

} // namespace sbie::cli
