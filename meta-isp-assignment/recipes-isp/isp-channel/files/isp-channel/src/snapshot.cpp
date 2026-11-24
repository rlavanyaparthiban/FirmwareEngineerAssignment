/* SPDX-License-Identifier: MIT */
#include "snapshot.h"

#include <cstdio>
#include <png.h>

#include "log.h"

namespace isp {

SnapshotWriter::SnapshotWriter()
	: thread_([this] { run(); })
{
}

SnapshotWriter::~SnapshotWriter()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		exit_ = true;
	}
	wake_.notify_all();
	thread_.join(); /* pending snapshots are written before exit */
}

void SnapshotWriter::submit(std::vector<uint8_t> &&rgb, unsigned width, unsigned height,
			    const std::string &path)
{
	std::lock_guard<std::mutex> lock(mutex_);
	jobs_.push_back({ std::move(rgb), width, height, path });
	wake_.notify_one();
}

void SnapshotWriter::run()
{
	for (;;) {
		Job job;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return exit_ || !jobs_.empty(); });
			if (jobs_.empty())
				return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}

		if (writePng(job.rgb.data(), job.width, job.height, job.path))
			LOG_INFO("snapshot saved: %s (%ux%u)", job.path.c_str(), job.width, job.height);
		else
			LOG_ERROR("failed to write snapshot %s", job.path.c_str());
	}
}

bool SnapshotWriter::writePng(const uint8_t *rgb, unsigned width, unsigned height,
			      const std::string &path)
{
	/* Write to a temporary name so readers never see a partial file. */
	const std::string temporary = path + ".part";
	FILE *file = std::fopen(temporary.c_str(), "wb");
	if (!file)
		return false;

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
	png_infop info = png ? png_create_info_struct(png) : nullptr;
	if (!png || !info || setjmp(png_jmpbuf(png))) {
		/* libpng reports errors by jumping back here. */
		png_destroy_write_struct(&png, &info);
		std::fclose(file);
		std::remove(temporary.c_str());
		return false;
	}

	png_init_io(png, file);
	png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_set_compression_level(png, 1); /* speed over size */
	png_write_info(png, info);
	for (unsigned y = 0; y < height; y++)
		png_write_row(png, const_cast<png_bytep>(rgb + size_t(y) * width * 3));
	png_write_end(png, nullptr);
	png_destroy_write_struct(&png, &info);

	const bool ok = std::fclose(file) == 0;
	return ok && std::rename(temporary.c_str(), path.c_str()) == 0;
}

} /* namespace isp */
