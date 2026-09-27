// Sandboxie-OSS — SbieCore/SvcClient/SvcClient.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 行为参考（按 01-license-map §2 允许范围）：QSbieAPI\SbieAPI.cpp 的
// CSbieAPI__ConnectPort / CSbieAPI__CallServer（LGPL-2.1，仅协议参考，
// 未复制代码）。协议规格逐条来自 03-svc-protocol.md §1-§2。

#include "SvcClient.h"
#include "../Util/Ntdll.h"

// vendor 协议头（msgids.h / sbieiniwire.h / ProcessWire.h；
// defines_win32.h 提供 BOXNAME_COUNT/PORT_MESSAGE）
#include "msgids.h"
#include "sbieiniwire.h"
#include "ProcessWire.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <vector>

namespace sbie::svc {

namespace {

// RunSandboxed 最近一次服务端 win32 错误（LastRunSandboxedWin32 读取）
ULONG s_lastRunWin32 = 0;

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

SbieStatus SvcClient::RunSandboxed(const std::wstring& box,
                                   const std::wstring& cmd,
                                   const std::wstring& dir,
                                   ULONG creationFlags, RunResult* out)
{
    if (box.size() >= BOXNAME_COUNT)
        return SbieStatus::INVALID;
    if (cmd.empty())
        return SbieStatus::INVALID;

    // 对齐 QSbieAPI（SbieAPI.cpp:1945-1946）：dir 空 = 调用方 cwd；
    // env = 调用方环境块原样继承（MakeEnvironment(true) 语义）。空块会令
    // 盒内进程在 SbieDll 初始化/运行期缺 %SystemRoot% 等而早夭（实测）。
    std::wstring workDir = dir;
    if (workDir.empty()) {
        wchar_t cwd[MAX_PATH];
        DWORD n = GetCurrentDirectoryW((DWORD)(sizeof(cwd) / sizeof(wchar_t)), cwd);
        workDir = n && n < sizeof(cwd) / sizeof(wchar_t) ? std::wstring(cwd, n)
                                                         : std::wstring(L"C:\\");
    }

    // 构造双 NUL 结尾的环境块（WCHAR）
    std::vector<WCHAR> envBlock;
    {
        LPWCH env = GetEnvironmentStringsW();
        if (env) {
            LPWCH p = env;
            while (*p) {
                size_t len = wcslen(p);
                envBlock.insert(envBlock.end(), p, p + len + 1);
                p += len + 1;
            }
            FreeEnvironmentStringsW(env);
        }
        if (envBlock.empty())
            envBlock.push_back(L'\0');   // 空环境：至少一个 NUL + 下方尾部 NUL
    }

    // 变长区布局对齐 QSbieAPI（SbieAPI.cpp:1975-2000）：ofs = 字节偏移（相对
    // 结构体起点），len = WCHAR 数（不含结尾 NUL）；每段后随 NUL
    const size_t fixedLen = sizeof(PROCESS_RUN_SANDBOXED_REQ);
    const size_t cmdChars = cmd.size();
    const size_t dirChars = workDir.size();
    const size_t envChars = envBlock.size();   // 含段间 NUL；不含公共尾部 NUL
    const size_t cmdOfs = fixedLen;
    const size_t dirOfs = cmdOfs + (cmdChars + 1) * sizeof(WCHAR);
    const size_t envOfs = dirOfs + (dirChars + 1) * sizeof(WCHAR);
    const size_t reqLen = envOfs + (envChars + 1) * sizeof(WCHAR);
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
    req->env_len = (ULONG)envChars;
    memcpy((UCHAR*)req + cmdOfs, cmd.c_str(), (cmdChars + 1) * sizeof(WCHAR));
    memcpy((UCHAR*)req + dirOfs, workDir.c_str(), (dirChars + 1) * sizeof(WCHAR));
    if (envChars)
        memcpy((UCHAR*)req + envOfs, envBlock.data(), envChars * sizeof(WCHAR));
    *(WCHAR*)((UCHAR*)req + envOfs + envChars * sizeof(WCHAR)) = L'\0';
    req->si_flags = STARTF_FORCEOFFFEEDBACK;
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
        s_lastRunWin32 = r->h.status;
        free(rpl);
        return SbieStatus::GENERIC;
    }
    s_lastRunWin32 = 0;
    out->hProcess = (HANDLE)(ULONG_PTR)r->hProcess;
    out->pid = r->dwProcessId;
    if (r->hThread)
        CloseHandle((HANDLE)(ULONG_PTR)r->hThread); // hThread 本项目暂不留用
    free(rpl);
    return SbieStatus::OK;
}

ULONG SvcClient::LastRunSandboxedWin32()
{
    return s_lastRunWin32;
}

} // namespace sbie::svc
