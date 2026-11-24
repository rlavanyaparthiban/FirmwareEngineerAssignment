/* SPDX-License-Identifier: MIT */
/*
 * Video encoder and sink, built from a GStreamer pipeline description.
 *
 * The description must start with "appsrc name=src". Everything after it
 * (encoder, muxer, file or network sink) is configuration, so switching
 * between software H.264, MJPEG, a hardware encoder or an RTP stream needs no
 * code change.
 */
#pragma once

#include <cstdint>
#include <string>

#include <gst/gst.h>

#include "image.h"

namespace isp {

class Encoder
{
public:
	/* A frame the ISP writes into directly; no copy on the way in. */
	struct Frame {
		GstBuffer *buffer = nullptr;
		GstMapInfo map;
		I420Frame image;
	};

	Encoder() = default;
	~Encoder();

	Encoder(const Encoder &) = delete;
	Encoder &operator=(const Encoder &) = delete;

	bool start(const std::string &description, unsigned width, unsigned height, double fps);

	/* Allocates an encoder input buffer and maps it for writing. */
	bool acquire(Frame &frame);

	/* Hands the frame to the pipeline. Blocks while the encoder is busy. */
	bool submit(Frame &frame, uint64_t ptsNs, uint64_t durationNs);

	/* Returns false once the pipeline has reported an error. */
	bool healthy();

	/* Sends end-of-stream and waits for the muxer to finalise its file. */
	void stop();

private:
	GstElement *pipeline_ = nullptr;
	GstElement *source_ = nullptr;
	unsigned width_ = 0, height_ = 0;
	bool failed_ = false;
};

} /* namespace isp */
