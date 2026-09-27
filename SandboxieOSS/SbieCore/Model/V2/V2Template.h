// Sandboxie-OSS — SbieCore/Model/V2/V2Template.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2 用户态模板引擎（docs/10-v2-design.md §3/§5）：
//   * 模板根 = 环境变量 SBIE_TEMPLATE_DIR（';' 分隔多根，按序扫描先命中先用）
//   * 组织 = <根>\<类别>\<名字>.ini，恰含一个 [Template] 节
//   * 元数据 Tmpl.* 不进缓存；内容键 = 任意沙盒配置键原样透传
//   * Template= 嵌套引用（用户态递归展开，深度上限 8，环检测）
//   * 变量两段式：%Tmpl.X% 用户态冻结（内置表 + sandbox.ini [TemplateVars] 覆盖）；
//     系统变量（%USER%/%SystemDrive%/…）原样透传给驱动展开（conf_expand.c:332-700）
//   * 合并序：sandbox.ini 直键在前（对齐 V1 驱动"盒自身键先于模板键、index0
//     = 文件首条"的读取序，conf.c:1318-1359），随后按 Template= 引用序追加模板键
//
// 产出 = 最终 KV 序列（供 V2Cache 写缓存）；零 Template= 残留是构造性保证 +
// 写后自检双重保证（docs/10 §5.4，硬性校验项）。

#pragma once

#include "V2Common.h"

namespace sbie::model::v2 {

// 展开结果（成功时 kv 为最终序列）
struct ExpandOutput {
    V2Err status;
    std::vector<IniKeyValue> kv;        // 最终 KV（含 FileRootPath / Enabled）
    std::vector<std::wstring> applied;  // 实际展开的模板引用（溯源，"Games\\MC" 形式）
};

// V2 扩展状态码映射（避免污染 SbieStatus 契约；此处用消息前缀区分模板类错误，
// cli 层统一映射 TEMPLATE_ERROR=11 退出码）
inline bool IsTemplateError(const V2Err& e)
{
    return !e.Ok() && e.msg.rfind(L"template:", 0) == 0;
}

// SBIE_TEMPLATE_DIR → 根列表（空环境变量 = 空列表；去空段）
std::vector<std::wstring> TemplateRoots();
// 调试/工具注入根（SetTemplateRootsOverride：进程内覆盖环境变量；测试与
// --migrate-templates 工具用；传空恢复环境变量语义）
void SetTemplateRootsOverride(const std::vector<std::wstring>& roots);

// 展开盒配置：sandbox.ini（盒目录内）+ 盒目录绝对路径 → 最终 KV。
// 行为：
//   * [TemplateVars] 节 = %Tmpl.X% 覆盖/新增（该节本身不进输出）
//   * 两遍合并：直键（含多值保序保重复）→ 模板键（按引用序、模板内保序）
//   * 补缺：FileRootPath=<boxDir>（盒内显式值优先）；Enabled=y（缺省补）
//   * 单值冲突不删除后到值（驱动 index0=首条即正确优先级；多值键本就需全量）
ExpandOutput ExpandBoxConfig(const std::wstring& sandboxIniPath,
                              const std::wstring& boxName,
                              const std::wstring& boxDir);

// 单模板加载（引用名 "类别\名字" 或裸名；裸名需在全部根中唯一）。
// 供模板迁移工具/未来 doctor 用；ExpandBoxConfig 内部同路径。
struct V2TemplateFile {
    std::wstring category;              // 一级子目录名
    std::wstring name;                  // 文件基名（无扩展）
    std::wstring title;                 // Tmpl.Title
    std::vector<IniKeyValue> entries;   // 含 Tmpl.* 元数据 + Template= 嵌套引用
};
V2Err LoadTemplate(const std::vector<std::wstring>& roots, const std::wstring& ref,
                   V2TemplateFile* out);

} // namespace sbie::model::v2
