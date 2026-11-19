#ifndef KYTY_BENCHMARK_H_
#define KYTY_BENCHMARK_H_

#include <cstdint>

namespace Libs::Graphics {
struct GraphicContext;
}

namespace Emulator {
struct RunOptions;

namespace Benchmark {

void Start(const RunOptions& options);
[[nodiscard]] bool Finish();

[[nodiscard]] bool DeadlineReached() noexcept;
[[nodiscard]] int LimitWaitMs(int wait_ms) noexcept;
void RecordPresent(const Libs::Graphics::GraphicContext& graphics, uint32_t width,
                   uint32_t height);

} // namespace Benchmark
} // namespace Emulator

#endif // KYTY_BENCHMARK_H_
