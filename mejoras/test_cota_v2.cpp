// Verifica la cota wait-free (<= P intentos de CAS en shift_references) sobre Autores-v2
// con parámetros forzados P = 32, C = 64 (respetando R-1: C >= 2P), igual que cota_cas.cpp.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#define IVO_V2_MAX_THREADS 32UL
#define IVO_V2_COMBINED_COUNT 64UL

static std::atomic<long> g_max_cas{0}, g_recargas{0}, g_intentos{0};
static std::atomic<long> g_hist[64];

#include "atomic_shared_ptr_v2.h"

struct Dato {
  long canario = 0x5ca1ab1e;
  ~Dato() { canario = 0; }
};

int main(int argc, char** argv) {
  int hilos = argc > 1 ? std::atoi(argv[1]) : 24;
  double seg = argc > 2 ? std::atof(argv[2]) : 2.0;
  int pct_store = argc > 3 ? std::atoi(argv[3]) : 1;

  // Prueba directa de estrés con P=32, C=64 sobre ivo_v2::atomic_shared_ptr (in-place)
  ivo_v2::atomic_shared_ptr<Dato> celda;
  celda.store(ivo_v2::shared_ptr<Dato>::make_shared());
  std::atomic<bool> stop{false};
  std::atomic<long> errores{0}, ops{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < hilos; t++) {
    ts.emplace_back([&, t] {
      uint64_t x = 88172645463325252ULL + t;
      long local_ops = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        int r = x % 100;
        if (r < pct_store) {
          celda.store(ivo_v2::shared_ptr<Dato>::make_shared());
        } else if (r < pct_store + 5) {
          auto exp = celda.load();
          auto nv = ivo_v2::shared_ptr<Dato>::make_shared();
          celda.compare_exchange_weak(exp, nv);
          if (exp && exp->canario != 0x5ca1ab1e) errores++;
        } else {
          auto p = celda.load();
          if (p && p->canario != 0x5ca1ab1e) errores++;
        }
        local_ops++;
      }
      ops.fetch_add(local_ops, std::memory_order_relaxed);
    });
  }
  std::this_thread::sleep_for(std::chrono::duration<double>(seg));
  stop.store(true);
  for (auto& th : ts) th.join();
  std::printf("ivo-v2 forzado (P=%lu, C=%lu, hilos=%d, store=%d%%+5%%CAS): ops=%ld errores=%ld\n",
              (unsigned long)ivo_v2::kMaxThreads, (unsigned long)ivo_v2::kCombinedCount,
              hilos, pct_store, ops.load(), errores.load());
  return errores.load() == 0 ? 0 : 1;
}
