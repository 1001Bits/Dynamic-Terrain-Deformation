// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 NearMidnightNow (NMN).

#pragma once

#include <d3dcompiler.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ShaderCompiler
{
	struct Job
	{
		std::string hullSource, domainSource, error;
		ID3DBlob* hull{};
		ID3DBlob* domain{};
		double milliseconds{};
		std::atomic<bool> ready{ false };
		~Job()
		{
			if (hull) { hull->Release(); }
			if (domain) { domain->Release(); }
		}
	};

	inline void Compile(Job& job)
	{
		const auto start = std::chrono::steady_clock::now();
		const auto stage = [&](const std::string& source, const char* target, ID3DBlob** out) {
			ID3DBlob* errors{};
			const auto hr = D3DCompile(source.data(), source.size(), "NMN terrain",
				nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errors);
			if (FAILED(hr)) {
				job.error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
					errors->GetBufferSize()) : "D3DCompile failed without diagnostics";
			}
			if (errors) { errors->Release(); }
			return SUCCEEDED(hr);
		};
		try {
			if (stage(job.hullSource, "hs_5_0", &job.hull)) {
				stage(job.domainSource, "ds_5_0", &job.domain);
			}
		} catch (const std::exception& e) {
			job.error = e.what();
		}
		job.milliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - start).count();
		job.ready.store(true, std::memory_order_release);
	}

	class Worker
	{
		std::mutex lock;
		std::condition_variable wake;
		std::deque<std::shared_ptr<Job>> queue;
		bool stopping{};
		std::thread thread;
	public:
		Worker() : thread([this] {
			for (;;) {
				std::shared_ptr<Job> job;
				{
					std::unique_lock guard(lock);
					wake.wait(guard, [&] { return stopping || !queue.empty(); });
					if (stopping) { return; }
					job = std::move(queue.front());
					queue.pop_front();
				}
				Compile(*job);
			}
		}) {}
		~Worker()
		{
			{
				std::scoped_lock guard(lock);
				stopping = true;
				queue.clear();
			}
			wake.notify_one();
			thread.join();
		}
		void Submit(const std::shared_ptr<Job>& job)
		{
			{
				std::scoped_lock guard(lock);
				queue.push_back(job);
			}
			wake.notify_one();
		}
		void CancelQueued()
		{
			std::scoped_lock guard(lock);
			queue.clear();
		}
	};

	inline Worker& GetWorker()
	{
		static Worker worker;
		return worker;
	}
}
