#ifndef MEJORAS_ELIMINATION_TABLE_V3_H_
#define MEJORAS_ELIMINATION_TABLE_V3_H_

// ============================================================================
// Tabla de Elimination-Backoff lock-free de una sola palabra atómica (64 bits)
// por ranura, particionada por clúster L2 y tipo de núcleo (P-core vs E-core).
//
// Protocolo linealizable, sin ABA y libre de data races (TSan-clean):
//   word = (uint64_t(payload32) << 32) | ((ver & 0x3FFFFFFF) << 2) | state
//   Estados:
//     0 = EMPTY      (ranura libre)
//     1 = PUSH_WAIT  (un push(x) publicó x con versión v y espera a un pop)
//     2 = POP_WAIT   (un pop() publicó boleto v y espera a un push(x))
//     3 = POP_GOT    (un push(x) entregó x al pop() de versión v; solo ese pop libera)
//
// Referencias:
//   - Hendler, Shavit & Yerushalmi (SPAA 2004, DOI 10.1145/1007912.1007944)
//   - Liu et al. (LibASL, PPoPP 2022, DOI 10.48550/arXiv.2108.03355)
//   - Ouyang & Zhu (Core-Aware Combining, JPDC 2022, DOI 10.1016/j.jpdc.2022.01.001)
// ============================================================================

#include <atomic>
#include <bit>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "pe_topology.h"

namespace ivo_v3 {

template <typename T>
struct alignas(128) EliminationSlot32 {
  static_assert(sizeof(T) <= 4 && std::is_trivially_copyable_v<T>,
                "EliminationSlot32 requiere T trivialmente copiable de <= 4 bytes");

  static constexpr uint64_t kStateMask = 0x3ULL;
  static constexpr uint64_t kVerMask = 0x3FFFFFFFULL;

  static constexpr uint64_t kEmpty = 0ULL;
  static constexpr uint64_t kPushWait = 1ULL;
  static constexpr uint64_t kPopWait = 2ULL;
  static constexpr uint64_t kPopGot = 3ULL;

  std::atomic<uint64_t> word{0};

  static inline uint32_t encode_val(T v) noexcept {
    uint32_t bits = 0;
    if constexpr (sizeof(T) == 4) {
      bits = std::bit_cast<uint32_t>(v);
    } else {
      __builtin_memcpy(&bits, &v, sizeof(T));
    }
    return bits;
  }

  static inline T decode_val(uint64_t w) noexcept {
    uint32_t bits = static_cast<uint32_t>(w >> 32);
    if constexpr (sizeof(T) == 4) {
      return std::bit_cast<T>(bits);
    } else {
      T out{};
      __builtin_memcpy(&out, &bits, sizeof(T));
      return out;
    }
  }

  static inline uint64_t pack(uint32_t payload, uint64_t ver, uint64_t st) noexcept {
    return (static_cast<uint64_t>(payload) << 32) | ((ver & kVerMask) << 2) | (st & kStateMask);
  }

  static inline uint64_t state_of(uint64_t w) noexcept { return w & kStateMask; }
  static inline uint64_t ver_of(uint64_t w) noexcept { return (w >> 2) & kVerMask; }

  // Intento no bloqueante de emparejar un push(val) con un POP_WAIT existente.
  inline bool try_match_push_immediate(T val) noexcept {
    uint64_t cur = word.load(std::memory_order_acquire);
    if (state_of(cur) != kPopWait) return false;
    uint64_t v = ver_of(cur);
    uint64_t desired = pack(encode_val(val), v, kPopGot);
    return word.compare_exchange_strong(cur, desired, std::memory_order_acq_rel,
                                        std::memory_order_relaxed);
  }

  // Intento no bloqueante de emparejar un pop() con un PUSH_WAIT existente.
  inline std::optional<T> try_match_pop_immediate() noexcept {
    uint64_t cur = word.load(std::memory_order_acquire);
    if (state_of(cur) != kPushWait) return std::nullopt;
    uint64_t v = ver_of(cur);
    uint64_t desired = pack(0, v + 1, kEmpty);
    if (word.compare_exchange_strong(cur, desired, std::memory_order_acq_rel,
                                     std::memory_order_relaxed)) {
      return decode_val(cur);
    }
    return std::nullopt;
  }

  // Intento completo de eliminación para push(val) con espera breve sin _mm_pause().
  inline bool exchange_push(T val, int spin_iters) noexcept {
    uint64_t cur = word.load(std::memory_order_acquire);
    uint64_t st = state_of(cur);
    uint64_t v = ver_of(cur);

    if (st == kPopWait) {
      uint64_t desired = pack(encode_val(val), v, kPopGot);
      if (word.compare_exchange_strong(cur, desired, std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
        return true;
      }
      return false;
    }

    if (st != kEmpty) {
      return false;
    }

    // Publicar PUSH_WAIT(val, v)
    uint64_t posted = pack(encode_val(val), v, kPushWait);
    if (!word.compare_exchange_strong(cur, posted, std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
      // Quizá un pop llegó primero en este mismo ciclo: aprovecharlo.
      if (state_of(cur) == kPopWait) {
        uint64_t v2 = ver_of(cur);
        uint64_t desired = pack(encode_val(val), v2, kPopGot);
        if (word.compare_exchange_strong(cur, desired, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
          return true;
        }
      }
      return false;
    }

    // Espera ligera en L2 (sin _mm_pause(), ya demostramos en v1p/v2p que PAUSE daña Arrow Lake).
    for (int i = 0; i < spin_iters; ++i) {
      uint64_t obs = word.load(std::memory_order_acquire);
      if (obs != posted) {
        // Un pop() realizó la transición PUSH_WAIT(v) -> EMPTY(v+1).
        return true;
      }
      asm volatile("" ::: "memory");
    }

    // Cancelar si nadie nos tomó.
    uint64_t expected = posted;
    uint64_t empty_next = pack(0, v + 1, kEmpty);
    if (word.compare_exchange_strong(expected, empty_next, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      return false;  // Cancelado limpiamente.
    }
    // Si falló la cancelación, un pop() ya tomó nuestro valor.
    return true;
  }

  // Intento completo de eliminación para pop() con espera breve sin _mm_pause().
  inline std::optional<T> exchange_pop(int spin_iters) noexcept {
    uint64_t cur = word.load(std::memory_order_acquire);
    uint64_t st = state_of(cur);
    uint64_t v = ver_of(cur);

    if (st == kPushWait) {
      uint64_t desired = pack(0, v + 1, kEmpty);
      if (word.compare_exchange_strong(cur, desired, std::memory_order_acq_rel,
                                       std::memory_order_relaxed)) {
        return decode_val(cur);
      }
      return std::nullopt;
    }

    if (st != kEmpty) {
      return std::nullopt;
    }

    // Publicar POP_WAIT(0, v)
    uint64_t posted = pack(0, v, kPopWait);
    if (!word.compare_exchange_strong(cur, posted, std::memory_order_acq_rel,
                                      std::memory_order_relaxed)) {
      // Quizá un push llegó primero en este mismo ciclo: aprovecharlo.
      if (state_of(cur) == kPushWait) {
        uint64_t v2 = ver_of(cur);
        uint64_t desired = pack(0, v2 + 1, kEmpty);
        if (word.compare_exchange_strong(cur, desired, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
          return decode_val(cur);
        }
      }
      return std::nullopt;
    }

    for (int i = 0; i < spin_iters; ++i) {
      uint64_t obs = word.load(std::memory_order_acquire);
      if (obs != posted) {
        // Solo un push() puede pasar POP_WAIT(v) -> POP_GOT(val, v).
        T val = decode_val(obs);
        word.store(pack(0, v + 1, kEmpty), std::memory_order_release);
        return val;
      }
      asm volatile("" ::: "memory");
    }

    // Cancelar si nadie entregó valor.
    uint64_t expected = posted;
    uint64_t empty_next = pack(0, v + 1, kEmpty);
    if (word.compare_exchange_strong(expected, empty_next, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) {
      return std::nullopt;
    }
    // Un push() entregó el valor justo antes de cancelar: consumirlo y liberar la ranura.
    T val = decode_val(expected);
    word.store(pack(0, v + 1, kEmpty), std::memory_order_release);
    return val;
  }
};

// ============================================================================
// EliminationTable<T, TopologyAware>:
//   - TopologyAware == false (Modo 1, control genérico): elige ranura aleatoria.
//   - TopologyAware == true  (Modo 2, L2 + P/E-aware): usa primero la ranura del
//     propio dominio L2 (35 ns en clúster E de 4 núcleos) con ventana de espera
//     ajustada a P-core vs E-core, y si está ocupada por la misma operación hace
//     1 sondeo oportunista en otra ranura.
// ============================================================================
template <typename T, bool TopologyAware>
class EliminationTable {
 public:
  static constexpr int kMaxSlots = PeTopology::kMaxDomains;

  EliminationTable() noexcept : num_slots_(PeTopology::instance().num_domains()) {}

  inline bool try_eliminate_push(T val) noexcept {
    auto& tls = thread_pe_state();
    int n = num_slots_;
    if constexpr (TopologyAware) {
      CpuInfo ci = tls.refresh_cpu_info();
      int primary = static_cast<int>(ci.domain_id) % n;
      // Ventana ligeramente mayor en E-cores (aprovecha L2 compartida de 4 núcleos)
      // y más corta en P-cores (menor latencia hacia head en L3).
      int spin = ci.is_p_core ? 48 : 96;
      if (slots_[primary].exchange_push(val, spin)) {
        return true;
      }
      // Sondeo oportunista no bloqueante en otro dominio (ayuda cuando u = 10 %).
      if (n > 1) {
        int alt = (primary + 1 + static_cast<int>(tls.next_rand() % static_cast<uint32_t>(n - 1))) % n;
        if (slots_[alt].try_match_push_immediate(val)) {
          return true;
        }
      }
      return false;
    } else {
      int idx = static_cast<int>(tls.next_rand() % static_cast<uint32_t>(n));
      return slots_[idx].exchange_push(val, 72);
    }
  }

  inline std::optional<T> try_eliminate_pop() noexcept {
    auto& tls = thread_pe_state();
    int n = num_slots_;
    if constexpr (TopologyAware) {
      CpuInfo ci = tls.refresh_cpu_info();
      int primary = static_cast<int>(ci.domain_id) % n;
      int spin = ci.is_p_core ? 48 : 96;
      if (auto res = slots_[primary].exchange_pop(spin); res.has_value()) {
        return res;
      }
      if (n > 1) {
        int alt = (primary + 1 + static_cast<int>(tls.next_rand() % static_cast<uint32_t>(n - 1))) % n;
        if (auto res = slots_[alt].try_match_pop_immediate(); res.has_value()) {
          return res;
        }
      }
      return std::nullopt;
    } else {
      int idx = static_cast<int>(tls.next_rand() % static_cast<uint32_t>(n));
      return slots_[idx].exchange_pop(72);
    }
  }

 private:
  int num_slots_;
  EliminationSlot32<T> slots_[kMaxSlots];
};

}  // namespace ivo_v3

#endif  // MEJORAS_ELIMINATION_TABLE_V3_H_
