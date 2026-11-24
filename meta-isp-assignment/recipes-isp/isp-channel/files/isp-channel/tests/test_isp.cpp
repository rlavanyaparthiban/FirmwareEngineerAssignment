/* SPDX-License-Identifier: MIT */
/*
 * Unit tests for the software ISP stages, the virtual sensor and the control
 * algorithms. No camera is involved: frames are synthesised in memory.
 */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "isp_control.h"
#include "sensor_model.h"
#include "soft_isp.h"

using namespace isp;

static int failures = 0;

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                              \
	} while (0)

#define CHECK_NEAR(a, b, tol)                                                  \
	do {                                                                   \
		if (std::fabs(double(a) - double(b)) > (tol)) {                \
			std::printf("FAIL %s:%d: %s = %g, expected %g +/- %g\n", \
				    __FILE__, __LINE__, #a, double(a), double(b), double(tol)); \
			failures++;                                            \
		}                                                              \
	} while (0)

static const Cfa kAllCfa[] = { Cfa::RGGB, Cfa::GRBG, Cfa::GBRG, Cfa::BGGR };

/* Colour (0=R, 1=G, 2=B) of the CFA site at (x, y). */
static int siteColour(Cfa cfa, unsigned x, unsigned y)
{
	const bool redRowHere = (y & 1) == redRow(cfa);
	const bool colourCol = (x & 1) == redCol(cfa);
	if (redRowHere)
		return colourCol ? 0 : 1;
	return colourCol ? 1 : 2;
}

/* A smooth test picture as NV12 (full range); grey-only when colour is false. */
struct TestPicture {
	unsigned w, h;
	std::vector<uint8_t> y, uv;

	TestPicture(unsigned width, unsigned height, bool colour = true)
		: w(width), h(height), y(w * h), uv(w * h / 2, 128)
	{
		for (unsigned j = 0; j < h; j++)
			for (unsigned i = 0; i < w; i++)
				y[j * w + i] = uint8_t(128 + 90 * std::sin(i * 0.05) * std::cos(j * 0.04));
		for (unsigned j = 0; colour && j < h / 2; j++)
			for (unsigned i = 0; i < w / 2; i++) {
				uv[j * w + 2 * i] = uint8_t(128 + 50 * std::sin(i * 0.07 + 1.0));
				uv[j * w + 2 * i + 1] = uint8_t(128 + 50 * std::cos(j * 0.09));
			}
	}

	Yuv420View view() const
	{
		Yuv420View v;
		v.y = y.data();
		v.u = uv.data();
		v.width = w;
		v.height = h;
		v.strideY = w;
		v.strideC = w;
		return v;
	}
};

static double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
	double sse = 0;
	for (size_t i = 0; i < n; i++) {
		const double d = double(a[i]) - double(b[i]);
		sse += d * d;
	}
	if (sse == 0)
		return 99.0;
	return 10.0 * std::log10(255.0 * 255.0 * n / sse);
}

struct I420Buffer {
	std::vector<uint8_t> data;
	I420Frame frame;

	I420Buffer(unsigned w, unsigned h) : data(i420Size(w, h))
	{
		frame.y = data.data();
		frame.u = frame.y + size_t(w) * h;
		frame.v = frame.u + size_t(w / 2) * (h / 2);
		frame.width = w;
		frame.height = h;
		frame.strideY = w;
		frame.strideC = w / 2;
	}
};

static void testBlackLevelAndGain()
{
	std::vector<uint16_t> lut;
	stage::buildBlcAwbLut(10, 64, 1.0f, lut);
	CHECK(lut.size() == 1024);
	CHECK(lut[0] == 0);
	CHECK(lut[64] == 0);            /* pedestal maps to zero */
	CHECK(lut[1023] == kLinearMax); /* sensor white maps to full scale */
	CHECK_NEAR(lut[64 + 480], kLinearMax / 2.0, 3.0);

	stage::buildBlcAwbLut(10, 64, 2.0f, lut);
	CHECK_NEAR(lut[64 + 240], kLinearMax / 2.0, 3.0); /* gain doubles the signal */
	CHECK(lut[1023] == kLinearMax);                   /* and clips at white */
}

static void testDemosaic()
{
	const unsigned w = 16, h = 16;
	const uint16_t colour[3] = { 1000, 2000, 3000 };

	for (Cfa cfa : kAllCfa) {
		/* Flat colour field: every interpolated pixel must be exact. */
		std::vector<uint16_t> rows[3];
		for (int r = 0; r < 3; r++) {
			rows[r].resize(w + 2);
			for (unsigned x = 0; x < w + 2; x++)
				rows[r][x] = colour[siteColour(cfa, x + 1, unsigned(r + 1))];
		}
		/* rows[1] is frame row y = 2 (padded index 0 is x = -1, an odd
		 * column, hence the x + 1 above). */
		std::vector<uint16_t> rgb(w * 3);
		const unsigned y = 2;
		stage::demosaicRow(rows[0].data(), rows[1].data(), rows[2].data(), w,
				   (y & 1) == redRow(cfa), redCol(cfa), rgb.data());
		for (unsigned x = 0; x < w; x++) {
			CHECK(rgb[3 * x] == colour[0]);
			CHECK(rgb[3 * x + 1] == colour[1]);
			CHECK(rgb[3 * x + 2] == colour[2]);
		}
		(void)h;
	}

	/* Bilinear weights at a red site of an RGGB frame. */
	const uint16_t prev[5] = { 10, 100, 30, 100, 0 };
	const uint16_t cur[5] = { 200, 500, 400, 0, 0 };
	const uint16_t next[5] = { 50, 300, 70, 0, 0 };
	uint16_t rgb[9];
	stage::demosaicRow(prev, cur, next, 3, true, 1, rgb);
	/* Pixel x = 1 (padded index 2) is the red site. */
	CHECK(rgb[3] == 400);
	CHECK(rgb[4] == (500 + 0 + 30 + 70 + 2) / 4);
	CHECK(rgb[5] == (100 + 100 + 300 + 0 + 2) / 4);
	/* Pixel x = 0 is a green site in a red row: red left/right, blue up/down. */
	CHECK(rgb[0] == (200 + 400 + 1) / 2);
	CHECK(rgb[1] == 500);
	CHECK(rgb[2] == (100 + 300 + 1) / 2);
}

static void testColourMatrix()
{
	uint16_t rgb[6] = { 1000, 2000, 3000, 4095, 4095, 4095 };
	const int32_t identity[9] = { 1024, 0, 0, 0, 1024, 0, 0, 0, 1024 };
	stage::ccmRow(rgb, 2, identity);
	CHECK(rgb[0] == 1000 && rgb[1] == 2000 && rgb[2] == 3000);

	/* Rows that sum to one keep neutral colours neutral. */
	const float m[9] = { 1.80f, -0.65f, -0.15f, -0.30f, 1.60f, -0.30f, -0.05f, -0.60f, 1.65f };
	int32_t q[9];
	for (int i = 0; i < 9; i++)
		q[i] = int32_t(std::lround(m[i] * 1024.0f));
	uint16_t grey[3] = { 2000, 2000, 2000 };
	stage::ccmRow(grey, 1, q);
	CHECK_NEAR(grey[0], 2000, 2);
	CHECK_NEAR(grey[1], 2000, 2);
	CHECK_NEAR(grey[2], 2000, 2);

	/* Saturated red: negative results clamp to zero, large ones to white. */
	uint16_t red[3] = { 4095, 0, 0 };
	stage::ccmRow(red, 1, q);
	CHECK(red[0] == kLinearMax && red[1] == 0 && red[2] == 0);
}

static void testGamma()
{
	uint8_t lut[kLinearMax + 1];
	stage::buildGammaLut(0.0f, 0.0f, 1.0f, lut);
	CHECK(lut[0] == 0);
	CHECK(lut[kLinearMax] == 255);
	for (int i = 1; i <= kLinearMax; i++)
		CHECK(lut[i] >= lut[i - 1]);
	/* 18% grey encodes to about 46% in sRGB. */
	CHECK_NEAR(lut[int(0.18 * kLinearMax)], 118, 2);

	uint8_t bright[kLinearMax + 1], flat[kLinearMax + 1], linear[kLinearMax + 1];
	stage::buildGammaLut(0.0f, 0.2f, 1.0f, bright);
	CHECK_NEAR(bright[1000] - lut[1000], 51, 1); /* +0.2 of full scale */
	stage::buildGammaLut(0.0f, 0.0f, 0.0f, flat);
	CHECK(flat[0] == 128 && flat[kLinearMax] == 128); /* zero contrast = mid grey */
	stage::buildGammaLut(1.0f, 0.0f, 1.0f, linear);
	CHECK_NEAR(linear[2048], 128, 1);
}

static void testColourSpaceConversion()
{
	const uint8_t white[6] = { 255, 255, 255, 255, 255, 255 };
	const uint8_t black[6] = { 0, 0, 0, 0, 0, 0 };
	const uint8_t red[6] = { 255, 0, 0, 255, 0, 0 };
	uint8_t y[2], u, v;

	stage::lumaRow(white, 2, y);
	CHECK_NEAR(y[0], 235, 1);
	stage::chromaRows(white, white, 2, &u, &v);
	CHECK(u == 128 && v == 128);

	stage::lumaRow(black, 2, y);
	CHECK(y[0] == 16);

	stage::lumaRow(red, 2, y);
	CHECK_NEAR(y[0], 63, 1); /* BT.709: 16 + 0.2126 * 219 */
	stage::chromaRows(red, red, 2, &u, &v);
	CHECK_NEAR(u, 102, 1);
	CHECK_NEAR(v, 240, 1);
}

static void testGrayWorld()
{
	ThreadPool pool(2);
	SensorModel sensor(pool);
	/* A neutral scene satisfies the gray-world assumption exactly. */
	TestPicture pic(256, 128, false);

	for (Cfa cfa : kAllCfa) {
		SensorParams sp;
		sp.cfa = cfa;
		sp.wbRed = 2.2f;
		sp.wbBlue = 1.3f;
		BayerView raw = sensor.capture(pic.view(), sp);
		IspStats stats = SoftIsp::measure(raw);
		float gr = 0, gb = 0;
		CHECK(SoftIsp::grayWorld(stats, gr, gb));
		CHECK_NEAR(gr, 2.2f, 0.03f);
		CHECK_NEAR(gb, 1.3f, 0.03f);
	}

	IspStats dark;
	float gr = 0, gb = 0;
	CHECK(!SoftIsp::grayWorld(dark, gr, gb));
}

/* Sensor model followed by the ISP must reproduce the source picture. */
static void testRoundTrip()
{
	const unsigned w = 320, h = 240;
	ThreadPool pool(3);
	SensorModel sensor(pool);
	SoftIsp isp(pool);
	TestPicture pic(w, h);

	std::vector<uint8_t> reference(w * h * 3), result(w * h * 3);
	sensor.referenceRgb(pic.view(), true, reference.data());

	for (Cfa cfa : kAllCfa) {
		SensorParams sp;
		sp.cfa = cfa;
		BayerView raw = sensor.capture(pic.view(), sp);

		IspParams ip;
		ip.gainR = sp.wbRed;
		ip.gainB = sp.wbBlue;
		std::memcpy(ip.ccm, sp.ccm, sizeof(ip.ccm));

		I420Buffer out(w, h);
		isp.process(raw, ip, out.frame, result.data());
		const double db = psnr(reference.data(), result.data(), reference.size());
		std::printf("  round trip %d: PSNR %.1f dB\n", int(cfa), db);
		CHECK(db > 32.0);

		/* Leaving a stage out must hurt, proving each one does work. */
		IspParams noWb = ip;
		noWb.gainR = noWb.gainB = 1.0f;
		isp.process(raw, noWb, out.frame, result.data());
		CHECK(psnr(reference.data(), result.data(), reference.size()) < db - 6.0);

		IspParams noCcm = ip;
		const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		std::memcpy(noCcm.ccm, identity, sizeof(identity));
		isp.process(raw, noCcm, out.frame, result.data());
		CHECK(psnr(reference.data(), result.data(), reference.size()) < db - 3.0);

		BayerView noBlc = raw;
		noBlc.blackLevel = 0;
		isp.process(noBlc, ip, out.frame, result.data());
		CHECK(psnr(reference.data(), result.data(), reference.size()) < db - 3.0);
	}
}

/* The result must not depend on how many threads share the work. */
static void testThreadInvariance()
{
	const unsigned w = 322, h = 198; /* deliberately awkward band sizes */
	TestPicture pic(w, h);
	std::vector<uint8_t> first;

	for (unsigned threads : { 1u, 2u, 3u, 4u, 7u }) {
		ThreadPool pool(threads);
		SensorModel sensor(pool);
		SoftIsp isp(pool);
		SensorParams sp;
		BayerView raw = sensor.capture(pic.view(), sp);
		IspParams ip;
		ip.gainR = sp.wbRed;
		ip.gainB = sp.wbBlue;
		I420Buffer out(w, h);
		std::vector<uint8_t> rgb(w * h * 3);
		isp.process(raw, ip, out.frame, rgb.data());

		std::vector<uint8_t> all(out.data);
		all.insert(all.end(), rgb.begin(), rgb.end());
		if (first.empty())
			first = all;
		else
			CHECK(all == first);
	}
}

static void testAutoExposure()
{
	const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	IspLimits limits;
	IspController ctrl(identity, limits);

	/* A scene whose brightness is proportional to exposure. */
	const double sceneAtNominal = 0.05; /* too dark at 10 ms, gain 1 */
	IspStats stats;
	stats.samples = 1000;
	for (int i = 0; i < 40; i++) {
		const double scale = ctrl.exposureTimeUs() / 10000.0 * ctrl.analogueGain();
		stats.meanR = stats.meanG = stats.meanB = sceneAtNominal * scale;
		ctrl.update(stats);
	}
	const double scale = ctrl.exposureTimeUs() / 10000.0 * ctrl.analogueGain();
	CHECK_NEAR(sceneAtNominal * scale, limits.aeTarget, 0.01);
	CHECK(ctrl.exposureTimeUs() <= limits.maxExposureUs);
	CHECK(ctrl.analogueGain() >= 1.0f);

	/* Manual exposure overrides the loop. */
	IspControls manual;
	manual.aeEnable = false;
	manual.exposureTimeUs = 5000.0f;
	manual.analogueGain = 2.0f;
	ctrl.setControls(manual);
	ctrl.update(stats);
	CHECK(ctrl.exposureTimeUs() == 5000.0f && ctrl.analogueGain() == 2.0f);
}

static void testWhiteBalanceModes()
{
	const float identity[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	IspController ctrl(identity, IspLimits());
	IspStats stats;
	stats.samples = 1000;
	stats.meanR = 0.1, stats.meanG = 0.2, stats.meanB = 0.16;

	ctrl.update(stats); /* auto: gray world */
	CHECK_NEAR(ctrl.gainR(), 2.0, 0.01);
	CHECK_NEAR(ctrl.gainB(), 1.25, 0.01);

	IspControls c;
	c.awbMode = AwbMode::Tungsten;
	ctrl.setControls(c);
	ctrl.update(stats);
	CHECK_NEAR(ctrl.gainR(), 1.20, 0.001);
	CHECK_NEAR(ctrl.gainB(), 2.60, 0.001);

	c.awbEnable = false; /* off: hold the last gains */
	ctrl.setControls(c);
	stats.meanR = 0.2;
	ctrl.update(stats);
	CHECK_NEAR(ctrl.gainR(), 1.20, 0.001);

	c.colourGainR = 1.5f, c.colourGainB = 1.7f; /* explicit manual gains */
	ctrl.setControls(c);
	ctrl.update(stats);
	CHECK_NEAR(ctrl.gainR(), 1.5, 0.001);
	CHECK_NEAR(ctrl.gainB(), 1.7, 0.001);

	c.brightness = 0.1f, c.contrast = 1.3f;
	ctrl.setControls(c);
	CHECK(ctrl.ispParams().brightness == 0.1f && ctrl.ispParams().contrast == 1.3f);
}

int main()
{
	struct {
		const char *name;
		void (*fn)();
	} tests[] = {
		{ "black level + white balance table", testBlackLevelAndGain },
		{ "bilinear demosaic", testDemosaic },
		{ "colour correction matrix", testColourMatrix },
		{ "gamma, brightness, contrast", testGamma },
		{ "RGB to YUV", testColourSpaceConversion },
		{ "gray-world estimate", testGrayWorld },
		{ "sensor + ISP round trip", testRoundTrip },
		{ "thread-count invariance", testThreadInvariance },
		{ "auto exposure", testAutoExposure },
		{ "white-balance modes", testWhiteBalanceModes },
	};

	for (const auto &t : tests) {
		const int before = failures;
		t.fn();
		std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", t.name);
	}
	std::printf("%d failure(s)\n", failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
