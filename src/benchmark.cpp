#include "benchmark.h"

#include "common/debugCounters.h"
#include "common/hostStats.h"
#include "common/stringUtils.h"
#include "common/systemInfo.h"
#include "emulator.h"
#include "graphics/host_gpu/graphicContext.h"
#include "kytyGitVersion.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <vector>
#include <magic_enum.hpp>
#include <nlohmann/json.hpp>

namespace Emulator::Benchmark {
namespace {

using Clock = std::chrono::steady_clock;
using Common::DebugCounters::Counter;
using Common::DebugCounters::Peak;
using Json = nlohmann::json;
constexpr size_t MaxIntervals = 262144;
constexpr auto MemorySampleInterval = std::chrono::milliseconds(500);

struct Session {
	std::filesystem::path output;
	Clock::time_point start;
	Clock::time_point deadline = Clock::time_point::max();
	Clock::time_point last_present {};
	Clock::time_point next_memory_sample {};
	std::optional<uint64_t> first_present_ns;
	std::optional<uint64_t> device_memory_peak;
	std::vector<uint64_t> intervals;
	uint64_t frames = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	bool extent_changed = false;
	Json metadata;
};

std::optional<Session> g_session;
std::mutex g_mutex;
std::atomic<bool> g_accepting {false};

uint64_t Ns(Clock::duration duration) noexcept {
	return static_cast<uint64_t>(
	    std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

Json OptionalValue(std::optional<uint64_t> value) {
	return value ? Json(*value) : Json(nullptr);
}

Json Percentiles(const std::vector<uint64_t>& intervals) {
	auto sorted = intervals;
	std::ranges::sort(sorted);
	const auto nearest_rank = [&](size_t percentile) -> Json {
		if (sorted.empty()) {
			return nullptr;
		}
		const auto rank = (sorted.size() * percentile + 99) / 100;
		return sorted[rank - 1];
	};
	return {{"p50", nearest_rank(50)}, {"p95", nearest_rank(95)}, {"p99", nearest_rank(99)}};
}

Json Counters() {
	using Common::DebugCounters::Get;
	return {
	    {"shaders_compiled", Get(Counter::ShadersCompiled)},
	    {"shader_compile_ns", Get(Counter::ShaderCompileNs)},
	    {"shader_compile_peak_ns", Get(Peak::ShaderCompileNs)},
	    {"shader_module_hits", Get(Counter::ShaderModuleHits)},
	    {"shader_module_misses", Get(Counter::ShaderModuleMisses)},
	    {"pipelines_created", Get(Counter::PipelinesCreated)},
	    {"pipeline_create_ns", Get(Counter::PipelineCreateNs)},
	    {"pipeline_create_peak_ns", Get(Peak::PipelineCreateNs)},
	    {"pipeline_hits", Get(Counter::PipelineHits)},
	    {"pipeline_misses", Get(Counter::PipelineMisses)},
	    {"pipelines_prewarmed", Get(Counter::PipelinesPrewarmed)},
	    {"pipeline_warmup_ns", Get(Counter::PipelineWarmupNs)},
	    {"driver_cache_bytes_loaded", Get(Counter::DriverCacheBytesLoaded)},
	    {"queue_submits", Get(Counter::QueueSubmits)},
	    {"gpu_waits", Get(Counter::GpuWaits)},
	    {"gpu_wait_ns", Get(Counter::GpuWaitNs)},
	    {"guest_file_read_bytes", Get(Counter::GuestFileReadBytes)},
	    {"dropped_presents", Get(Counter::DroppedPresents)},
	    {"flips_rejected", Get(Counter::FlipsRejected)},
	    {"late_vblanks", Get(Counter::LateVblanks)},
	    {"swapchain_recreations", Get(Counter::SwapchainRecreations)},
	    {"audio_underruns", Get(Counter::AudioUnderruns)},
	    {"emulated_instructions", Get(Counter::EmulatedInstructions)},
	    {"texture_upload_bytes", Get(Counter::TextureUploadBytes)},
	    {"texture_download_bytes", Get(Counter::TextureDownloadBytes)},
	    {"texture_evictions", Get(Counter::TextureEvictions)},
	    {"buffer_upload_bytes", Get(Counter::BufferUploadBytes)},
	    {"buffer_download_bytes", Get(Counter::BufferDownloadBytes)},
	    {"buffer_evictions", Get(Counter::BufferEvictions)},
	};
}

bool WriteReport(const std::filesystem::path& path, const std::string& report) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	auto* file = ::_wfopen(path.c_str(), L"wbx");
#else
	auto* file = std::fopen(path.c_str(), "wbx");
#endif
	if (file == nullptr) {
		std::fprintf(stderr, "Cannot create benchmark report: %s\n",
		             Common::PathToString(path).c_str());
		return false;
	}
	const auto written = std::fwrite(report.data(), 1, report.size(), file);
	const auto closed = std::fclose(file);
	if (written != report.size() || closed != 0) {
		std::fprintf(stderr, "Cannot finish writing benchmark report: %s\n",
		             Common::PathToString(path).c_str());
		return false;
	}
	return true;
}

} // namespace

void Start(const RunOptions& options) {
	if (options.benchmark_output.empty()) {
		return;
	}
	auto& session = g_session.emplace();
	session.output = options.benchmark_output;
	session.intervals.reserve(MaxIntervals);
	session.metadata = {
	    {"build", KYTY_BUILD_LABEL},
	    {"commit", KYTY_GIT_VERSION},
	    {"cpu", Common::GetSystemInfo().ProcessorName},
	    {"gpu", nullptr},
	    {"vulkan_driver_version", nullptr},
	    {"vulkan_vendor_id", nullptr},
	    {"vulkan_device_id", nullptr},
	    {"guest_elf", Common::PathToString(options.elf.filename())},
	    {"requested_window_width", options.config.screen_width},
	    {"requested_window_height", options.config.screen_height},
	    {"present_mode", magic_enum::enum_name(options.config.present_mode)},
	    {"vblank_hz", options.config.vblank_frequency},
	    {"persistent_shader_cache_enabled", options.config.shader_cache_enabled},
	    {"pipeline_warmup_limit", OptionalValue(options.config.pipeline_warmup_limit)},
	    {"cache_state", options.benchmark_cache_state.empty() ? "unspecified" :
	                                                              options.benchmark_cache_state},
	    {"duration_seconds_requested", options.benchmark_seconds},
	};
	session.start = Clock::now();
	if (options.benchmark_seconds != 0) {
		session.deadline = session.start + std::chrono::seconds(options.benchmark_seconds);
	}
	Common::DebugCounters::SetBenchmarkEnabled(true);
	g_accepting.store(true, std::memory_order_release);
}

bool DeadlineReached() noexcept {
	return g_session && g_session->deadline != Clock::time_point::max() &&
	       Clock::now() >= g_session->deadline;
}

int LimitWaitMs(int wait_ms) noexcept {
	if (!g_session || g_session->deadline == Clock::time_point::max()) {
		return wait_ms;
	}
	const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
	    g_session->deadline - Clock::now()).count();
	return static_cast<int>(std::clamp<int64_t>(remaining, 0, wait_ms));
}

void RecordPresent(const Libs::Graphics::GraphicContext& graphics, uint32_t width,
                   uint32_t height) {
	if (!g_accepting.load(std::memory_order_acquire)) {
		return;
	}
	const auto now = Clock::now();
	std::scoped_lock lock(g_mutex);
	auto& session = *g_session;
	if (!g_accepting.load(std::memory_order_relaxed) || now >= session.deadline) {
		return;
	}
	if (session.frames == 0) {
		const auto& device = graphics.GetPhysicalDeviceProperties();
		session.metadata["gpu"] = device.deviceName.data();
		session.metadata["vulkan_driver_version"] = device.driverVersion;
		session.metadata["vulkan_vendor_id"] = device.vendorID;
		session.metadata["vulkan_device_id"] = device.deviceID;
		session.first_present_ns = Ns(now - session.start);
		session.width = width;
		session.height = height;
	} else if (session.intervals.size() < MaxIntervals) {
		session.intervals.push_back(Ns(now - session.last_present));
	}
	session.extent_changed |= width != session.width || height != session.height;
	++session.frames;
	session.last_present = now;
	if (graphics.CanReportMemoryUsage() && now >= session.next_memory_sample) {
		session.device_memory_peak = std::max(session.device_memory_peak.value_or(0),
		                                      graphics.GetDeviceMemoryUsage());
		session.next_memory_sample = now + MemorySampleInterval;
	}
}

bool Finish() {
	if (!g_session) {
		return true;
	}
	bool expected = true;
	if (!g_accepting.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
		return true;
	}
	Common::DebugCounters::SetBenchmarkEnabled(false);
	const auto stop = Clock::now();
	std::scoped_lock lock(g_mutex);
	const auto& session = *g_session;
	const auto elapsed = Ns(std::min(session.deadline, stop) - session.start);
	const auto counters = Counters();
	Json report = {
	    {"schema_version", 1},
	    {"run", session.metadata},
	    {"elapsed_ns", elapsed},
	    {"ended_at_deadline", stop >= session.deadline},
	    {"first_present_ns", OptionalValue(session.first_present_ns)},
	    {"presented_guest_frames", session.frames},
	    {"first_guest_width", session.frames ? Json(session.width) : Json(nullptr)},
	    {"first_guest_height", session.frames ? Json(session.height) : Json(nullptr)},
	    {"guest_extent_changed", session.extent_changed},
	    {"frame_intervals_truncated", session.frames > session.intervals.size() + 1},
	    {"frame_percentiles_ns", Percentiles(session.intervals)},
	    {"frame_intervals_ns", session.intervals},
	    {"peak_process_resident_bytes", OptionalValue(Common::GetPeakResidentBytes())},
	    {"peak_sampled_device_memory_bytes", OptionalValue(session.device_memory_peak)},
	    {"guest_read_bytes_per_second",
	     elapsed ? Json(counters["guest_file_read_bytes"].get<double>() * 1e9 / double(elapsed))
	             : Json(nullptr)},
	    {"counters", counters},
	};
	return WriteReport(session.output, report.dump(2) + '\n');
}

} // namespace Emulator::Benchmark
