// Sandboxie-OSS — SbieCore/Util/Zip.h
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// 最小 zip 归档读写器（07-P1-2 沙箱导出/导入；决策记录 docs/04 §17）。
//
// 自研依据（零第三方依赖约束，docs/07 §3.2 07-P1-2 建议）：仅实现
// **store（无压缩）** 条目 + UTF-8 文件名（通用位标志 bit 11），数据流式
// 读写（内存占用恒定 64 KiB 块 + 集中目录记录）。格式面：
//   写：本地文件头（0x04034b50）顺序流出 → 集中目录（0x02014b50）→
//       EOCD（0x06054b50）；
//   读：自文件尾向后扫 EOCD → 集中目录全量载入（≤ 8 MiB 防病态包）→
//       各条目按本地头偏移流式解出（校验 CRC32）。
// 限制（文档化决策）：
//   * 仅 store 条目——读侧遇到 method != 0 报错（本工具产出物与
//     Windows 资源管理器"发送到压缩文件夹"的兼容读取不受影响——后者可读
//     store；本工具不读第三方压缩包）；
//   * 32 位尺寸（单条目 < 4 GiB，无 zip64）；
//   * 目录条目 = 名以 '/' 结尾 + 外部属性 dir 位（0x10）；
//   * mtime 以 DOS 日期时间字段保存/还原（2 秒精度，zip 格式固有）；
//     文件属性（只读等）不保存。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

namespace sbie::util {

class ZipWriter {
public:
    ZipWriter() = default;
    ~ZipWriter();
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    // 创建/截断归档文件（CREATE_ALWAYS）。失败返回 false（LastError 置位）。
    bool Open(const std::wstring& path);
    // 添加文件条目：name 用 '/' 分隔的归档内相对路径；数据从 srcPath
    // 流式读入（含 CRC32 计算与 mtime 采集）。目录名带尾 '/' 由 AddDir 表达。
    bool AddFile(const std::wstring& name, const std::wstring& srcPath);
    // 添加内存数据条目（box.ini 等小文本）。
    bool AddData(const std::wstring& name, const void* data, size_t len);
    // 目录条目：name 以 '/' 结尾。
    bool AddDir(const std::wstring& name);
    // 收尾：集中目录 + EOCD。成功后句柄关闭。
    bool Finalize();
    // 放弃（不写目录尾——归档不完整，仅供错误路径清理）。
    void Abandon();
    const std::wstring& LastError() const { return err_; }

private:
    struct Central {
        std::string name;      // UTF-8
        uint32_t crc = 0;
        uint32_t size = 0;     // 未压缩 = 压缩尺寸（store）
        uint32_t offset = 0;   // 本地头偏移
        uint16_t dosTime = 0;
        uint16_t dosDate = 0;
        bool isDir = false;
    };

    bool WriteAll(const void* data, size_t len);
    bool WriteU16(uint16_t v) { return WriteAll(&v, 2); }
    bool WriteU32(uint32_t v) { return WriteAll(&v, 4); }
    bool BeginEntry(const std::wstring& name, uint16_t dosTime,
                    uint16_t dosDate, uint32_t crc, uint32_t size, bool isDir);
    bool ReserveEntry(const std::wstring& name, uint16_t dosTime,
                      uint16_t dosDate, uint32_t crc, uint32_t size,
                      bool isDir, uint32_t start);

    HANDLE file_ = nullptr;
    uint64_t offset_ = 0;
    std::vector<Central> entries_;
    std::wstring err_;
};

class ZipReader {
public:
    struct Entry {
        std::wstring name;     // UTF-8 → 宽字符；目录条目带尾 '/'
        uint32_t crc = 0;
        uint32_t size = 0;
        uint32_t offset = 0;   // 本地头偏移
        uint16_t dosTime = 0;  // mtime（DOS 字段；ExtractTo 还原）
        uint16_t dosDate = 0;
        bool isDir = false;
    };

    ~ZipReader();
    ZipReader() = default;
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool Open(const std::wstring& path);           // 解析 EOCD + 集中目录
    size_t Count() const { return entries_.size(); }
    const Entry* At(size_t i) const
    {
        return i < entries_.size() ? &entries_[i] : nullptr;
    }
    // 按归档名查找（大小写不敏感、'/' 归一）
    const Entry* Find(const std::wstring& name) const;
    // 条目内容整体读入内存（box.ini 等小条目；大条目用 ExtractTo 流式）
    bool ReadEntry(const Entry& e, std::vector<uint8_t>* out);
    // 条目流式解出到磁盘（CREATE_ALWAYS；恢复 mtime；CRC 校验）。
    // 目录条目 = CreateDirectoryW。
    bool ExtractTo(const Entry& e, const std::wstring& destPath);
    const std::wstring& LastError() const { return err_; }

private:
    bool ReadAllAt(uint64_t offset, void* buf, size_t len);

    HANDLE file_ = nullptr;
    std::vector<Entry> entries_;
    std::wstring err_;
};

// CRC32（IEEE 802.3，多项式 0xEDB88320）。ZipCrc32Update 可链式增量：
// crc = ZipCrc32Update(crc, chunk1); crc = ZipCrc32Update(crc, chunk2);
// 首块传 0。
uint32_t ZipCrc32Update(uint32_t crc, const void* data, size_t len);
inline uint32_t ZipCrc32(const void* data, size_t len)
{
    return ZipCrc32Update(0, data, len);
}

} // namespace sbie::util
