#include "pch.h"
#include "QrCodec.h"

extern "C" {
#include "third_party/qrencode/qrencode.h"
#include "third_party/quirc/quirc.h"
}

bool qrEncode(const std::string& utf8, QrBitmap& out, int ec)
{
	if (utf8.empty()) return false;

	// 用 level 编一份到 tmp。version 0 = 按内容自动挑最小版本；
	// QR_MODE_8 = 字节模式（UTF-8 字节流直接进码，绝大多数扫码器按 UTF-8 解，
	// 中文因此不会乱码）；caseSensitive = 1
	const auto encode = [&utf8](int level, QrBitmap& tmp) -> bool {
		QRcode* qr = QRcode_encodeString(utf8.c_str(), 0, (QRecLevel)level, QR_MODE_8, 1);
		if (!qr) return false;
		tmp.size = qr->width;
		tmp.modules.resize((size_t)qr->width * (size_t)qr->width);
		for (int i = 0; i < qr->width * qr->width; i++) {
			tmp.modules[i] = (uint8_t)(qr->data[i] & 1);
		}
		QRcode_free(qr);
		return true;
	};

	// 加 Logo：草料默认最高容错 H（30%），Logo 遮挡也能扫。
	// 内容超出 H 的容量编不出来时退回下面的自动选择 —— 有码可扫比容错打满更重要
	if (ec == QrEcH && encode(QR_ECLEVEL_H, out)) return true;

	// 版本号 = (边长 - 17) / 4
	const auto versionOf = [](const QrBitmap& b) { return (b.size - 17) / 4; };

	// QrEcAuto：草料同款「优先保持码型紧凑」。基线 = L 的最小版本：
	//   Q 与基线同版本 → Q（容错更高又不涨尺寸）；Q 升了版本而 M 没升 → M；
	//   连 M 都升 → 回 Q（版本反正要涨，优先保容错）。
	// L 都编不出来 = 内容超出容量，Q/M 必然也编不出，直接失败
	QrBitmap bl;
	if (!encode(QR_ECLEVEL_L, bl)) return false;
	const int vBase = versionOf(bl);
	QrBitmap bq, bm;
	if (encode(QR_ECLEVEL_Q, bq) && versionOf(bq) == vBase) { out = std::move(bq); return true; }
	if (encode(QR_ECLEVEL_M, bm) && versionOf(bm) == vBase) { out = std::move(bm); return true; }
	// Q、M 都得升版本：回 Q 优先保容错；Q 没编出来（理论到不了）退 M
	if (bq.size > 0) { out = std::move(bq); return true; }
	if (bm.size > 0) { out = std::move(bm); return true; }
	return false;
}

std::string qrDecode(const uint8_t* bgra, int w, int h, int scaleHint)
{
	if (!bgra || w <= 0 || h <= 0) return {};
	const int sw = w * (scaleHint < 1 ? 1 : scaleHint);
	const int sh = h * (scaleHint < 1 ? 1 : scaleHint);

	struct quirc* q = quirc_new();
	if (!q) return {};
	// resize 失败一般是尺寸非法或内存不够，这种情况下下面所有调用都没有意义
	if (quirc_resize(q, sw, sh) < 0) {
		quirc_destroy(q);
		return {};
	}
	uint8_t* buf = quirc_begin(q, nullptr, nullptr);
	if (!buf) {
		quirc_destroy(q);
		return {};
	}
	if (sw == w && sh == h) {
		for (int y = 0; y < h; y++) {
			const uint8_t* line = bgra + (size_t)y * w * 4;
			uint8_t* dst = buf + (size_t)y * w;
			for (int x = 0; x < w; x++) {
				// BGRA -> 亮度，BT.601 整数系数
				const uint8_t* p = line + x * 4;
				dst[x] = (uint8_t)(((int)p[2] * 77 + (int)p[1] * 150 + (int)p[0] * 29) >> 8);
			}
		}
	}
	else {
		// 最近邻放大：插值会让模块边界变灰，反而降低 quirc 的定位成功率
		for (int y = 0; y < sh; y++) {
			const uint8_t* line = bgra + (size_t)(y / scaleHint) * w * 4;
			uint8_t* dst = buf + (size_t)y * sw;
			for (int x = 0; x < sw; x++) {
				const uint8_t* p = line + (size_t)(x / scaleHint) * 4;
				dst[x] = (uint8_t)(((int)p[2] * 77 + (int)p[1] * 150 + (int)p[0] * 29) >> 8);
			}
		}
	}
	quirc_end(q);

	std::string result;
	const int count = quirc_count(q);
	for (int i = 0; i < count; i++) {
		// extract 的返回值是 void，失败时既不写 code 也不初始化它，直接拿去 decode
		// 等于读栈上的垃圾：code.size 是野值，quirc_decode 会按那个尺寸越界采样。
		// 先清零，再按 QR 规范的模块数上限（版本 40 = 177）挡一道
		quirc_code code{};
		quirc_data data{};
		quirc_extract(q, i, &code);
		if (code.size <= 0 || code.size > 177) continue;
		if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
			// payload 是 QRIS / 纯字节，不一定是 C 字符串：可能含 '\0'，也可能没有结尾符。
			// 一律按 payload_len 截断构造，不要当 const char* 直接喂给 string
			result.assign((const char*)data.payload, (size_t)data.payload_len);
			break;
		}
	}
	quirc_destroy(q);
	return result;
}
