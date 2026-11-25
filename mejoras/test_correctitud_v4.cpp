// ============================================================================
// Pruebas de correctitud, liberación inmediata, sanitizers (ASan+UBSan y TSan)
// y ataque adversarial (lectores continuos vs escritor CAS) para Autores-v4.
// ============================================================================

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "atomic_shared_ptr_v4.h"

namespace {

struct Payload {
  static inline std::atomic<int64_t> alive{0};
  int64_t value;
  explicit Payload(int64_t v = 0) : value(v) {
    alive.fetch_add(1, std::memory_order_relaxed);
  }
  ~Payload() {
    alive.fetch_sub(1, std::memory_order_relaxed);
  }
};

void test_unit_and_leak_free() {
  Payload::alive.store(0);
  {
    auto p1 = ivo_v4::shared_ptr<Payload>::make_shared(42);
    auto p2 = ivo_v4::shared_ptr<Payload>::make_shared(99);
    ivo_v4::atomic_shared_ptr<Payload> ap(p1);
    assert(ap.load()->value == 42);
    assert(ap.compare_and_set(p1, p2));
    assert(ap.load()->value == 99);
    assert(!ap.compare_and_set(p1, p2));
    auto exp = p1;
    assert(!ap.compare_exchange_weak(exp, p2));
    assert(exp->value == 99);
    ap.store(nullptr);
    assert(!ap.load());
  }
  ivo_v4::drain_all_retired();
  assert(Payload::alive.load() == 0);
  std::puts("[PASS] Pruebas unitarias y conteo exacto de destrucción (alive=0)");
}

void test_adversarial_readers_vs_cas_writer(int num_readers, int cas_ops) {
  Payload::alive.store(0);
  {
    auto init = ivo_v4::shared_ptr<Payload>::make_shared(0);
    ivo_v4::atomic_shared_ptr<Payload> ap(init);
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total_reads{0};

    std::vector<std::thread> readers;
    for (int r = 0; r < num_readers; ++r) {
      readers.emplace_back([&]() {
        uint64_t local_reads = 0;
        uint64_t checksum = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          auto sp = ap.load();
          if (sp) {
            checksum += static_cast<uint64_t>(sp->value);
            ++local_reads;
          }
        }
        if (checksum == 0xFFFFFFFFFFFFFFFFULL) std::puts("never");
        total_reads.fetch_add(local_reads, std::memory_order_relaxed);
      });
    }

    uint32_t max_retries = 0;
    uint64_t sum_retries = 0;
    auto cur = init;
    for (int i = 1; i <= cas_ops; ++i) {
      auto next = ivo_v4::shared_ptr<Payload>::make_shared(i);
      uint32_t retries = 0;
      bool ok = ap.compare_and_set_counted(cur, next, retries);
      assert(ok);
      if (retries > max_retries) max_retries = retries;
      sum_retries += retries;
      cur = std::move(next);
    }

    stop.store(true, std::memory_order_release);
    for (auto& th : readers) th.join();
    assert(ap.load()->value == cas_ops);
    std::printf("[PASS] Estrés adversarial (%d lectores + 1 escritor × %d CAS): "
                "lecturas=%llu, max_reintentos=%u, prom_reintentos=%.2f\n",
                num_readers, cas_ops, (unsigned long long)total_reads.load(),
                max_retries, static_cast<double>(sum_retries) / cas_ops);
  }
  ivo_v4::drain_all_retired();
  assert(Payload::alive.load() == 0);
  std::puts("[PASS] 0 fugas tras estrés adversarial (alive=0)");
}

void test_mixed_multiwriter_and_readers(int nthreads, int ops_per_thread) {
  Payload::alive.store(0);
  {
    ivo_v4::atomic_shared_ptr<Payload> ap(ivo_v4::shared_ptr<Payload>::make_shared(1));
    std::vector<std::thread> ths;
    for (int t = 0; t < nthreads; ++t) {
      ths.emplace_back([&, t]() {
        for (int i = 0; i < ops_per_thread; ++i) {
          int mode = (i + t) % 10;
          if (mode < 6) {
            auto sp = ap.load();
            assert(!sp || sp->value >= 1);
          } else if (mode < 9) {
            auto exp = ap.load();
            auto nw = ivo_v4::shared_ptr<Payload>::make_shared(i + 1);
            ap.compare_exchange_weak(exp, std::move(nw));
          } else {
            ap.store(ivo_v4::shared_ptr<Payload>::make_shared(i + 1));
          }
        }
      });
    }
    for (auto& th : ths) th.join();
  }
  ivo_v4::drain_all_retired();
  assert(Payload::alive.load() == 0);
  std::printf("[PASS] Estrés mixto load/store/CAS (%d hilos × %d ops): alive=0\n",
              nthreads, ops_per_thread);
}

// Prueba con nodos enlazados (destrucción recursiva Node -> next) que dispara
// reentrancia en reclaim_or_retire/drain_local bajo alta contención.
struct StackNode {
  static inline std::atomic<int64_t> alive{0};
  int val;
  ivo_v4::shared_ptr<StackNode> next;
  explicit StackNode(int v = 0) : val(v), next(nullptr) {
    alive.fetch_add(1, std::memory_order_relaxed);
  }
  ~StackNode() { alive.fetch_sub(1, std::memory_order_relaxed); }
};

void test_treiber_stack_reentrancy(int nthreads, int ops_per_thread) {
  StackNode::alive.store(0);
  {
    ivo_v4::atomic_shared_ptr<StackNode> head;
    for (int i = 0; i < 32; ++i) {
      auto n = ivo_v4::shared_ptr<StackNode>::make_shared(i);
      n->next = head.load();
      while (!head.compare_exchange_weak(n->next, n)) {}
    }

    std::vector<std::thread> ths;
    for (int t = 0; t < nthreads; ++t) {
      ths.emplace_back([&, t]() {
        for (int i = 0; i < ops_per_thread; ++i) {
          if (((i + t) % 5) == 0) {
            auto cur = head.load();
            while (cur && !head.compare_exchange_weak(cur, cur->next)) {}
            if (cur) {
              auto nw = ivo_v4::shared_ptr<StackNode>::make_shared(cur->val + 1);
              nw->next = head.load();
              while (!head.compare_exchange_weak(nw->next, nw)) {}
            }
          } else {
            auto cur = head.load();
            if (cur) {
              volatile int sink = cur->val;
              (void)sink;
            }
          }
        }
      });
    }
    for (auto& th : ths) th.join();

    // Desenlazar y vaciar
    auto cur = head.load();
    head.store(nullptr);
    while (cur) {
      auto nxt = cur->next;
      cur->next = nullptr;
      cur = std::move(nxt);
    }
  }
  ivo_v4::drain_all_retired();
  assert(StackNode::alive.load() == 0);
  std::printf("[PASS] Pila Treiber recursiva (%d hilos × %d ops): alive=0\n",
              nthreads, ops_per_thread);
}

}  // namespace

int main() {
  test_unit_and_leak_free();
  test_adversarial_readers_vs_cas_writer(10, 5000);
  test_mixed_multiwriter_and_readers(8, 15000);
  test_treiber_stack_reentrancy(16, 15000);
  std::puts("TODAS LAS PRUEBAS DE Autores-v4 PASARON SIN ERRORES.");
  return 0;
}
