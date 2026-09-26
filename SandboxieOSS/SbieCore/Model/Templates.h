// Sandboxie-OSS — SbieCore/Model/Templates.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 模板注册表（契约：04-modules.md §2.4；签名冻结）。
// 激活方式（04 §8.1 已核实）：box 节多值设置 "Template=<名>"；
// 模板本体 = [Template_<名>] 节，类目 = 其 Tmpl.Class 值。
// M1 状态：契约头 + List（读驱动缓存）已实现；Info/Apply/Revoke/Applied 占位
// （写路径经 SbieSvc；Applied 读路径可直连，M2 补全）。

#pragma once

#include "Boxes.h"
#include "../Util/Status.h"

#include <string>
#include <utility>
#include <vector>

namespace sbie::model {

struct TemplateInfo {
    std::wstring name, clazz, descr;
};

class TemplateRegistry {
public:
    explicit TemplateRegistry(sbie::drv::Api* api, sbie::svc::SvcClient& svc);

    std::vector<TemplateInfo> List(const std::wstring& clazzFilter /*L"*"=全部*/);
    SbieStatus Info(const std::wstring& name,
                    std::vector<std::pair<std::wstring, std::wstring>>* settings);
    SbieStatus Apply(const std::wstring& box, const std::wstring& tmpl);   // Append "Template"
    SbieStatus Revoke(const std::wstring& box, const std::wstring& tmpl);  // Delete 该值
    std::vector<std::wstring> Applied(const std::wstring& box);

private:
    sbie::drv::Api* api_;
    sbie::svc::SvcClient& svc_;
};

} // namespace sbie::model
