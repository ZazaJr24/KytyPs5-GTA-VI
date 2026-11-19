#ifndef EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_
#define EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_

#include "common/additionalContent.h"
#include "common/emulatorConfig.h"
#include "common/stringUtils.h"

#include <cstdint>
#include <filesystem>

namespace Emulator {

struct RunOptions {
	Config::ConfigOptions                config;
	std::filesystem::path                app0_dir;
	std::filesystem::path                elf;
	std::filesystem::path                game_patch;
	std::filesystem::path                benchmark_output;
	std::string                          benchmark_cache_state;
	uint32_t                             benchmark_seconds = 0;
	bool                                 archive_read_ahead = false;
	std::filesystem::path                addcont_dir;
	std::vector<Common::AddContentEntry> addcont;
};

void Run(const RunOptions& options);
int LoadExec(const char* path, const char* const argv[]);

} // namespace Emulator

#endif /* EMULATOR_INCLUDE_EMULATOR_EMULATOR_H_ */
