/* SPDX-License-Identifier: MIT */
/*
 * Software ISP: RAW Bayer in, 8-bit I420 (and optionally RGB888) out.
 *
 * Stages, in order:
 *   1. black-level correction   subtract the sensor pedestal
 *   2. white balance            per-channel gains (gray-world or manual)
 *   3. demosaic                 bilinear interpolation of the missing colours
 *   4. colour correction        3x3 matrix, camera RGB -> sRGB primaries
 *   5. gamma                    sRGB transfer curve, with brightness/contrast
 *   6. colour-space conversion  RGB -> BT.709 limited-range YUV 4:2:0
 *
 * The working precision between stages 2 and 5 is 12-bit linear. Stages 1+2
 * and stage 5 are point operations and are implemented as lookup tables;
 * stages 3, 4 and 6 are per-row loops. Rows are processed in horizontal
 * bands, one band per pool thread, with every stage run on a row while it is
 * still in the CPU cache.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "image.h"
#include "thread_pool.h"

namespace isp {

constexpr int kLinearMax = 4095; /* 12-bit linear working range */

struct IspParams {
	/* Stage 2: white-balance gains (green is the reference). */
	float gainR = 1.0f, gainG = 1.0f, gainB = 1.0f;
	/* Stage 4: colour correction matrix, row major. */
	float ccm[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	/* Stage 5: 0 selects the sRGB curve, otherwise out = in^(1/gamma). */
	float gamma = 0.0f;
	float brightness = 0.0f; /* -1 .. 1, added after gamma */
	float contrast = 1.0f;   /*  0 .. 2, gain around mid-grey after gamma */
};

/* Channel means of a RAW frame after black-level correction, in [0, 1]. */
struct IspStats {
	double meanR = 0, meanG = 0, meanB = 0;
	unsigned samples = 0;
};

/* The individual stages, exposed so that the unit tests can check each one. */
namespace stage {

/* Stages 1+2 as a table: raw code -> 12-bit linear, for one colour channel. */
void buildBlcAwbLut(unsigned bits, unsigned blackLevel, float gain, std::vector<uint16_t> &lut);

/* Stage 5 as a table: 12-bit linear -> 8-bit display code. */
void buildGammaLut(float gamma, float brightness, float contrast, uint8_t lut[kLinearMax + 1]);

/*
 * Stage 3 for one row. prev/cur/next are the rows above, at and below the
 * output row, each padded by one sample on both sides (index 0 is x = -1).
 * rgb receives width interleaved 12-bit R,G,B triplets.
 */
void demosaicRow(const uint16_t *prev, const uint16_t *cur, const uint16_t *next,
		 unsigned width, bool isRedRow, unsigned redColParity, uint16_t *rgb);

/* Stage 4 for one row, in place. m is the CCM in Q10 fixed point. */
void ccmRow(uint16_t *rgb, unsigned width, const int32_t m[9]);

/* Stage 5 for one row. */
void gammaRow(const uint16_t *rgb, unsigned width, const uint8_t *lut, uint8_t *out);

/* Stage 6: luma for one row, and chroma for a pair of rows (2x2 average). */
void lumaRow(const uint8_t *rgb, unsigned width, uint8_t *y);
void chromaRows(const uint8_t *rgb0, const uint8_t *rgb1, unsigned width, uint8_t *u, uint8_t *v);

} /* namespace stage */

class SoftIsp
{
public:
	explicit SoftIsp(ThreadPool &pool);

	/* Statistics for AWB and AE, measured on a subsampled grid. */
	static IspStats measure(const BayerView &raw);

	/* Gray-world white balance: gains that make the channel means equal. */
	static bool grayWorld(const IspStats &stats, float &gainR, float &gainB);

	/*
	 * Runs all stages. out must be a width x height I420 frame. rgbOut, if
	 * not null, also receives the stage 5 output as packed RGB888.
	 */
	void process(const BayerView &raw, const IspParams &params,
		     const I420Frame &out, uint8_t *rgbOut = nullptr);

private:
	struct Scratch {
		std::vector<uint16_t> lines[3];
		std::vector<uint16_t> rgb16;
		std::vector<uint8_t> rgb8[2];
	};

	ThreadPool &pool_;
	std::vector<Scratch> scratch_;
	std::vector<uint16_t> lut_[3];
	uint8_t gammaLut_[kLinearMax + 1];
};

} /* namespace isp */
