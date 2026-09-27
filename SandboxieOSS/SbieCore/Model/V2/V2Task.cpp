// Sandboxie-OSS — SbieCore/Model/V2/V2Task.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// V2Task.h 实现。

#include "V2Task.h"
#include "../../Util/Json.h"
#include "../../Util/Utf8.h"

#include <windows.h>

namespace sbie::model::v2 {

bool TaskExists(const std::wstring& box)
{
    return PathExists(TaskPathFor(box));
}

V2Err WriteTask(const TaskEntry& t)
{
    if (!EnsureRuntimeDirs())
        return {SbieStatus::GENERIC, L"cannot create runtime dirs"};
    json::JsonValue o = json::JsonValue::Object();
    o.set(L"v", json::JsonValue(1));
    o.set(L"box", json::JsonValue(t.box));
    o.set(L"box_path", json::JsonValue(t.boxPath));
    o.set(L"cache", json::JsonValue(t.cache));
    o.set(L"alias", json::JsonValue(t.alias));
    o.set(L"creator_pid", json::JsonValue((long long)t.creatorPid));
    o.set(L"created", json::JsonValue(t.created));
    return WriteTextFileAtomic(TaskPathFor(t.box), json::SerializeUtf8(o));
}

V2Err DeleteTask(const std::wstring& box)
{
    const std::wstring p = TaskPathFor(box);
    if (!PathExists(p))
        return {};
    if (!DeleteFileW(p.c_str()))
        return {SbieStatus::GENERIC, L"cannot delete task: " + p};
    return {};
}

std::vector<TaskEntry> EnumTasks()
{
    std::vector<TaskEntry> list;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((MonitorsDir() + L"\\*.task").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return list;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::wstring stem(fd.cFileName);
        size_t dot = stem.rfind(L'.');
        if (dot != std::wstring::npos)
            stem.resize(dot);
        V2Err e;
        std::wstring text = FileReadAll(MonitorsDir() + L"\\" + fd.cFileName, &e);
        if (!e.Ok() || text.empty())
            continue;
        json::JsonValue root;
        SbieStatus je;
        if (!json::Parse(util::WideToUtf8(text), &root, &je) || !root.isObject())
            continue;
        TaskEntry t;
        t.box = stem;   // 文件名即权威（内容缺 box 也能对账）
        if (const json::JsonValue* v = root.find(L"box"))
            if (v->isString() && !v->asString().empty())
                t.box = v->asString();
        if (const json::JsonValue* v = root.find(L"box_path"))
            t.boxPath = v->asString();
        if (const json::JsonValue* v = root.find(L"cache"))
            t.cache = v->asString();
        if (const json::JsonValue* v = root.find(L"alias"))
            t.alias = v->asString();
        if (const json::JsonValue* v = root.find(L"creator_pid"))
            if (v->isInt())
                t.creatorPid = (DWORD)v->asInt();
        if (const json::JsonValue* v = root.find(L"created"))
            t.created = v->asString();
        if (!t.box.empty())
            list.push_back(std::move(t));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return list;
}

} // namespace sbie::model::v2
