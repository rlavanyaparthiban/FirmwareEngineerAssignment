/* SPDX-License-Identifier: MIT */
#include "encoder.h"

#include <cmath>

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "log.h"

namespace isp {

Encoder::~Encoder()
{
	stop();
}

bool Encoder::start(const std::string &description, unsigned width, unsigned height, double fps)
{
	gst_init(nullptr, nullptr);

	GError *error = nullptr;
	pipeline_ = gst_parse_launch(description.c_str(), &error);
	if (!pipeline_ || error) {
		LOG_ERROR("invalid encoder pipeline: %s", error ? error->message : "unknown error");
		g_clear_error(&error);
		return false;
	}

	source_ = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
	if (!source_ || !GST_IS_APP_SRC(source_)) {
		LOG_ERROR("encoder pipeline must start with 'appsrc name=src'");
		return false;
	}

	width_ = width;
	height_ = height;
	const int fpsNum = int(std::lround(fps * 1000.0));
	GstCaps *caps = gst_caps_new_simple("video/x-raw",
					    "format", G_TYPE_STRING, "I420",
					    "width", G_TYPE_INT, int(width),
					    "height", G_TYPE_INT, int(height),
					    "framerate", GST_TYPE_FRACTION, fpsNum, 1000,
					    "colorimetry", G_TYPE_STRING, "bt709",
					    nullptr);
	/*
	 * block + max-bytes: when the encoder cannot keep up, submit() waits
	 * instead of queueing frames without bound. Memory stays flat and the
	 * back-pressure reaches the request loop.
	 */
	g_object_set(source_,
		     "caps", caps,
		     "format", GST_FORMAT_TIME,
		     "is-live", TRUE,
		     "block", TRUE,
		     "max-bytes", guint64(2 * i420Size(width, height)),
		     nullptr);
	gst_caps_unref(caps);

	if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
		LOG_ERROR("encoder pipeline failed to start");
		healthy(); /* logs the reason from the bus */
		return false;
	}

	LOG_INFO("encoder pipeline: %s", description.c_str());
	return true;
}

bool Encoder::acquire(Frame &frame)
{
	const size_t size = i420Size(width_, height_);
	frame.buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
	if (!frame.buffer)
		return false;

	if (!gst_buffer_map(frame.buffer, &frame.map, GST_MAP_WRITE)) {
		gst_buffer_unref(frame.buffer);
		frame.buffer = nullptr;
		return false;
	}

	/* The buffer stays mapped until submit(). */
	frame.image.width = width_;
	frame.image.height = height_;
	frame.image.strideY = width_;
	frame.image.strideC = width_ / 2;
	frame.image.y = frame.map.data;
	frame.image.u = frame.map.data + size_t(width_) * height_;
	frame.image.v = frame.image.u + size_t(width_ / 2) * (height_ / 2);
	return true;
}

bool Encoder::submit(Frame &frame, uint64_t ptsNs, uint64_t durationNs)
{
	gst_buffer_unmap(frame.buffer, &frame.map);
	GST_BUFFER_PTS(frame.buffer) = ptsNs;
	GST_BUFFER_DTS(frame.buffer) = GST_CLOCK_TIME_NONE;
	GST_BUFFER_DURATION(frame.buffer) = durationNs;

	/* push_buffer takes ownership of the buffer in every case. */
	const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(source_), frame.buffer);
	frame.buffer = nullptr;
	if (ret != GST_FLOW_OK) {
		LOG_ERROR("encoder rejected a frame: %s", gst_flow_get_name(ret));
		failed_ = true;
		return false;
	}
	return true;
}

bool Encoder::healthy()
{
	if (!pipeline_)
		return false;

	GstBus *bus = gst_element_get_bus(pipeline_);
	while (GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
		GError *error = nullptr;
		gchar *debug = nullptr;
		gst_message_parse_error(message, &error, &debug);
		LOG_ERROR("encoder pipeline error: %s (%s)", error->message, debug ? debug : "-");
		g_clear_error(&error);
		g_free(debug);
		gst_message_unref(message);
		failed_ = true;
	}
	gst_object_unref(bus);
	return !failed_;
}

void Encoder::stop()
{
	if (!pipeline_)
		return;

	if (source_ && !failed_) {
		/* Let the muxer write its index; a truncated MP4 is unplayable. */
		gst_app_src_end_of_stream(GST_APP_SRC(source_));
		GstBus *bus = gst_element_get_bus(pipeline_);
		GstMessage *message = gst_bus_timed_pop_filtered(
			bus, 30 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
		if (!message)
			LOG_WARN("encoder did not finish within 30 s");
		else
			gst_message_unref(message);
		gst_object_unref(bus);
	}

	gst_element_set_state(pipeline_, GST_STATE_NULL);
	if (source_)
		gst_object_unref(source_);
	gst_object_unref(pipeline_);
	source_ = nullptr;
	pipeline_ = nullptr;
}

} /* namespace isp */
