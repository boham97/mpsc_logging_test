// Quill 의 benchmarks/backend_throughput 과 같은 조건으로 spdlog 비동기 로거를 잰다.
// 호출 스레드 CPU 1, 로깅 스레드 CPU 5, 400만 줄을 넣고 flush 가 끝날 때까지.
#include "spdlog/async.h"
#include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include <cstdio>
#include <pthread.h>

static void pin(int cpu)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}

int main()
{
  constexpr size_t total_iterations = 4'000'000;
  pin(1);
  spdlog::init_thread_pool(131072, 1, []() { pin(5); });
  auto logger = spdlog::create_async<spdlog::sinks::basic_file_sink_mt>("bench_logger", "spdlog_throughput.log", true);
  logger->set_pattern("%T.%F [%t] %s:%# %l %v");
  SPDLOG_LOGGER_INFO(logger, "preallocate");
  logger->flush();

  auto const start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < total_iterations; ++i)
    SPDLOG_LOGGER_INFO(logger, "Iteration: {} int: {} double: {}", i, i * 2, static_cast<double>(i) / 2);
  // 비동기 flush 는 요청만 넣고 돌아오므로, 로거와 스레드 풀을 해제해 로깅 스레드가 큐를 다 비우고
  // 파일을 닫을 때(join)까지 잰다
  logger.reset();
  spdlog::shutdown();
  auto const sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("Throughput is %.2f million msgs/sec average, total time elapsed: %.0f ms for %zu log messages\n",
              total_iterations / sec / 1e6, sec * 1e3, total_iterations);
}
