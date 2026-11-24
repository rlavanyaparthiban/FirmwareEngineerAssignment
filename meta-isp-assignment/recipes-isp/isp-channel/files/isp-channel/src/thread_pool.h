/* SPDX-License-Identifier: MIT */
/*
 * Fixed-size worker pool with a single operation: run a function over
 * [0, count) and wait. The ISP uses it to process horizontal bands of a frame
 * in parallel.
 */
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace isp {

class ThreadPool
{
public:
	/* threads == 0 selects the number of online CPUs. */
	explicit ThreadPool(unsigned threads = 0);
	~ThreadPool();

	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;

	unsigned size() const { return size_; }

	/* Calls fn(i) for every i in [0, count), then returns. */
	void parallelFor(unsigned count, const std::function<void(unsigned)> &fn);

private:
	void workerLoop();

	unsigned size_;
	std::vector<std::thread> workers_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::condition_variable done_;
	const std::function<void(unsigned)> *job_ = nullptr;
	unsigned next_ = 0;
	unsigned count_ = 0;
	unsigned pending_ = 0;
	bool exit_ = false;
};

} /* namespace isp */
