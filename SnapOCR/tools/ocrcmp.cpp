// 构建期资源打包器：把 OCR 资源压成 SOCP 容器，供 Resource.rc 内嵌。
// 用 Windows 自带 compressapi（LZMS / XPRESS-Huffman），运行期用系统 API 解压，
// 所以 exe 里既不需要 zlib 也不需要我们自己写 inflate。
//
//   ocrcmp.exe <in> <out>
//
// 容器格式（9 字节头 + 载荷），自描述，OcrEngine 靠 magic 判断要不要解压：
//   [0..3] "SOCP"
//   [4]    算法 0=原样 1=XPRESS-HUFF 2=LZMS
//   [5..8] 原始长度 u32 小端
//   [9..]  载荷
#include <windows.h>
#include <compressapi.h>
#include <cstdio>
#include <vector>

namespace
{
    struct Algo { DWORD id; BYTE tag; const wchar_t* name; };

    // 按压缩率从好到差试。LZMS 对模型权重的收益明显好过 XPRESS
    const Algo kAlgos[] = {
        { COMPRESS_ALGORITHM_LZMS,        2, L"lzms" },
        { COMPRESS_ALGORITHM_XPRESS_HUFF, 1, L"xpress-huff" },
    };

    bool packWith(const Algo& a, const std::vector<BYTE>& src, std::vector<BYTE>& dst)
    {
        COMPRESSOR_HANDLE comp = nullptr;
        if (!CreateCompressor(a.id, nullptr, &comp)) return false;
        bool ok = false;
        SIZE_T bound = 0;
        if (Compress(comp, src.data(), src.size(), nullptr, 0, &bound) ||
            GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            dst.resize(bound);
            SIZE_T got = 0;
            if (Compress(comp, src.data(), src.size(), dst.data(), dst.size(), &got) && got > 0) {
                dst.resize(got);
                ok = true;
            }
        }
        CloseCompressor(comp);
        return ok;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { fwprintf(stderr, L"usage: ocrcmp <in> <out>\n"); return 2; }

    FILE* f = _wfopen(argv[1], L"rb");
    if (!f) { fwprintf(stderr, L"open failed: %ls\n", argv[1]); return 2; }
    _fseeki64(f, 0, SEEK_END);
    const long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); fwprintf(stderr, L"empty: %ls\n", argv[1]); return 2; }
    std::vector<BYTE> src((size_t)sz);
    const bool readOk = fread(src.data(), 1, src.size(), f) == src.size();
    fclose(f);
    if (!readOk) { fwprintf(stderr, L"read failed: %ls\n", argv[1]); return 2; }

    std::vector<BYTE> best = src;
    BYTE tag = 0;
    const wchar_t* bestName = L"raw";
    for (const auto& a : kAlgos) {
        std::vector<BYTE> out;
        if (!packWith(a, src, out)) continue;
        if (out.size() < best.size()) { best.swap(out); tag = a.tag; bestName = a.name; }
    }
    // 压不动就原样存，保证不会比源文件更大
    if (best.size() + 9 >= src.size()) { best = src; tag = 0; bestName = L"raw"; }

    const DWORD orig = (DWORD)src.size();
    FILE* g = _wfopen(argv[2], L"wb");
    if (!g) { fwprintf(stderr, L"write failed: %ls\n", argv[2]); return 2; }
    fwrite("SOCP", 1, 4, g);
    fwrite(&tag, 1, 1, g);
    fwrite(&orig, 1, 4, g);
    fwrite(best.data(), 1, best.size(), g);
    fclose(g);

    wprintf(L"  %-28ls %8llu -> %8llu  %3d%%  (%ls)\n", argv[1],
        (unsigned long long)src.size(), (unsigned long long)(best.size() + 9),
        (int)((best.size() + 9) * 100 / src.size()), bestName);
    return 0;
}
