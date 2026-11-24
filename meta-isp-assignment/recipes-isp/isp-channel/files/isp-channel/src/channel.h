/* SPDX-License-Identifier: MIT */
/*
 * The ISP channel: camera -> (virtual sensor) -> software ISP -> encoder.
 * Owns every stage and runs the frame loop.
 */
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "camera_source.h"
#include "config.h"
#include "control_socket.h"
#include "encoder.h"
#include "frame_stats.h"
#include "isp_control.h"
#include "sensor_model.h"
#include "snapshot.h"
#include "soft_isp.h"
#include "thread_pool.h"

namespace isp {

class Channel
{
public:
	explicit Channel(const Config &config);

	/* Brings up the encoder, the camera and the control socket. */
	bool init();

	/* Runs the frame loop until stopped; returns the process exit code. */
	int run();

	/* Both are safe to call from any thread. */
	void requestStop();
	void requestSnapshot();

private:
	std::string handleCommand(const std::string &command);
	std::string setControl(const std::string &name, const std::vector<std::string> &values);
	void applyControls(bool logRouting);
	void saveSnapshot(std::vector<uint8_t> &&rgb, const CapturedFrame &frame);
	std::string statusText();

	template<typename T, typename V>
	bool sendToCamera(const libcamera::Control<T> &control, const V &value,
			  const char *text, bool logRouting);

	const Config config_;
	ThreadPool pool_;
	SensorModel sensor_;
	SoftIsp isp_;
	IspController controller_;
	SensorParams sensorParams_;
	CameraSource camera_;
	Encoder encoder_;
	SnapshotWriter snapshots_;
	ControlSocket socket_;
	FrameStats stats_;

	std::atomic<bool> stop_{ false };
	std::atomic<unsigned> snapshotRequests_{ 0 };

	/* Control state shared with the control-socket thread. */
	std::mutex controlMutex_;
	IspControls desired_;
	bool controlsDirty_ = true;
	std::string lastReport_;
};

} /* namespace isp */
