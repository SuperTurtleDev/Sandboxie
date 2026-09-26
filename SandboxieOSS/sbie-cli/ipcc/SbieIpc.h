// Sandboxie-OSS — sbie-cli/ipcc/SbieIpc.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// client↔server 唯一共享协议头（契约：04-modules.md §2.5）。
//
// 帧格式（命名管道字节流，小端）：
//   struct Frame { u32 magic = 'SBOS'; u32 version = 1; u32 msgid; u32 payloadLen;
//                  u8  payload[payloadLen]; }
// payload 恒为 UTF-8 JSON 对象。
// 请求:  {"op":"<OpName>", "params":{…}}
// 回复:  成功 {"ok":true, "data":{…}}；失败 {"ok":false,
//        "error":{"code":<int 退出码语义>, "message":"…", "ntstatus":"0xC0000022"(可选)}}
// msgid：每连接自增的关联号，回复回填同值；错误回复 msgid 高位置
// kMsgIdErrorFlag（00 §6）。server 主动推送（log.event）用 msgid=0。
// OpName 全集 = 04 §4 命令树的 "ipc op" 列（下方 kOp* 常量全集）；server 按
// op 分派到 Model 层。
//
// 管道命名（00-architecture §3）：\\.\pipe\SbieOSS_Cli_S<session_id>。
// server 单实例互斥体（00 §4）：Local\SbieOSS_Server_S<session_id>。

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include <windows.h>

namespace sbie::ipc {

constexpr uint32_t kIpcMagic   = 0x53424F53;  // 'SBOS'（小端读作 "SBOS"）
constexpr uint32_t kIpcVersion = 1;
constexpr size_t   kFrameHeaderLen = 16;      // magic+version+msgid+payloadLen
constexpr uint32_t kMaxPayloadLen  = 1 << 20; // 1 MiB 上限（防恶意管道）
constexpr int      kMaxPipeInstances = 16;    // 00 §3

// ---- OpName 全集（04 §4） ----
constexpr const char* kOpStatus     = "status";
constexpr const char* kOpVersion    = "version";
constexpr const char* kOpServerShutdown = "server.shutdown";
constexpr const char* kOpBoxList    = "box.list";
constexpr const char* kOpBoxInfo    = "box.info";
constexpr const char* kOpBoxCreate  = "box.create";
constexpr const char* kOpBoxDelete  = "box.delete";
constexpr const char* kOpBoxRename  = "box.rename";
constexpr const char* kOpBoxSetEnabled = "box.setEnabled";
constexpr const char* kOpBoxSet     = "box.set";
constexpr const char* kOpBoxGet     = "box.get";
constexpr const char* kOpBoxListSetting = "box.listSetting";
constexpr const char* kOpBoxClean   = "box.clean";
constexpr const char* kOpBoxSize    = "box.size";
constexpr const char* kOpRecoverList = "recover.list";   // P0-12 文件恢复
constexpr const char* kOpRecoverCopy = "recover.copy";   //（写语义 retry=false）
constexpr const char* kOpRecoverAdd  = "recover.add";    //（写语义 retry=false）
constexpr const char* kOpBoxSnapList   = "box.snap.list";
constexpr const char* kOpBoxSnapTake   = "box.snap.take";
constexpr const char* kOpBoxSnapRemove = "box.snap.remove";
constexpr const char* kOpBoxSnapSelect = "box.snap.select";
constexpr const char* kOpBoxSnapSetInfo= "box.snap.setInfo";
constexpr const char* kOpProcList   = "proc.list";
constexpr const char* kOpProcInfo   = "proc.info";
constexpr const char* kOpProcStart  = "proc.start";
constexpr const char* kOpProcKill   = "proc.kill";
constexpr const char* kOpProcKillAll= "proc.killAll";
constexpr const char* kOpProcSuspend  = "proc.suspend";
constexpr const char* kOpProcResume   = "proc.resume";
constexpr const char* kOpCfgGet     = "cfg.get";
constexpr const char* kOpCfgSet     = "cfg.set";
constexpr const char* kOpCfgUnset   = "cfg.unset";
constexpr const char* kOpCfgListSetting = "cfg.listSetting";
constexpr const char* kOpCfgReload  = "cfg.reload";
constexpr const char* kOpCfgPath    = "cfg.path";
constexpr const char* kOpCfgLock    = "cfg.lock";
constexpr const char* kOpCfgUnlock  = "cfg.unlock";
constexpr const char* kOpTplList   = "tpl.list";
constexpr const char* kOpTplInfo   = "tpl.info";
constexpr const char* kOpTplApply  = "tpl.apply";
constexpr const char* kOpTplRevoke = "tpl.revoke";
constexpr const char* kOpTplCheck  = "tpl.check";
constexpr const char* kOpLogWatch  = "log.watch";
constexpr const char* kOpLogDump   = "log.dump";
constexpr const char* kOpLogEvent  = "log.event";   // server → 订阅连接推送
// P1 清尾波次（docs/04 §15）：force 禁用强制运行（直驱动，无 SbieSvc）；
// proc.killAll 参数化 box 可空 = 全局终止（P1-3，沿用 kOpProcKillAll）
constexpr const char* kOpForceSet    = "force.set";
constexpr const char* kOpForceStatus = "force.status";

// 会话级命名（00 §2-§4）
std::wstring PipeName();     // \\.\pipe\SbieOSS_Cli_S<N>
std::wstring MutexName();    // Local\SbieOSS_Server_S<N>
uint32_t    CurrentSessionId();

// ---------------------------------------------------------------------------
// 帧编解码（server 波次实现；client 直连模式不经管道）
// ---------------------------------------------------------------------------

struct FrameHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t msgid;
    uint32_t payloadLen;
};
static_assert(sizeof(FrameHeader) == kFrameHeaderLen, "frame header layout");

// 回复帧 msgid 高位错误标志（00 §6：错误回复置位；与 JSON error 字段冗余双保险）
constexpr uint32_t kMsgIdErrorFlag = 0x80000000u;

// 阻塞式帧写出（单次 WriteFile = 单条消息，消息模式管道保边界）。
// 句柄须为同步模式。payloadLen 超 kMaxPayloadLen 或写失败 → false。
bool WriteFrame(HANDLE h, uint32_t msgid, const void* payload, uint32_t payloadLen);

// 阻塞式帧读取：先读满 16 字节头（校验 magic/version/payloadLen），再读满
// payload。任何短读/校验失败 → false（调用方按连接断裂处理）。
bool ReadFrame(HANDLE h, FrameHeader* outHeader, std::vector<uint8_t>* outPayload);

// client 侧连接：探测/打开会话管道 + 请求往返（00 §5）。
// 单连接单线程使用（CLI 短生命周期进程内串行）。
class PipeClient {
public:
    PipeClient() = default;
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;

    // 打开 \\.\pipe\SbieOSS_Cli_S<N>。ERROR_PIPE_BUSY 时在 waitBusyMs 预算内
    // WaitNamedPipeW + 重试（00 §5 (D)）。成功后置于 MESSAGE 读模式。
    bool Open(DWORD waitBusyMs);
    void Close();
    bool IsOpen() const { return pipe_ != nullptr; }
    HANDLE Handle() const { return pipe_; }
    DWORD LastError() const { return lastErr_; }

    // 一次请求往返：发送 {"op":…,"params":…}，等待同 msgid 回复帧。
    // 返回 false = 传输层失败（server 死亡/管道断裂/帧损坏，连接已关闭）。
    // replyJson 接收回复 payload 原文（UTF-8 JSON，调用方解析）。
    bool RoundTrip(const char* op, const std::string& paramsJson,
                   std::string* replyJson);

private:
    HANDLE pipe_ = nullptr;
    uint32_t nextMsgId_ = 1;
    DWORD lastErr_ = 0;
};

} // namespace sbie::ipc
