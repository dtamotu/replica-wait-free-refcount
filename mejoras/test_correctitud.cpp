// Suite de validación de correctitud para Autores-v1 (1A) y Autores-v2 (1A+1B):
//   1. Carrera read-reclaim intensa (load + store + CAS simultáneos) sobre shared_ptr externo e in-place.
//   2. Pila de Treiber concurrente con verificación estricta de conservación de elementos y 0 fugas.
//   3. Auto-asignación, copias/movimientos, nullptr y destructores no triviales.
// Compilar con ASan+UBSan y con TSan (setarch -R).
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include "atomic_shared_ptr_v1.h"
#include "atomic_shared_ptr_v2.h"

static std::atomic<long> g_vivos{0};

struct Dato {
  uint64_t canario = 0x5ca1ab1e12345678ULL;
  long valor = 0;
  explicit Dato(long v = 0) : valor(v) { g_vivos.fetch_add(1, std::memory_order_relaxed); }
  ~Dato() {
    if (canario != 0x5ca1ab1e12345678ULL) {
      std::printf("ERROR: canario corrupto en destructor!\n");
      std::abort();
    }
    canario = 0;
    g_vivos.fetch_sub(1, std::memory_order_relaxed);
  }
};

template <template <class> class ASP, template <class> class SP>
struct Nodo {
  uint64_t canario = 0xdeadbeefcafe0001ULL;
  long val = 0;
  SP<Nodo> next;
  explicit Nodo(long v = 0) : val(v) { g_vivos.fetch_add(1, std::memory_order_relaxed); }
  ~Nodo() {
    if (canario != 0xdeadbeefcafe0001ULL) {
      std::printf("ERROR: nodo corrupto!\n");
      std::abort();
    }
    canario = 0;
    g_vivos.fetch_sub(1, std::memory_order_relaxed);
  }
};

template <template <class> class ASP, template <class> class SP>
void probar_basico(const char* nombre) {
  long antes = g_vivos.load();
  {
    auto a = SP<Dato>::make_shared(10);
    a = a;  // auto-asignación con rc == 1
    assert(a && a->valor == 10);
    auto b = SP<Dato>(new Dato(20));
    b = b;
    assert(b && b->valor == 20);
    SP<Dato> nulo(nullptr);
    assert(!nulo && nulo.get() == nullptr);

    ASP<Dato> celda;
    assert(!celda.load());
    celda.store(a);
    auto leido = celda.load();
    assert(leido && leido->valor == 10);
    // CAS debe fallar con esperado distinto (pre-chequeo perezoso)
    assert(!celda.compare_and_set(b, nulo));
    // CAS debe tener éxito con esperado igual (lvalue y rvalue)
    assert(celda.compare_and_set(leido, b));
    assert(celda.load()->valor == 20);
    assert(celda.compare_and_set(b, SP<Dato>::make_shared(30)));
    assert(celda.load()->valor == 30);
    celda.store(nullptr);
    assert(!celda.load());
  }
  assert(g_vivos.load() == antes);
  std::printf("[OK] %-10s pruebas unitarias (0 fugas)\n", nombre);
}

template <template <class> class ASP, template <class> class SP>
void probar_read_reclaim_y_cas(const char* nombre, int hilos, int ops_por_hilo, bool usar_make_shared) {
  long antes = g_vivos.load();
  std::atomic<long> errores{0};
  {
    ASP<Dato> celda(usar_make_shared ? SP<Dato>::make_shared(1) : SP<Dato>(new Dato(1)));
    std::vector<std::thread> ts;
    for (int t = 0; t < hilos; t++) {
      ts.emplace_back([&, t] {
        std::mt19937_64 rng(999 + t);
        for (int i = 0; i < ops_por_hilo; i++) {
          int r = rng() % 100;
          if (r < 15) {
            auto nuevo = usar_make_shared ? SP<Dato>::make_shared(i) : SP<Dato>(new Dato(i));
            celda.store(std::move(nuevo));
          } else if (r < 45) {
            auto exp = celda.load();
            auto nuevo = usar_make_shared ? SP<Dato>::make_shared(i) : SP<Dato>(new Dato(i));
            celda.compare_exchange_weak(exp, nuevo);
            if (exp && exp->canario != 0x5ca1ab1e12345678ULL) errores++;
          } else {
            auto p = celda.load();
            if (p && p->canario != 0x5ca1ab1e12345678ULL) errores++;
          }
        }
      });
    }
    for (auto& th : ts) th.join();
    celda.store(nullptr);
  }
  assert(errores.load() == 0);
  assert(g_vivos.load() == antes);
  std::printf("[OK] %-10s read-reclaim+CAS (%s, %d hilos × %d ops: 0 errores, 0 fugas)\n",
              nombre, usar_make_shared ? "in-place" : "externo", hilos, ops_por_hilo);
}

template <template <class> class ASP, template <class> class SP>
void probar_pila_conservacion(const char* nombre, int hilos, int ops_por_hilo) {
  long antes = g_vivos.load();
  using N = Nodo<ASP, SP>;
  std::atomic<long long> suma_push{0}, suma_pop{0};
  {
    ASP<N> head;
    for (int i = 1; i <= 64; i++) {
      auto n = SP<N>::make_shared(i);
      n->next = head.load();
      while (!head.compare_exchange_weak(n->next, n)) {}
      suma_push.fetch_add(i, std::memory_order_relaxed);
    }

    std::vector<std::thread> ts;
    for (int t = 0; t < hilos; t++) {
      ts.emplace_back([&, t] {
        std::mt19937_64 rng(7777 + t);
        long long lp = 0, lq = 0;
        for (int i = 0; i < ops_por_hilo; i++) {
          int r = rng() % 100;
          if (r < 50) {
            // pop + push
            auto ss = head.load();
            while (ss && !head.compare_exchange_weak(ss, ss->next)) {}
            if (ss) {
              lq += ss->val;
              long v = ss->val;
              auto nn = SP<N>::make_shared(v);
              nn->next = head.load();
              while (!head.compare_exchange_weak(nn->next, nn)) {}
              lp += v;
            }
          } else {
            auto ss = head.load();
            if (ss && ss->canario != 0xdeadbeefcafe0001ULL) std::abort();
          }
        }
        suma_push.fetch_add(lp, std::memory_order_relaxed);
        suma_pop.fetch_add(lq, std::memory_order_relaxed);
      });
    }
    for (auto& th : ts) th.join();

    // Vaciar pila y comprobar conservación exacta
    while (true) {
      auto ss = head.load();
      if (!ss) break;
      if (head.compare_exchange_weak(ss, ss->next)) {
        suma_pop.fetch_add(ss->val, std::memory_order_relaxed);
      }
    }
  }
  assert(suma_push.load() == suma_pop.load());
  assert(g_vivos.load() == antes);
  std::printf("[OK] %-10s pila Treiber (%d hilos × %d ops: conservación exacta, 0 fugas)\n",
              nombre, hilos, ops_por_hilo);
}

template <class T> using V1_ASP = ivo_v1::atomic_shared_ptr<T, false>;
template <class T> using V1P_ASP = ivo_v1::atomic_shared_ptr<T, true>;
template <class T> using V2_ASP = ivo_v2::atomic_shared_ptr<T, false>;
template <class T> using V2P_ASP = ivo_v2::atomic_shared_ptr<T, true>;

int main(int argc, char** argv) {
  int hilos = argc > 1 ? std::atoi(argv[1]) : 12;
  int ops = argc > 2 ? std::atoi(argv[2]) : 50000;

  std::printf("Tamaños: sizeof(ivo_v1::shared_ptr)=%zu, sizeof(ivo_v2::shared_ptr)=%zu\n",
              sizeof(ivo_v1::shared_ptr<Dato>), sizeof(ivo_v2::shared_ptr<Dato>));
  std::printf("Tamaños Nodo: v1 cb=%zu+node=%zu, v2 cb_inplace=%zu\n",
              sizeof(ivo_v1::ptr_control_block<Nodo<V1_ASP, ivo_v1::shared_ptr>>),
              sizeof(Nodo<V1_ASP, ivo_v1::shared_ptr>),
              sizeof(ivo_v2::ptr_control_block<Nodo<V2_ASP, ivo_v2::shared_ptr>>));

  probar_basico<V1_ASP, ivo_v1::shared_ptr>("ivo-v1");
  probar_basico<V1P_ASP, ivo_v1::shared_ptr>("ivo-v1p");
  probar_basico<V2_ASP, ivo_v2::shared_ptr>("ivo-v2");
  probar_basico<V2P_ASP, ivo_v2::shared_ptr>("ivo-v2p");

  probar_read_reclaim_y_cas<V1_ASP, ivo_v1::shared_ptr>("ivo-v1", hilos, ops, true);
  probar_read_reclaim_y_cas<V1P_ASP, ivo_v1::shared_ptr>("ivo-v1p", hilos, ops, false);
  probar_read_reclaim_y_cas<V2_ASP, ivo_v2::shared_ptr>("ivo-v2", hilos, ops, true);
  probar_read_reclaim_y_cas<V2_ASP, ivo_v2::shared_ptr>("ivo-v2", hilos, ops, false);
  probar_read_reclaim_y_cas<V2P_ASP, ivo_v2::shared_ptr>("ivo-v2p", hilos, ops, true);

  probar_pila_conservacion<V1_ASP, ivo_v1::shared_ptr>("ivo-v1", hilos, ops);
  probar_pila_conservacion<V1P_ASP, ivo_v1::shared_ptr>("ivo-v1p", hilos, ops);
  probar_pila_conservacion<V2_ASP, ivo_v2::shared_ptr>("ivo-v2", hilos, ops);
  probar_pila_conservacion<V2P_ASP, ivo_v2::shared_ptr>("ivo-v2p", hilos, ops);

  std::printf("TODAS LAS PRUEBAS DE CORRECTITUD PASARON SIN ERRORES.\n");
  return 0;
}
