// Quill 원본 benchmarks/hot_path_latency/quill_hot_path_rdtsc_clock.cpp 와 같은 설정.
// 달라진 점: argv[1] 로 메시지 종류를 고른다 (numbers: 원본 그대로, string: README 의 large string).
#include "hot_path_bench.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/sinks/FileSink.h"

#include <cstring>
#include <string>

struct FrontendOptions : quill::FrontendOptions
{
};
using Frontend = quill::FrontendImpl<FrontendOptions>;
using Logger = quill::LoggerImpl<FrontendOptions>;

int main(int argc, char** argv)
{
  bool const str = argc > 1 && std::strcmp(argv[1], "string") == 0;
  size_t const iterations = argc > 2 ? std::stoul(argv[2]) : ITERATIONS;

  quill::BackendOptions backend_options;
  backend_options.cpu_affinity = {5};
  backend_options.sleep_duration = std::chrono::nanoseconds{0};
  quill::Backend::start(backend_options);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  auto file_sink = Frontend::create_or_get_sink<quill::FileSink>(
    "quill_hot_path_rdtsc_clock.log",
    []()
    {
      quill::FileSinkConfig cfg;
      cfg.set_open_mode('w');
      return cfg;
    }(),
    quill::FileEventNotifier{});

  Logger* logger = Frontend::create_or_get_logger(
    "bench_logger", std::move(file_sink),
    quill::PatternFormatterOptions{
      "%(time) [%(thread_id)] %(short_source_location) %(log_level) %(message)", "%H:%M:%S.%Qns",
      quill::Timezone::LocalTime, false});
  logger->set_log_level(quill::LogLevel::Info);

  auto on_start = [logger]()
  {
    LOG_INFO(logger, "preallocate");
    logger->flush_log(0);
  };
  auto on_exit = [logger]() { logger->flush_log(); };

  std::string const large{"Lorem ipsum dolor sit amet, consectetur adipiscing elit"};
  auto log_numbers = [logger](uint64_t k, uint64_t i, double d)
  { LOG_INFO(logger, "Logging iteration: {}, message: {}, double: {}", k, i, d); };
  auto log_string = [logger, &large](uint64_t k, uint64_t i, double)
  { LOG_INFO(logger, "Logging int: {}, int: {}, string: {}", k, i, large); };

  for (auto tc : THREAD_LIST_COUNT)
  {
    if (str)
      run_benchmark("Logger: Quill - Benchmark: Hot Path Latency (large string) / Nanoseconds", tc,
                    iterations, MESSAGES_PER_ITERATION, on_start, log_string, on_exit);
    else
      run_benchmark("Logger: Quill - Benchmark: Hot Path Latency (numbers) / Nanoseconds", tc,
                    iterations, MESSAGES_PER_ITERATION, on_start, log_numbers, on_exit);
  }
}
