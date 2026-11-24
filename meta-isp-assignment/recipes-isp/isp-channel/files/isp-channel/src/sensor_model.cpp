/* SPDX-License-Identifier: MIT */
#include "sensor_model.h"

#include <algorithm>
#include <cmath>

namespace isp {

bool invert3x3(const float m[9], float out[9])
{
	const float a = m[0], b = m[1], c = m[2];
	const float d = m[3], e = m[4], f = m[5];
	const float g = m[6], h = m[7], i = m[8];
	const float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
	if (std::fabs(det) < 1e-9f)
		return false;
	const float r = 1.0f / det;
	out[0] = (e * i - f * h) * r;
	out[1] = (c * h - b * i) * r;
	out[2] = (b * f - c * e) * r;
	out[3] = (f * g - d * i) * r;
	out[4] = (a * i - c * g) * r;
	out[5] = (c * d - a * f) * r;
	out[6] = (d * h - e * g) * r;
	out[7] = (b * g - a * h) * r;
	out[8] = (a * e - b * d) * r;
	return true;
}

float srgbToLinear(float v)
{
	return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float v)
{
	return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

namespace {

inline int clamp255(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/*
 * Integer YUV -> RGB conversion tables (BT.601, as used by JPEG and by
 * libyuv's ARGBToNV12). Q16 fixed point.
 */
struct YuvTables {
	int y[256], rv[256], gu[256], gv[256], bu[256];

	explicit YuvTables(bool fullRange)
	{
		const double ys = fullRange ? 1.0 : 255.0 / 219.0;
		const double cs = fullRange ? 1.0 : 255.0 / 224.0;
		const int yo = fullRange ? 0 : 16;
		for (int i = 0; i < 256; i++) {
			y[i] = int(std::lround((i - yo) * ys * 65536.0));
			rv[i] = int(std::lround((i - 128) * cs * 1.402 * 65536.0));
			gu[i] = int(std::lround((i - 128) * cs * -0.344136 * 65536.0));
			gv[i] = int(std::lround((i - 128) * cs * -0.714136 * 65536.0));
			bu[i] = int(std::lround((i - 128) * cs * 1.772 * 65536.0));
		}
	}

	inline void rgb(int yy, int u, int v, int &r, int &g, int &b) const
	{
		const int base = y[yy] + 32768;
		r = clamp255((base + rv[v]) >> 16);
		g = clamp255((base + gu[u] + gv[v]) >> 16);
		b = clamp255((base + bu[u]) >> 16);
	}
};

inline void chromaAt(const Yuv420View &s, unsigned x, unsigned y, int &u, int &v)
{
	const unsigned cx = x >> 1, cy = y >> 1;
	if (s.v) {
		u = s.u[cy * s.strideC + cx];
		v = s.v[cy * s.strideC + cx];
	} else {
		u = s.u[cy * s.strideC + 2 * cx];
		v = s.u[cy * s.strideC + 2 * cx + 1];
	}
}

} /* namespace */

BayerView SensorModel::capture(const Yuv420View &src, const SensorParams &p)
{
	const unsigned w = src.width, h = src.height;
	raw_.resize(size_t(w) * h);

	const YuvTables yuv(p.fullRangeSource);

	/* sRGB code value -> linear light, Q15. */
	int32_t lin[256];
	for (int i = 0; i < 256; i++)
		lin[i] = int32_t(std::lround(srgbToLinear(i / 255.0f) * 32768.0f));

	/*
	 * Per CFA colour, one row of (inverse CCM * illuminant * exposure),
	 * scaled to the sensor's code range. Q8, applied to Q15 linear values.
	 */
	float inv[9];
	if (!invert3x3(p.ccm, inv)) {
		static const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		std::copy(identity, identity + 9, inv);
	}
	const int maxCode = (1 << p.bits) - 1;
	const float range = float(maxCode - int(p.blackLevel));
	const float exposure = (p.exposureTimeUs / p.nominalExposureUs) * p.analogueGain;
	const float cast[3] = { 1.0f / p.wbRed, 1.0f, 1.0f / p.wbBlue };
	int64_t k[3][3];
	for (int c = 0; c < 3; c++)
		for (int j = 0; j < 3; j++)
			k[c][j] = int64_t(std::llround(double(inv[c * 3 + j]) * cast[c] *
						       exposure * range * 256.0));

	const unsigned ry = redRow(p.cfa), rx = redCol(p.cfa);
	const int black = int(p.blackLevel);
	uint16_t *out = raw_.data();

	const unsigned bands = pool_.size();
	pool_.parallelFor(bands, [&](unsigned band) {
		const unsigned y0 = (uint64_t(h) * band / bands) & ~1u;
		const unsigned y1 = band + 1 == bands ? h : (uint64_t(h) * (band + 1) / bands) & ~1u;
		for (unsigned y = y0; y < y1; y++) {
			const uint8_t *ys = src.y + size_t(y) * src.strideY;
			uint16_t *dst = out + size_t(y) * w;
			const bool isRedRow = (y & 1) == ry;
			for (unsigned x = 0; x < w; x++) {
				int u, v, r, g, b;
				chromaAt(src, x, y, u, v);
				yuv.rgb(ys[x], u, v, r, g, b);

				/* Which colour does the filter pass at this site? */
				int c;
				if (isRedRow)
					c = ((x & 1) == rx) ? 0 : 1;
				else
					c = ((x & 1) == rx) ? 1 : 2;

				const int64_t acc = k[c][0] * lin[r] + k[c][1] * lin[g] + k[c][2] * lin[b];
				int code = black + int((acc + (int64_t(1) << 22)) >> 23);
				dst[x] = uint16_t(code < 0 ? 0 : (code > maxCode ? maxCode : code));
			}
		}
	});

	BayerView view;
	view.data = raw_.data();
	view.width = w;
	view.height = h;
	view.stride = w;
	view.cfa = p.cfa;
	view.bits = p.bits;
	view.blackLevel = p.blackLevel;
	return view;
}

void SensorModel::referenceRgb(const Yuv420View &src, bool fullRange, uint8_t *rgb) const
{
	const YuvTables yuv(fullRange);
	for (unsigned y = 0; y < src.height; y++) {
		const uint8_t *ys = src.y + size_t(y) * src.strideY;
		uint8_t *dst = rgb + size_t(y) * src.width * 3;
		for (unsigned x = 0; x < src.width; x++) {
			int u, v, r, g, b;
			chromaAt(src, x, y, u, v);
			yuv.rgb(ys[x], u, v, r, g, b);
			dst[3 * x] = uint8_t(r);
			dst[3 * x + 1] = uint8_t(g);
			dst[3 * x + 2] = uint8_t(b);
		}
	}
}

} /* namespace isp */
