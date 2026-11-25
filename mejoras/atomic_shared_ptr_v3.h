#ifndef MEJORAS_ATOMIC_SHARED_PTR_V3_H_
#define MEJORAS_ATOMIC_SHARED_PTR_V3_H_

// ============================================================================
// Autores-v3 (Propuesta 3 sobre Autores-v2):
//   - ivo_v3::atomic_shared_ptr_backoff<T>      (ivo-v3a):
//       Mismo layout in-place + 8B de v2, añadiendo backoff ligero consciente
//       de P-core / E-core (sin _mm_pause()) únicamente tras >= 2 fallos CAS
//       consecutivos causados por contención de lectores en compare_and_set.
//   - ivo_v3::atomic_shared_ptr_elim_generic<T> (ivo-v3g):
//       Autores-v2 + etiqueta kEliminationMode = 1 (Elimination-Backoff genérico
//       aleatorio, control ciego a la topología).
//   - ivo_v3::atomic_shared_ptr_elim_l2<T>      (ivo-v3):
//       Autores-v2 + etiqueta kEliminationMode = 2 (Elimination-Backoff local
//       por clúster L2 y tipo de núcleo P/E).
//
// Garantías preservadas:
//   - load() y store() siguen 100 % wait-free (idénticos a Autores-v2).
//   - Liberación inmediata (0 bloques diferidos).
// ============================================================================

#include "atomic_shared_ptr_v2.h"
#include "pe_topology.h"

namespace ivo_v3 {

template <class T>
using shared_ptr = ivo_v2::shared_ptr<T>;

// Trazo para que atomic_stack en stack.h habilite eliminación sin tocar
// ningún algoritmo original (kEliminationMode == 0 por defecto).
template <typename AtomicSP>
struct elimination_mode {
  static constexpr int value = 0;
};

template <typename AtomicSP>
inline constexpr int elimination_mode_v = elimination_mode<AtomicSP>::value;

// ----------------------------------------------------------------------------
// Componente 3a: Autores-v2 + backoff ligero P/E-aware en compare_and_set
// (sin _mm_pause(), usando espera acotada en registros tras >= 2 colisiones).
// ----------------------------------------------------------------------------
template <class T>
class atomic_shared_ptr_backoff {
 private:
  ivo_v2::atomic_shared_ptr<T, false> inner_;

  static inline void pe_light_backoff(uint32_t fails) noexcept {
    if (fails < 2) return;
    auto& tls = thread_pe_state();
    CpuInfo ci = tls.refresh_cpu_info();
    // LibASL-inspired asymmetry: P-cores esperan menos ciclos que E-cores para
    // evitar convoy y respetar la diferencia de latencia de coherencia.
    uint32_t cap = ci.is_p_core ? 24u : 64u;
    uint32_t shift = fails < 6u ? (fails - 1u) : 5u;
    uint32_t limit = 4u << shift;
    if (limit > cap) limit = cap;
    uint32_t spins = (tls.next_rand() % limit) + 1u;
    for (uint32_t i = 0; i < spins; ++i) {
      asm volatile("" ::: "memory");
    }
  }

 public:
  atomic_shared_ptr_backoff() noexcept = default;
  explicit(false) atomic_shared_ptr_backoff(shared_ptr<T> sp) : inner_(std::move(sp)) {}

  shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) {
    return inner_.load(order);
  }

  void store(shared_ptr<T> sp, std::memory_order order = std::memory_order_seq_cst) {
    inner_.store(std::move(sp), order);
  }

  bool compare_and_set(const shared_ptr<T>& expected, const shared_ptr<T>& new_ptr,
                       std::memory_order success_order = std::memory_order_seq_cst,
                       std::memory_order failure_order = std::memory_order_seq_cst) {
    return inner_.compare_and_set(expected, new_ptr, success_order, failure_order);
  }

  bool compare_and_set(const shared_ptr<T>& expected, shared_ptr<T>&& new_ptr,
                       std::memory_order success_order = std::memory_order_seq_cst,
                       std::memory_order failure_order = std::memory_order_seq_cst) {
    return inner_.compare_and_set(expected, std::move(new_ptr), success_order, failure_order);
  }

  bool compare_exchange_weak(shared_ptr<T>& expected, const shared_ptr<T>& new_ptr,
                             std::memory_order success_order = std::memory_order_seq_cst,
                             std::memory_order failure_order = std::memory_order_seq_cst) {
    if (inner_.compare_exchange_weak(expected, new_ptr, success_order, failure_order)) {
      thread_pe_state().fail_streak = 0;
      return true;
    }
    auto& tls = thread_pe_state();
    if (tls.fail_streak < 8) ++tls.fail_streak;
    pe_light_backoff(tls.fail_streak);
    return false;
  }

  bool compare_exchange_weak(shared_ptr<T>& expected, shared_ptr<T>&& new_ptr,
                             std::memory_order success_order = std::memory_order_seq_cst,
                             std::memory_order failure_order = std::memory_order_seq_cst) {
    if (inner_.compare_exchange_weak(expected, std::move(new_ptr), success_order, failure_order)) {
      thread_pe_state().fail_streak = 0;
      return true;
    }
    auto& tls = thread_pe_state();
    if (tls.fail_streak < 8) ++tls.fail_streak;
    pe_light_backoff(tls.fail_streak);
    return false;
  }

  bool compare_exchange_strong(shared_ptr<T>& expected, const shared_ptr<T>& new_ptr,
                               std::memory_order success_order = std::memory_order_seq_cst,
                               std::memory_order failure_order = std::memory_order_seq_cst) {
    return inner_.compare_exchange_strong(expected, new_ptr, success_order, failure_order);
  }
};

// ----------------------------------------------------------------------------
// Componente 3b: Envoltorios finos de Autores-v2 para habilitar en atomic_stack:
//   - atomic_shared_ptr_elim_generic (Mode 1: eliminación aleatoria genérica)
//   - atomic_shared_ptr_elim_l2      (Mode 2: eliminación L2-cluster + P/E-aware)
// ----------------------------------------------------------------------------
template <class T>
struct atomic_shared_ptr_elim_generic : public ivo_v2::atomic_shared_ptr<T, false> {
  using base = ivo_v2::atomic_shared_ptr<T, false>;
  using base::base;
};

template <class T>
struct atomic_shared_ptr_elim_l2 : public ivo_v2::atomic_shared_ptr<T, false> {
  using base = ivo_v2::atomic_shared_ptr<T, false>;
  using base::base;
};

template <class T>
struct elimination_mode<atomic_shared_ptr_elim_generic<T>> {
  static constexpr int value = 1;
};

template <class T>
struct elimination_mode<atomic_shared_ptr_elim_l2<T>> {
  static constexpr int value = 2;
};

}  // namespace ivo_v3

#endif  // MEJORAS_ATOMIC_SHARED_PTR_V3_H_
