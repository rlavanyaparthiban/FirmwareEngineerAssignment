/* SPDX-License-Identifier: MIT */
#include "soft_isp.h"

#include <algorithm>
#include <cmath>

#include "sensor_model.h" /* linearToSrgb() */

namespace isp {

namespace stage {

void buildBlcAwbLut(unsigned bits, unsigned blackLevel, float gain, std::vector<uint16_t> &lut)
{
	const unsigned size = 1u << bits;
	const float range = float(size - 1 - blackLevel);
	lut.resize(size);
	for (unsigned code = 0; code < size; code++) {
		/* Stage 1: remove the pedestal; codes below it are noise floor. */
		const float signal = code > blackLevel ? float(code - blackLevel) : 0.0f;
		/* Stage 2: white-balance gain, then clip at sensor white. */
		const float v = signal * gain * kLinearMax / range;
		lut[code] = uint16_t(std::min(float(kLinearMax), v + 0.5f));
	}
}

void buildGammaLut(float gamma, float brightness, float contrast, uint8_t lut[kLinearMax + 1])
{
	for (int i = 0; i <= kLinearMax; i++) {
		const float lin = float(i) / kLinearMax;
		float v = gamma > 0.0f ? std::pow(lin, 1.0f / gamma) : linearToSrgb(lin);
		v = (v - 0.5f) * contrast + 0.5f + brightness;
		lut[i] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
	}
}

void demosaicRow(const uint16_t *prev, const uint16_t *cur, const uint16_t *next,
		 unsigned width, bool isRedRow, unsigned redColParity, uint16_t *rgb)
{
	/* Skip the left padding sample so that index x is pixel x. */
	prev++, cur++, next++;

	/* Signed index: x - 1 must reach the padding sample, not wrap. */
	for (int x = 0; x < int(width); x++, rgb += 3) {
		const int c = cur[x];
		const int horiz = (cur[x - 1] + cur[x + 1] + 1) >> 1;
		const int vert = (prev[x] + next[x] + 1) >> 1;
		/* In a red row the red samples sit at redColParity; in a blue
		 * row the blue samples sit on the other column parity. */
		const bool colourSite = ((unsigned(x) & 1) == redColParity) == isRedRow;

		if (colourSite) {
			/* Red or blue site: green from the 4 edge neighbours,
			 * the opposite colour from the 4 diagonal neighbours. */
			const int cross = (cur[x - 1] + cur[x + 1] + prev[x] + next[x] + 2) >> 2;
			const int diag = (prev[x - 1] + prev[x + 1] + next[x - 1] + next[x + 1] + 2) >> 2;
			rgb[0] = uint16_t(isRedRow ? c : diag);
			rgb[1] = uint16_t(cross);
			rgb[2] = uint16_t(isRedRow ? diag : c);
		} else {
			/* Green site: the row's own colour lies left/right,
			 * the other colour above/below. */
			rgb[0] = uint16_t(isRedRow ? horiz : vert);
			rgb[1] = uint16_t(c);
			rgb[2] = uint16_t(isRedRow ? vert : horiz);
		}
	}
}

void ccmRow(uint16_t *rgb, unsigned width, const int32_t m[9])
{
	for (unsigned x = 0; x < width; x++, rgb += 3) {
		const int32_t r = rgb[0], g = rgb[1], b = rgb[2];
		const int32_t ro = (m[0] * r + m[1] * g + m[2] * b + 512) >> 10;
		const int32_t go = (m[3] * r + m[4] * g + m[5] * b + 512) >> 10;
		const int32_t bo = (m[6] * r + m[7] * g + m[8] * b + 512) >> 10;
		rgb[0] = uint16_t(std::clamp(ro, 0, kLinearMax));
		rgb[1] = uint16_t(std::clamp(go, 0, kLinearMax));
		rgb[2] = uint16_t(std::clamp(bo, 0, kLinearMax));
	}
}

void gammaRow(const uint16_t *rgb, unsigned width, const uint8_t *lut, uint8_t *out)
{
	for (unsigned i = 0; i < width * 3; i++)
		out[i] = lut[rgb[i]];
}

/* BT.709 limited-range RGB -> YCbCr, coefficients scaled by 256. */
void lumaRow(const uint8_t *rgb, unsigned width, uint8_t *y)
{
	for (unsigned x = 0; x < width; x++, rgb += 3)
		y[x] = uint8_t(16 + ((47 * rgb[0] + 157 * rgb[1] + 16 * rgb[2] + 128) >> 8));
}

void chromaRows(const uint8_t *rgb0, const uint8_t *rgb1, unsigned width, uint8_t *u, uint8_t *v)
{
	for (unsigned x = 0; x + 1 < width; x += 2, rgb0 += 6, rgb1 += 6) {
		/* Sum of the 2x2 block; the >> 10 folds in the divide by 4. */
		const int r = rgb0[0] + rgb0[3] + rgb1[0] + rgb1[3];
		const int g = rgb0[1] + rgb0[4] + rgb1[1] + rgb1[4];
		const int b = rgb0[2] + rgb0[5] + rgb1[2] + rgb1[5];
		u[x >> 1] = uint8_t(128 + ((-26 * r - 86 * g + 112 * b + 512) >> 10));
		v[x >> 1] = uint8_t(128 + ((112 * r - 102 * g - 10 * b + 512) >> 10));
	}
}

} /* namespace stage */

SoftIsp::SoftIsp(ThreadPool &pool)
	: pool_(pool), scratch_(pool.size())
{
}

IspStats SoftIsp::measure(const BayerView &raw)
{
	const unsigned ry = redRow(raw.cfa), rx = redCol(raw.cfa);
	const unsigned maxCode = (1u << raw.bits) - 1;
	const unsigned black = raw.blackLevel;
	/* Clipped samples carry no colour information; leave them out. */
	const unsigned saturated = black + (maxCode - black) * 97 / 100;

	uint64_t sumR = 0, sumG = 0, sumB = 0;
	unsigned count = 0;

	/* One 2x2 quad out of every 8x8 block is plenty for a global mean. */
	for (unsigned y = 0; y + 1 < raw.height; y += 8) {
		const uint16_t *row0 = raw.data + size_t(y) * raw.stride;
		const uint16_t *row1 = row0 + raw.stride;
		const uint16_t *redLine = ry == 0 ? row0 : row1;
		const uint16_t *blueLine = ry == 0 ? row1 : row0;
		for (unsigned x = 0; x + 1 < raw.width; x += 8) {
			const unsigned r = redLine[x + rx];
			const unsigned g1 = redLine[x + (rx ^ 1)];
			const unsigned g2 = blueLine[x + rx];
			const unsigned b = blueLine[x + (rx ^ 1)];
			if (r >= saturated || g1 >= saturated || g2 >= saturated || b >= saturated)
				continue;
			sumR += r > black ? r - black : 0;
			sumG += (g1 > black ? g1 - black : 0) + (g2 > black ? g2 - black : 0);
			sumB += b > black ? b - black : 0;
			count++;
		}
	}

	IspStats stats;
	stats.samples = count;
	if (count) {
		const double range = double(maxCode - black);
		stats.meanR = double(sumR) / count / range;
		stats.meanG = double(sumG) / (2.0 * count) / range;
		stats.meanB = double(sumB) / count / range;
	}
	return stats;
}

bool SoftIsp::grayWorld(const IspStats &stats, float &gainR, float &gainB)
{
	/* Too dark or too few unclipped samples: no reliable estimate. */
	if (stats.samples < 16 || stats.meanR < 1e-3 || stats.meanG < 1e-3 || stats.meanB < 1e-3)
		return false;

	gainR = std::clamp(float(stats.meanG / stats.meanR), 0.25f, 8.0f);
	gainB = std::clamp(float(stats.meanG / stats.meanB), 0.25f, 8.0f);
	return true;
}

void SoftIsp::process(const BayerView &raw, const IspParams &params,
		      const I420Frame &out, uint8_t *rgbOut)
{
	const unsigned w = raw.width, h = raw.height;
	const unsigned ry = redRow(raw.cfa), rx = redCol(raw.cfa);

	/* Point-operation tables, rebuilt per frame (a few thousand entries). */
	stage::buildBlcAwbLut(raw.bits, raw.blackLevel, params.gainR, lut_[0]);
	stage::buildBlcAwbLut(raw.bits, raw.blackLevel, params.gainG, lut_[1]);
	stage::buildBlcAwbLut(raw.bits, raw.blackLevel, params.gainB, lut_[2]);
	stage::buildGammaLut(params.gamma, params.brightness, params.contrast, gammaLut_);

	int32_t ccm[9];
	for (int i = 0; i < 9; i++)
		ccm[i] = int32_t(std::lround(params.ccm[i] * 1024.0f));

	/*
	 * Stages 1+2 for one RAW row into a padded line buffer. Rows and
	 * columns outside the frame are mirrored two samples in, which keeps
	 * the CFA phase intact.
	 */
	auto correctLine = [&](int y, uint16_t *line) {
		if (y < 0)
			y = 1;
		else if (y >= int(h))
			y = int(h) - 2;
		const uint16_t *src = raw.data + size_t(y) * raw.stride;
		const bool isRedRow = (unsigned(y) & 1) == ry;
		/* Table for even and odd columns of this row. */
		const uint16_t *even, *odd;
		if (isRedRow) {
			even = rx == 0 ? lut_[0].data() : lut_[1].data();
			odd = rx == 0 ? lut_[1].data() : lut_[0].data();
		} else {
			even = rx == 0 ? lut_[1].data() : lut_[2].data();
			odd = rx == 0 ? lut_[2].data() : lut_[1].data();
		}
		uint16_t *dst = line + 1;
		for (unsigned x = 0; x + 1 < w; x += 2) {
			dst[x] = even[src[x]];
			dst[x + 1] = odd[src[x + 1]];
		}
		line[0] = dst[1];
		dst[w] = dst[w - 2];
	};

	const unsigned bands = pool_.size();
	pool_.parallelFor(bands, [&](unsigned band) {
		const unsigned y0 = (uint64_t(h) * band / bands) & ~1u;
		const unsigned y1 = band + 1 == bands ? h : (uint64_t(h) * (band + 1) / bands) & ~1u;
		if (y0 >= y1)
			return;

		Scratch &s = scratch_[band];
		for (auto &line : s.lines)
			line.resize(w + 2);
		s.rgb16.resize(size_t(w) * 3);
		s.rgb8[0].resize(size_t(w) * 3);
		s.rgb8[1].resize(size_t(w) * 3);

		uint16_t *prev = s.lines[0].data();
		uint16_t *cur = s.lines[1].data();
		uint16_t *next = s.lines[2].data();
		correctLine(int(y0) - 1, prev);
		correctLine(int(y0), cur);

		for (unsigned y = y0; y < y1; y++) {
			correctLine(int(y) + 1, next);

			uint8_t *rgb8 = s.rgb8[y & 1].data();
			stage::demosaicRow(prev, cur, next, w, (y & 1) == ry, rx, s.rgb16.data());
			stage::ccmRow(s.rgb16.data(), w, ccm);
			stage::gammaRow(s.rgb16.data(), w, gammaLut_, rgb8);

			stage::lumaRow(rgb8, w, out.y + size_t(y) * out.strideY);
			if (y & 1)
				stage::chromaRows(s.rgb8[0].data(), s.rgb8[1].data(), w,
						  out.u + size_t(y >> 1) * out.strideC,
						  out.v + size_t(y >> 1) * out.strideC);
			if (rgbOut)
				std::copy(rgb8, rgb8 + size_t(w) * 3, rgbOut + size_t(y) * w * 3);

			/* Slide the three-row window down by one. */
			uint16_t *recycled = prev;
			prev = cur;
			cur = next;
			next = recycled;
		}
	});
}

} /* namespace isp */
