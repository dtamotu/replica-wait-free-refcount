// ============================================================================
// Pruebas de correctitud, linealizabilidad (conservación exacta de suma/conteo),
// vaciado de pila y sanitizers (ASan+UBSan y TSan) para Autores-v3.
// ============================================================================

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

#include "atomic_shared_ptr_v3.h"
#include "elimination_table_v3.h"

namespace {

template <template <typename> typename AtomicSP>
class TestElimStack {
 public:
  struct Node;
  using sp_t = ivo_v2::shared_ptr<Node>;
  static constexpr int kElimMode = ivo_v3::elimination_mode_v<AtomicSP<Node>>;
  using elim_t = ivo_v3::EliminationTable<int, (kElimMode == 2)>;

  struct Node {
    int val;
    sp_t next;
    Node() : val(0), next(nullptr) {}
    explicit Node(int v) : val(v), next(nullptr) {}
  };

  AtomicSP<Node> head;
  std::atomic<elim_t*> elim{nullptr};

  ~TestElimStack() {
    auto cur = head.load();
    head.store(nullptr);
    while (cur) {
      auto nxt = cur->next;
      cur->next = nullptr;
      cur = nxt;
    }
    delete elim.load(std::memory_order_relaxed);
  }

  elim_t* get_or_create_elim() {
    elim_t* tbl = elim.load(std::memory_order_acquire);
    if (tbl) return tbl;
    auto* fresh = new elim_t();
    elim_t* expected = nullptr;
    if (elim.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      return fresh;
    }
    delete fresh;
    return expected;
  }

  void push(int v) {
    if constexpr (kElimMode > 0) {
      auto& tls = ivo_v3::thread_pe_state();
      if (tls.fail_streak >= 2) {
        if (elim_t* tbl = elim.load(std::memory_order_acquire)) {
          if (tbl->try_eliminate_push(v)) return;
        }
      }
      auto node = ivo_v2::shared_ptr<Node>::make_shared(v);
      node->next = head.load();
      while (!head.compare_exchange_weak(node->next, node)) {
        if (tls.fail_streak < 4) ++tls.fail_streak;
        if (get_or_create_elim()->try_eliminate_push(v)) return;
      }
      if (tls.fail_streak > 0) --tls.fail_streak;
      return;
    }
    auto node = ivo_v2::shared_ptr<Node>::make_shared(v);
    node->next = head.load();
    while (!head.compare_exchange_weak(node->next, node)) {}
  }

  std::optional<int> pop() {
    if constexpr (kElimMode > 0) {
      auto& tls = ivo_v3::thread_pe_state();
      if (tls.fail_streak >= 2) {
        if (elim_t* tbl = elim.load(std::memory_order_acquire)) {
          if (auto r = tbl->try_eliminate_pop(); r.has_value()) return r;
        }
      }
      auto cur = head.load();
      while (cur) {
        if (head.compare_exchange_weak(cur, cur->next)) {
          if (tls.fail_streak > 0) --tls.fail_streak;
          return cur->val;
        }
        if (tls.fail_streak < 4) ++tls.fail_streak;
        if (auto r = get_or_create_elim()->try_eliminate_pop(); r.has_value()) return r;
      }
      tls.fail_streak = 0;
      return std::nullopt;
    }
    auto cur = head.load();
    while (cur && !head.compare_exchange_weak(cur, cur->next)) {}
    if (cur) return cur->val;
    return std::nullopt;
  }

  std::optional<int> peek() {
    auto cur = head.load();
    if (cur) return cur->val;
    return std::nullopt;
  }
};

void test_topology() {
  const auto& topo = ivo_v3::PeTopology::instance();
  int p_cores = 0, e_cores = 0;
  for (int c = 0; c < 24; ++c) {
    auto ci = topo.info_for_cpu(c);
    if (ci.is_p_core) ++p_cores;
    else ++e_cores;
  }
  std::printf("[topo] dominios=%d, p_cores(0..23)=%d, e_cores(0..23)=%d\n",
              topo.num_domains(), p_cores, e_cores);
  assert(topo.num_domains() >= 1);
}

template <bool TopoAware>
void test_elimination_table_conservation(const char* name, int nthreads, int ops) {
  ivo_v3::EliminationTable<int, TopoAware> tbl;
  std::atomic<int64_t> pushed_sum{0};
  std::atomic<int64_t> popped_sum{0};
  std::atomic<int64_t> pushed_cnt{0};
  std::atomic<int64_t> popped_cnt{0};

  std::vector<std::thread> ths;
  for (int t = 0; t < nthreads; ++t) {
    ths.emplace_back([&, t]() {
      int64_t l_push_sum = 0, l_pop_sum = 0, l_push_cnt = 0, l_pop_cnt = 0;
      for (int i = 0; i < ops; ++i) {
        if (((t + i) & 1) == 0) {
          int v = (t + 1) * 100000 + i;
          if (tbl.try_eliminate_push(v)) {
            l_push_sum += v;
            ++l_push_cnt;
          }
        } else {
          if (auto r = tbl.try_eliminate_pop(); r.has_value()) {
            l_pop_sum += *r;
            ++l_pop_cnt;
          }
        }
      }
      pushed_sum.fetch_add(l_push_sum, std::memory_order_relaxed);
      popped_sum.fetch_add(l_pop_sum, std::memory_order_relaxed);
      pushed_cnt.fetch_add(l_push_cnt, std::memory_order_relaxed);
      popped_cnt.fetch_add(l_pop_cnt, std::memory_order_relaxed);
    });
  }
  for (auto& th : ths) th.join();

  assert(pushed_cnt.load() == popped_cnt.load());
  assert(pushed_sum.load() == popped_sum.load());
  std::printf("[PASS] %s: emparejamientos=%lld, suma=%lld (exacta 1:1)\n",
              name, (long long)pushed_cnt.load(), (long long)pushed_sum.load());
}

template <template <typename> typename AtomicSP>
void test_stack_conservation_and_readers(const char* name, int nthreads, int ops) {
  TestElimStack<AtomicSP> st;
  int64_t init_sum = 0;
  constexpr int kInit = 64;
  for (int i = 1; i <= kInit; ++i) {
    st.push(i);
    init_sum += i;
  }

  std::atomic<int64_t> delta_sum{0};
  std::atomic<int64_t> delta_cnt{0};
  std::atomic<int64_t> reads{0};

  std::vector<std::thread> ths;
  for (int t = 0; t < nthreads; ++t) {
    ths.emplace_back([&, t]() {
      int64_t l_sum = 0, l_cnt = 0, l_reads = 0;
      for (int i = 0; i < ops; ++i) {
        if ((i & 3) == 0) {
          auto p = st.peek();
          l_reads += p.has_value();
        } else {
          auto v = st.pop();
          if (v.has_value()) {
            l_sum -= *v;
            --l_cnt;
            int nv = *v + 1;
            st.push(nv);
            l_sum += nv;
            ++l_cnt;
          }
        }
      }
      delta_sum.fetch_add(l_sum, std::memory_order_relaxed);
      delta_cnt.fetch_add(l_cnt, std::memory_order_relaxed);
      reads.fetch_add(l_reads, std::memory_order_relaxed);
    });
  }
  for (auto& th : ths) th.join();

  // Vaciar toda la pila y verificar conservación EXACTA de conteo y suma.
  int64_t final_sum = 0;
  int64_t final_cnt = 0;
  while (auto v = st.pop()) {
    final_sum += *v;
    ++final_cnt;
  }
  assert(final_cnt == kInit + delta_cnt.load());
  assert(final_sum == init_sum + delta_sum.load());
  // Verificar que pop() sobre pila vacía retorna nullopt sin bloquearse.
  assert(!st.pop().has_value());
  std::printf("[PASS] %s: nodos=%lld, suma=%lld (exacta), lecturas=%lld\n",
              name, (long long)final_cnt, (long long)final_sum, (long long)reads.load());
}

}  // namespace

int main() {
  test_topology();
  test_elimination_table_conservation<false>("ElimTable genérico", 8, 25000);
  test_elimination_table_conservation<true>("ElimTable L2+P/E", 8, 25000);
  test_stack_conservation_and_readers<ivo_v3::atomic_shared_ptr_backoff>("Autores-v3a (P/E backoff)", 8, 20000);
  test_stack_conservation_and_readers<ivo_v3::atomic_shared_ptr_elim_generic>("Autores-v3g (Elim gen)", 8, 20000);
  test_stack_conservation_and_readers<ivo_v3::atomic_shared_ptr_elim_l2>("Autores-v3 (Elim L2+PE)", 8, 20000);
  std::puts("TODAS LAS PRUEBAS DE Autores-v3 PASARON SIN ERRORES.");
  return 0;
}
