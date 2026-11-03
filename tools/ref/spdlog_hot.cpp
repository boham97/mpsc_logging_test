// Quill 의 hot_path_bench.h 하네스로 spdlog 비동기 로거를 잰다 (logger_benchmarks 의 spdlog 항목과 같은 역할).
// 큐가 넘치면 기다린다(block). 로깅 스레드는 CPU 5 에 고정 (spdlog 는 바쁜 대기 옵션이 없어 조건 변수로 잠든다).
#include "hot_path_bench.h"

#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/spdlog.h"

#include <cstring>
#include <pthread.h>
#include <string>

int main(int argc, char** argv)
{
  bool const str = argc > 1 && std::strcmp(argv[1], "string") == 0;
  size_t const iterations = argc > 2 ? std::stoul(argv[2]) : ITERATIONS;

  spdlog::init_thread_pool(131072, 1,
                           []()
                           {
                             cpu_set_t set;
                             CPU_ZERO(&set);
                             CPU_SET(5, &set);
                             pthread_setaffinity_np(pthread_self(), sizeof set, &set);
                           });
  auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("bench_logger", "spdlog_hot_path.log", true);
  logger->set_pattern("%T.%F [%t] %s:%# %l %v");
  logger->set_level(spdlog::level::info);
  std::this_thread::sleep_for(std::chrono::seconds(1));

  auto on_start = [&logger]()
  {
    SPDLOG_LOGGER_INFO(logger, "preallocate");
    logger->flush();
  };
  // spdlog 의 비동기 flush 는 요청만 넣고 바로 돌아오므로, Quill 의 blocking flush 처럼 큐가 빌 때까지 기다린다
  auto on_exit = [&logger]()
  {
    logger->flush();
    while (spdlog::thread_pool()->queue_size() != 0)
      std::this_thread::yield();
  };

  std::string const large{"Lorem ipsum dolor sit amet, consectetur adipiscing elit"};
  auto log_numbers = [&logger](uint64_t k, uint64_t i, double d)
  { SPDLOG_LOGGER_INFO(logger, "Logging iteration: {}, message: {}, double: {}", k, i, d); };
  auto log_string = [&logger, &large](uint64_t k, uint64_t i, double)
  { SPDLOG_LOGGER_INFO(logger, "Logging int: {}, int: {}, string: {}", k, i, large); };

  for (auto tc : THREAD_LIST_COUNT)
  {
    if (str)
      run_benchmark("Logger: spdlog async - Benchmark: Hot Path Latency (large string) / Nanoseconds", tc,
                    iterations, MESSAGES_PER_ITERATION, on_start, log_string, on_exit);
    else
      run_benchmark("Logger: spdlog async - Benchmark: Hot Path Latency (numbers) / Nanoseconds", tc,
                    iterations, MESSAGES_PER_ITERATION, on_start, log_numbers, on_exit);
  }
  spdlog::shutdown();
}
