/* SPDX-License-Identifier: MIT */
/*
 * isp-bench: times the virtual sensor and the software ISP on a synthetic
 * frame for several thread counts. No camera or encoder involved.
 *
 *   isp-bench [WIDTH HEIGHT [FRAMES]]
 */
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "sensor_model.h"
#include "soft_isp.h"

using namespace isp;

int main(int argc, char **argv)
{
	const unsigned w = argc > 2 ? unsigned(std::atoi(argv[1])) : 1920;
	const unsigned h = argc > 2 ? unsigned(std::atoi(argv[2])) : 1080;
	const unsigned frames = argc > 3 ? unsigned(std::atoi(argv[3])) : 20;
	if (w < 16 || h < 16 || w % 4 || h % 2 || frames == 0) {
		std::fprintf(stderr, "Usage: %s [WIDTH HEIGHT [FRAMES]]\n", argv[0]);
		return 2;
	}

	/* Synthetic NV12 picture with detail in luma and chroma. */
	std::vector<uint8_t> y(size_t(w) * h), uv(size_t(w) * h / 2);
	for (unsigned j = 0; j < h; j++)
		for (unsigned i = 0; i < w; i++)
			y[size_t(j) * w + i] = uint8_t(128 + 90 * std::sin(i * 0.05) * std::cos(j * 0.04));
	for (size_t i = 0; i < uv.size(); i++)
		uv[i] = uint8_t(128 + 40 * std::sin(double(i) * 0.01));
	Yuv420View src;
	src.y = y.data();
	src.u = uv.data();
	src.width = w;
	src.height = h;
	src.strideY = src.strideC = w;

	std::vector<uint8_t> out(i420Size(w, h));
	I420Frame dst;
	dst.y = out.data();
	dst.u = dst.y + size_t(w) * h;
	dst.v = dst.u + size_t(w / 2) * (h / 2);
	dst.width = w;
	dst.height = h;
	dst.strideY = w;
	dst.strideC = w / 2;

	const unsigned cpus = std::thread::hardware_concurrency();
	std::printf("isp-bench: %ux%u, %u frames per run, %u CPU(s) online\n", w, h, frames, cpus);
	std::printf("%8s %12s %12s %10s %10s\n", "threads", "sensor ms", "isp ms", "isp fps", "speed-up");

	double baseline = 0;
	for (unsigned threads : { 1u, 2u, 4u, 8u }) {
		if (threads > 1 && threads > cpus)
			break;
		ThreadPool pool(threads);
		SensorModel sensor(pool);
		SoftIsp isp(pool);
		SensorParams sp;
		IspParams ip;
		ip.gainR = sp.wbRed;
		ip.gainB = sp.wbBlue;
		for (int i = 0; i < 9; i++)
			ip.ccm[i] = sp.ccm[i];

		using Clock = std::chrono::steady_clock;
		double sensorMs = 0, ispMs = 0;
		for (unsigned f = 0; f <= frames; f++) {
			const auto t0 = Clock::now();
			BayerView raw = sensor.capture(src, sp);
			const auto t1 = Clock::now();
			SoftIsp::measure(raw);
			isp.process(raw, ip, dst);
			const auto t2 = Clock::now();
			if (f == 0)
				continue; /* warm-up: first-touch page faults */
			sensorMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
			ispMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
		}
		sensorMs /= frames;
		ispMs /= frames;
		if (threads == 1)
			baseline = ispMs;
		std::printf("%8u %12.2f %12.2f %10.1f %9.2fx\n", threads, sensorMs, ispMs,
			    1000.0 / ispMs, baseline / ispMs);
	}
	return 0;
}
