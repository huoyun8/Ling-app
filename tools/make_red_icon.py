# -*- coding: utf-8 -*-
"""把 C 版橙色图标转成红色调，逐尺寸处理并重新封装为标准 .ico。

为什么不用 PIL 直接 save：PIL 保存 ICO 时会从一张大图 resize 出所有尺寸，
而原图每个尺寸是设计师单独调过的（小尺寸线条更粗）。这里逐尺寸读原 blob、
只改颜色再按原尺寸写回，细节一点不丢。

ICO 编码：<=128 用 32bpp BMP(DIB) + AND 掩码（Windows 最兼容），
256 用内嵌 PNG（Vista 起支持，体积也小得多）。
"""
from PIL import Image
import colorsys
import io
import os
import struct
import sys

SRC = r'C:\Users\2026\WorkBuddy\2026-11-09-09-55-39\SnapCore-C.ico'
DST = sys.argv[1] if len(sys.argv) > 1 else r'C:\Users\2026\WorkBuddy\2026-11-09-09-55-39\SnapCore-R-red.ico'
PREVIEW = sys.argv[2] if len(sys.argv) > 2 else None

# 橙色实际落在 10°~30°。映射到 354°~6°（跨 0° 的纯红区间），
# 跨度从 20° 压到 12°：所有像素都稳稳在红色附近，不会滑到品红那头。
HUE_LO, HUE_HI = 10.0, 30.0
RED_LO, RED_SPAN = 354.0, 12.0


def redden(im):
    """把橙色像素的色相压到红色区间；其它（白字、白框、灰阴影）原样保留。"""
    im = im.convert('RGBA')
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            hh, s, v = colorsys.rgb_to_hsv(r / 255.0, g / 255.0, b / 255.0)
            deg = hh * 360.0
            # 只动橙色系：饱和度够、色相在橙区。白色文档 / 白字 S 很低，跳过
            if s < 0.12 or not (HUE_LO - 6.0 <= deg <= HUE_HI + 6.0):
                continue
            t = (deg - HUE_LO) / (HUE_HI - HUE_LO)
            t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
            nd = (RED_LO + t * RED_SPAN) % 360.0
            nr, ng, nb = colorsys.hsv_to_rgb(nd / 360.0, s, v)
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
    src = Image.open(SRC)
    sizes = sorted(src.info.get('sizes', []))
    if not sizes:
        raise SystemExit('源 ico 没有可用尺寸')
    blobs = []       # (w, h, data)
    previews = {}
    for (sw, sh) in sizes:
        src.size = (sw, sh)
        src.load()
        im = redden(src)
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
    with open(DST, 'wb') as f:
        f.write(bytes(out))
    print('wrote', DST, os.path.getsize(DST), 'bytes, sizes', [b[0] for b in blobs])

    if PREVIEW:
        for side in (256, 48):
            if side in previews:
                p = PREVIEW.replace('.png', f'-{side}.png')
                im = previews[side]
                if side < 256:
                    im = im.resize((256, 256), Image.NEAREST)
                im.save(p)
                print('preview', p)


if __name__ == '__main__':
    main()
