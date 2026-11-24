/* SPDX-License-Identifier: MIT */
/*
 * Plain image descriptors shared by the virtual sensor, the software ISP and
 * the encoder. They are non-owning views: whoever allocates the memory keeps
 * it alive for the duration of the call.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace isp {

/* Colour filter array layout, named after the top-left 2x2 quad. */
enum class Cfa { RGGB, GRBG, GBRG, BGGR };

/* True if rows with parity redRow(cfa) contain red samples. */
constexpr unsigned redRow(Cfa c) { return (c == Cfa::RGGB || c == Cfa::GRBG) ? 0 : 1; }
/* Column parity of the red sample inside a red row. */
constexpr unsigned redCol(Cfa c) { return (c == Cfa::RGGB || c == Cfa::GBRG) ? 0 : 1; }

/* RAW Bayer frame, one 16-bit little-endian word per sample. */
struct BayerView {
	const uint16_t *data = nullptr;
	unsigned width = 0;
	unsigned height = 0;
	unsigned stride = 0; /* in samples, >= width */
	Cfa cfa = Cfa::RGGB;
	unsigned bits = 10;      /* significant bits per sample */
	unsigned blackLevel = 64; /* sensor pedestal, in sample units */
};

/* 8-bit YUV 4:2:0 frame. Semi-planar (NV12) when v == nullptr. */
struct Yuv420View {
	const uint8_t *y = nullptr;
	const uint8_t *u = nullptr; /* U plane, or interleaved UV plane for NV12 */
	const uint8_t *v = nullptr; /* nullptr for NV12 */
	unsigned width = 0;
	unsigned height = 0;
	unsigned strideY = 0;
	unsigned strideC = 0;
};

/* Writable planar I420 destination. */
struct I420Frame {
	uint8_t *y = nullptr;
	uint8_t *u = nullptr;
	uint8_t *v = nullptr;
	unsigned width = 0;
	unsigned height = 0;
	unsigned strideY = 0;
	unsigned strideC = 0;
};

inline size_t i420Size(unsigned w, unsigned h) { return size_t(w) * h * 3 / 2; }

} /* namespace isp */
