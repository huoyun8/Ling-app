#pragma once
// GIF 录制的编码端。原项目用的是 gifski（Rust 静态库），本工程没有那个 .lib，
// 所以这里只保留基于 cgif 的那条路（原文件里的 createGif2），函数名统一成 createGif。
// 成一个文件、一次录完才收尾，中途只往 cgif 里喂帧。
//
// 抓帧与编码分在两条线程上：抓帧线程只做 BitBlt → BGRA → RGB → 入队，
// 量化 / 差分窗口 / 写盘这些重活全在编码线程里做（cgif 的 addframe 是同线程同步的，
// 以前它直接压在抓帧循环上，编码一慢这一帧的 Sleep 就被吃掉，帧率跟着抖）。
// 队列有界（按帧字节数算，最多 8 帧、约 64MB），编码持续跟不上时抓帧线程等空位，
// 宁可帧率掉一点也不让内存无限涨。

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "pch.h"
#include "cgif/cgif.h"

namespace VideoGif {

    struct GifParam
    {
        std::atomic<bool> isFinish{ false };
        // 真正喂进 cgif 的帧数。和 mp4 那条路一样，这里也要能区分"录了东西"和
        // "刚开录就停了"：0 帧时 cgif_rgb_close 只写出一个文件头，存出去是个打不开的 gif
        std::atomic<long long> framesWritten{ 0 };
        std::wstring path;
        int w{ 0 };
        int h{ 0 };
        int x{ 0 };
        int y{ 0 };
        UINT fps{ 16 };
    };

    // 把鼠标画进这一帧。光标是独立于桌面内容的合成层，GDI 的 BitBlt 抓不到它，
    // 不补这一下视频里就看不到鼠标
    inline void drawCursor(HDC hMemDC, GifParam* param) {
        CURSORINFO cursorInfo = { sizeof(CURSORINFO) };
        if (!GetCursorInfo(&cursorInfo)) return;
        if (cursorInfo.flags == CURSOR_SHOWING) {
            ICONINFO iconInfo{};
            // GetIconInfo 失败时不会填 iconInfo，直接 DeleteObject 里面那两个句柄
            // 就是把垃圾值交给 GDI；失败这一帧就只画桌面
            if (!GetIconInfo(cursorInfo.hCursor, &iconInfo)) return;
            int localX = cursorInfo.ptScreenPos.x - param->x - iconInfo.xHotspot;
            int localY = cursorInfo.ptScreenPos.y - param->y - iconInfo.yHotspot;
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            if (localX >= 0 && localX < param->w && localY >= 0 && localY < param->h) {
                DrawIconEx(hMemDC, localX, localY, cursorInfo.hCursor, 0, 0, 0, nullptr, DI_NORMAL | DI_DEFAULTSIZE);
            }
            // 注意：hCursor 是系统共享的光标句柄，GetCursorInfo 只是"借用"，
            // 不能 DestroyIcon —— 销毁它等于把系统光标（以及别处正用着它的程序）一起废掉
        }
    }

    namespace detail {
        // 队列里的一帧：紧密排列的 RGB。缓冲归编码线程回收，抓帧线程不释放
        struct GifFrame
        {
            std::vector<unsigned char> rgb;
        };

        // 文件 + cgif + 一条编码线程。生产者（抓帧线程）acquire 拿一块缓冲、
        // 填完 submit；cgif 只在编码线程里被碰，所以不需要额外的锁保护编码器本身
        class GifWriter
        {
        public:
            ~GifWriter() { finish(); }

            bool open(GifParam* param)
            {
                // cgif 的 path 是窄字符串，内部走 fopen —— 按 ANSI 代码页解释路径，
                // 用户名带中文时 %appdata% 下那条路径根本打不开，GIF 一帧都写不出来。
                // 所以文件由我们自己用宽字符 API 打开，只把写回调交给 cgif
                this->param = param;
                if (_wfopen_s(&file, param->path.data(), L"wb") != 0 || !file) return false;
                CGIFrgb_Config config = { 0 };
                config.pWriteFn = [](void* ctx, const uint8_t* data, const size_t size) -> int {
                    return fwrite(data, 1, size, static_cast<FILE*>(ctx)) == size ? 0 : -1; //0 成功，-1 失败
                    };
                config.pContext = file;
                config.width = param->w;
                config.height = param->h;
                config.attrFlags = 0;
                // 启用帧优化：只编码变化区域 + 透明优化
                config.genFlags = CGIF_FRAME_GEN_USE_DIFF_WINDOW | CGIF_FRAME_GEN_USE_TRANSPARENCY;
                gif = cgif_rgb_newgif(&config);
                if (!gif) {
                    fclose(file);
                    file = nullptr;
                    return false;
                }
                fconfig.fmtChan = CGIF_CHAN_FMT_RGB;
                fconfig.delay = static_cast<uint16_t>((std::max)(2, static_cast<int>(100.0 / param->fps + 0.5)));
                fconfig.attrFlags = CGIF_RGB_FRAME_ATTR_NO_DITHERING; // 禁用抖动以减小文件大小
                delayMs = fconfig.delay * 10;
                // 队列按字节封顶：1080p 满屏一帧 RGB 约 6MB，8 帧就是 48MB；
                // 选区小的时候 8 帧才几 MB，缓冲多一点反而能吸收编码的抖动
                const size_t frameBytes = static_cast<size_t>(param->w) * 3 * param->h;
                maxQueued = (std::max)(static_cast<size_t>(2), (std::min)(static_cast<size_t>(8),
                    static_cast<size_t>(64) << 20) / frameBytes);
                try {
                    worker = std::thread([this] { encodeLoop(); });
                }
                catch (...) {
                    // 线程起不来就别留一个只写文件头的空壳
                    cgif_rgb_close(gif);
                    gif = nullptr;
                    fclose(file);
                    file = nullptr;
                    return false;
                }
                return true;
            }

            // 每帧在 gif 里占的时间，抓帧循环按它配速
            int frameDelayMs() const { return delayMs; }

            // 生产者：拿一块可以写的缓冲；队列满 / 编码出错 / 正在收尾时回空指针
            GifFrame* acquire()
            {
                std::unique_lock<std::mutex> lk(m);
                cvSpace.wait(lk, [this] {
                    return failed || closing || queue.size() < maxQueued;
                    });
                if (failed || closing) return nullptr;
                if (!spare.empty()) {
                    GifFrame* f = spare.back();
                    spare.pop_back();
                    return f;
                }
                // 一帧 RGB 的定长缓冲，只在这一处建，之后全靠 spare 复用
                owner.emplace_back(std::make_unique<GifFrame>());
                GifFrame* f = owner.back().get();
                f->rgb.resize(static_cast<size_t>(param->w) * 3 * param->h);
                return f;
            }

            void submit(GifFrame* f)
            {
                std::lock_guard<std::mutex> lk(m);
                if (closing || failed) {
                    spare.push_back(f); // 收尾阶段来的帧：只回收，不编码
                    return;
                }
                queue.push_back(f);
                cvFrame.notify_one();
            }

            // 收尾：让编码线程把队列里剩的帧排空、关 gif、关文件
            void finish()
            {
                if (worker.joinable()) {
                    {
                        std::lock_guard<std::mutex> lk(m);
                        closing = true;
                        cvFrame.notify_all();
                        cvSpace.notify_all();
                    }
                    worker.join();
                }
                if (gif) {
                    cgif_rgb_close(gif);
                    gif = nullptr;
                }
                if (file) {
                    fclose(file); //走的是写回调，文件句柄归我们自己管，cgif 不会去关
                    file = nullptr;
                }
            }

        private:
            void encodeLoop()
            {
                for (;;) {
                    GifFrame* f = nullptr;
                    {
                        std::unique_lock<std::mutex> lk(m);
                        cvFrame.wait(lk, [this] { return closing || !queue.empty(); });
                        if (queue.empty()) break; // closing 且已排空
                        f = queue.front();
                        queue.pop_front();
                        cvSpace.notify_one();
                    }
                    if (!failed) {
                        fconfig.pImageData = f->rgb.data();
                        // PENDING 也是"这一帧已交给库、只等下一次调用推进写出"，不能当失败。
                        // 只有 ERROR / EWRITE / EALLOC 这类才是真的没写进去
                        const cgif_result addResult = cgif_rgb_addframe(gif, &fconfig);
                        if (addResult == CGIF_OK || addResult == CGIF_PENDING) {
                            param->framesWritten++;
                        }
                        else {
                            // 编码器已经坏了：抓帧那边不用再等，线程继续把队列排空（只回收缓冲）
                            std::lock_guard<std::mutex> lk(m);
                            failed = true;
                            cvSpace.notify_all();
                        }
                    }
                    {
                        std::lock_guard<std::mutex> lk(m);
                        spare.push_back(f);
                        cvSpace.notify_one();
                    }
                }
                cgif_rgb_close(gif);
                gif = nullptr;
            }

            GifParam* param{ nullptr };
            CGIFrgb* gif{ nullptr };
            FILE* file{ nullptr };
            CGIFrgb_FrameConfig fconfig{};
            int delayMs{ 60 };
            size_t maxQueued{ 8 };
            std::mutex m;
            std::condition_variable cvFrame;  // 编码线程等新帧
            std::condition_variable cvSpace;  // 抓帧线程等空位 / 空缓冲
            std::deque<GifFrame*> queue;
            std::vector<GifFrame*> spare;     // 编完待复用的缓冲
            std::vector<std::unique_ptr<GifFrame>> owner;
            bool closing{ false };
            bool failed{ false };
            std::thread worker;
        };
    }

    inline void createGif(GifParam* param) {
        // 宽高至少 1：0 宽会让缓冲区和位图都成空，cgif 拿到空指针直接崩。
        // 先判掉，也别留下一个只有文件头的 gif
        if (param->w <= 0 || param->h <= 0) return;
        detail::GifWriter writer;
        if (!writer.open(param)) return;

        uint32_t srcRowBytes = param->w * 4;
        uint32_t dstRowBytes = param->w * 3;
        // BGRA 只给抓帧线程用，不进队列（队列里只放转好的 RGB）
        std::vector<unsigned char> bgra_buffer(srcRowBytes * param->h);
        HDC hScreenDC = GetDC(nullptr);
        HDC hMemDC = hScreenDC ? CreateCompatibleDC(hScreenDC) : nullptr;
        HBITMAP hBitmap = hMemDC ? CreateCompatibleBitmap(hScreenDC, param->w, param->h) : nullptr;
        // 三件套少一件，后面 BitBlt/GetDIBits 全是静默失败，录出来是一整片黑。
        // 与其写一个纯黑 gif，不如当场收摊
        if (!hScreenDC || !hMemDC || !hBitmap) {
            if (hBitmap) DeleteObject(hBitmap);
            if (hMemDC) DeleteDC(hMemDC);
            if (hScreenDC) ReleaseDC(nullptr, hScreenDC);
            writer.finish();
            return;
        }
        HGDIOBJ hOldBitmap = SelectObject(hMemDC, hBitmap);
        BITMAPINFO bmi = { sizeof(BITMAPINFOHEADER), param->w, 0 - param->h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
        const int actualDelayMs = writer.frameDelayMs();
        while (!param->isFinish) {
            auto tickStart = GetTickCount64();
            BitBlt(hMemDC, 0, 0, param->w, param->h, hScreenDC, param->x, param->y, SRCCOPY);
            drawCursor(hMemDC, param);
            GetDIBits(hMemDC, hBitmap, 0, param->h, (void*)bgra_buffer.data(), &bmi, DIB_RGB_COLORS);
            // 先抓帧再等缓冲：编码那边出事 / 已经在收尾时，抓帧循环也顺势结束
            detail::GifFrame* frame = writer.acquire();
            if (!frame) break;
            // BGRA → RGB，逐行转换到紧密排列的缓冲区
            for (int row = 0; row < param->h; row++) {
                const unsigned char* srcRow = bgra_buffer.data() + row * srcRowBytes;
                unsigned char* dstRow = frame->rgb.data() + row * dstRowBytes;
                for (int col = 0; col < param->w; col++) {
                    dstRow[col * 3 + 0] = srcRow[col * 4 + 2]; // R
                    dstRow[col * 3 + 1] = srcRow[col * 4 + 1]; // G
                    dstRow[col * 3 + 2] = srcRow[col * 4 + 0]; // B
                }
            }
            writer.submit(frame);
            auto elapsed = GetTickCount64() - tickStart;
            int sleepTime = actualDelayMs - static_cast<int>(elapsed);
            if (sleepTime > 0) {
                Sleep(sleepTime);
            }
        }
        writer.finish();
        SelectObject(hMemDC, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hMemDC);
        ReleaseDC(nullptr, hScreenDC);
    }
}
