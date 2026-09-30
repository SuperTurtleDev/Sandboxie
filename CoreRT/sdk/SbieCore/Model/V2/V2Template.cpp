// Sandboxie-OSS — SbieCore/Model/V2/V2Template.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2Template.h 实现。

#include "V2Template.h"
#include "../../Util/Utf8.h"

#include <windows.h>

#include <map>

namespace sbie::model::v2 {

namespace {

std::vector<std::wstring> g_rootsOverride;
constexpr size_t kMaxDepth = 8;

// 大小写不敏感比较器（std::map 用）
struct CILess {
    bool operator()(const std::wstring& a, const std::wstring& b) const
    {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    }
};

// 内置 %Tmpl.X% 变量表：源自官方 install\Templates.ini [TemplateSettings]
// （GPLv3 资产，值原样固化；保留 %AppData% 等系统变量供驱动按盒用户上下文展开）。
// 迁移脚本（--migrate-templates）产出的模板引用这些名字。
const std::pair<const wchar_t*, const wchar_t*> kBuiltInTmplVars[] = {
    {L"Tmpl.Firefox",       L"%AppData%\\Mozilla\\Firefox\\Profiles\\*"},
    {L"Tmpl.Waterfox",      L"%AppData%\\Waterfox\\Profiles\\*"},
    {L"Tmpl.PaleMoon",      L"%AppData%\\Moonchild Productions\\Pale Moon\\Profiles\\*"},
    {L"Tmpl.SeaMonkey",     L"%AppData%\\Mozilla\\SeaMonkey\\Profiles\\*"},
    {L"Tmpl.LibreWolf",     L"%AppData%\\LibreWolf\\Profiles\\*"},
    {L"Tmpl.Zotero",        L"%Tmpl.Firefox%\\zotero"},
    {L"Tmpl.Chrome",        L"%Local AppData%\\Google\\Chrome\\User Data\\Default"},
    {L"Tmpl.Edge",          L"%Local AppData%\\Microsoft\\Edge\\User Data\\Default"},
    {L"Tmpl.Vivaldi",       L"%Local AppData%\\Vivaldi\\User Data\\Default"},
    {L"Tmpl.Brave",         L"%Local AppData%\\BraveSoftware\\Brave-Browser\\User Data\\Default"},
    {L"Tmpl.Opera",         L"%AppData%\\Opera Software\\Opera Stable\\Default"},
    {L"Tmpl.Yandex",        L"%Local AppData%\\Yandex\\YandexBrowser\\User Data\\Default"},
    {L"Tmpl.Ungoogled",     L"%Local AppData%\\Chromium\\User Data\\Default"},
    {L"Tmpl.Iron",          L"%Local AppData%\\Chromium\\User Data\\Default"},
    {L"Tmpl.Maxthon_6",     L"%Local AppData%\\Maxthon\\Application\\User Data\\Default"},
    {L"Tmpl.Dragon",        L"%Local AppData%\\Comodo\\Dragon\\User Data\\Default"},
    {L"Tmpl.Osiris",        L"%Local AppData%\\Osiris\\Osiris-Browser\\User Data\\Default"},
    {L"Tmpl.Slimjet",       L"%Local AppData%\\Slimjet\\User Data\\Default"},
    {L"Tmpl.Office_Outlook",L"%Local AppData%\\Microsoft\\Outlook"},
    {L"Tmpl.Windows_Vista_Mail",     L"%Local AppData%\\Microsoft\\Windows Mail"},
    {L"Tmpl.Windows_Live_Mail",      L"%Local AppData%\\Microsoft\\Windows Live Mail"},
    {L"Tmpl.Thunderbird",   L"%AppData%\\Thunderbird"},
    {L"Tmpl.RoboForm",      L"%AppData%\\RoboForm"},
    {L"Tmpl.FinePrint",     L"%ProgramFiles%\\FinePrint"},
};

// 展开 %Tmpl.X%（仅模板变量；系统变量透传）。递归解析变量值里的 %Tmpl.Y%
// （如 Tmpl.Zotero = %Tmpl.Firefox%\zotero），深度上限防环。
bool ResolveTmplVar(const std::map<std::wstring, std::wstring, CILess>& vars,
                    const std::wstring& name, std::wstring* out, int depth)
{
    if (depth > 4)
        return false;
    auto it = vars.find(name);
    if (it == vars.end())
        return false;
    const std::wstring& v = it->second;
    // 变量值内再查 %Tmpl.*%
    std::wstring result;
    size_t pos = 0;
    for (;;) {
        size_t p1 = v.find(L'%', pos);
        if (p1 == std::wstring::npos) {
            result += v.substr(pos);
            break;
        }
        size_t p2 = v.find(L'%', p1 + 1);
        if (p2 == std::wstring::npos) {
            result += v.substr(pos);
            break;
        }
        result += v.substr(pos, p1 - pos);
        std::wstring inner = v.substr(p1 + 1, p2 - p1 - 1);
        if (_wcsnicmp(inner.c_str(), L"Tmpl.", 5) == 0) {
            std::wstring resolved;
            if (!ResolveTmplVar(vars, inner, &resolved, depth + 1))
                return false;
            result += resolved;
        } else {
            result += v.substr(p1, p2 - p1 + 1);   // 系统变量原样保留
        }
        pos = p2 + 1;
    }
    *out = result;
    return true;
}

// 值内全部 %Tmpl.X% 冻结；未定义变量报错
V2Err FreezeTmplVars(const std::map<std::wstring, std::wstring, CILess>& vars,
                     const std::wstring& in, std::wstring* out)
{
    std::wstring result;
    size_t pos = 0;
    for (;;) {
        size_t p1 = in.find(L'%', pos);
        if (p1 == std::wstring::npos) {
            result += in.substr(pos);
            break;
        }
        size_t p2 = in.find(L'%', p1 + 1);
        if (p2 == std::wstring::npos) {
            // 尾部孤立 %：原样收尾（驱动 Conf_Expand 同样不解析不成对 %）
            result += in.substr(pos);
            break;
        }
        if (p2 == p1 + 1) {
            // "%%" 转义为字面 %
            result += in.substr(pos, p1 - pos);
            result += L'%';
            pos = p2 + 1;
            continue;
        }
        result += in.substr(pos, p1 - pos);
        std::wstring var = in.substr(p1 + 1, p2 - p1 - 1);
        if (_wcsnicmp(var.c_str(), L"Tmpl.", 5) == 0) {
            std::wstring resolved;
            if (!ResolveTmplVar(vars, var, &resolved, 0))
                return {SbieStatus::INVALID,
                        L"template: undefined template variable %" + var + L"%"};
            result += resolved;
        } else {
            result += in.substr(p1, p2 - p1 + 1);   // 系统变量透传给驱动
        }
        pos = p2 + 1;
    }
    *out = result;
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// 模板根
// ---------------------------------------------------------------------------

std::vector<std::wstring> TemplateRoots()
{
    if (!g_rootsOverride.empty())
        return g_rootsOverride;
    std::vector<std::wstring> roots;
    wchar_t env[2048];
    DWORD n = GetEnvironmentVariableW(L"SBIE_TEMPLATE_DIR", env,
                                      (DWORD)(sizeof(env) / sizeof(wchar_t)));
    if (n != 0 && n < sizeof(env) / sizeof(wchar_t)) {
        std::wstring all(env, n);
        size_t pos = 0;
        for (;;) {
            size_t sc = all.find(L';', pos);
            std::wstring seg = all.substr(
                pos, sc == std::wstring::npos ? std::wstring::npos : sc - pos);
            // 去尾随 '\'
            while (seg.size() > 3 && seg.back() == L'\\')
                seg.pop_back();
            if (!seg.empty())
                roots.push_back(seg);
            if (sc == std::wstring::npos)
                break;
            pos = sc + 1;
        }
    }
    // 内置兜底根：<exe 目录>\templates（dist 零配置自洽：SBIE_TEMPLATE_DIR
    // 未设或未命中时，随包分发的 V2 模板树仍可用；环境变量根优先）。
    wchar_t self[MAX_PATH * 2];
    DWORD m = GetModuleFileNameW(nullptr, self,
                                 (DWORD)(sizeof(self) / sizeof(wchar_t)));
    if (m > 0 && m < sizeof(self) / sizeof(wchar_t)) {
        std::wstring dir(self, m);
        size_t cut = dir.find_last_of(L'\\');
        if (cut != std::wstring::npos) {
            std::wstring fallback = dir.substr(0, cut) + L"\\templates";
            bool dup = false;
            for (const auto& r : roots)
                if (_wcsicmp(r.c_str(), fallback.c_str()) == 0) { dup = true; break; }
            if (!dup && PathExists(fallback))
                roots.push_back(std::move(fallback));
        }
    }
    return roots;
}

void SetTemplateRootsOverride(const std::vector<std::wstring>& roots)
{
    g_rootsOverride = roots;
}

// ---------------------------------------------------------------------------
// 单模板加载
// ---------------------------------------------------------------------------

V2Err LoadTemplate(const std::vector<std::wstring>& roots, const std::wstring& ref,
                   V2TemplateFile* out)
{
    // ref 规范化：剥空白；'/' → '\'（容忍用户写法）
    std::wstring r = ref;
    for (auto& c : r)
        if (c == L'/')
            c = L'\\';
    std::wstring cat, name;
    size_t bs = r.find_last_of(L'\\');
    if (bs == std::wstring::npos) {
        name = r;
    } else {
        cat = r.substr(0, bs);
        name = r.substr(bs + 1);
        if (cat.empty() || name.empty() || name.find(L'\\') != std::wstring::npos)
            return {SbieStatus::NOT_FOUND, L"template: bad reference " + ref};
    }
    if (name.find(L'.') != std::wstring::npos && cat.empty())
        return {SbieStatus::NOT_FOUND,
                L"template: bare name must not contain '.' (ambiguous with file "
                L"extension): " + ref};

    // 候选：带类别 = 唯一路径；裸名 = 枚举根下一级目录（类别）逐一探测
    std::vector<std::wstring> candidates;
    for (const auto& root : roots) {
        if (!cat.empty()) {
            candidates.push_back(root + L"\\" + cat + L"\\" + name + L".ini");
        } else {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    // 类别 = 一级子目录（§3.1）；"."/".." 会把根级/根上级
                    // 的同名 ini 混进候选，排除
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                        && _wcsicmp(fd.cFileName, L".") != 0
                        && _wcsicmp(fd.cFileName, L"..") != 0) {
                        std::wstring p = root + L"\\" + fd.cFileName + L"\\"
                                         + name + L".ini";
                        if (PathExists(p))
                            candidates.push_back(p);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
    }
    // 去重（不同根下同类别同名 = 先者胜）
    std::vector<std::wstring> uniq;
    for (const auto& c : candidates) {
        bool dup = false;
        for (const auto& u : uniq)
            if (_wcsicmp(u.c_str(), c.c_str()) == 0) { dup = true; break; }
        if (!dup)
            uniq.push_back(c);
    }
    if (uniq.empty())
        return {SbieStatus::NOT_FOUND, L"template: not found: " + ref};
    if (cat.empty() && uniq.size() > 1)
        return {SbieStatus::INVALID,
                L"template: ambiguous reference " + ref + L" (matched multiple categories)"};

    // 按扫描序取第一个存在的
    std::wstring path;
    for (const auto& c : uniq) {
        if (PathExists(c)) {
            path = c;
            break;
        }
    }
    if (path.empty())
        return {SbieStatus::NOT_FOUND, L"template: not found: " + ref};

    IniFileData ini;
    V2Err e = ParseIniFile(path, &ini);
    if (!e.Ok())
        return {e.code, L"template: " + e.msg + L" (" + ref + L")"};
    const IniSectionData* sec = ini.Find(L"Template");
    if (!sec)
        return {SbieStatus::INVALID,
                L"template: missing [Template] section in " + path};
    if (ini.sections.size() != 1)
        return {SbieStatus::INVALID,
                L"template: must contain exactly one [Template] section: " + path};

    out->category = cat;
    out->name = name;
    out->title.clear();
    out->entries = sec->entries;
    for (const auto& kv : sec->entries)
        if (_wcsicmp(kv.key.c_str(), L"Tmpl.Title") == 0)
            out->title = kv.value;
    return {};
}

// ---------------------------------------------------------------------------
// 盒配置展开
// ---------------------------------------------------------------------------

namespace {

// 递归展开一个模板引用到 out 尾部（DFS：先展开其嵌套引用自身的直接键）
struct ExpandCtx {
    const std::map<std::wstring, std::wstring, CILess>* vars;
    std::vector<IniKeyValue>* out;
    std::vector<std::wstring>* applied;
    std::vector<std::wstring> stack;    // 环检测：当前引用路径栈
    const std::vector<std::wstring>* roots;
};

V2Err ExpandTemplateRef(ExpandCtx& ctx, const std::wstring& ref, int depth)
{
    if (depth > (int)kMaxDepth)
        return {SbieStatus::INVALID,
                L"template: nesting deeper than 8 at " + ref};
    for (const auto& s : ctx.stack)
        if (_wcsicmp(s.c_str(), ref.c_str()) == 0)
            return {SbieStatus::INVALID, L"template: reference cycle at " + ref};
    ctx.stack.push_back(ref);

    V2TemplateFile tf;
    V2Err e = LoadTemplate(*ctx.roots, ref, &tf);
    if (!e.Ok()) {
        ctx.stack.pop_back();
        return e;
    }
    ctx.applied->push_back(ref);

    // 两遍：先递归嵌套引用（保持"被引者先落地"的直觉序），再落本模板直接键。
    // 注：V1 驱动合并不做嵌套（官方模板互不引用），此序为 V2 规格
    // （docs/10 §3.2/§5.3：被引模板键先于引用者的直接键）。
    for (const auto& kv : tf.entries) {
        if (_wcsicmp(kv.key.c_str(), L"Template") == 0) {
            e = ExpandTemplateRef(ctx, kv.value, depth + 1);
            if (!e.Ok()) {
                ctx.stack.pop_back();
                return e;
            }
        }
    }
    for (const auto& kv : tf.entries) {
        if (_wcsicmp(kv.key.c_str(), L"Template") == 0)
            continue;                                   // 引用已展开
        if (_wcsnicmp(kv.key.c_str(), L"Tmpl.", 5) == 0)
            continue;                                   // 元数据不进缓存
        std::wstring frozen;
        e = FreezeTmplVars(*ctx.vars, kv.value, &frozen);
        if (!e.Ok()) {
            ctx.stack.pop_back();
            return {e.code, e.msg + L" (in template " + ref + L")"};
        }
        IniKeyValue o;
        o.key = kv.key;
        o.value = frozen;
        ctx.out->push_back(std::move(o));
    }
    ctx.stack.pop_back();
    return {};
}

} // namespace

ExpandOutput ExpandBoxConfig(const std::wstring& sandboxIniPath,
                              const std::wstring& boxName,
                              const std::wstring& boxDir)
{
    ExpandOutput out;
    IniFileData ini;
    V2Err e = ParseIniFile(sandboxIniPath, &ini);
    if (!e.Ok()) {
        out.status = e;
        return out;
    }
    const IniSectionData* box = ini.Find(boxName);
    if (!box)
        box = ini.Find(L"");            // 单节文件允许省节头
    if (!box) {
        out.status = {SbieStatus::INVALID,
                      L"sandbox.ini has no [" + boxName + L"] section"};
        return out;
    }

    // 变量表（优先级低 → 高）：内置表 → templates\Basic.ini 的
    // [TemplateSettings]（自动 Include：各模板根按扫描序取第一个命中；
    // §2 分离规格）→ 盒 sandbox.ini [TemplateVars] 覆盖。
    std::map<std::wstring, std::wstring, CILess> vars;
    for (const auto& kv : kBuiltInTmplVars)
        vars[kv.first] = kv.second;
    for (const auto& root : TemplateRoots()) {
        IniFileData basic;
        if (ParseIniFile(root + L"\\Basic.ini", &basic).Ok()) {
            if (const IniSectionData* ts = basic.Find(L"TemplateSettings")) {
                for (const auto& kv : ts->entries)
                    if (_wcsnicmp(kv.key.c_str(), L"Tmpl.", 5) == 0)
                        vars[kv.key] = kv.value;
            }
            break;   // 第一个命中即用（与模板引用同语义）
        }
    }
    if (const IniSectionData* tv = ini.Find(L"TemplateVars")) {
        for (const auto& kv : tv->entries)
            vars[kv.key] = kv.value;
    }

    const std::vector<std::wstring> roots = TemplateRoots();

    // 第一遍：直键（Template= 引用除外；值冻结模板变量）
    std::vector<IniKeyValue> direct;
    std::vector<std::wstring> refs;
    for (const auto& kv : box->entries) {
        if (_wcsicmp(kv.key.c_str(), L"Template") == 0) {
            refs.push_back(kv.value);
            continue;
        }
        if (_wcsnicmp(kv.key.c_str(), L"Tmpl.", 5) == 0)
            continue;   // 元数据不入缓存
        std::wstring frozen;
        V2Err fe = FreezeTmplVars(vars, kv.value, &frozen);
        if (!fe.Ok()) {
            out.status = fe;
            return out;
        }
        IniKeyValue d;
        d.key = kv.key;
        d.value = frozen;
        direct.push_back(std::move(d));
    }

    // 第二遍：模板键（按引用序）
    ExpandCtx ctx{&vars, &out.kv, &out.applied, {}, &roots};
    for (const auto& kv : direct)
        out.kv.push_back(kv);
    for (const auto& ref : refs) {
        V2Err re = ExpandTemplateRef(ctx, ref, 1);
        if (!re.Ok()) {
            out.status = re;
            out.kv.clear();
            out.applied.clear();
            return out;
        }
    }

    // 补缺：FileRootPath（显式直键优先）与 Enabled=y（conf_user.c:381 强制）
    bool haveRoot = false, haveEnabled = false;
    for (const auto& kv : out.kv) {
        if (_wcsicmp(kv.key.c_str(), L"FileRootPath") == 0)
            haveRoot = true;
        if (_wcsicmp(kv.key.c_str(), L"Enabled") == 0)
            haveEnabled = true;
    }
    if (!haveRoot) {
        IniKeyValue kv;
        kv.key = L"FileRootPath";
        kv.value = boxDir;
        out.kv.push_back(std::move(kv));
    }
    if (!haveEnabled) {
        IniKeyValue kv;
        kv.key = L"Enabled";
        kv.value = L"y";
        out.kv.push_back(std::move(kv));
    }
    return out;
}

} // namespace sbie::model::v2
