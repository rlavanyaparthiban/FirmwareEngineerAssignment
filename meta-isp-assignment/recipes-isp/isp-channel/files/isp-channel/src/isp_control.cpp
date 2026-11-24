/* SPDX-License-Identifier: MIT */
#include "isp_control.h"

#include <algorithm>
#include <cmath>

namespace isp {

bool awbPreset(AwbMode mode, float &gainR, float &gainB)
{
	/* Gains for the virtual sensor under each illuminant; warmer light
	 * needs less red and more blue gain. */
	switch (mode) {
	case AwbMode::Incandescent:
	case AwbMode::Tungsten:
		gainR = 1.20f, gainB = 2.60f;
		return true;
	case AwbMode::Indoor:
		gainR = 1.40f, gainB = 2.30f;
		return true;
	case AwbMode::Fluorescent:
		gainR = 1.55f, gainB = 2.10f;
		return true;
	case AwbMode::Daylight:
		gainR = 1.90f, gainB = 1.60f;
		return true;
	case AwbMode::Cloudy:
		gainR = 2.10f, gainB = 1.45f;
		return true;
	default:
		return false;
	}
}

IspController::IspController(const float ccm[9], const IspLimits &limits)
	: limits_(limits), exposureUs_(controls_.exposureTimeUs), gain_(controls_.analogueGain)
{
	std::copy(ccm, ccm + 9, ccm_);
}

void IspController::update(const IspStats &stats)
{
	/* ---- White balance ---- */
	float r = gainR_, b = gainB_;
	if (!controls_.awbEnable || controls_.awbMode == AwbMode::Custom) {
		/* Manual: explicit gains if given, otherwise hold. */
		if (controls_.colourGainR > 0.0f && controls_.colourGainB > 0.0f) {
			gainR_ = controls_.colourGainR;
			gainB_ = controls_.colourGainB;
		}
		awbConverged_ = false;
	} else if (awbPreset(controls_.awbMode, r, b)) {
		gainR_ = r;
		gainB_ = b;
		awbConverged_ = false;
	} else if (SoftIsp::grayWorld(stats, r, b)) {
		/* Jump on the first estimate, then low-pass to avoid flicker. */
		const float alpha = awbConverged_ ? 0.25f : 1.0f;
		gainR_ += alpha * (r - gainR_);
		gainB_ += alpha * (b - gainB_);
		awbConverged_ = true;
	}

	/* ---- Exposure ---- */
	if (!controls_.aeEnable) {
		exposureUs_ = std::clamp(controls_.exposureTimeUs, limits_.minExposureUs, limits_.maxExposureUs);
		gain_ = std::clamp(controls_.analogueGain, limits_.minGain, limits_.maxGain);
		return;
	}

	/* Mean luminance of the white-balanced frame (Rec. 709 weights). */
	const double luma = 0.2126 * stats.meanR * gainR_ + 0.7152 * stats.meanG +
			    0.0722 * stats.meanB * gainB_;
	if (stats.samples < 16 || luma < 1e-4)
		return;

	double ratio = std::clamp(limits_.aeTarget / luma, 0.5, 2.0);
	if (std::fabs(ratio - 1.0) < 0.03)
		return; /* dead band: do not hunt around the target */

	/* The new exposure only takes effect on the next frame, so move part
	 * of the way (square root = half the error in stops) each time. */
	double total = double(exposureUs_) * gain_ * std::sqrt(ratio);
	total = std::clamp(total, double(limits_.minExposureUs) * limits_.minGain,
			   double(limits_.maxExposureUs) * limits_.maxGain);

	/* Prefer exposure time (no noise penalty); use gain once it is maxed. */
	exposureUs_ = float(std::min(total / limits_.minGain, double(limits_.maxExposureUs)));
	gain_ = float(std::clamp(total / exposureUs_, double(limits_.minGain), double(limits_.maxGain)));
}

IspParams IspController::ispParams() const
{
	IspParams p;
	p.gainR = gainR_;
	p.gainG = 1.0f;
	p.gainB = gainB_;
	std::copy(ccm_, ccm_ + 9, p.ccm);
	p.gamma = 0.0f;
	p.brightness = controls_.brightness;
	p.contrast = controls_.contrast;
	return p;
}

} /* namespace isp */
