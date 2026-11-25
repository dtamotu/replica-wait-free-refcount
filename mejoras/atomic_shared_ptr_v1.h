#pragma once
// Autores-v1 (Experimento 1A): optimización del camino CAS de de Wolff et al. (Euro-Par 2024)
// Derivado directamente del header oficial del artefacto (wait-free-atomic-arc/cpp/atomic_shared_ptr.h).
//
// Cambios frente al original (ivo):
//   1. Paso por referencia (const shared_ptr& y shared_ptr&&) en compare_and_set,
//      compare_exchange_weak y compare_exchange_strong: elimina hasta 4 RMW atómicas
//      desperdiciadas por copias de expected y new_ptr en cada intento de CAS.
//   2. Pre-chequeo perezoso (Mellor-Crummey & Scott, ACM TOCS 1991): comprueba
//      unpack_ptr(observed) == expected.control_block ANTES de reservar peso en new_cb,
//      eliminando 2 RMW atómicas adicionales cuando el puntero ya cambió.
//   3. Relajación de memory_order_relaxed en rc_increment cuando el llamante ya posee
//      una referencia viva (estándar C++11/Rust Arc::clone), manteniendo memory_order_acquire
//      en shift_references (increase_weight) antes del CAS relajado de recarga.
//   4. Guardia contra auto-asignación en operator=.
//   5. Parámetro de plantilla UsePause para ablación limpia con/sin _mm_pause() en reintentos.
//
// Garantías preservadas:
//   - load() y store() siguen siendo estrictamente WAIT-FREE (mismo protocolo O(P) / O(1)).
//   - Liberación inmediata (0 memoria diferida).

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace ivo_v1 {

inline constexpr size_t kFreeBitsMsb = 16;
inline constexpr size_t kFreeBitsLsb = 3;
inline constexpr size_t kTagBits = kFreeBitsMsb + kFreeBitsLsb;
inline constexpr uint64_t kMaxLocalRefcount = (1UL << kTagBits) - 1;

#ifndef IVO_V1_MAX_THREADS
inline constexpr uint64_t kMaxThreads = (1UL << 16);
#else
inline constexpr uint64_t kMaxThreads = IVO_V1_MAX_THREADS;
#endif

#ifndef IVO_V1_COMBINED_COUNT
inline constexpr uint64_t kCombinedCount = kMaxLocalRefcount + 1;
inline constexpr uint64_t kDangerZone = kMaxLocalRefcount - kMaxThreads;
#else
inline constexpr uint64_t kCombinedCount = IVO_V1_COMBINED_COUNT;
inline constexpr uint64_t kDangerZone = (kCombinedCount - 1) - kMaxThreads;
#endif

inline constexpr uint64_t kMaxRefcount = (1UL << 63) - 1;

inline void cpu_pause() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

template <class T, bool UsePause = false>
class atomic_shared_ptr;

template <class T>
struct ptr_control_block {
  template <class U, bool P>
  friend class atomic_shared_ptr;

 private:
  std::atomic<uint64_t> reference_count;
  T* ptr;

 public:
  ptr_control_block(uint64_t rc, T* ptr) : reference_count(rc), ptr(ptr) {}

  // Incremento relajado: seguro cuando el hilo ya posee una referencia viva y
  // sincroniza antes de publicar el puntero (o en copy-ctor de shared_ptr).
  inline void rc_increment_relaxed(uint64_t count) {
    uint64_t rc = this->reference_count.fetch_add(count, std::memory_order_relaxed);
    if (rc + count > kMaxRefcount) [[unlikely]] {
      std::printf("Reference count overflow\n");
      std::abort();
    }
  }

  // Incremento con orden acquire (usado en shift_references antes del CAS relajado
  // para impedir que el CAS se reordene antes del incremento del contador).
  inline void rc_increment_acquire(uint64_t count) {
    uint64_t rc = this->reference_count.fetch_add(count, std::memory_order_acquire);
    if (rc + count > kMaxRefcount) [[unlikely]] {
      std::printf("Reference count overflow\n");
      std::abort();
    }
  }

  inline void rc_decrement(uint64_t count) {
    uint64_t rc = this->reference_count.fetch_sub(count, std::memory_order_release);
    if (rc == count) {
      this->reference_count.load(std::memory_order_acquire);
      delete this->ptr;
      delete this;
    } else if (rc < count) [[unlikely]] {
      std::printf("Negative reference count\n");
      std::abort();
    }
  }

  inline void rc_decrement_live(uint64_t count) {
    uint64_t rc = this->reference_count.fetch_sub(count, std::memory_order_release);
    if (rc <= count) [[unlikely]] {
      std::printf("Reference count invalid while object is still live\n");
      std::abort();
    }
  }
};

template <class T>
struct shared_ptr {
  template <class U, bool P>
  friend class atomic_shared_ptr;

 private:
  T* ptr;
  ptr_control_block<T>* control_block;

  void drop() {
    if (this->control_block != nullptr) {
      this->control_block->rc_decrement(1);
    }
  }
  void forget() {
    this->control_block = nullptr;
    this->ptr = nullptr;
  }

  shared_ptr(ptr_control_block<T>* control_block, T* ptr)
      : ptr(ptr), control_block(control_block) {}

 public:
  shared_ptr() : ptr(nullptr), control_block(nullptr) {}
  shared_ptr(std::nullptr_t) : ptr(nullptr), control_block(nullptr) {}
  explicit(false) shared_ptr(T* ptr) : ptr(ptr) {
    if (ptr == nullptr) {
      this->control_block = nullptr;
    } else {
      this->control_block = new ptr_control_block<T>(1, ptr);
    }
  }
  shared_ptr(const shared_ptr& other) : ptr(other.ptr), control_block(other.control_block) {
    if (this->control_block != nullptr) {
      this->control_block->rc_increment_relaxed(1);
    }
  }
  shared_ptr(shared_ptr&& other) noexcept
      : ptr(other.ptr), control_block(other.control_block) {
    other.ptr = nullptr;
    other.control_block = nullptr;
  }
  ~shared_ptr() { this->drop(); }

  shared_ptr& operator=(const shared_ptr& other) {
    if (this->control_block == other.control_block) {
      this->ptr = other.ptr;
      return *this;
    }
    if (other.control_block != nullptr) {
      other.control_block->rc_increment_relaxed(1);
    }
    this->drop();
    this->ptr = other.ptr;
    this->control_block = other.control_block;
    return *this;
  }
  shared_ptr& operator=(shared_ptr&& other) noexcept {
    if (this == &other) return *this;
    this->drop();
    this->ptr = other.ptr;
    this->control_block = other.control_block;
    other.ptr = nullptr;
    other.control_block = nullptr;
    return *this;
  }

  T* get() const noexcept { return this->ptr; }
  T& operator*() const noexcept { return *this->ptr; }
  T* operator->() const noexcept { return this->ptr; }

  template <typename... Args>
  static shared_ptr make_shared(Args&&... args) {
    auto p = new T(std::forward<Args>(args)...);
    return shared_ptr(p);
  }

  explicit operator bool() const noexcept { return get() != nullptr; }
};

template <class T>
inline ptr_control_block<T>* unpack_ptr(size_t tagged_pointer) noexcept {
  size_t ptr = (((ptrdiff_t)(tagged_pointer << kTagBits)) >> kFreeBitsMsb);
  return (ptr_control_block<T>*)ptr;
}

inline uint64_t unpack_tag(size_t tagged_pointer) noexcept {
  constexpr size_t bits = sizeof(size_t) * 8;
  return tagged_pointer >> (bits - kTagBits);
}

template <class T>
inline size_t pack(ptr_control_block<T>* ptr, uint64_t tag) noexcept {
  constexpr size_t bits = sizeof(size_t) * 8;
  constexpr size_t ptr_mask = (1UL << (bits - kFreeBitsMsb)) - 1;
  return (((size_t)ptr & ptr_mask) >> kFreeBitsLsb) | (tag << (bits - kTagBits));
}

inline constexpr size_t pack_tag(uint64_t tag) noexcept {
  constexpr size_t bits = sizeof(size_t) * 8;
  return tag << (bits - kTagBits);
}

template <class T>
inline void release(size_t tagged_pointer) {
  ptr_control_block<T>* control_block = unpack_ptr<T>(tagged_pointer);
  if (control_block == nullptr) return;
  uint64_t tag = unpack_tag(tagged_pointer);
  control_block->rc_decrement(kCombinedCount - tag);
}

inline std::memory_order memory_order_load(std::memory_order order) noexcept {
  if (order == std::memory_order_release) return std::memory_order_relaxed;
  if (order == std::memory_order_acq_rel) return std::memory_order_acquire;
  return order;
}

inline std::memory_order memory_order_max(std::memory_order o1, std::memory_order o2) noexcept {
  return o1 >= o2 ? o1 : o2;
}

template <class T, bool UsePause>
class atomic_shared_ptr {
 private:
  std::atomic<size_t> tagged_pointer;

  void shift_references(ptr_control_block<T>* control_block, uint64_t tag) {
    control_block->rc_increment_acquire(tag);

    uint64_t current_tag = tag;
    while (true) {
      size_t expected = pack<T>(control_block, current_tag);
      size_t new_value = pack<T>(control_block, current_tag - tag);
      if (this->tagged_pointer.compare_exchange_strong(
              expected, new_value, std::memory_order_relaxed, std::memory_order_relaxed)) {
        return;
      }
      if (unpack_ptr<T>(expected) != control_block) break;
      uint64_t new_tag = unpack_tag(expected);
      if (new_tag < current_tag) break;
      if (new_tag == current_tag) [[unlikely]] {
        std::printf("Spurious fails should be impossible with strong compare_exchange\n");
        std::abort();
      }
      current_tag = new_tag;
    }

    control_block->rc_decrement_live(tag);
  }

  template <bool ConsumeNew>
  bool compare_and_set_impl(const shared_ptr<T>& expected, ptr_control_block<T>* new_cb,
                            shared_ptr<T>* new_to_forget, std::memory_order success_order,
                            std::memory_order failure_order) {
    // Paso 1 (pre-chequeo perezoso): leer el puntero actual ANTES de tocar new_cb.
    const std::memory_order load_ord =
        memory_order_max(memory_order_load(success_order), failure_order);
    size_t observed = this->tagged_pointer.load(load_ord);
    if (unpack_ptr<T>(observed) != expected.control_block) {
      return false;
    }

    // Paso 2: solo si el puntero coincide, reservar el peso en new_cb.
    constexpr uint64_t delta = ConsumeNew ? (kCombinedCount - 1) : kCombinedCount;
    if (new_cb != nullptr) {
      if (success_order == std::memory_order_relaxed) {
        new_cb->rc_increment_acquire(delta);
      } else {
        new_cb->rc_increment_relaxed(delta);
      }
    }

    const size_t desired = pack<T>(new_cb, 0);
    while (true) {
      if (unpack_ptr<T>(observed) != expected.control_block) {
        if (new_cb != nullptr) {
          new_cb->rc_decrement_live(delta);
        }
        return false;
      }
      if (this->tagged_pointer.compare_exchange_weak(observed, desired, success_order,
                                                     failure_order)) {
        if constexpr (ConsumeNew) {
          new_to_forget->forget();
        }
        release<T>(observed);
        return true;
      }
      if constexpr (UsePause) {
        cpu_pause();
      }
    }
  }

 public:
  atomic_shared_ptr() noexcept : tagged_pointer(0) {}
  explicit(false) atomic_shared_ptr(shared_ptr<T> shared_pointer) {
    if (shared_pointer.control_block != nullptr) {
      shared_pointer.control_block->rc_increment_relaxed(kCombinedCount - 1);
    }
    this->tagged_pointer.store(pack<T>(shared_pointer.control_block, 0),
                               std::memory_order_release);
    shared_pointer.forget();
  }

  ~atomic_shared_ptr() {
    size_t tp = this->tagged_pointer.load(std::memory_order_relaxed);
    release<T>(tp);
  }

  shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) {
    size_t result = this->tagged_pointer.fetch_add(pack_tag(1), order);
    ptr_control_block<T>* control_block = unpack_ptr<T>(result);
    if (control_block == nullptr) return shared_ptr<T>(nullptr, nullptr);
    uint64_t tag = unpack_tag(result);
    if (tag >= kDangerZone) [[unlikely]] {
      this->shift_references(control_block, tag + 1);
    }
    return shared_ptr<T>(control_block, control_block->ptr);
  }

  void store(shared_ptr<T> shared_pointer, std::memory_order order = std::memory_order_seq_cst) {
    ptr_control_block<T>* control_block = shared_pointer.control_block;
    shared_pointer.forget();
    if (control_block != nullptr) {
      if (order == std::memory_order_relaxed) {
        control_block->rc_increment_acquire(kCombinedCount - 1);
      } else {
        control_block->rc_increment_relaxed(kCombinedCount - 1);
      }
    }
    size_t old = this->tagged_pointer.exchange(pack<T>(control_block, 0), order);
    release<T>(old);
  }

  bool compare_and_set(const shared_ptr<T>& expected, const shared_ptr<T>& new_ptr,
                       std::memory_order success_order = std::memory_order_seq_cst,
                       std::memory_order failure_order = std::memory_order_seq_cst) {
    return compare_and_set_impl<false>(expected, new_ptr.control_block, nullptr, success_order,
                                       failure_order);
  }

  bool compare_and_set(const shared_ptr<T>& expected, shared_ptr<T>&& new_ptr,
                       std::memory_order success_order = std::memory_order_seq_cst,
                       std::memory_order failure_order = std::memory_order_seq_cst) {
    return compare_and_set_impl<true>(expected, new_ptr.control_block, &new_ptr, success_order,
                                      failure_order);
  }

  bool compare_exchange_weak(shared_ptr<T>& expected, const shared_ptr<T>& new_ptr,
                             std::memory_order success_order = std::memory_order_seq_cst,
                             std::memory_order failure_order = std::memory_order_seq_cst) {
    if (this->compare_and_set(expected, new_ptr, success_order, failure_order)) {
      return true;
    }
    expected = this->load(failure_order);
    return false;
  }

  bool compare_exchange_weak(shared_ptr<T>& expected, shared_ptr<T>&& new_ptr,
                             std::memory_order success_order = std::memory_order_seq_cst,
                             std::memory_order failure_order = std::memory_order_seq_cst) {
    if (this->compare_and_set(expected, std::move(new_ptr), success_order, failure_order)) {
      return true;
    }
    expected = this->load(failure_order);
    return false;
  }

  bool compare_exchange_strong(shared_ptr<T>& expected, const shared_ptr<T>& new_ptr,
                               std::memory_order success_order = std::memory_order_seq_cst,
                               std::memory_order failure_order = std::memory_order_seq_cst) {
    while (true) {
      if (this->compare_and_set(expected, new_ptr, success_order, failure_order)) {
        return true;
      }
      shared_ptr<T> observed = this->load(failure_order);
      if (observed.control_block == expected.control_block) {
        continue;
      }
      expected = std::move(observed);
      return false;
    }
  }

  atomic_shared_ptr(const atomic_shared_ptr&) = delete;
  atomic_shared_ptr& operator=(const atomic_shared_ptr&) = delete;
};

template <class T>
using atomic_shared_ptr_pause = atomic_shared_ptr<T, true>;

}  // namespace ivo_v1
