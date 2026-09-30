// Sandboxie-OSS — sbie-cli/v3/test_conf_crud.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// User-mode unit test for the dynamic-box configuration layer
// (docs/12-dynamic-arch.md §3.2/§5).
//
// What it verifies (logic-level, kernel structures mirrored here):
//
//   1. KV grammar accepted by BoxDyn_LoadConfigText (box_dynamic.c):
//      the parse loop below is a byte-for-byte port of the kernel loop
//      with Conf_AddTempSetting replaced by a recording callback.
//      MUST be kept in sync with core/drv/box_dynamic.c.
//
//   2. Temp-section CRUD semantics of Conf_CreateTempSection /
//      Conf_AddTempSetting / Conf_DeleteTempSection / Conf_HasSection
//      (core/drv/conf.c):  create -> exists, duplicate create refused,
//      repeated setting names append (list semantics), deleting a
//      non-virtual (ini-sourced) section refused, deleting a virtual
//      section removes it, is_virtual survives a simulated reload
//      re-import (the Conf_Read preservation rule).
//
// Build:  cl /nologo /EHsc /W4 test_conf_crud.cpp && test_conf_crud.exe

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// part 1: KV grammar (port of BoxDyn_LoadConfigText from box_dynamic.c)
// ---------------------------------------------------------------------------

struct KvPair {
    std::wstring name;
    std::wstring value;
};

// returns false where the kernel path would return STATUS_INVALID_PARAMETER
static bool KvParse(const std::wstring& text_in, std::vector<KvPair>* out)
{
    // kernel parses its private mutable copy; emulate that
    std::wstring text(text_in);
    wchar_t* line = &text[0];
    bool ok = true;

    while (line) {

        wchar_t* eol = wcschr(line, L'\n');
        if (eol) {
            if (eol > line && eol[-1] == L'\r')
                eol[-1] = L'\0';
            *eol = L'\0';
        }

        while (*line == L' ' || *line == L'\t')
            ++line;

        if (*line == L'\0' || *line == L'#' || *line == L';')
            goto next_line;

        wchar_t* name = line;
        wchar_t* name_end = wcschr(name, L'=');
        if ((!name_end) || name_end == name) {
            ok = false;
            break;
        }
        wchar_t* value = name_end + 1;

        while (name_end > name &&
                (name_end[-1] == L' ' || name_end[-1] == L'\t'))
            --name_end;
        *name_end = L'\0';

        while (*value == L' ' || *value == L'\t')
            ++value;

        wchar_t* value_end = value + wcslen(value);
        while (value_end > value &&
                (value_end[-1] == L' ' || value_end[-1] == L'\t'))
            --value_end;
        *value_end = L'\0';

        if (*value == L'\0') {
            ok = false;
            break;
        }

        out->push_back(KvPair{ name, value });

    next_line:

        line = eol ? eol + 1 : NULL;
    }

    return ok;
}

// ---------------------------------------------------------------------------
// part 2: temp-section CRUD semantics (mirror of conf.c structures)
// ---------------------------------------------------------------------------

struct TestSection {
    std::wstring name;
    std::vector<KvPair> settings;
    bool from_template = false;
    bool is_virtual = false;
};

struct TestConf {
    std::vector<TestSection> sections;

    TestSection* Find(const std::wstring& name) {
        for (auto& s : sections)
            if (_wcsicmp(s.name.c_str(), name.c_str()) == 0)
                return &s;
        return nullptr;
    }

    // Conf_CreateTempSection
    int CreateTempSection(const std::wstring& name) {
        if (Find(name))
            return 0xC0000035;              // STATUS_OBJECT_NAME_EXISTS
        TestSection s;
        s.name = name;
        s.is_virtual = true;                // <- key marker
        sections.push_back(s);
        return 0;
    }

    // Conf_AddTempSetting (append, list semantics)
    int AddTempSetting(const std::wstring& sec, const std::wstring& k,
                       const std::wstring& v) {
        TestSection* s = Find(sec);
        if (!s)
            return 0xC0000034;              // STATUS_OBJECT_NAME_NOT_FOUND
        if (!s->is_virtual)
            return 0xC0000022;              // STATUS_ACCESS_DENIED
        s->settings.push_back(KvPair{ k, v });
        return 0;
    }

    // Conf_DeleteTempSection (refuses non-virtual)
    int DeleteTempSection(const std::wstring& name) {
        for (size_t i = 0; i < sections.size(); ++i) {
            if (_wcsicmp(sections[i].name.c_str(), name.c_str()) == 0) {
                if (!sections[i].is_virtual)
                    return 0xC0000022;      // STATUS_ACCESS_DENIED
                sections.erase(sections.begin() + i);
                return 0;
            }
        }
        return 0xC0000034;
    }

    bool HasSection(const std::wstring& name) { return Find(name) != nullptr; }

    // Conf_Read reload rule: is_virtual sections are re-imported into the
    // new data; from_template settings are dropped, the section is created
    // as virtual again
    TestConf SimulateReload() {
        TestConf next;
        for (auto& s : sections) {
            if (!s.is_virtual)
                continue;
            next.CreateTempSection(s.name);
            TestSection* ns = next.Find(s.name);
            ns->from_template = false;      // Conf_Update re-adds as virtual
            for (auto& kv : s.settings)
                ns->settings.push_back(kv);
        }
        return next;
    }
};

// ---------------------------------------------------------------------------
// test driver
// ---------------------------------------------------------------------------

static int g_failures = 0;

static void Check(bool cond, const char* what) {
    if (cond) {
        printf("PASS  %s\n", what);
    } else {
        printf("FAIL  %s\n", what);
        ++g_failures;
    }
}

static void TestKvGrammar()
{
    printf("--- KV grammar (BoxDyn_LoadConfigText) ---\n");

    std::vector<KvPair> kv;

    // normal content, CRLF, comments, blank lines, trimming
    Check(KvParse(
        L"# comment\r\n"
        L"FileRootPath=\\??\\C:\\Sandbox\\Dyn\\%SANDBOX%\r\n"
        L"\r\n"
        L"  KeyRootPath = HKEY_CURRENT_USER\\Dyn_%SANDBOX%  \r\n"
        L"; semicolon comment\n"
        L"OpenFilePath=\\Device\\NamedPipe\\x\n"
        L"OpenFilePath=\\Device\\NamedPipe\\y\n"      // repeated key appends
        L"Enabled=y\n",
        &kv), "parse mixed KV text");
    Check(kv.size() == 5, "pair count (comments/blanks skipped)");
    Check(kv[0].name == L"FileRootPath" &&
          kv[0].value == L"\\??\\C:\\Sandbox\\Dyn\\%SANDBOX%",
          "plain pair");
    Check(kv[1].name == L"KeyRootPath" &&
          kv[1].value == L"HKEY_CURRENT_USER\\Dyn_%SANDBOX%",
          "name/value whitespace trimmed");
    Check(kv[2].name == L"OpenFilePath" && kv[3].name == L"OpenFilePath" &&
          kv[2].value == L"\\Device\\NamedPipe\\x" &&
          kv[3].value == L"\\Device\\NamedPipe\\y",
          "repeated keys append (list semantics)");
    Check(kv[4].value == L"y", "Enabled=y present");

    kv.clear();
    Check(!KvParse(L"novalue\n", &kv), "line without '=' rejected");
    kv.clear();
    Check(!KvParse(L"=novalue\n", &kv), "empty name rejected");
    kv.clear();
    Check(!KvParse(L"Key=\n", &kv), "empty value rejected");
    kv.clear();
    Check(KvParse(L"a=1\nlast=no-newline", &kv) && kv.size() == 2,
          "final line without newline accepted");
}

static void TestCrudSemantics()
{
    printf("--- temp-section CRUD (Conf_*TempSection) ---\n");

    TestConf conf;

    Check(conf.CreateTempSection(L"BoxConfig_1") == 0, "create temp section");
    Check(conf.HasSection(L"BoxConfig_1"), "HasSection after create");
    Check(conf.CreateTempSection(L"BoxConfig_1") == 0xC0000035,
          "duplicate create -> STATUS_OBJECT_NAME_EXISTS");
    Check(conf.AddTempSetting(L"BoxConfig_1", L"FileRootPath",
                              L"\\??\\C:\\Dyn") == 0, "add setting");
    Check(conf.AddTempSetting(L"BoxConfig_1", L"OpenFilePath", L"a") == 0 &&
          conf.AddTempSetting(L"BoxConfig_1", L"OpenFilePath", L"b") == 0,
          "append second same-name setting");
    Check(conf.Find(L"BoxConfig_1")->settings.size() == 3,
          "settings stored in order, duplicates kept");

    Check(conf.AddTempSetting(L"BoxConfig_missing", L"k", L"v")
              == 0xC0000034,
          "setting into missing section -> NOT_FOUND");

    // an ini-sourced (non-virtual) section must be protected
    TestSection ini;
    ini.name = L"StaticBox";
    ini.is_virtual = false;
    conf.sections.push_back(ini);
    Check(conf.DeleteTempSection(L"StaticBox") == 0xC0000022,
          "delete of ini section refused (ACCESS_DENIED)");
    Check(conf.AddTempSetting(L"StaticBox", L"k", L"v") == 0xC0000022,
          "add to ini section via temp API refused");

    Check(conf.DeleteTempSection(L"BoxConfig_1") == 0, "delete temp section");
    Check(!conf.HasSection(L"BoxConfig_1"), "gone after delete");

    // reload preservation
    TestConf conf2;
    conf2.CreateTempSection(L"BoxConfig_7");
    conf2.AddTempSetting(L"BoxConfig_7", L"Enabled", L"y");
    TestSection static_sec;
    static_sec.name = L"FromIni";
    static_sec.is_virtual = false;
    conf2.sections.push_back(static_sec);

    TestConf reloaded = conf2.SimulateReload();
    Check(reloaded.HasSection(L"BoxConfig_7") &&
              reloaded.Find(L"BoxConfig_7")->is_virtual &&
              reloaded.Find(L"BoxConfig_7")->settings.size() == 1,
          "is_virtual section survives reload with settings");
    Check(!reloaded.HasSection(L"FromIni"),
          "non-virtual section comes from ini alone (not re-imported)");
}

int main()
{
    TestKvGrammar();
    TestCrudSemantics();

    printf("\n%s (%d failure%s)\n",
           g_failures == 0 ? "ALL PASS" : "FAILURES",
           g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
