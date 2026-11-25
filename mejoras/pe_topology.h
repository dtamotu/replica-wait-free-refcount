#ifndef MEJORAS_PE_TOPOLOGY_H_
#define MEJORAS_PE_TOPOLOGY_H_

// ============================================================================
// Detector de topología híbrida P-core / E-core y clústeres L2 en Linux.
// Lee una sola vez /sys/devices/system/cpu/cpu*/cache/index2/shared_cpu_list
// para mapear cada CPU lógica a:
//   - is_p_core: true si tiene L2 privada (1 CPU en shared_cpu_list, p. ej. cpu0..7 en Arrow Lake)
//   - domain_id: dominio de eliminación de 4 núcleos (cada clúster E de 4 núcleos
//                comparte 1 dominio L2; los P-cores se agrupan de 4 en 4).
// Ref: Schöne et al. (ICPE 2024), Liu et al. (LibASL, PPoPP 2022), Ouyang & Zhu (JPDC 2022).
// ============================================================================

#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <sched.h>
#include <string>

namespace ivo_v3 {

struct CpuInfo {
  uint8_t domain_id = 0;
  bool is_p_core = false;
};

class PeTopology {
 public:
  static constexpr int kMaxCpus = 128;
  static constexpr int kMaxDomains = 16;

  static const PeTopology& instance() noexcept {
    static const PeTopology topo;
    return topo;
  }

  inline CpuInfo info_for_cpu(int cpu) const noexcept {
    if (cpu < 0 || cpu >= kMaxCpus) return {0, false};
    return cpus_[cpu];
  }

  inline int num_domains() const noexcept { return num_domains_; }

  inline CpuInfo current_cpu_info() const noexcept {
    int cpu = ::sched_getcpu();
    return info_for_cpu(cpu);
  }

 private:
  std::array<CpuInfo, kMaxCpus> cpus_{};
  int num_domains_ = 1;

  static int count_cpus_in_list(const std::string& list) {
    int count = 0;
    size_t i = 0;
    while (i < list.size()) {
      while (i < list.size() && (list[i] == ' ' || list[i] == ',' || list[i] == '\n' || list[i] == '\r')) {
        ++i;
      }
      if (i >= list.size()) break;
      int a = 0;
      while (i < list.size() && list[i] >= '0' && list[i] <= '9') {
        a = a * 10 + (list[i++] - '0');
      }
      if (i < list.size() && list[i] == '-') {
        ++i;
        int b = 0;
        while (i < list.size() && list[i] >= '0' && list[i] <= '9') {
          b = b * 10 + (list[i++] - '0');
        }
        count += (b - a + 1);
      } else {
        count += 1;
      }
    }
    return count;
  }

  PeTopology() {
    std::map<std::string, int> e_cluster_map;
    int p_count = 0;
    int detected_cpus = 0;

    // Primera pasada: clasificar cada CPU según su lista L2 compartida.
    std::array<bool, kMaxCpus> is_p{};
    std::array<std::string, kMaxCpus> l2_lists{};

    for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
      std::string path =
          "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index2/shared_cpu_list";
      std::ifstream f(path);
      if (!f.is_open()) continue;
      std::string line;
      std::getline(f, line);
      if (line.empty()) continue;
      detected_cpus = cpu + 1;
      l2_lists[cpu] = line;
      int shared_cnt = count_cpus_in_list(line);
      if (shared_cnt <= 1) {
        is_p[cpu] = true;
        ++p_count;
      } else {
        is_p[cpu] = false;
        if (e_cluster_map.find(line) == e_cluster_map.end()) {
          int idx = static_cast<int>(e_cluster_map.size());
          e_cluster_map[line] = idx;
        }
      }
    }

    if (detected_cpus == 0) {
      // Respaldo genérico: grupos de 4 CPUs.
      num_domains_ = 6;
      for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
        cpus_[cpu] = {static_cast<uint8_t>((cpu / 4) % num_domains_), false};
      }
      return;
    }

    // Asignar dominios:
    //   - Cada clúster L2 compartido (p. ej. 4 E-cores) obtiene 1 dominio propio.
    //   - Los P-cores (L2 privada) se agrupan de 4 en 4 en dominios P dedicados.
    int next_domain = static_cast<int>(e_cluster_map.size());
    int p_seen = 0;
    for (int cpu = 0; cpu < detected_cpus; ++cpu) {
      if (l2_lists[cpu].empty()) {
        cpus_[cpu] = {0, false};
        continue;
      }
      if (is_p[cpu]) {
        int dom = next_domain + (p_seen / 4);
        ++p_seen;
        cpus_[cpu] = {static_cast<uint8_t>(dom % kMaxDomains), true};
      } else {
        int dom = e_cluster_map[l2_lists[cpu]];
        cpus_[cpu] = {static_cast<uint8_t>(dom % kMaxDomains), false};
      }
    }

    int p_domains = (p_count + 3) / 4;
    num_domains_ = static_cast<int>(e_cluster_map.size()) + p_domains;
    if (num_domains_ < 1) num_domains_ = 1;
    if (num_domains_ > kMaxDomains) num_domains_ = kMaxDomains;
  }
};

struct ThreadLocalPeState {
  uint32_t rng = 0x9e3779b9u;
  uint8_t fail_streak = 0;
  uint8_t sample_tick = 0;
  CpuInfo cpu_info{};

  ThreadLocalPeState() {
    // Semilla distinta por hilo mezclando dirección del objeto thread_local.
    uintptr_t addr = reinterpret_cast<uintptr_t>(this);
    rng = static_cast<uint32_t>((addr ^ (addr >> 16)) | 1u);
    cpu_info = PeTopology::instance().current_cpu_info();
  }

  inline uint32_t next_rand() noexcept {
    uint32_t x = rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng = x;
    return x;
  }

  inline CpuInfo refresh_cpu_info() noexcept {
    if ((++sample_tick & 31u) == 0u) {
      cpu_info = PeTopology::instance().current_cpu_info();
    }
    return cpu_info;
  }
};

inline ThreadLocalPeState& thread_pe_state() noexcept {
  thread_local ThreadLocalPeState state;
  return state;
}

}  // namespace ivo_v3

#endif  // MEJORAS_PE_TOPOLOGY_H_
