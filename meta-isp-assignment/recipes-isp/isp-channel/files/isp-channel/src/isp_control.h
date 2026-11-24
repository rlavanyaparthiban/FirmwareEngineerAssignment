/* SPDX-License-Identifier: MIT */
/*
 * Control state of the software channel and the two small algorithms that
 * depend on it: auto white balance and auto exposure.
 *
 * The fields mirror the libcamera controls of the same name. The application
 * sends a control to the camera when the camera supports it and applies it
 * here otherwise.
 */
#pragma once

#include "sensor_model.h"
#include "soft_isp.h"

namespace isp {

/* Same numbering as libcamera's controls::AwbModeEnum. */
enum class AwbMode {
	Auto = 0,
	Incandescent,
	Tungsten,
	Fluorescent,
	Indoor,
	Daylight,
	Cloudy,
	Custom,
};

struct IspControls {
	bool aeEnable = true;
	bool awbEnable = true;
	AwbMode awbMode = AwbMode::Auto;
	float exposureTimeUs = 10000.0f; /* used when aeEnable is false */
	float analogueGain = 1.0f;       /* used when aeEnable is false */
	float brightness = 0.0f;
	float contrast = 1.0f;
	/* Manual gains for AwbMode::Custom; 0 means "keep the current gains". */
	float colourGainR = 0.0f;
	float colourGainB = 0.0f;
};

struct IspLimits {
	float minExposureUs = 100.0f;
	float maxExposureUs = 33333.0f; /* one frame period */
	float minGain = 1.0f;
	float maxGain = 16.0f;
	float aeTarget = 0.18f; /* mean linear luminance to aim for */
};

class IspController
{
public:
	IspController(const float ccm[9], const IspLimits &limits);

	void setControls(const IspControls &controls) { controls_ = controls; }
	const IspControls &controls() const { return controls_; }

	/* Feeds one frame's statistics to AWB and AE. */
	void update(const IspStats &stats);

	/* Parameters for the frame the statistics were measured on. */
	IspParams ispParams() const;

	/* Exposure the sensor should use for the next frame. */
	float exposureTimeUs() const { return exposureUs_; }
	float analogueGain() const { return gain_; }
	float gainR() const { return gainR_; }
	float gainB() const { return gainB_; }

private:
	IspControls controls_;
	IspLimits limits_;
	float ccm_[9];
	float gainR_ = 1.0f, gainB_ = 1.0f;
	bool awbConverged_ = false;
	float exposureUs_, gain_;
};

/* Preset white-balance gains for a fixed illuminant. */
bool awbPreset(AwbMode mode, float &gainR, float &gainB);

} /* namespace isp */
