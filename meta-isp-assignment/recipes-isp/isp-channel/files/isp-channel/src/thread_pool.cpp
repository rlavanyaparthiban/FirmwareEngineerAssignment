/* SPDX-License-Identifier: MIT */
#include "thread_pool.h"

namespace isp {

ThreadPool::ThreadPool(unsigned threads)
{
	if (threads == 0)
		threads = std::thread::hardware_concurrency();
	size_ = threads ? threads : 1;

	/* The calling thread takes part in every job, so spawn size_ - 1. */
	for (unsigned i = 1; i < size_; i++)
		workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		exit_ = true;
	}
	wake_.notify_all();
	for (std::thread &t : workers_)
		t.join();
}

void ThreadPool::parallelFor(unsigned count, const std::function<void(unsigned)> &fn)
{
	if (count == 0)
		return;

	std::unique_lock<std::mutex> lock(mutex_);
	job_ = &fn;
	next_ = 0;
	count_ = count;
	pending_ = count;
	wake_.notify_all();

	/* Work alongside the pool instead of just sleeping. */
	while (next_ < count_) {
		unsigned index = next_++;
		lock.unlock();
		fn(index);
		lock.lock();
		pending_--;
	}

	done_.wait(lock, [this] { return pending_ == 0; });
	job_ = nullptr;
}

void ThreadPool::workerLoop()
{
	std::unique_lock<std::mutex> lock(mutex_);
	for (;;) {
		wake_.wait(lock, [this] { return exit_ || (job_ && next_ < count_); });
		if (exit_)
			return;

		unsigned index = next_++;
		const std::function<void(unsigned)> *job = job_;
		lock.unlock();
		(*job)(index);
		lock.lock();

		if (--pending_ == 0)
			done_.notify_one();
	}
}

} /* namespace isp */
