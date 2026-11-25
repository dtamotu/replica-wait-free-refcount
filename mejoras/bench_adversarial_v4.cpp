// ============================================================================
// Banco adversarial para evaluar §5.1 de Euro-Par 2024:
//   23 hilos lectores bombardean load() en bucle cerrado sobre 1 solo puntero
//   compartido mientras 1 hilo escritor intenta completar compare_and_set()
//   (o compare_exchange_strong en Anderson/Folly) repetidamente.
//
// Mide para Autores (orig.), Autores-v2, Autores-v4, Anderson y Folly:
//   - Latencia del escritor CAS: p50, p99, p99.95, máxima (ns)
//   - Rendimiento del escritor (Kops/s) y rendimiento agregado de lectores (Mop/s)
//   - Reintentos máximos (en Autores-v4)
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../replica/completa/artefacto/concurrent_deferred_rc/benchmarks/external/wait-free-atomic-arc/cpp/atomic_shared_ptr.h"
#include "../replica/completa/artefacto/concurrent_deferred_rc/benchmarks/external/atomic_shared_ptr/include/parlay/atomic_shared_ptr_custom.hpp"
#include <folly/concurrency/AtomicSharedPtr.h>

#include "atomic_shared_ptr_v2.h"
#include "atomic_shared_ptr_v4.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Metrics {
  double p50_ns;
  double p99_ns;
  double p9995_ns;
  double max_ns;
  double writer_kops;
  double reader_mops;
  uint64_t cas_completed;
};

double pct(const std::vector<uint64_t>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
  return static_cast<double>(sorted[idx]);
}

template <typename FnInit, typename FnRead, typename FnCas>
Metrics run_adversarial(int num_readers, double duration_sec, FnInit init_fn, FnRead read_fn,
                        FnCas cas_fn) {
  init_fn();
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::atomic<int> ready{0};
  std::atomic<uint64_t> total_reads{0};

  std::vector<std::thread> readers;
  readers.reserve(num_readers);
  for (int r = 0; r < num_readers; ++r) {
    readers.emplace_back([&]() {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {}
      uint64_t lreads = 0;
      uint64_t sink = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        sink += read_fn();
        ++lreads;
      }
      if (sink == 0xFFFFFFFFFFFFFFFFULL) std::puts("never");
      total_reads.fetch_add(lreads, std::memory_order_relaxed);
    });
  }

  while (ready.load(std::memory_order_acquire) < num_readers) {}
  std::vector<uint64_t> lats;
  lats.reserve(200000);

  auto t0 = Clock::now();
  start.store(true, std::memory_order_release);
  auto deadline = t0 + std::chrono::duration<double>(duration_sec);

  uint64_t step = 1;
  while (Clock::now() < deadline) {
    auto a = Clock::now();
    bool ok = cas_fn(static_cast<int>(step));
    auto b = Clock::now();
    if (!ok) {
      std::fprintf(stderr, "ERROR: CAS único escritor devolvió false inesperadamente!\n");
      std::abort();
    }
    lats.push_back(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    ++step;
  }

  auto t1 = Clock::now();
  stop.store(true, std::memory_order_release);
  for (auto& th : readers) th.join();

  double secs = std::chrono::duration<double>(t1 - t0).count();
  std::sort(lats.begin(), lats.end());
  return Metrics{
      pct(lats, 0.50),
      pct(lats, 0.99),
      pct(lats, 0.9995),
      lats.empty() ? 0.0 : static_cast<double>(lats.back()),
      static_cast<double>(lats.size()) / secs / 1e3,
      static_cast<double>(total_reads.load()) / secs / 1e6,
      static_cast<uint64_t>(lats.size()),
  };
}

}  // namespace

int main(int argc, char** argv) {
  int num_readers = 23;
  double duration = 2.0;
  std::string target = "all";
  if (argc >= 2) target = argv[1];
  if (argc >= 3) num_readers = std::atoi(argv[2]);
  if (argc >= 4) duration = std::atof(argv[3]);

  auto report = [&](const char* name, const Metrics& m) {
    std::printf("%s,%d,%.2f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%llu\n",
                name, num_readers, duration, m.p50_ns, m.p99_ns, m.p9995_ns, m.max_ns,
                m.writer_kops, m.reader_mops, (unsigned long long)m.cas_completed);
    std::fflush(stdout);
  };

  if (target == "header") {
    std::puts("impl,lectores,segundos,p50_ns,p99_ns,p9995_ns,max_ns,writer_kops,reader_mops,cas_completados");
    return 0;
  }

  if (target == "all" || target == "ivo") {
    auto cur = ivo::shared_ptr<int>::make_shared(0);
    ivo::atomic_shared_ptr<int> ap(cur);
    auto m = run_adversarial(
        num_readers, duration, [&]() {},
        [&]() -> uint64_t {
          auto s = ap.load();
          return s ? static_cast<uint64_t>(*s) : 0ULL;
        },
        [&](int v) -> bool {
          auto nw = ivo::shared_ptr<int>::make_shared(v);
          bool ok = ap.compare_and_set(cur, nw);
          if (ok) cur = std::move(nw);
          return ok;
        });
    report("Autores", m);
  }

  if (target == "all" || target == "ivo-v2") {
    auto cur = ivo_v2::shared_ptr<int>::make_shared(0);
    ivo_v2::atomic_shared_ptr<int, false> ap(cur);
    auto m = run_adversarial(
        num_readers, duration, [&]() {},
        [&]() -> uint64_t {
          auto s = ap.load();
          return s ? static_cast<uint64_t>(*s) : 0ULL;
        },
        [&](int v) -> bool {
          auto nw = ivo_v2::shared_ptr<int>::make_shared(v);
          bool ok = ap.compare_and_set(cur, nw);
          if (ok) cur = std::move(nw);
          return ok;
        });
    report("Autores-v2", m);
  }

  if (target == "all" || target == "ivo-v4") {
    auto cur = ivo_v4::shared_ptr<int>::make_shared(0);
    ivo_v4::atomic_shared_ptr<int> ap(cur);
    auto m = run_adversarial(
        num_readers, duration, [&]() {},
        [&]() -> uint64_t {
          auto s = ap.load();
          return s ? static_cast<uint64_t>(*s) : 0ULL;
        },
        [&](int v) -> bool {
          auto nw = ivo_v4::shared_ptr<int>::make_shared(v);
          bool ok = ap.compare_and_set(cur, nw);
          if (ok) cur = std::move(nw);
          return ok;
        });
    ivo_v4::drain_all_retired();
    report("Autores-v4", m);
  }

  if (target == "all" || target == "mine") {
    auto cur = parlay::make_shared<int>(0);
    parlay::atomic_shared_ptr<int> ap(cur);
    auto m = run_adversarial(
        num_readers, duration, [&]() {},
        [&]() -> uint64_t {
          auto s = ap.load();
          return s ? static_cast<uint64_t>(*s) : 0ULL;
        },
        [&](int v) -> bool {
          auto nw = parlay::make_shared<int>(v);
          auto exp = cur;
          bool ok = ap.compare_exchange_strong(exp, nw);
          if (ok) cur = std::move(nw);
          return ok;
        });
    report("Anderson", m);
  }

  if (target == "all" || target == "folly") {
    auto cur = std::make_shared<int>(0);
    folly::atomic_shared_ptr<int> ap(cur);
    auto m = run_adversarial(
        num_readers, duration, [&]() {},
        [&]() -> uint64_t {
          auto s = ap.load();
          return s ? static_cast<uint64_t>(*s) : 0ULL;
        },
        [&](int v) -> bool {
          auto nw = std::make_shared<int>(v);
          auto exp = cur;
          bool ok = ap.compare_exchange_strong(exp, nw);
          if (ok) cur = std::move(nw);
          return ok;
        });
    report("Folly", m);
  }

  return 0;
}
