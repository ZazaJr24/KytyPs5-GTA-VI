#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include "common/debugCounters.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/parallelFor.h"
#include "common/stringUtils.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ir/Serialize.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <fmt/format.h>
#include <ranges>
#include <xxhash.h>
#include <zstd.h>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// The shader store: a header, then zstd-compressed records of everything a run created first.
// Records are only appended, so a store cut short by a crash keeps every complete record.
namespace Libs::Graphics {

namespace {

using ShaderRecompiler::IR::IoAll;
using ShaderRecompiler::IR::Reader;
using ShaderRecompiler::IR::Writer;

enum class Record : uint8_t { Module, GraphicsPipeline, TessellationLayout };

constexpr uint32_t MaxRecordBytes = 32 * 1024 * 1024;

// Precedes each record's zstd frame.
struct Frame {
	uint32_t compressed_size = 0;
	uint32_t raw_size        = 0;
	uint64_t checksum        = 0; // XXH3 of the compressed bytes.
};

// Changes with every build, so records never meet code that encodes them differently. Zero when
// the executable cannot be found.
uint64_t BuildStamp() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	std::array<wchar_t, 32768> name {};
	GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
	const std::filesystem::path path = name.data();
#elif defined(__APPLE__)
	std::array<char, 4096> name {};
	auto size = static_cast<uint32_t>(name.size());
	_NSGetExecutablePath(name.data(), &size);
	const std::filesystem::path path = name.data();
#else
	const std::filesystem::path path = "/proc/self/exe";
#endif
	std::error_code error;
	const auto      time = std::filesystem::last_write_time(path, error).time_since_epoch().count();
	return error ? 0 : static_cast<uint64_t>(time);
}

// What every record depends on besides its own fields; empty when the build is unknown.
std::vector<uint8_t> StoreHeader(const GraphicContext& graphics) {
	const auto stamp = BuildStamp();
	if (stamp == 0) {
		return {};
	}
	Writer header;
	IoAll(header, stamp, SpirvProfile(graphics),
	      graphics.GetPhysicalDeviceProperties().limits.maxComputeSharedMemorySize);
	return std::move(header.bytes);
}

// Record fields, shared by writing and reading.
template <class S, class Key> void IoKey(S& io, Key& key) {
	auto& [stage, hash, user_data_count, static_state] = key;
	IoAll(io, stage, hash, user_data_count, static_state);
}

template <class S, class Module> void IoModule(S& io, Module& stage) {
	// The handle comes from the SPIR-V; stored programs have no function code.
	auto& [id, handle, info, specialization, function_code, vertex_formats, rect_parameters,
	       mesh_task] = stage;
	IoAll(io, id, info, specialization, vertex_formats, rect_parameters, mesh_task);
}

} // namespace

void ShaderStoreWriter::Open(const std::filesystem::path& path, std::span<const uint8_t> header,
                             uint64_t kept) {
	Common::File::CreateDirectories(path.parent_path());
	if (kept == 0) {
		if (m_file.Create(path)) {
			m_file.Write(header.data(), static_cast<uint32_t>(header.size()));
		}
	} else if (m_file.Open(path, Common::File::Mode::ReadWrite)) {
		m_file.Truncate(kept);
		m_file.Seek(kept);
	}
}

ShaderStoreWriter::ShaderStoreWriter() {
	m_thread = std::jthread([this](std::stop_token stop) {
		std::unique_lock lock(m_mutex);
		// Once stop is requested the wait returns false only when the queue is empty.
		while (m_ready.wait(lock, stop, [this] { return !m_jobs.empty(); })) {
			const auto job = std::move(m_jobs.front());
			m_jobs.pop();
			lock.unlock();
			job();
			lock.lock();
		}
	});
}

void ShaderStoreWriter::Append(std::vector<uint8_t>&& record) {
	if (record.size() > MaxRecordBytes) {
		return;
	}
	Post([this, record = std::move(record)] {
		std::vector<uint8_t> bytes(sizeof(Frame) + ZSTD_compressBound(record.size()));
		const auto size = ZSTD_compress(bytes.data() + sizeof(Frame), bytes.size() - sizeof(Frame),
		                                record.data(), record.size(), 3);
		EXIT_IF(ZSTD_isError(size));
		const Frame frame {.compressed_size = static_cast<uint32_t>(size),
		                   .raw_size        = static_cast<uint32_t>(record.size()),
		                   .checksum        = XXH3_64bits(bytes.data() + sizeof(Frame), size)};
		std::memcpy(bytes.data(), &frame, sizeof(frame));
		// A failed write leaves a partial record; the next run drops it and everything after it.
		m_file.Write(bytes.data(), static_cast<uint32_t>(sizeof(Frame) + size));
	});
}

void ShaderStoreWriter::Post(Common::UniqueFunction<void>&& job) {
	{
		std::scoped_lock lock(m_mutex);
		m_jobs.push(std::move(job));
	}
	m_ready.notify_one();
}

void PipelineCache::WarmUp(const std::filesystem::path& path) {
	if (path.empty()) {
		return;
	}
	const auto header = StoreHeader(m_graphics);
	if (header.empty()) {
		return;
	}
	using Common::DebugCounters::Add;
	using Common::DebugCounters::Counter;
	Common::DebugCounters::ScopedTimer timer(Counter::PipelineWarmupNs);
	const auto start = std::chrono::steady_clock::now();
	std::error_code size_error;
	const auto size = std::filesystem::file_size(path, size_error);
	const auto warmup_limit = Config::GetPipelineWarmupLimit();
	const auto workers = warmup_limit ? std::min(size_t {4}, Common::ParallelThreads())
	                                  : Common::ParallelThreads();
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: compiling shaders and pipelines from {} ({:.1f} MB) with up to {} threads\n",
	    Common::PathToString(path), size_error ? 0.0 : static_cast<double>(size) / 1'000'000.0,
	    workers));
	std::vector<WarmupEntry> warmup_entries;
	std::ifstream input(path, std::ios::binary);
	uint64_t kept = 0;
	if (input) {
		std::vector<uint8_t> existing_header(header.size());
		const auto read = [&](auto& buffer) -> bool {
			return static_cast<bool>(input.read(reinterpret_cast<char*>(buffer.data()),
			                                    static_cast<std::streamsize>(buffer.size())));
		};
		if (read(existing_header) && existing_header == header) {
			kept = header.size();
			std::vector<uint8_t> compressed;
			std::vector<uint8_t> raw;
			Frame frame;
			while (input.read(reinterpret_cast<char*>(&frame), sizeof(frame))) {
				if (frame.raw_size == 0 || frame.raw_size > MaxRecordBytes ||
				    frame.compressed_size == 0 ||
				    frame.compressed_size > ZSTD_compressBound(frame.raw_size)) {
					break;
				}
				compressed.resize(frame.compressed_size);
				if (!read(compressed) ||
				    XXH3_64bits(compressed.data(), compressed.size()) != frame.checksum ||
				    ZSTD_getFrameContentSize(compressed.data(), compressed.size()) != frame.raw_size) {
					break;
				}
				raw.resize(frame.raw_size);
				if (ZSTD_decompress(raw.data(), raw.size(), compressed.data(), compressed.size()) !=
				        raw.size() ||
				    !LoadRecord(raw, warmup_entries)) {
					break;
				}
				kept += sizeof(frame) + compressed.size();
			}
		}
		input.close();
	} else if (size_error != std::errc::no_such_file_or_directory) {
		// A failed open can be transient. Leave the existing store alone.
		return;
	}
	// A damaged record ends the store; the records before it stay.
	m_store.Open(path, header, kept);

	const auto modules = MapModules();
	std::vector<Common::UniqueFunction<void>> jobs;
	const auto job_limit = warmup_limit ? std::min<size_t>(*warmup_limit, warmup_entries.size())
	                                    : warmup_entries.size();
	jobs.reserve(job_limit);
	for (const auto& entry: warmup_entries) {
		if (jobs.size() == job_limit) {
			break;
		}
		if (const auto* graphics = std::get_if<GraphicsPipelineMap::value_type*>(&entry)) {
			auto* node = *graphics;
			jobs.emplace_back([this, &modules, node] {
				if (CreateGraphicsPipeline(node->second, node->first, modules)) {
					Add(Counter::PipelinesPrewarmed);
				}
			});
		} else {
			const auto* module = modules.at(std::get<uint64_t>(entry));
			auto [iter, inserted] = m_compute_pipelines.try_emplace(module->id);
			if (inserted) {
				jobs.emplace_back([this, module, &pipeline = iter->second] {
					CreatePipelineInternal(m_graphics, pipeline, *module, m_driver_cache);
					Add(Counter::PipelinesPrewarmed);
				});
			}
		}
	}
	Common::ParallelFor(jobs.size(), [&](size_t i) { jobs[i](); }, workers);
	const auto graphics_ready = std::ranges::count_if(
	    m_graphics_pipelines, [](const auto& entry) { return entry.second.pipeline != nullptr; });
	if (!warmup_limit) {
		std::erase_if(m_graphics_pipelines,
		              [](const auto& entry) { return entry.second.pipeline == nullptr; });
	}
	if (graphics_ready != 0 || !m_compute_pipelines.empty()) {
		CheckpointDriverCache();
	}
	Log::WriteToConsoleAndLog(fmt::format(
	    "Shader cache: loaded {} programs, {} modules, {} pipelines in {} ms\n",
	    m_program_cache.size(), modules.size() - 1,
	    static_cast<size_t>(graphics_ready) + m_compute_pipelines.size(),
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
	                                                          start)
	        .count()));
}

bool PipelineCache::LoadRecord(std::span<const uint8_t> bytes,
                               std::vector<WarmupEntry>& warmup_entries) {
	Reader     reader {bytes};
	auto       type     = Record::Module;
	const auto complete = [&] { return reader.ok && reader.bytes.empty(); };
	IoAll(reader, type);
	switch (type) {
		case Record::Module: {
			ProgramKey            key;
			std::vector<uint8_t>  plan;
			ShaderModule          module;
			std::vector<uint32_t> spirv;
			IoKey(reader, key);
			IoAll(reader, plan);
			IoModule(reader, module);
			IoAll(reader, spirv);
			if (!complete()) {
				return false;
			}
			const auto [program, inserted] = m_program_cache.try_emplace(std::move(key));
			if (inserted &&
			    !ShaderRecompiler::IR::LoadResourcePlan(plan, program->second.resource_plan)) {
				m_program_cache.erase(program);
				return false;
			}
			module.module = CompileSPV(spirv, m_graphics.device);
			if (module.info.stage == ShaderType::Compute) {
				warmup_entries.emplace_back(module.id);
			}
			program->second.modules.push_back(std::move(module));
			break;
		}
		case Record::GraphicsPipeline: {
			GraphicsPipelineKey key;
			reader.Raw(&key, sizeof(key));
			if (!complete()) {
				return false;
			}
			auto [entry, inserted] = m_graphics_pipelines.try_emplace(key);
			if (inserted) {
				warmup_entries.emplace_back(&*entry);
			}
			break;
		}
		case Record::TessellationLayout: {
			uint64_t                    key = 0;
			ShaderTessellationInputInfo layout;
			IoAll(reader, key, layout);
			if (!complete()) {
				return false;
			}
			m_tess_layouts.try_emplace(key, layout);
			break;
		}
		default: return false;
	}
	return true;
}

// Records are encoded here, since programs and modules change later.
void PipelineCache::StoreModule(const ProgramKey& key, const Program& program,
                                const ShaderModule& module, const std::vector<uint32_t>& spirv) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::Module);
	IoKey(record, key);
	IoAll(record, ShaderRecompiler::IR::SaveResourcePlan(program.resource_plan));
	IoModule(record, module);
	IoAll(record, spirv);
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StorePipeline(const GraphicsPipelineKey& key) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::GraphicsPipeline);
	record.Raw(&key, sizeof(key));
	m_store.Append(std::move(record.bytes));
}

void PipelineCache::StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout) {
	if (!m_store) {
		return;
	}
	Writer record;
	IoAll(record, Record::TessellationLayout, key, layout);
	m_store.Append(std::move(record.bytes));
}

PipelineCache::ModuleMap PipelineCache::MapModules() const {
	ModuleMap modules {{0, nullptr}};
	for (const auto& program: m_program_cache | std::views::values) {
		for (const auto& module: program.modules) {
			modules.emplace(module.id, &module);
		}
	}
	return modules;
}

bool PipelineCache::CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
                                           const ModuleMap& modules) {
	std::array<const ShaderModule*, 4> stages {};
	for (size_t i = 0; i < stages.size(); i++) {
		const auto found = modules.find(key.stage_ids[i]);
		if (found == modules.end()) {
			return false;
		}
		stages[i] = found->second;
	}
	CreatePipelineInternal(m_graphics, pipeline, key, stages, m_driver_cache);
	return true;
}

} // namespace Libs::Graphics
