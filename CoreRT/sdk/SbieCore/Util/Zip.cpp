// Sandboxie-OSS — SbieCore/Util/Zip.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 最小 zip 读写器实现（格式面与限制见 Zip.h 文件头）。本文件为格式自研
//（PKWARE APPNOTE 的 store 子集），未参考任何第三方实现代码。

#include "Zip.h"
#include "Utf8.h"

#include <cwchar>
#include <cstring>

namespace sbie::util {

namespace {

// ---- 小端整数（本机小端架构直写；x64/ARM64 Windows 均为小端，WriteU16/
// WriteU32 与 RdU16/RdU32 的直写依赖此事实） ----

constexpr uint32_t kSigLocal   = 0x04034B50;
constexpr uint32_t kSigCentral = 0x02014B50;
constexpr uint32_t kSigEocd    = 0x06054B50;
constexpr uint16_t kFlagUtf8   = 0x0800;
constexpr size_t   kChunkLen   = 64 * 1024;
// EOCD 最大回扫窗口：EOCD(22) + 注释上限(65535)
constexpr size_t   kEocdWindow = 22 + 65535;
// 集中目录载入上限（防病态包：8 MiB）
constexpr size_t   kMaxCentralDir = 8u * 1024u * 1024u;
// 单条目尺寸上限（zip 经典 32 位；留 4 GiB 减 1 的形式化上限）
constexpr uint64_t kMaxEntrySize = 0xFFFFFFFFull;

const uint32_t* CrcTable()
{
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    return table;
}

// FILETIME（UTC）→ DOS 日期/时间（本地时区，zip 惯例）
void FileTimeToDos(const FILETIME& ftUtc, uint16_t* dosDate, uint16_t* dosTime)
{
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ftUtc, &local)
        || !FileTimeToSystemTime(&local, &st)) {
        *dosDate = 0;
        *dosTime = 0;
        return;
    }
    const WORD year = st.wYear < 1980 ? 1980 : (st.wYear > 2107 ? 2107 : st.wYear);
    *dosDate = (uint16_t)(((year - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    *dosTime = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
}

void DosToFileTime(uint16_t dosDate, uint16_t dosTime, FILETIME* ftUtc)
{
    FILETIME local{};
    SYSTEMTIME st{};
    st.wYear = (WORD)(1980 + (dosDate >> 9));
    st.wMonth = (WORD)((dosDate >> 5) & 0x0F);
    st.wDay = (WORD)(dosDate & 0x1F);
    st.wHour = (WORD)(dosTime >> 11);
    st.wMinute = (WORD)((dosTime >> 5) & 0x3F);
    st.wSecond = (WORD)((dosTime & 0x1F) * 2);
    if (!SystemTimeToFileTime(&st, &local)
        || !LocalFileTimeToFileTime(&local, ftUtc))
        *ftUtc = FILETIME{ 0, 0 };
}

} // namespace

uint32_t ZipCrc32Update(uint32_t crc, const void* data, size_t len)
{
    const uint32_t* table = CrcTable();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// ZipWriter
// ---------------------------------------------------------------------------

ZipWriter::~ZipWriter()
{
    Abandon();
}

bool ZipWriter::Open(const std::wstring& path)
{
    Abandon();
    file_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = nullptr;
        err_ = L"cannot create archive: " + path;
        return false;
    }
    offset_ = 0;
    return true;
}

bool ZipWriter::WriteAll(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        const DWORD n = len > 0xFFFFFFFFull ? 0xFFFFFFFFu : (DWORD)len;
        DWORD written = 0;
        if (!WriteFile(file_, p, n, &written, nullptr) || written != n) {
            err_ = L"archive write failed";
            return false;
        }
        p += n;
        len -= n;
        offset_ += n;
    }
    return true;
}

bool ZipWriter::BeginEntry(const std::wstring& name, uint16_t dosTime,
                           uint16_t dosDate, uint32_t crc, uint32_t size,
                           bool isDir)
{
    // 条目起点 = 本地头签名落笔前（实测修复：原实现名字写完后以
    // offset_-30 回推，多扣了名字长 → 集中目录记录的本地头偏移错位，
    // 自产自读与第三方解压器均报坏档）
    const uint32_t start = (uint32_t)offset_;
    if (!WriteU32(kSigLocal) || !WriteU16(20) || !WriteU16(kFlagUtf8)
        || !WriteU16(0 /*store*/) || !WriteU16(dosTime) || !WriteU16(dosDate)
        || !WriteU32(crc) || !WriteU32(size) || !WriteU32(size))
        return false;
    const std::string n = WideToUtf8(name);
    if (n.size() > 0xFFFF) {
        err_ = L"entry name too long: " + name;
        return false;
    }
    return WriteU16((uint16_t)n.size()) && WriteU16(0) && WriteAll(n.data(), n.size())
        && ReserveEntry(name, dosTime, dosDate, crc, size, isDir, start);
}

bool ZipWriter::ReserveEntry(const std::wstring& name, uint16_t dosTime,
                             uint16_t dosDate, uint32_t crc, uint32_t size,
                             bool isDir, uint32_t start)
{
    Central c;
    c.name = WideToUtf8(name);
    c.crc = crc;
    c.size = size;
    c.offset = start;
    c.dosTime = dosTime;
    c.dosDate = dosDate;
    c.isDir = isDir;
    entries_.push_back(std::move(c));
    return true;
}

bool ZipWriter::AddFile(const std::wstring& name, const std::wstring& srcPath)
{
    if (!file_) {
        err_ = L"archive not open";
        return false;
    }
    HANDLE src = CreateFileW(srcPath.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (src == INVALID_HANDLE_VALUE) {
        err_ = L"cannot open source file: " + srcPath;
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(src, &sz) || sz.QuadPart < 0
        || (uint64_t)sz.QuadPart > kMaxEntrySize) {
        CloseHandle(src);
        err_ = L"file too large for zip (4 GiB entry limit): " + srcPath;
        return false;
    }
    FILETIME mt{};
    GetFileTime(src, nullptr, nullptr, &mt);
    uint16_t dosDate = 0, dosTime = 0;
    FileTimeToDos(mt, &dosDate, &dosTime);

    // CRC 先行整算（流式）+ 数据直写（两遍读：CRC 一遍 + 写一遍会破坏流式
    // 单遍性，改为：先读全量 CRC（seek 0），再 seek 0 写出——store 条目头需要
    // 预知 CRC/尺寸）
    uint32_t crc = 0;
    std::vector<uint8_t> buf(kChunkLen);
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(src, buf.data(), (DWORD)buf.size(), &n, nullptr)) {
            CloseHandle(src);
            err_ = L"read failed: " + srcPath;
            return false;
        }
        if (n == 0)
            break;
        crc = ZipCrc32Update(crc, buf.data(), n);
    }
    LARGE_INTEGER zero{};
    SetFilePointerEx(src, zero, nullptr, FILE_BEGIN);

    if (!BeginEntry(name, dosTime, dosDate, crc, (uint32_t)sz.QuadPart, false)) {
        CloseHandle(src);
        return false;
    }
    for (uint32_t left = (uint32_t)sz.QuadPart; left > 0;) {
        const DWORD want = left > (uint32_t)buf.size() ? (DWORD)buf.size() : left;
        DWORD n = 0;
        if (!ReadFile(src, buf.data(), want, &n, nullptr) || n != want) {
            CloseHandle(src);
            err_ = L"read failed: " + srcPath;
            return false;
        }
        if (!WriteAll(buf.data(), n)) {
            CloseHandle(src);
            return false;
        }
        left -= n;
    }
    CloseHandle(src);
    return true;
}

bool ZipWriter::AddData(const std::wstring& name, const void* data, size_t len)
{
    if (!file_) {
        err_ = L"archive not open";
        return false;
    }
    if (len > kMaxEntrySize) {
        err_ = L"entry too large (4 GiB limit)";
        return false;
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const uint16_t dosDate =
        (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    const uint16_t dosTime =
        (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    const uint32_t crc = ZipCrc32(data, len);
    if (!BeginEntry(name, dosTime, dosDate, crc, (uint32_t)len, false))
        return false;
    return WriteAll(data, len);
}

bool ZipWriter::AddDir(const std::wstring& name)
{
    if (!file_) {
        err_ = L"archive not open";
        return false;
    }
    std::wstring n = name;
    if (n.empty() || n.back() != L'/')
        n += L'/';
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const uint16_t dosDate =
        (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
    const uint16_t dosTime =
        (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    return BeginEntry(n, dosTime, dosDate, 0, 0, true);
}

bool ZipWriter::Finalize()
{
    if (!file_) {
        err_ = L"archive not open";
        return false;
    }
    const uint64_t cdOffset = offset_;
    for (const Central& c : entries_) {
        const uint16_t extAttr = c.isDir ? 0x10 : 0;
        // 集中目录固定头 46 字节（APPNOTE §4.3.12）：签名4 + 制作版2 +
        // 需求版2 + 旗标2 + 方法2 + 时间2 + 日期2 + CRC4 + 压缩4 + 原始4 +
        // 名长2 + extra2 + 注长2 + 起始盘2 + 内部属性2 + 外部属性4 +
        // 本地头偏移4 + 名。（实测修复：原实现注释长后多写一个 u16 零，
        // 每条目 48 字节 → 外部解压器报 "Invalid central directory
        // signature"）
        if (!WriteU32(kSigCentral) || !WriteU16(20) || !WriteU16(20)
            || !WriteU16(kFlagUtf8) || !WriteU16(0) || !WriteU16(c.dosTime)
            || !WriteU16(c.dosDate) || !WriteU32(c.crc) || !WriteU32(c.size)
            || !WriteU32(c.size) || !WriteU16((uint16_t)c.name.size())
            || !WriteU16(0) || !WriteU16(0) || !WriteU16(0) || !WriteU16(0)
            || !WriteU32(extAttr) || !WriteU32(c.offset)
            || !WriteAll(c.name.data(), c.name.size()))
            return false;
    }
    const uint64_t cdSize = offset_ - cdOffset;
    if (cdSize > 0xFFFFFFFF || cdOffset > 0xFFFFFFFF
        || entries_.size() > 0xFFFF) {
        err_ = L"archive too large for classic zip (no zip64)";
        return false;
    }
    if (!WriteU32(kSigEocd) || !WriteU16(0) || !WriteU16(0)
        || !WriteU16((uint16_t)entries_.size())
        || !WriteU16((uint16_t)entries_.size()) || !WriteU32((uint32_t)cdSize)
        || !WriteU32((uint32_t)cdOffset) || !WriteU16(0))
        return false;
    CloseHandle(file_);
    file_ = nullptr;
    return true;
}

void ZipWriter::Abandon()
{
    if (file_) {
        CloseHandle(file_);
        file_ = nullptr;
    }
    entries_.clear();
    offset_ = 0;
}

// ---------------------------------------------------------------------------
// ZipReader
// ---------------------------------------------------------------------------

ZipReader::~ZipReader()
{
    if (file_)
        CloseHandle(file_);
}

namespace {

uint16_t RdU16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t RdU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
           | ((uint32_t)p[3] << 24);
}

} // namespace

bool ZipReader::ReadAllAt(uint64_t offset, void* buf, size_t len)
{
    LARGE_INTEGER pos{};
    pos.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file_, pos, nullptr, FILE_BEGIN)) {
        err_ = L"seek failed in archive";
        return false;
    }
    uint8_t* p = (uint8_t*)buf;
    while (len > 0) {
        const DWORD n = len > 0xFFFFFFFFull ? 0xFFFFFFFFu : (DWORD)len;
        DWORD got = 0;
        if (!ReadFile(file_, p, n, &got, nullptr) || got != n) {
            err_ = L"read failed in archive";
            return false;
        }
        p += n;
        len -= n;
    }
    return true;
}

bool ZipReader::Open(const std::wstring& path)
{
    if (file_) {
        CloseHandle(file_);
        file_ = nullptr;
    }
    entries_.clear();
    file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        file_ = nullptr;
        err_ = L"cannot open archive: " + path;
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(file_, &sz) || sz.QuadPart < 22) {
        err_ = L"not a zip archive (too small): " + path;
        return false;
    }
    // EOCD 回扫
    const size_t window = (size_t)sz.QuadPart < kEocdWindow
        ? (size_t)sz.QuadPart : kEocdWindow;
    std::vector<uint8_t> tail(window);
    if (!ReadAllAt((uint64_t)sz.QuadPart - window, tail.data(), tail.size()))
        return false;
    size_t eocd = SIZE_MAX;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (RdU32(&tail[i]) == kSigEocd) {
            eocd = i;
            break;
        }
    }
    if (eocd == SIZE_MAX) {
        err_ = L"not a zip archive (no end record): " + path;
        return false;
    }
    const uint8_t* e = &tail[eocd];
    const uint16_t count = RdU16(e + 10);
    const uint32_t cdSize = RdU32(e + 12);
    const uint32_t cdOffset = RdU32(e + 16);
    if (cdSize > kMaxCentralDir) {
        err_ = L"zip central directory too large";
        return false;
    }
    if ((uint64_t)cdOffset + cdSize > (uint64_t)sz.QuadPart - 22) {
        err_ = L"zip central directory out of range";
        return false;
    }
    std::vector<uint8_t> cd(cdSize ? cdSize : 1);
    if (cdSize && !ReadAllAt(cdOffset, cd.data(), cdSize))
        return false;
    // 逐条解析集中目录
    size_t pos = 0;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 46 > cdSize) {
            err_ = L"zip central directory truncated";
            return false;
        }
        const uint8_t* c = &cd[pos];
        if (RdU32(c) != kSigCentral) {
            err_ = L"zip central directory corrupt";
            return false;
        }
        const uint16_t method = RdU16(c + 10);
        if (method != 0) {
            err_ = L"archive uses compression; only store-mode archives are"
                   L" supported";
            return false;
        }
        Entry en;
        en.crc = RdU32(c + 16);
        en.size = RdU32(c + 24);
        en.offset = RdU32(c + 42);
        en.dosTime = RdU16(c + 12);
        en.dosDate = RdU16(c + 14);
        const uint16_t nlen = RdU16(c + 28);
        const uint16_t elen = RdU16(c + 30);
        const uint16_t clen = RdU16(c + 32);
        if (pos + 46 + nlen > cdSize) {
            err_ = L"zip central directory truncated (name)";
            return false;
        }
        en.name = Utf8ToWide(std::string((const char*)c + 46, nlen));
        en.isDir = !en.name.empty() && en.name.back() == L'/';
        // 外部属性 dir 位（DOS 0x10 / Unix 0x10 的高字节 <<16）兼容
        if (!en.isDir && (RdU32(c + 38) & 0x10))
            en.isDir = true;
        entries_.push_back(std::move(en));
        pos += 46 + (size_t)nlen + elen + clen;
    }
    return true;
}

const ZipReader::Entry* ZipReader::Find(const std::wstring& name) const
{
    std::wstring want = name;
    for (wchar_t& c : want)
        if (c == L'\\')
            c = L'/';
    for (const Entry& e : entries_) {
        if (_wcsicmp(e.name.c_str(), want.c_str()) == 0)
            return &e;
    }
    return nullptr;
}

bool ZipReader::ReadEntry(const Entry& e, std::vector<uint8_t>* out)
{
    out->clear();
    if (e.isDir || e.size == 0)
        return true;   // 目录 / 空文件 = 空载荷
    uint8_t lh[30];
    if (!ReadAllAt(e.offset, lh, sizeof(lh)))
        return false;
    if (RdU32(lh) != kSigLocal) {
        err_ = L"zip local header corrupt";
        return false;
    }
    const uint16_t nlen = RdU16(lh + 26);
    const uint16_t elen = RdU16(lh + 28);
    out->resize(e.size);
    if (!ReadAllAt(e.offset + 30 + nlen + elen, out->data(), out->size()))
        return false;
    if (ZipCrc32(out->data(), out->size()) != e.crc) {
        err_ = L"zip entry CRC mismatch: " + e.name;
        return false;
    }
    return true;
}

bool ZipReader::ExtractTo(const Entry& e, const std::wstring& destPath)
{
    if (e.isDir) {
        if (!CreateDirectoryW(destPath.c_str(), nullptr)
            && GetLastError() != ERROR_ALREADY_EXISTS) {
            err_ = L"cannot create directory: " + destPath;
            return false;
        }
        return true;
    }
    uint8_t lh[30];
    if (!ReadAllAt(e.offset, lh, sizeof(lh)))
        return false;
    if (RdU32(lh) != kSigLocal) {
        err_ = L"zip local header corrupt";
        return false;
    }
    const uint16_t nlen = RdU16(lh + 26);
    const uint16_t elen = RdU16(lh + 28);
    const uint64_t dataOffset = e.offset + 30 + nlen + elen;

    HANDLE dst = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dst == INVALID_HANDLE_VALUE) {
        err_ = L"cannot create file: " + destPath;
        return false;
    }
    std::vector<uint8_t> buf(kChunkLen);
    uint32_t crc = 0;
    uint32_t left = e.size;
    bool ok = true;
    while (left > 0 && ok) {
        const uint32_t want = left > (uint32_t)buf.size() ? (uint32_t)buf.size() : left;
        if (!ReadAllAt(dataOffset + (e.size - left), buf.data(), want)) {
            ok = false;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(dst, buf.data(), want, &written, nullptr) || written != want) {
            err_ = L"write failed: " + destPath;
            ok = false;
            break;
        }
        crc = ZipCrc32Update(crc, buf.data(), want);
        left -= want;
    }
    if (ok && crc != e.crc) {
        err_ = L"zip entry CRC mismatch: " + e.name;
        ok = false;
    }
    if (ok) {
        FILETIME mt{};
        DosToFileTime(e.dosDate, e.dosTime, &mt);
        SetFileTime(dst, nullptr, nullptr, &mt);   // mtime 还原（2s 精度）
    }
    CloseHandle(dst);
    if (!ok)
        DeleteFileW(destPath.c_str());
    return ok;
}

} // namespace sbie::util
