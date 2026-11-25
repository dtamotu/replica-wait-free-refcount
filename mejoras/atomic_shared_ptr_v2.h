#pragma once
// Autores-v2 (Experimentos 1A + 1B): optimización de CAS + bloque de control in-place
// y shared_ptr compacto de 8 bytes (como Rust Arc<T> en codigo/autores/rust/src/arc.rs).
//
// Cambios acumulados sobre el original (ivo):
//   [1A] Paso por referencia (const& / &&), pre-chequeo perezoso antes de tocar new_cb,
//        rc_increment_relaxed cuando hay referencia viva, guardia de auto-asignación y
//        ablación opcional con _mm_pause().
//   [1B] Bloque de control in-place sin vtable (Dimov & Dawes N2351; Anderson et al. PLDI 2022;
//        de Wolff et al. Euro-Par 2024 §2.1 Fig. 1a):
//          - make_shared<T>() hace UNA sola reserva contigua (reference_count + T en la misma
//            línea de caché de 64 B), eliminando el doble new/delete y el pointer chasing.
//          - shared_ptr<T> ocupa solo 8 bytes (puntero único a control_block), reduciendo
//            sizeof(Node) en la pila de Treiber de 24 B a 16 B y el bloque total a 32 B.
//          - load() NO desreferencia control_block->ptr en la ruta rápida: devuelve el puntero
//            al bloque tras el único fetch_add, evitando tocar una segunda línea caliente
//            durante load() cuando el llamante aún no desreferencia el dato.
//          - Mantiene compatibilidad total con shared_ptr<T>(new T(...)) para bench_atomic_shared_ptrs
//            y bench_basura mediante unión + bandera inmutable is_inplace.
//
// Garantías preservadas:
//   - load() y store() siguen siendo estrictamente WAIT-FREE (O(P) peor caso / O(1) habitual).
//   - Liberación inmediata (0 bloques diferidos).

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace ivo_v2 {

inline constexpr size_t kFreeBitsMsb = 16;
inline constexpr size_t kFreeBitsLsb = 3;
inline constexpr size_t kTagBits = kFreeBitsMsb + kFreeBitsLsb;
inline constexpr uint64_t kMaxLocalRefcount = (1UL << kTagBits) - 1;

#ifndef IVO_V2_MAX_THREADS
inline constexpr uint64_t kMaxThreads = (1UL << 16);
#else
inline constexpr uint64_t kMaxThreads = IVO_V2_MAX_THREADS;
#endif

#ifndef IVO_V2_COMBINED_COUNT
inline constexpr uint64_t kCombinedCount = kMaxLocalRefcount + 1;
inline constexpr uint64_t kDangerZone = kMaxLocalRefcount - kMaxThreads;
#else
inline constexpr uint64_t kCombinedCount = IVO_V2_COMBINED_COUNT;
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

struct inplace_tag {};

template <class T>
struct alignas(8) ptr_control_block {
  template <class U, bool P>
  friend class atomic_shared_ptr;
  template <class U>
  friend struct shared_ptr;

 private:
  std::atomic<uint64_t> reference_count;
  union Storage {
    T* ext_ptr;
    T value;
    Storage() noexcept {}
    ~Storage() noexcept {}
  } storage;
  bool is_inplace;

 public:
  // Constructor externo: envuelve un puntero asignado con new T(...)
  ptr_control_block(uint64_t rc, T* ptr) noexcept : reference_count(rc), is_inplace(false) {
    storage.ext_ptr = ptr;
  }

  // Constructor in-place: construye T directamente dentro del bloque de control
  template <typename... Args>
  ptr_control_block(uint64_t rc, inplace_tag, Args&&... args)
      : reference_count(rc), is_inplace(true) {
    ::new (static_cast<void*>(&storage.value)) T(std::forward<Args>(args)...);
  }

  ~ptr_control_block() = default;

  inline T* get_ptr() noexcept {
    if (is_inplace) [[likely]] {
      return &storage.value;
    }
    return storage.ext_ptr;
  }

  inline void rc_increment_relaxed(uint64_t count) {
    uint64_t rc = this->reference_count.fetch_add(count, std::memory_order_relaxed);
    if (rc + count > kMaxRefcount) [[unlikely]] {
      std::printf("Reference count overflow\n");
      std::abort();
    }
  }

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
      if (is_inplace) [[likely]] {
        storage.value.~T();
      } else {
        delete storage.ext_ptr;
      }
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
  ptr_control_block<T>* control_block;

  void drop() {
    if (this->control_block != nullptr) {
      this->control_block->rc_decrement(1);
    }
  }
  void forget() noexcept { this->control_block = nullptr; }

  explicit shared_ptr(ptr_control_block<T>* cb) noexcept : control_block(cb) {}

 public:
  shared_ptr() noexcept : control_block(nullptr) {}
  shared_ptr(std::nullptr_t) noexcept : control_block(nullptr) {}
  explicit(false) shared_ptr(T* ptr) {
    if (ptr == nullptr) {
      this->control_block = nullptr;
    } else {
      this->control_block = new ptr_control_block<T>(1, ptr);
    }
  }
  shared_ptr(const shared_ptr& other) : control_block(other.control_block) {
    if (this->control_block != nullptr) {
      this->control_block->rc_increment_relaxed(1);
    }
  }
  shared_ptr(shared_ptr&& other) noexcept : control_block(other.control_block) {
    other.control_block = nullptr;
  }
  ~shared_ptr() { this->drop(); }

  shared_ptr& operator=(const shared_ptr& other) {
    if (this->control_block == other.control_block) return *this;
    if (other.control_block != nullptr) {
      other.control_block->rc_increment_relaxed(1);
    }
    this->drop();
    this->control_block = other.control_block;
    return *this;
  }
  shared_ptr& operator=(shared_ptr&& other) noexcept {
    if (this == &other) return *this;
    this->drop();
    this->control_block = other.control_block;
    other.control_block = nullptr;
    return *this;
  }

  T* get() const noexcept {
    return this->control_block != nullptr ? this->control_block->get_ptr() : nullptr;
  }
  T& operator*() const noexcept { return *this->control_block->get_ptr(); }
  T* operator->() const noexcept { return this->control_block->get_ptr(); }

  template <typename... Args>
  static shared_ptr make_shared(Args&&... args) {
    auto* cb = new ptr_control_block<T>(1, inplace_tag{}, std::forward<Args>(args)...);
    return shared_ptr(cb);
  }

  explicit operator bool() const noexcept { return this->control_block != nullptr; }
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
    const std::memory_order load_ord =
        memory_order_max(memory_order_load(success_order), failure_order);
    size_t observed = this->tagged_pointer.load(load_ord);
    if (unpack_ptr<T>(observed) != expected.control_block) {
      return false;
    }

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
    if (control_block == nullptr) return shared_ptr<T>(nullptr);
    uint64_t tag = unpack_tag(result);
    if (tag >= kDangerZone) [[unlikely]] {
      this->shift_references(control_block, tag + 1);
    }
    // Sin desreferenciar control_block->ptr en la ruta rápida:
    return shared_ptr<T>(control_block);
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

}  // namespace ivo_v2
