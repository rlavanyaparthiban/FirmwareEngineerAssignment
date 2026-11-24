/* SPDX-License-Identifier: MIT */
/*
 * Virtual sensor ("mosaic step").
 *
 * libcamera's virtual pipeline handler only delivers processed NV12 frames;
 * it has no RAW stream. To give the software ISP a realistic input, this
 * class turns each delivered frame back into what an image sensor would have
 * produced for that scene: linear light, the sensor's own colour response,
 * an uncorrected illuminant cast, a black-level pedestal, exposure scaling
 * and finally one colour sample per pixel behind a Bayer colour filter array.
 *
 * It is the exact inverse of the ISP stages, which makes the ISP verifiable:
 * running the ISP on the output must give the source picture back.
 */
#pragma once

#include <cstdint>
#include <vector>

#include "image.h"
#include "thread_pool.h"

namespace isp {

struct SensorParams {
	Cfa cfa = Cfa::RGGB;
	unsigned bits = 10;
	unsigned blackLevel = 64;

	/* Colour matrix the ISP will apply (camera RGB -> sRGB), row major. */
	float ccm[9] = { 1.80f, -0.65f, -0.15f,
			 -0.30f, 1.60f, -0.30f,
			 -0.05f, -0.60f, 1.65f };

	/*
	 * Illuminant cast, expressed as the white-balance gains that undo it.
	 * The sensor attenuates red by 1/wbRed and blue by 1/wbBlue.
	 */
	float wbRed = 1.90f;
	float wbBlue = 1.60f;

	/* Exposure: scene radiance is scaled by (time / nominal) * gain. */
	float exposureTimeUs = 10000.0f;
	float nominalExposureUs = 10000.0f;
	float analogueGain = 1.0f;

	/* true: source YUV is full range (JPEG); false: limited range. */
	bool fullRangeSource = true;
};

class SensorModel
{
public:
	explicit SensorModel(ThreadPool &pool) : pool_(pool) {}

	/*
	 * Converts a YUV 4:2:0 frame to a Bayer frame. The returned view points
	 * into a buffer owned by this object, valid until the next call.
	 */
	BayerView capture(const Yuv420View &src, const SensorParams &params);

	/* YUV -> RGB888 with the same maths; used to score the ISP round trip. */
	void referenceRgb(const Yuv420View &src, bool fullRange, uint8_t *rgb) const;

private:
	ThreadPool &pool_;
	std::vector<uint16_t> raw_;
};

/* 3x3 matrix inverse, row major. Returns false if singular. */
bool invert3x3(const float m[9], float out[9]);

/* sRGB transfer functions on [0, 1]. */
float srgbToLinear(float v);
float linearToSrgb(float v);

} /* namespace isp */
