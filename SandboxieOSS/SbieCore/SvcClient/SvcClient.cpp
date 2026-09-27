// Sandboxie-OSS — SbieCore/SvcClient/SvcClient.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 行为参考（按 01-license-map §2 允许范围）：QSbieAPI\SbieAPI.cpp 的
// CSbieAPI__ConnectPort / CSbieAPI__CallServer（LGPL-2.1，仅协议参考，
// 未复制代码）。协议规格逐条来自 03-svc-protocol.md §1-§2。

#include "SvcClient.h"
#include "../Util/Ntdll.h"

// vendor 协议头（msgids.h / sbieiniwire.h / ProcessWire.h / MountManagerWire.h；
// defines_win32.h 提供 BOXNAME_COUNT/PORT_MESSAGE）
#include "msgids.h"
#include "sbieiniwire.h"
#include "ProcessWire.h"
#include "MountManagerWire.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>

namespace sbie::svc {

namespace {

// LPC 端口状态（SvcClient::Impl 的私有载荷；自由函数只依赖本结构）
struct PortState {
    HANDLE port = nullptr;
    ULONG  maxDataLen = 0;   // 每消息数据区容量（字节）
    ULONG  seqNumber = 0;
    bool   wow64 = false;

    void Close()
    {
        if (port) {
            nt::NtdllApi* ntdll = nt::Get();
            if (ntdll && ntdll->NtClose)
                ntdll->NtClose(port);
            port = nullptr;
        }
    }
};

} // namespace

struct SvcClient::Impl {
    std::mutex mtx;          // 串行化全部 LPC 请求（03 §1；server 波次改专职线程）
    PortState st;

    ~Impl() { st.Close(); }
};

SvcClient& SvcClient::Instance()
{
    static SvcClient inst;
    return inst;
}

SvcClient::Impl* SvcClient::ImplPtr()
{
    static Impl impl;
    return &impl;
}

namespace {

// 端口名：\RPC Control\SbieSvcPort（03 §1；与 SbieDll_PortName 常量一致）
constexpr const wchar_t* kSbieSvcPort = L"\\RPC Control\\SbieSvcPort";

bool IsWow64()
{
#if defined(_WIN64)
    return false; // 本项目仅 x64 目标（05-build §2）
#else
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    return wow != FALSE;
#endif
}

SbieStatus ConnectPort(PortState* m)
{
    if (m->port)
        return SbieStatus::OK;

    nt::NtdllApi* ntdll = nt::Get();
    if (!ntdll || !ntdll->NtConnectPort)
        return SbieStatus::ERR_SVC_TRANSPORT;

    SECURITY_QUALITY_OF_SERVICE qos;
    qos.Length = sizeof(qos);
    qos.ImpersonationLevel = SecurityImpersonation;   // SbieSvc 鉴权必需（03 §1）
    qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    qos.EffectiveOnly = TRUE;

    SBIE_UNICODE_STRING portName;
    portName.Buffer = const_cast<PWSTR>(kSbieSvcPort);
    portName.Length = (USHORT)(wcslen(kSbieSvcPort) * sizeof(WCHAR));
    portName.MaximumLength = portName.Length + sizeof(WCHAR);

    HANDLE h = nullptr;
    ULONG maxLen = 0;
    LONG st = ntdll->NtConnectPort(&h, &portName, &qos, nullptr, nullptr,
                                   &maxLen, nullptr, nullptr);
    if (st < 0)
        return SbieStatus::ERR_SVC_TRANSPORT; // SbieSvc 未运行/端口不存在
    m->port = h;

    ULONG sizeofPortMsg = sizeof(PORT_MESSAGE);
    m->wow64 = IsWow64();
    if (m->wow64)
        sizeofPortMsg += sizeof(ULONG) * 4;   // 03 §2.5
    m->maxDataLen = maxLen > sizeofPortMsg ? maxLen - sizeofPortMsg : 0;
    if (m->maxDataLen == 0)
        m->maxDataLen = 200; // 保守下限（正常应为 328-40=288）
    return SbieStatus::OK;
}

// 分块收发（03 §2；一次性实现完整状态机，供全部 MSGID 复用）
SbieStatus CallPort(PortState* m, const void* req, size_t reqLen,
                    void** outRpl, size_t* outRplLen)
{
    *outRpl = nullptr;
    *outRplLen = 0;
    nt::NtdllApi* ntdll = nt::Get();

    const ULONG kSizeofPortMsg = sizeof(PORT_MESSAGE) + (m->wow64 ? sizeof(ULONG) * 4 : 0);

    UCHAR reqBuf[MAX_PORTMSG_LENGTH];
    UCHAR rplBuf[MAX_PORTMSG_LENGTH];
    PORT_MESSAGE* reqHdr = (PORT_MESSAGE*)reqBuf;
    UCHAR* reqData = reqBuf + kSizeofPortMsg;
    PORT_MESSAGE* rplHdr = (PORT_MESSAGE*)rplBuf;
    UCHAR* rplData = rplBuf + kSizeofPortMsg;

    const UCHAR seq = (UCHAR)(m->seqNumber++);

    // client 侧断言：请求总长 <= 0x00FFFFFF（03 §8.2，序号机制上限）
    const MSG_HEADER* hdr = (const MSG_HEADER*)req;
    if (reqLen != hdr->length || hdr->length > 0x00FFFFFF)
        return SbieStatus::GENERIC;

    const UCHAR* cur = (const UCHAR*)req;
    ULONG remain = (ULONG)reqLen;
    while (remain) {
        ULONG send = remain > m->maxDataLen ? m->maxDataLen : remain;
        memset(reqHdr, 0, kSizeofPortMsg);
        reqHdr->u1.s1.DataLength = (USHORT)send;
        reqHdr->u1.s1.TotalLength = (USHORT)(kSizeofPortMsg + send);
        memcpy(reqData, cur, send);
        if (cur == (const UCHAR*)req)
            reqData[3] = seq;   // 首块：h.length 最高字节作序号（03 §2.2）
        cur += send;
        remain -= send;

        LONG st = ntdll->NtRequestWaitReplyPort(m->port, reqBuf, rplBuf);
        if (st < 0) {
            m->Close();
            return SbieStatus::ERR_SVC_TRANSPORT;
        }
        if (remain && rplHdr->u1.s1.DataLength)
            return SbieStatus::ERR_SVC_TRANSPORT; // early reply
    }

    // 末块回复携带首块回复
    ULONG total;
    if (rplHdr->u1.s1.DataLength >= sizeof(MSG_HEADER)) {
        if (rplData[3] != seq)
            return SbieStatus::ERR_SVC_TRANSPORT; // mismatched reply
        rplData[3] = 0;   // 清序号，还原真实 h.length
        total = ((MSG_HEADER*)rplData)->length;
    } else {
        total = 0;
    }
    if (total == 0)
        return SbieStatus::ERR_SVC_TRANSPORT; // null reply

    // 收剩余块（空请求续拉）
    UCHAR* rpl = (UCHAR*)malloc(total);
    if (!rpl)
        return SbieStatus::GENERIC;
    UCHAR* dst = rpl;
    ULONG left = total;
    for (;;) {
        LONG st;
        if (rplHdr->u1.s1.DataLength > left) {
            st = 0xC000002FL; // STATUS_PORT_MESSAGE_TOO_LONG
        } else {
            memcpy(dst, rplData, rplHdr->u1.s1.DataLength);
            dst += rplHdr->u1.s1.DataLength;
            left -= rplHdr->u1.s1.DataLength;
            if (!left)
                break;
            memset(reqHdr, 0, kSizeofPortMsg);
            reqHdr->u1.s1.TotalLength = (USHORT)kSizeofPortMsg; // DataLength=0
            st = ntdll->NtRequestWaitReplyPort(m->port, reqBuf, rplBuf);
        }
        if (st < 0) {
            free(rpl);
            m->Close();
            return SbieStatus::ERR_SVC_TRANSPORT;
        }
    }
    *outRpl = rpl;
    *outRplLen = total;
    return SbieStatus::OK;
}

} // namespace

bool SvcClient::Connected()
{
    Impl* m = ImplPtr();
    std::lock_guard<std::mutex> lock(m->mtx);
    return Ok(ConnectPort(&m->st));
}

SbieStatus SvcClient::Call(const void* req, size_t reqLen, void** outRpl,
                           size_t* outRplLen)
{
    Impl* m = ImplPtr();
    std::lock_guard<std::mutex> lock(m->mtx);
    SbieStatus st = Ok(ConnectPort(&m->st))
                        ? CallPort(&m->st, req, reqLen, outRpl, outRplLen)
                        : SbieStatus::ERR_SVC_TRANSPORT;
    if (!Ok(st)) {
        // 内部重连一次（04 §2.2 契约）
        m->st.Close();
        if (Ok(ConnectPort(&m->st)))
            st = CallPort(&m->st, req, reqLen, outRpl, outRplLen);
    }
    return st;
}

// ---------------------------------------------------------------------------
// 便捷层：SBIE_INI 系列（03 §3）
// ---------------------------------------------------------------------------

namespace {

template <typename T>
SbieStatus CallShort(SvcClient* c, T* req, void** rpl, size_t* rplLen)
{
    req->h.length = sizeof(T);
    return c->Call(req, sizeof(T), rpl, rplLen);
}

} // namespace

SbieStatus SvcClient::IniGetVersion(std::wstring* version, ULONG* abi)
{
    SBIE_INI_GET_VERSION_REQ req{};
    req.h.msgid = MSGID_SBIE_INI_GET_VERSION;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    SBIE_INI_GET_VERSION_RPL* r = (SBIE_INI_GET_VERSION_RPL*)rpl;
    if (r->h.status != 0) {
        st = FromNtStatus((LONG)r->h.status);
        free(rpl);
        return st;
    }
    if (abi)
        *abi = r->abi_ver;
    if (version)
        version->assign(r->version);
    free(rpl);
    return SbieStatus::OK;
}

SbieStatus SvcClient::IniGetPath(std::wstring* path, bool* isHome)
{
    SBIE_INI_GET_PATH_REQ req{};
    req.h.msgid = MSGID_SBIE_INI_GET_PATH;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    SBIE_INI_GET_PATH_RPL* r = (SBIE_INI_GET_PATH_RPL*)rpl;
    if (r->h.status != 0) {
        st = FromNtStatus((LONG)r->h.status);
        free(rpl);
        return st;
    }
    if (isHome)
        *isHome = r->is_home_path != FALSE;
    if (path)
        path->assign(r->path);
    free(rpl);
    return SbieStatus::OK;
}

SbieStatus SvcClient::IniGetUser(bool* admin, std::wstring* section,
                                 std::wstring* name)
{
    SBIE_INI_GET_USER_REQ req{};
    req.h.msgid = MSGID_SBIE_INI_GET_USER;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    SBIE_INI_GET_USER_RPL* r = (SBIE_INI_GET_USER_RPL*)rpl;
    if (r->h.status != 0) {
        st = FromNtStatus((LONG)r->h.status);
        free(rpl);
        return st;
    }
    if (admin)
        *admin = r->admin != FALSE;
    if (section)
        section->assign(r->section);
    if (name && r->name_len > 0) {
        // name_len 为 WCHAR 数；防越界
        size_t avail = (rplLen >= sizeof(*r) + (size_t)r->name_len * sizeof(WCHAR))
                     ? r->name_len
                     : (rplLen > sizeof(*r) ? (rplLen - sizeof(*r)) / sizeof(WCHAR) : 0);
        name->assign(r->name, avail);
    }
    free(rpl);
    return SbieStatus::OK;
}

SbieStatus SvcClient::IniGetSetting(const std::wstring& section,
                                    const std::wstring& setting,
                                    std::wstring* value)
{
    if (section.size() > 64 || setting.size() > 64)
        return SbieStatus::INVALID;
    // 服务端 GetSetting 同样要求 h.length ≥ sizeof(SBIE_INI_SETTING_REQ)
    // （sbieiniserver.cpp:984-986；M1 误按 offsetof(value) 组包 → 必被拒）。
    // GET 不携带 value 数据，定长头即可。
    SBIE_INI_SETTING_REQ req{};
    const size_t reqLen = sizeof(req);
    req.h.msgid = MSGID_SBIE_INI_GET_SETTING;
    req.h.length = (ULONG)reqLen;
    req.refresh = FALSE;
    wcsncpy_s(req.section, section.c_str(), _TRUNCATE);
    wcsncpy_s(req.setting, setting.c_str(), _TRUNCATE);
    req.value_len = 0;

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = Call(&req, reqLen, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    SBIE_INI_SETTING_RPL* r = (SBIE_INI_SETTING_RPL*)rpl;
    if (r->h.status != 0) {
        st = FromNtStatus((LONG)r->h.status);
        free(rpl);
        return st;
    }
    // 服务端 value_len = 数据长 + 1（含结尾 NUL，GetSetting:1012）；多值以
    // L'\n' 连接（CIniFile::GetValue）。剥结尾 NUL 后整体交付，由调用方拆分。
    size_t avail = (rplLen >= sizeof(*r) + (size_t)r->value_len * sizeof(WCHAR))
                 ? r->value_len
                 : (rplLen > sizeof(*r) ? (rplLen - sizeof(*r)) / sizeof(WCHAR) : 0);
    if (avail && r->value[avail - 1] == L'\0')
        --avail;
    value->assign(r->value, avail);
    free(rpl);
    return SbieStatus::OK;
}

// 长度约定（QSbieAPI SbieIniSet，SandboxiePlus\QSbieAPI\SbieAPI.cpp:1259-1288；
// 与服务端 sbieiniserver.cpp CheckRequest :432-449 逐条对齐，坑记录 04 §8.10）：
//   - value_len = wcslen(value)（不含 NUL）——服务端校验点 req->value[req->value_len]
//     必须为 NUL（:446），由 calloc 零化保证（绝不越界读）；
//   - h.length ≥ sizeof(SBIE_INI_SETTING_REQ)（:433 下限；M1 误按 offsetof(value)
//     计，空 value 少 4 字节必被拒）；且 offset(value) + value_len*2 ≤ h.length。
// 与 ConfigStore.cpp::IniSetRaw（同规则的独立落地）行为等价。
// 语义注意：Delete 模式**透传 value**——value_len==0 时服务端 DelSetting 转投
// SetSetting（删整 setting），非空时 RemoveValue（只删该值本身）。
SbieStatus SvcClient::IniSetSetting(const std::wstring& section,
                                    const std::wstring& setting,
                                    const std::wstring& value, SetMode mode,
                                    bool refresh, const std::wstring& password)
{
    if (section.size() > 64 || setting.size() > 64)
        return SbieStatus::INVALID;
    if (password.size() > 64)   // password[66] 定长；显式截断（03 §8.6）
        return SbieStatus::INVALID;

    static const ULONG msgids[] = {
        MSGID_SBIE_INI_SET_SETTING, MSGID_SBIE_INI_ADD_SETTING,
        MSGID_SBIE_INI_INS_SETTING, MSGID_SBIE_INI_DEL_SETTING,
    };
    const ULONG msgid = msgids[(size_t)mode];

    const ULONG valLen = (ULONG)value.size();   // 不含 NUL
    size_t reqLen = offsetof(SBIE_INI_SETTING_REQ, value)
                    + ((size_t)valLen + 1) * sizeof(WCHAR);
    if (reqLen < sizeof(SBIE_INI_SETTING_REQ))
        reqLen = sizeof(SBIE_INI_SETTING_REQ);
    SBIE_INI_SETTING_REQ* req = (SBIE_INI_SETTING_REQ*)calloc(1, reqLen);
    if (!req)
        return SbieStatus::GENERIC;
    req->h.msgid = msgid;
    req->h.length = (ULONG)reqLen;
    wcsncpy_s(req->password, password.c_str(), 64);
    req->refresh = refresh ? TRUE : FALSE;
    wcsncpy_s(req->section, section.c_str(), _TRUNCATE);
    wcsncpy_s(req->setting, setting.c_str(), _TRUNCATE);
    req->value_len = valLen;
    if (valLen)
        memcpy(req->value, value.c_str(), (size_t)valLen * sizeof(WCHAR));
    // value[valLen] 落在 calloc 清零区：满足 CheckRequest 尾零校验

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = Call(req, reqLen, &rpl, &rplLen);
    free(req);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

SbieStatus SvcClient::SetPassword(const std::wstring& oldPw,
                                  const std::wstring& newPw)
{
    if (oldPw.size() > 64 || newPw.size() > 64)
        return SbieStatus::INVALID; // 密码超 64 WCHAR（04 §4.5 cfg lock 退出码 7 语义）
    SBIE_INI_PASSWORD_REQ req{};
    req.h.msgid = MSGID_SBIE_INI_SET_PASSWORD;
    wcsncpy_s(req.old_password, oldPw.c_str(), 64);
    wcsncpy_s(req.new_password, newPw.c_str(), 64);
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

SbieStatus SvcClient::TestPassword(const std::wstring& pw)
{
    if (pw.size() > 64)
        return SbieStatus::INVALID;
    SBIE_INI_PASSWORD_REQ req{};
    req.h.msgid = MSGID_SBIE_INI_TEST_PASSWORD;
    wcsncpy_s(req.old_password, pw.c_str(), 64);
    req.new_password[0] = L'\0';
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

// ---------------------------------------------------------------------------
// 便捷层：ProcessServer 系列（03 §4）
// ---------------------------------------------------------------------------

SbieStatus SvcClient::KillOne(ULONG pid)
{
    PROCESS_KILL_ONE_REQ req{};
    req.h.msgid = MSGID_PROCESS_KILL_ONE;
    req.pid = pid;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

SbieStatus SvcClient::KillAll(const std::wstring& box, ULONG sessionId)
{
    if (box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;
    PROCESS_KILL_ALL_REQ req{};
    req.h.msgid = MSGID_PROCESS_KILL_ALL;
    req.session_id = sessionId;
    wcsncpy_s(req.boxname, box.c_str(), _TRUNCATE);
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

SbieStatus SvcClient::SuspendResume(ULONG pid, bool suspend)
{
    PROCESS_SUSPEND_RESUME_ONE_REQ req{};
    req.h.msgid = MSGID_PROCESS_SUSPEND_RESUME_ONE;
    req.pid = pid;
    req.suspend = suspend ? TRUE : FALSE;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

SbieStatus SvcClient::SuspendResumeAll(const std::wstring& box, bool suspend)
{
    if (box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;
    PROCESS_SUSPEND_RESUME_ALL_REQ req{};
    req.h.msgid = MSGID_PROCESS_SUSPEND_RESUME_ALL;
    req.session_id = (ULONG)-1;
    wcsncpy_s(req.boxname, box.c_str(), _TRUNCATE);
    req.suspend = suspend ? TRUE : FALSE;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    MSG_HEADER* r = (MSG_HEADER*)rpl;
    st = r->status == 0 ? SbieStatus::OK : FromNtStatus((LONG)r->status);
    free(rpl);
    return st;
}

SbieStatus SvcClient::GetProcInfo(ULONG pid, unsigned infoClasses, ProcInfo* out)
{
    PROCESS_GET_INFO_REQ req{};
    req.h.msgid = MSGID_PROCESS_GET_INFO;
    req.dwProcessId = pid;
    req.dwInfoClasses = infoClasses;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallShort(this, &req, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    PROCESS_INFO_RPL* r = (PROCESS_INFO_RPL*)rpl;
    if (r->h.status != 0) {
        st = FromNtStatus((LONG)r->h.status);
        free(rpl);
        return st;
    }
    out->parentId = r->dwParentId;
    out->flags = r->dwInfo;
    out->suspended = r->bSuspended != FALSE;
    auto take = [&](ULONG ofs, ULONG lenChars) -> std::wstring {
        if (ofs == 0 || lenChars == 0 || ofs >= rplLen)
            return {};
        size_t avail = (rplLen - ofs) / sizeof(WCHAR);
        if (avail < lenChars)
            lenChars = (ULONG)avail;
        const WCHAR* s = (const WCHAR*)((const UCHAR*)r + ofs);
        // ofs 相对结构体起点（03 §4），len 为 WCHAR 数
        return std::wstring(s, wcsnlen(s, lenChars));
    };
    out->image = take(r->app_ofs, r->app_len);
    out->cmdline = take(r->cmd_ofs, r->cmd_len);
    out->workdir = take(r->dir_ofs, r->dir_len);
    free(rpl);
    return SbieStatus::OK;
}

SbieStatus SvcClient::RunSandboxed(const std::wstring& box,
                                   const std::wstring& cmd,
                                   const std::wstring& dir,
                                   ULONG creationFlags, RunResult* out)
{
    if (box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;
    // 变长区布局对齐 QSbieAPI（SbieAPI.cpp:1975-2000）：ofs = 字节偏移（相对
    // 结构体起点），len = WCHAR 数（不含结尾 NUL）；每段后随 NUL
    const size_t fixedLen = sizeof(PROCESS_RUN_SANDBOXED_REQ);
    const size_t cmdChars = cmd.size();
    const size_t dirChars = dir.size();
    const size_t cmdOfs = fixedLen;
    const size_t dirOfs = cmdOfs + (cmdChars + 1) * sizeof(WCHAR);
    const size_t envOfs = dirOfs + (dirChars + 1) * sizeof(WCHAR);
    const size_t reqLen = envOfs + sizeof(WCHAR); // 空 env：仅 NUL
    PROCESS_RUN_SANDBOXED_REQ* req = (PROCESS_RUN_SANDBOXED_REQ*)calloc(1, reqLen);
    if (!req)
        return SbieStatus::GENERIC;
    req->h.msgid = MSGID_PROCESS_RUN_SANDBOXED;
    req->h.length = (ULONG)reqLen;
    wcsncpy_s(req->boxname, box.c_str(), _TRUNCATE);
    req->cmd_ofs = (ULONG)cmdOfs;
    req->cmd_len = (ULONG)cmdChars;
    req->dir_ofs = (ULONG)dirOfs;
    req->dir_len = (ULONG)dirChars;
    req->env_ofs = (ULONG)envOfs;
    req->env_len = 0;
    memcpy((UCHAR*)req + cmdOfs, cmd.c_str(), (cmdChars + 1) * sizeof(WCHAR));
    memcpy((UCHAR*)req + dirOfs, dir.c_str(), (dirChars + 1) * sizeof(WCHAR));
    *(WCHAR*)((UCHAR*)req + envOfs) = L'\0';
    req->si_flags = 0;
    req->si_show_window = 1; // SW_SHOWNORMAL
    req->creation_flags = creationFlags;

    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = Call(req, reqLen, &rpl, &rplLen);
    free(req);
    if (!Ok(st))
        return st;
    PROCESS_RUN_SANDBOXED_RPL* r = (PROCESS_RUN_SANDBOXED_RPL*)rpl;
    if (r->h.status != 0) {
        // 该消息的 status 为 win32 错误（03 §4 / sbieiniwire 惯例）
        st = SbieStatus::GENERIC;
        free(rpl);
        return st;
    }
    out->hProcess = (HANDLE)(ULONG_PTR)r->hProcess;
    out->pid = r->dwProcessId;
    if (r->hThread)
        CloseHandle((HANDLE)(ULONG_PTR)r->hThread); // hThread 本项目暂不留用
    free(rpl);
    return SbieStatus::OK;
}

// ---------------------------------------------------------------------------
// 便捷层：ImBox / MountManager 系列（波 D2，docs/04 §18）
// ---------------------------------------------------------------------------

namespace {

// MountManager 回复 status 是 win32 错误码（SHORT_REPLY(ERROR_*)；唯一例外是
// 沙箱内调用者的 STATUS_ACCESS_DENIED 守卫）。区分高位：NTSTATUS 失败码恒
// 0xC000xxxx，win32 错误远小于之。
SbieStatus ImBoxStatusOf(ULONG status)
{
    if (status == 0)
        return SbieStatus::OK;
    if (status & 0x80000000ul)
        return FromNtStatus((LONG)status);
    switch (status) {
    case ERROR_NOT_FOUND:            // 1168：根未挂载
        return SbieStatus::NOT_FOUND;
    case ERROR_DEVICE_NOT_AVAILABLE: // 4319：ImDisk 驱动缺席（SandboxieTools）
        return SbieStatus::DRIVER_UNAVAILABLE;
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
        return SbieStatus::NOT_FOUND;
    case ERROR_ACCESS_DENIED:
        return SbieStatus::ACCESS_DENIED;
    case ERROR_INVALID_PARAMETER:
        return SbieStatus::INVALID;
    default:
        return SbieStatus::GENERIC;  // ERROR_FUNCTION_FAILED 等
    }
}

SbieStatus CallImBoxShort(const void* req, size_t reqLen, ULONG* statusOut,
                          void** rplOut, size_t* rplLenOut)
{
    void* rpl = nullptr;
    size_t rplLen = 0;
    SvcClient* c = &SvcClient::Instance();
    SbieStatus st = c->Call(req, reqLen, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    // SHORT_REPLY 的回复只有 MSG_HEADER；LONG_REPLY 带载荷（调用方 free）
    *statusOut = ((MSG_HEADER*)rpl)->status;
    *rplOut = rpl;
    *rplLenOut = rplLen;
    return SbieStatus::OK;
}

} // namespace

SbieStatus SvcClient::ImBoxCreate(const std::wstring& fileRootDos,
                                  unsigned long long sizeKb,
                                  const std::wstring& password)
{
    if (fileRootDos.empty() || password.size() > 128)
        return SbieStatus::INVALID;
    // 服务端校验 h.length ≥ sizeof(IMBOX_CREATE_REQ)；变长 file_root 尾随其后
    const std::wstring fileRoot = L"\\??\\" + fileRootDos;
    const size_t reqLen = sizeof(IMBOX_CREATE_REQ)
                          + fileRoot.size() * sizeof(WCHAR);
    IMBOX_CREATE_REQ* req = (IMBOX_CREATE_REQ*)calloc(1, reqLen);
    if (!req)
        return SbieStatus::GENERIC;
    req->h.msgid = MSGID_IMBOX_CREATE;
    req->h.length = (ULONG)reqLen;
    req->image_size = sizeKb;
    wcsncpy_s(req->password, password.c_str(), _TRUNCATE);
    // file_root 声明为 [1]：变长尾区，memcpy 越过定长界（calloc 覆盖整个 reqLen）
    memcpy(req->file_root, fileRoot.c_str(),
           (fileRoot.size() + 1) * sizeof(WCHAR));

    ULONG status = 0;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallImBoxShort(req, reqLen, &status, &rpl, &rplLen);
    free(req);
    if (!Ok(st))
        return st;
    free(rpl);
    return ImBoxStatusOf(status);
}

SbieStatus SvcClient::ImBoxMount(const std::wstring& regRootNt,
                                 const std::wstring& fileRootDos,
                                 const std::wstring& password,
                                 bool protectRoot, bool adminOnly,
                                 bool autoUnmount)
{
    if (fileRootDos.empty() || regRootNt.size() >= MAX_REG_ROOT_LEN
        || password.size() > 128)
        return SbieStatus::INVALID;
    const std::wstring fileRoot = L"\\??\\" + fileRootDos;
    const size_t reqLen = sizeof(IMBOX_MOUNT_REQ)
                          + fileRoot.size() * sizeof(WCHAR);
    IMBOX_MOUNT_REQ* req = (IMBOX_MOUNT_REQ*)calloc(1, reqLen);
    if (!req)
        return SbieStatus::GENERIC;
    req->h.msgid = MSGID_IMBOX_MOUNT;
    req->h.length = (ULONG)reqLen;
    wcsncpy_s(req->password, password.c_str(), _TRUNCATE);
    req->protect_root = protectRoot ? TRUE : FALSE;
    req->admin_only = adminOnly ? TRUE : FALSE;
    req->auto_unmount = autoUnmount ? TRUE : FALSE;
    wcscpy_s(req->reg_root, regRootNt.c_str());
    // file_root 声明为 [1]：变长尾区，memcpy 越过定长界（calloc 覆盖整个 reqLen）
    memcpy(req->file_root, fileRoot.c_str(),
           (fileRoot.size() + 1) * sizeof(WCHAR));

    ULONG status = 0;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallImBoxShort(req, reqLen, &status, &rpl, &rplLen);
    free(req);
    if (!Ok(st))
        return st;
    free(rpl);
    return ImBoxStatusOf(status);
}

SbieStatus SvcClient::ImBoxUnmount(const std::wstring& regRootNt)
{
    if (regRootNt.empty() || regRootNt.size() >= MAX_REG_ROOT_LEN)
        return SbieStatus::INVALID;
    IMBOX_UNMOUNT_REQ req{};
    req.h.msgid = MSGID_IMBOX_UNMOUNT;
    req.h.length = sizeof(req);
    wcscpy_s(req.reg_root, regRootNt.c_str());

    ULONG status = 0;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallImBoxShort(&req, sizeof(req), &status, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    free(rpl);
    return ImBoxStatusOf(status);
}

SbieStatus SvcClient::ImBoxEnum(std::vector<std::wstring>* regRoots)
{
    regRoots->clear();
    IMBOX_ENUM_REQ req{};
    req.h.msgid = MSGID_IMBOX_ENUM;
    req.h.length = sizeof(req);

    ULONG status = 0;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallImBoxShort(&req, sizeof(req), &status, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    if (status != 0) {
        free(rpl);
        return ImBoxStatusOf(status);
    }
    // rpl 为 LONG_REPLY：reg_roots = 多串（逐项 NUL 结尾，末尾额外 NUL）
    IMBOX_ENUM_RPL* r = (IMBOX_ENUM_RPL*)rpl;
    const WCHAR* p = r->reg_roots;
    const WCHAR* end = (const WCHAR*)((const UCHAR*)rpl + rplLen);
    while (p < end && *p) {
        const WCHAR* itemEnd = p;
        while (itemEnd < end && *itemEnd)
            ++itemEnd;
        regRoots->emplace_back(p, itemEnd - p);
        p = itemEnd + 1;
    }
    free(rpl);
    return SbieStatus::OK;
}

SbieStatus SvcClient::ImBoxQuery(const std::wstring& regRootNt,
                                 ImDiskMount* out)
{
    if (regRootNt.size() >= MAX_REG_ROOT_LEN)
        return SbieStatus::INVALID;
    IMBOX_QUERY_REQ req{};
    req.h.msgid = MSGID_IMBOX_QUERY;
    req.h.length = sizeof(req);
    wcscpy_s(req.reg_root, regRootNt.c_str());

    ULONG status = 0;
    void* rpl = nullptr;
    size_t rplLen = 0;
    SbieStatus st = CallImBoxShort(&req, sizeof(req), &status, &rpl, &rplLen);
    if (!Ok(st))
        return st;
    if (status != 0) {
        free(rpl);
        return ImBoxStatusOf(status);
    }
    IMBOX_QUERY_RPL* r = (IMBOX_QUERY_RPL*)rpl;
    out->diskSize = r->disk_size;
    out->usedSize = r->used_size;
    // disk_root：NUL 结尾，落在 (rplLen - offsetof(disk_root)) 界内
    const WCHAR* dr = r->disk_root;
    size_t avail = 0;
    if (rplLen > offsetof(IMBOX_QUERY_RPL, disk_root))
        avail = (rplLen - offsetof(IMBOX_QUERY_RPL, disk_root)) / sizeof(WCHAR);
    out->diskRoot.assign(dr, avail ? wcsnlen(dr, avail) : 0);
    out->mounted = true;
    free(rpl);
    return SbieStatus::OK;
}

} // namespace sbie::svc
