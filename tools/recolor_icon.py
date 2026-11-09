# -*- coding: utf-8 -*-
"""给图标换色调（通用版，源色相自动识别）。

用法：
    python recolor_icon.py <src.ico> <dst.ico> <hue_lo> <hue_hi> [s_mul] [v_mul] [preview.png]

源图的彩色色相范围**自动检测**（取饱和度足够像素的 2%~98% 分位），
所以同一个脚本既能处理橙底的 snap 标（10°~30°），也能处理紫底的 OCR 标（250°~279°）。
变换只改色相，饱和度 S 与明度 V 原样保留 —— 原来的渐变层次、阴影、立体感一点不丢。

为什么逐尺寸处理：PIL 保存 ICO 时会从一张大图 resize 出所有尺寸，
而原图每个尺寸是设计师单独调过的（小尺寸线条更粗）。这里逐尺寸读原 blob、
只改颜色再按原尺寸写回，细节一点不丢。

ICO 编码：<=128 用 32bpp BMP(DIB) + AND 掩码（Windows 最兼容），
256 用内嵌 PNG（Vista 起支持，体积小得多）。
"""
from PIL import Image
import colorsys
import io
import os
import struct
import sys

SAT_MIN = 0.12      # 饱和度低于此值视为白/灰（白字、白框、文档），不参与换色
AUTO_PAD = 2.0      # 分位裁剪后再外扩几度，避免把边缘色丢掉


def src_hue_range(im):
    """检测源图彩色像素的色相范围（2%~98% 分位，剔除抗锯齿杂色）。"""
    rgba = im.convert('RGBA')
    px = rgba.load()
    w, h = rgba.size
    hues = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                continue
            hh, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            if s >= SAT_MIN:
                hues.append(hh * 360.0)
    if not hues:
        return 0.0, 360.0
    hues.sort()
    lo = hues[int(len(hues) * 0.02)]
    hi = hues[min(len(hues) - 1, int(len(hues) * 0.98))]
    lo = max(0.0, lo - AUTO_PAD)
    hi = min(359.9, hi + AUTO_PAD)
    if hi - lo < 5.0:                # 极端情况：几乎单色
        lo, hi = max(0.0, lo - 5.0), min(359.9, hi + 5.0)
    return lo, hi


def recolor(im, s_lo, s_hi, d_lo, d_hi, s_mul=1.0, v_mul=1.0):
    """把源区间的色相线性搬到目标区间（可跨 0°）。

    s_mul / v_mul 是饱和度、明度的乘数（默认 1.0 = 原样）。
    存在这两个参数是因为：换色相只决定「什么颜色」，决定不了「颜色有多冲」。
    大块纯色 S=1.0 在任务栏里会显得廉价，把 S 收到 0.95 左右会沉稳很多。
    """
    im = im.convert('RGBA')
    px = im.load()
    w, h = im.size
    s_span = (s_hi - s_lo) or 1.0
    d_span = (d_hi - d_lo) % 360.0 or 360.0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a < 128:
                continue
            hh, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            if s < SAT_MIN:
                continue
            deg = hh * 360.0
            t = (deg - s_lo) / s_span
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            nd = (d_lo + t * d_span) % 360.0
            ns = min(1.0, s * s_mul)
            nv = min(1.0, v * v_mul)
            nr, ng, nb = colorsys.hsv_to_rgb(nd / 360.0, ns, nv)
            px[x, y] = (int(round(nr * 255)), int(round(ng * 255)), int(round(nb * 255)), a)
    return im


def dib_bytes(im):
    """32bpp BMP(DIB) 块：BITMAPINFOHEADER + 自底向上 BGRA + AND 掩码。"""
    w, h = im.size
    px = im.load()
    rows = []
    for y in range(h - 1, -1, -1):          # bottom-up
        row = bytearray()
        for x in range(w):
            r, g, b, a = px[x, y]
            row += bytes((b, g, r, a))
        rows.append(bytes(row))
    pixels = b''.join(rows)
    # AND 掩码：1bit/像素，行按 4 字节对齐；有 alpha 的图标这里全 0 即可
    mask_row = ((w + 31) // 32) * 4
    mask = b'\x00' * (mask_row * h)
    hdr = struct.pack('<IiiHHIIiiII', 40, w, h * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
    return hdr + pixels + mask


def png_bytes(im):
    buf = io.BytesIO()
    im.save(buf, format='PNG', optimize=True)
    return buf.getvalue()


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__)
    src_path, dst_path = sys.argv[1], sys.argv[2]
    d_lo, d_hi = float(sys.argv[3]), float(sys.argv[4])
    s_mul = float(sys.argv[5]) if len(sys.argv) > 5 else 1.0
    v_mul = float(sys.argv[6]) if len(sys.argv) > 6 else 1.0
    preview = sys.argv[7] if len(sys.argv) > 7 else None

    src = Image.open(src_path)
    sizes = sorted(src.info.get('sizes', []))
    if not sizes:
        raise SystemExit('源 ico 没有可用尺寸')

    # 源色相只在最大尺寸上测一次，全尺寸共用同一映射 —— 各尺寸各测的话，
    # 小图标像素少、分位点漂移，同一张图不同尺寸会配出不一致的颜色
    big = max(sizes)
    src.size = (big[0], big[1])
    src.load()
    s_lo, s_hi = src_hue_range(src)
    print(f'源色相 {s_lo:.1f}°~{s_hi:.1f}°  ->  目标 {d_lo:.0f}°~{d_hi:.0f}°  Sx{s_mul} Vx{v_mul}')

    blobs = []
    previews = {}
    for (sw, sh) in sizes:
        src.size = (sw, sh)
        src.load()
        im = recolor(src, s_lo, s_hi, d_lo, d_hi, s_mul, v_mul)
        side = max(sw, sh)
        previews[side] = im
        blobs.append((sw, sh, png_bytes(im) if side >= 256 else dib_bytes(im)))
        print(f'  {sw:>3}x{sh:<3} -> {"PNG" if side >= 256 else "BMP"} {len(blobs[-1][2])} B')

    n = len(blobs)
    out = bytearray(struct.pack('<HHH', 0, 1, n))
    offset = 6 + 16 * n
    body = bytearray()
    for (w, h, data) in blobs:
        out += struct.pack('<BBBBHHII', w % 256, h % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
        body += data
    out += body
    with open(dst_path, 'wb') as f:
        f.write(bytes(out))
    print('wrote', dst_path, os.path.getsize(dst_path), 'bytes, sizes', [b[0] for b in blobs])

    if preview and 256 in previews:
        previews[256].save(preview)
        print('preview', preview)


if __name__ == '__main__':
    main()
