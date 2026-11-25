#ifndef MEJORAS_ATOMIC_SHARED_PTR_V4_H_
#define MEJORAS_ATOMIC_SHARED_PTR_V4_H_

// ============================================================================
// Autores-v4 (Propuesta 4 sobre Autores-v2):
//   Soluciona la limitación teórica declarada en §1.2 y §5.1 de de Wolff et al.
//   (Euro-Par 2024): compare_and_set deja de sufrir inanición por lectores
//   concurrentes mediante un esquema híbrido Fast-Path / Slow-Path:
//
//   1. Camino rápido (slow_writers == 0, >99.9 % del tiempo):
//      - Idéntico a Autores-v2: bloque in-place sin vtable, shared_ptr de 8 B,
//        load() con un solo fetch_add(pack_tag(1)) wait-free, store() con un
//        solo exchange() wait-free y liberación inmediata (0 bloques diferidos).
//
//   2. Camino lento (cuando un escritor en compare_and_set acumula >= K fallos
//      consecutivos inducidos únicamente por cambios de `tag` de lectores):
//      - El escritor incrementa `slow_writers` (en la misma línea de caché de
//        `tagged_pointer`).
//      - Los lectores que ven `slow_writers > 0` dejan de modificar `tagged_pointer`
//        con `fetch_add` y usan protección por ranura local (`hp[tid]`) +
//        `rc_try_increment()` (solo lecturas compartidas sobre `tagged_pointer`,
//        sin invalidar la línea de caché ni mover `tag`).
//      - Con `tag` quieto (a lo sumo P-1 lectores en vuelo), el `CAS` del escritor
//        termina en pasos acotados O(P) sin bloquear a ningún lector ni escritor
//        incluso si el escritor lento es expulsado por el planificador.
//      - La reclamación verifica `active_hp_slots == 0` en 1 instrucción en la
//        ruta rápida (liberación inmediata) y solo retira acotadamente (<= P)
//        los bloques protegidos activamente por un lector lento en vuelo.
//
// Referencias:
//   - de Wolff et al. (Euro-Par 2024, §1.2 y §5.1, DOI 10.1007/978-3-031-69583-4_8)
//   - Kogan & Petrank (PPoPP 2012, DOI 10.1145/2370036.2145835)
//   - Timnat & Petrank (PPoPP 2014, DOI 10.1145/2555243.2555261)
//   - Nikolaev & Ravindran (Crystalline-W, PLDI 2024, DOI 10.1145/3658851)
// ============================================================================

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <utility>
#include <vector>

namespace ivo_v4 {

constexpr uint64_t kFreeBitsMsb = 16;
constexpr uint64_t kFreeBitsLsb = 0;
constexpr uint64_t kTagBits = kFreeBitsMsb + kFreeBitsLsb;
constexpr uint64_t kMaxLocalRefcount = (1ULL << kTagBits) - 1;
constexpr uint64_t kMaxThreads = 64;
constexpr uint64_t kCombinedCount = kMaxLocalRefcount - kMaxThreads;
constexpr uint64_t kDangerZone = kMaxLocalRefcount / 2;
constexpr uint64_t kMaxRefcount = 1ULL << 62;

// Umbral de fallos CAS consecutivos por interferencia de lectores antes de
// activar el camino lento (Fast-Path / Slow-Path, Kogan & Petrank 2012).
constexpr uint32_t kSlowPathThreshold = 4;
// Máximo de reintentos por cambios de escritores en slow_load antes de caer
// una sola vez a fetch_add (garantiza progreso wait-free de load incluso ante
// escritores infinitos).
constexpr uint32_t kMaxSlowLoadRetries = 8;

// ============================================================================
// Registro global ligero de ranuras de protección para lectores en camino lento
// ============================================================================
struct RetiredBlock {
  void* ptr;
  void (*deleter)(void*);
  RetiredBlock* next;
};

class SlowHazardRegistry {
 public:
  static constexpr size_t kMaxSlots = 128;

  struct alignas(128) Slot {
    std::atomic<bool> in_use{false};
    std::atomic<const void*> hazard{nullptr};
  };

  static SlowHazardRegistry& instance() noexcept {
    static SlowHazardRegistry reg;
    return reg;
  }

  // Contador global de lectores actualmente ejecutando el camino lento.
  // Cuando vale 0 (99.9 % del tiempo), delete es inmediato sin escanear nada.
  std::atomic<uint32_t> active_hp_readers{0};

  int acquire_slot() noexcept {
    for (size_t i = 0; i < kMaxSlots; ++i) {
      bool expected = false;
      if (slots_[i].in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
        slots_[i].hazard.store(nullptr, std::memory_order_relaxed);
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  void release_slot(int idx) noexcept {
    if (idx < 0 || static_cast<size_t>(idx) >= kMaxSlots) return;
    slots_[idx].hazard.store(nullptr, std::memory_order_release);
    slots_[idx].in_use.store(false, std::memory_order_release);
  }

  inline void set_hazard(int idx, const void* ptr) noexcept {
    slots_[idx].hazard.store(ptr, std::memory_order_seq_cst);
  }

  inline void clear_hazard(int idx) noexcept {
    slots_[idx].hazard.store(nullptr, std::memory_order_release);
  }

  bool is_protected(const void* ptr) const noexcept {
    for (size_t i = 0; i < kMaxSlots; ++i) {
      if (slots_[i].in_use.load(std::memory_order_acquire)) {
        if (slots_[i].hazard.load(std::memory_order_seq_cst) == ptr) {
          return true;
        }
      }
    }
    return false;
  }

  void push_global_retired(void* ptr, void (*deleter)(void*)) {
    auto* node = new RetiredBlock{ptr, deleter, nullptr};
    RetiredBlock* head = global_retired_.load(std::memory_order_relaxed);
    do {
      node->next = head;
    } while (!global_retired_.compare_exchange_weak(head, node, std::memory_order_acq_rel,
                                                    std::memory_order_relaxed));
  }

  void try_drain_global() {
    if (global_retired_.load(std::memory_order_acquire) == nullptr) return;
    RetiredBlock* list = global_retired_.exchange(nullptr, std::memory_order_acq_rel);
    RetiredBlock* keep_head = nullptr;
    RetiredBlock* keep_tail = nullptr;
    while (list != nullptr) {
      RetiredBlock* cur = list;
      list = list->next;
      if (!is_protected(cur->ptr)) {
        cur->deleter(cur->ptr);
        delete cur;
      } else {
        cur->next = keep_head;
        keep_head = cur;
        if (keep_tail == nullptr) keep_tail = cur;
      }
    }
    if (keep_head != nullptr) {
      RetiredBlock* head = global_retired_.load(std::memory_order_relaxed);
      do {
        keep_tail->next = head;
      } while (!global_retired_.compare_exchange_weak(head, keep_head, std::memory_order_acq_rel,
                                                      std::memory_order_relaxed));
    }
  }

  ~SlowHazardRegistry() {
    RetiredBlock* list = global_retired_.exchange(nullptr, std::memory_order_acq_rel);
    while (list != nullptr) {
      RetiredBlock* cur = list;
      list = list->next;
      cur->deleter(cur->ptr);
      delete cur;
    }
  }

 private:
  Slot slots_[kMaxSlots];
  std::atomic<RetiredBlock*> global_retired_{nullptr};
};

inline thread_local bool g_tls_hazard_destroyed = false;

struct ThreadHazardHandle {
  struct Entry {
    void* ptr;
    void (*deleter)(void*);
  };

  int slot_idx = -1;
  bool draining = false;
  std::vector<Entry> local_retired;

  ThreadHazardHandle() {
    slot_idx = SlowHazardRegistry::instance().acquire_slot();
    local_retired.reserve(16);
  }

  ~ThreadHazardHandle() {
    g_tls_hazard_destroyed = true;
    auto& reg = SlowHazardRegistry::instance();
    if (slot_idx >= 0) {
      reg.release_slot(slot_idx);
    }
    std::vector<Entry> batch;
    batch.swap(local_retired);
    for (auto& e : batch) {
      if (!reg.is_protected(e.ptr)) {
        e.deleter(e.ptr);
      } else {
        reg.push_global_retired(e.ptr, e.deleter);
      }
    }
    reg.try_drain_global();
  }

  void drain_local() {
    if (draining || local_retired.empty()) return;
    draining = true;
    auto& reg = SlowHazardRegistry::instance();
    std::vector<Entry> batch;
    batch.swap(local_retired);
    for (auto& e : batch) {
      if (!reg.is_protected(e.ptr)) {
        e.deleter(e.ptr);
      } else {
        local_retired.push_back(e);
      }
    }
    draining = false;
  }
};

inline ThreadHazardHandle& thread_hazard_handle() {
  thread_local ThreadHazardHandle handle;
  return handle;
}

inline void reclaim_or_retire(void* ptr, void (*deleter)(void*)) {
  auto& reg = SlowHazardRegistry::instance();
  // Camino rápido (99.9 % de las veces): 0 lectores lentos -> liberación inmediata!
  if (reg.active_hp_readers.load(std::memory_order_seq_cst) == 0) [[likely]] {
    deleter(ptr);
    reg.try_drain_global();
    return;
  }
  if (g_tls_hazard_destroyed) [[unlikely]] {
    if (!reg.is_protected(ptr)) {
      deleter(ptr);
    } else {
      reg.push_global_retired(ptr, deleter);
    }
    return;
  }
  auto& h = thread_hazard_handle();
  h.drain_local();
  if (!reg.is_protected(ptr)) {
    deleter(ptr);
  } else {
    h.local_retired.push_back({ptr, deleter});
  }
  reg.try_drain_global();
}

inline void drain_all_retired() {
  thread_hazard_handle().drain_local();
  SlowHazardRegistry::instance().try_drain_global();
}

// ============================================================================
// Bloque de control in-place sin vtable (heredado de Autores-v2) + rc_try_increment
// ============================================================================
template <class T>
class shared_ptr;

template <class T>
class atomic_shared_ptr;

template <class T>
struct ptr_control_block {
  std::atomic<uint64_t> reference_count;
  bool is_inplace;
  union Storage {
    T* ext_ptr;
    T value;
    Storage() {}
    ~Storage() {}
  } storage;

  struct InplaceTag {};

  explicit ptr_control_block(T* p) noexcept : reference_count(1), is_inplace(false) {
    storage.ext_ptr = p;
  }

  template <typename... Args>
  explicit ptr_control_block(InplaceTag, Args&&... args)
      : reference_count(1), is_inplace(true) {
    ::new (static_cast<void*>(&storage.value)) T(std::forward<Args>(args)...);
  }

  ~ptr_control_block() {
    if (is_inplace) {
      storage.value.~T();
    } else {
      delete storage.ext_ptr;
    }
  }

  static void destroy_cb(void* raw) {
    delete static_cast<ptr_control_block<T>*>(raw);
  }

  inline T* get_ptr() noexcept {
    return is_inplace ? &storage.value : storage.ext_ptr;
  }

  inline const T* get_ptr() const noexcept {
    return is_inplace ? &storage.value : storage.ext_ptr;
  }

  inline void rc_increment_relaxed(uint64_t count) noexcept {
    uint64_t prev = this->reference_count.fetch_add(count, std::memory_order_relaxed);
    if (prev > kMaxRefcount) [[unlikely]] {
      std::printf("Reference count overflow\n");
      std::abort();
    }
  }

  inline void rc_increment_acquire(uint64_t count) noexcept {
    uint64_t prev = this->reference_count.fetch_add(count, std::memory_order_acquire);
    if (prev > kMaxRefcount) [[unlikely]] {
      std::printf("Reference count overflow\n");
      std::abort();
    }
  }

  // Incremento seguro solo si reference_count > 0 (evita resurrección en slow_load).
  inline bool rc_try_increment_one() noexcept {
    uint64_t cur = this->reference_count.load(std::memory_order_relaxed);
    while (cur != 0) {
      if (this->reference_count.compare_exchange_weak(
              cur, cur + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  inline void rc_decrement_live(uint64_t count) noexcept {
    this->reference_count.fetch_sub(count, std::memory_order_release);
  }

  inline void rc_decrement(uint64_t count) {
    uint64_t prev = this->reference_count.fetch_sub(count, std::memory_order_release);
    if (prev == count) [[unlikely]] {
      (void)this->reference_count.load(std::memory_order_acquire);
      reclaim_or_retire(this, &ptr_control_block<T>::destroy_cb);
    }
  }
};

// ============================================================================
// shared_ptr<T> compacto de 8 bytes
// ============================================================================
template <class T>
class shared_ptr {
  friend class atomic_shared_ptr<T>;

 private:
  ptr_control_block<T>* control_block;

  explicit shared_ptr(ptr_control_block<T>* cb) noexcept : control_block(cb) {}

  inline void forget() noexcept { this->control_block = nullptr; }

 public:
  shared_ptr() noexcept : control_block(nullptr) {}
  explicit(false) shared_ptr(std::nullptr_t) noexcept : control_block(nullptr) {}
  explicit shared_ptr(T* ptr)
      : control_block(ptr != nullptr ? new ptr_control_block<T>(ptr) : nullptr) {}

  template <typename... Args>
  static shared_ptr<T> make_shared(Args&&... args) {
    return shared_ptr<T>(new ptr_control_block<T>(
        typename ptr_control_block<T>::InplaceTag{}, std::forward<Args>(args)...));
  }

  shared_ptr(const shared_ptr<T>& other) noexcept : control_block(other.control_block) {
    if (this->control_block != nullptr) {
      this->control_block->rc_increment_relaxed(1);
    }
  }

  shared_ptr(shared_ptr<T>&& other) noexcept : control_block(other.control_block) {
    other.control_block = nullptr;
  }

  ~shared_ptr() {
    if (this->control_block != nullptr) {
      this->control_block->rc_decrement(1);
    }
  }

  shared_ptr<T>& operator=(const shared_ptr<T>& other) {
    if (this->control_block == other.control_block) [[unlikely]] {
      return *this;
    }
    ptr_control_block<T>* old = this->control_block;
    this->control_block = other.control_block;
    if (this->control_block != nullptr) {
      this->control_block->rc_increment_relaxed(1);
    }
    if (old != nullptr) {
      old->rc_decrement(1);
    }
    return *this;
  }

  shared_ptr<T>& operator=(shared_ptr<T>&& other) {
    if (this == &other) [[unlikely]] {
      return *this;
    }
    ptr_control_block<T>* old = this->control_block;
    this->control_block = other.control_block;
    other.control_block = nullptr;
    if (old != nullptr) {
      old->rc_decrement(1);
    }
    return *this;
  }

  shared_ptr<T>& operator=(std::nullptr_t) {
    if (this->control_block != nullptr) {
      this->control_block->rc_decrement(1);
      this->control_block = nullptr;
    }
    return *this;
  }

  inline T* get() const noexcept {
    return this->control_block != nullptr ? this->control_block->get_ptr() : nullptr;
  }

  inline T& operator*() const noexcept { return *this->control_block->get_ptr(); }
  inline T* operator->() const noexcept { return this->control_block->get_ptr(); }
  explicit operator bool() const noexcept { return this->control_block != nullptr; }

  bool operator==(const shared_ptr<T>& other) const noexcept {
    return this->control_block == other.control_block;
  }
  bool operator!=(const shared_ptr<T>& other) const noexcept {
    return this->control_block != other.control_block;
  }
};

static_assert(sizeof(shared_ptr<int>) == 8, "ivo_v4::shared_ptr debe medir 8 bytes");

// ============================================================================
// Empaquetado de tagged_pointer (64 bits: 16 bits tag + 48 bits ptr)
// ============================================================================
constexpr size_t kPtrMask = (1ULL << 48) - 1;

inline constexpr size_t pack_tag(uint64_t tag) noexcept {
  return tag << (64 - kFreeBitsMsb);
}

template <class T>
inline size_t pack(ptr_control_block<T>* ptr, uint64_t tag) noexcept {
  return (reinterpret_cast<size_t>(ptr) & kPtrMask) | pack_tag(tag);
}

template <class T>
inline ptr_control_block<T>* unpack_ptr(size_t tp) noexcept {
  return reinterpret_cast<ptr_control_block<T>*>(tp & kPtrMask);
}

inline uint64_t unpack_tag(size_t tp) noexcept {
  return tp >> (64 - kFreeBitsMsb);
}

template <class T>
inline void release(size_t tp) {
  ptr_control_block<T>* cb = unpack_ptr<T>(tp);
  if (cb == nullptr) return;
  uint64_t tag = unpack_tag(tp);
  cb->rc_decrement(kCombinedCount - tag);
}

inline std::memory_order memory_order_load(std::memory_order order) noexcept {
  if (order == std::memory_order_release) return std::memory_order_relaxed;
  if (order == std::memory_order_acq_rel) return std::memory_order_acquire;
  return order;
}

inline std::memory_order memory_order_max(std::memory_order o1, std::memory_order o2) noexcept {
  return o1 >= o2 ? o1 : o2;
}

// ============================================================================
// atomic_shared_ptr<T> (Autores-v4: Fast-Path Weighted-RC + Slow-Path Wait-Free CAS)
// ============================================================================
template <class T>
class atomic_shared_ptr {
 private:
  // Ambos campos comparten la misma línea de caché de 64 B para costo mínimo.
  std::atomic<size_t> tagged_pointer;
  std::atomic<uint32_t> slow_writers;

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

  inline shared_ptr<T> load_fast_path(std::memory_order order) {
    size_t result = this->tagged_pointer.fetch_add(pack_tag(1), order);
    ptr_control_block<T>* cb = unpack_ptr<T>(result);
    if (cb == nullptr) return shared_ptr<T>(nullptr);
    uint64_t tag = unpack_tag(result);
    if (tag >= kDangerZone) [[unlikely]] {
      this->shift_references(cb, tag + 1);
    }
    return shared_ptr<T>(cb);
  }

  // Camino lento de lectura: NO escribe sobre `tagged_pointer` (no mueve `tag`!),
  // permitiendo que cualquier compare_and_set en camino lento complete en O(P) pasos.
  shared_ptr<T> load_slow_path(std::memory_order order) {
    auto& reg = SlowHazardRegistry::instance();
    auto& handle = thread_hazard_handle();
    if (handle.slot_idx < 0) [[unlikely]] {
      return load_fast_path(order);
    }

    reg.active_hp_readers.fetch_add(1, std::memory_order_seq_cst);
    const std::memory_order ld = memory_order_load(order);

    for (uint32_t attempt = 0; attempt < kMaxSlowLoadRetries; ++attempt) {
      ptr_control_block<T>* cb = unpack_ptr<T>(this->tagged_pointer.load(ld));
      if (cb == nullptr) {
        reg.clear_hazard(handle.slot_idx);
        reg.active_hp_readers.fetch_sub(1, std::memory_order_release);
        return shared_ptr<T>(nullptr);
      }
      reg.set_hazard(handle.slot_idx, cb);
      ptr_control_block<T>* cb2 = unpack_ptr<T>(this->tagged_pointer.load(std::memory_order_seq_cst));
      if (cb2 != cb) {
        continue;  // Un escritor cambió el puntero; reintentar.
      }
      if (cb->rc_try_increment_one()) {
        reg.clear_hazard(handle.slot_idx);
        reg.active_hp_readers.fetch_sub(1, std::memory_order_release);
        return shared_ptr<T>(cb);
      }
      // cb llegó a 0 justo antes de rc_try_increment_one -> ya fue reemplazado.
    }

    // Respaldo acotado (wait-free incluso ante escritores infinitos):
    reg.clear_hazard(handle.slot_idx);
    reg.active_hp_readers.fetch_sub(1, std::memory_order_release);
    return load_fast_path(order);
  }

  template <bool ConsumeNew>
  bool compare_and_set_impl(const shared_ptr<T>& expected, ptr_control_block<T>* new_cb,
                            shared_ptr<T>* new_to_forget, std::memory_order success_order,
                            std::memory_order failure_order, uint32_t* out_retries = nullptr) {
    const std::memory_order load_ord =
        memory_order_max(memory_order_load(success_order), failure_order);
    size_t observed = this->tagged_pointer.load(load_ord);
    if (unpack_ptr<T>(observed) != expected.control_block) {
      if (out_retries) *out_retries = 0;
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
    uint32_t retries = 0;
    bool in_slow_path = false;

    while (true) {
      if (unpack_ptr<T>(observed) != expected.control_block) {
        if (in_slow_path) {
          this->slow_writers.fetch_sub(1, std::memory_order_release);
        }
        if (new_cb != nullptr) {
          new_cb->rc_decrement_live(delta);
        }
        if (out_retries) *out_retries = retries;
        return false;
      }
      if (this->tagged_pointer.compare_exchange_weak(observed, desired, success_order,
                                                     failure_order)) {
        if constexpr (ConsumeNew) {
          new_to_forget->forget();
        }
        release<T>(observed);
        if (in_slow_path) {
          this->slow_writers.fetch_sub(1, std::memory_order_release);
        }
        if (out_retries) *out_retries = retries;
        return true;
      }
      ++retries;
      if (!in_slow_path && retries >= kSlowPathThreshold) [[unlikely]] {
        this->slow_writers.fetch_add(1, std::memory_order_seq_cst);
        in_slow_path = true;
      }
    }
  }

 public:
  atomic_shared_ptr() noexcept : tagged_pointer(0), slow_writers(0) {}

  explicit(false) atomic_shared_ptr(shared_ptr<T> shared_pointer) : slow_writers(0) {
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
    if (this->slow_writers.load(std::memory_order_acquire) == 0) [[likely]] {
      return load_fast_path(order);
    }
    return load_slow_path(order);
  }

  void store(shared_ptr<T> shared_pointer, std::memory_order order = std::memory_order_seq_cst) {
    ptr_control_block<T>* cb = shared_pointer.control_block;
    shared_pointer.forget();
    if (cb != nullptr) {
      if (order == std::memory_order_relaxed) {
        cb->rc_increment_acquire(kCombinedCount - 1);
      } else {
        cb->rc_increment_relaxed(kCombinedCount - 1);
      }
    }
    size_t old = this->tagged_pointer.exchange(pack<T>(cb, 0), order);
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

  // Sobrecarga instrumentada para medir reintentos CAS en el banco adversarial.
  bool compare_and_set_counted(const shared_ptr<T>& expected, const shared_ptr<T>& new_ptr,
                               uint32_t& out_retries,
                               std::memory_order success_order = std::memory_order_seq_cst,
                               std::memory_order failure_order = std::memory_order_seq_cst) {
    return compare_and_set_impl<false>(expected, new_ptr.control_block, nullptr, success_order,
                                       failure_order, &out_retries);
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
};

}  // namespace ivo_v4

#endif  // MEJORAS_ATOMIC_SHARED_PTR_V4_H_
