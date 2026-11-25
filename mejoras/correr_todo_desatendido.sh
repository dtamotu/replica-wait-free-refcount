#!/usr/bin/env bash
# ==============================================================================
# Runner maestro 100 % desatendido (ideal para ejecutar en `multi-user.target`
# con `tmux` o `nohup`, tanto en Intel Core Ultra 9 285 como en Core i5-10400).
#
# Características:
#   - Nunca sobrescribe datos cerrados: escribe en una carpeta nueva con fecha/host
#     (o reanuda si pasas --salida <carpeta_existente>).
#   - Detecta automáticamente el número de hilos (`nproc`):
#       * >= 24 hilos (Core Ultra 9 285): corre las baterías completas con afinidad
#         híbrida P/E (correr_v1_v2.py, correr_v3.py, correr_v4.py) y, en modo
#         `completo`, también `replica/completa/baterias.py`.
#       * < 24 hilos (p. ej. i5-10400 de 12 hilos): usa `correr_portatil_maquina.py`
#         adaptado a `nproc` sin intentar fijar CPUs inexistentes.
#   - Ejecuta sanitizers (ASan+UBSan y TSan) antes de los benchmarks.
#   - Si se ejecuta con `--performance` y hay permisos sudo/root, fija el
#     gobernador de CPU a `performance` y lo restaura automáticamente al salir.
#
# Uso recomendado en Linux nativo (multi-user.target):
#   1) sudo systemctl isolate multi-user.target
#   2) Iniciar sesión en consola y lanzar:
#        nohup bash mejoras/correr_todo_desatendido.sh --modo completo --rondas 5 \
#          > corrida_multiuser.log 2>&1 &
#      (o `--modo mejoras` para ~45 min; añade `--con-paridad` para 10s x 7 nocturno)
#   3) Al terminar: sudo systemctl isolate graphical.target
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ART="$REPO_ROOT/replica/completa/artefacto"

MODO="mejoras"          # mejoras | completo
RONDAS=3
SEGUNDOS=2.0
CON_PARIDAD=0
USAR_PERFORMANCE=0
SALIDA=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --modo)        MODO="$2"; shift 2 ;;
    --rondas)      RONDAS="$2"; shift 2 ;;
    --segundos)    SEGUNDOS="$2"; shift 2 ;;
    --con-paridad) CON_PARIDAD=1; shift ;;
    --performance) USAR_PERFORMANCE=1; shift ;;
    --salida)      SALIDA="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,28p' "$0"
      exit 0 ;;
    *)
      echo "Opción desconocida: $1" >&2; exit 1 ;;
  esac
done

NPROC="$(nproc)"
HOST="$(hostname -s)"
FECHA="$(date +%Y%m%d_%H%M%S)"
if [[ -z "$SALIDA" ]]; then
  SALIDA="$SCRIPT_DIR/resultados_desatendido_${HOST}_${NPROC}h_${FECHA}"
fi
mkdir -p "$SALIDA"

GOV_PREV=""
restaurar_gov() {
  if [[ -n "$GOV_PREV" ]] && command -v cpupower >/dev/null 2>&1; then
    echo "[cleanup] Restaurando gobernador CPU a $GOV_PREV..."
    cpupower frequency-set -g "$GOV_PREV" >/dev/null 2>&1 || true
  fi
}
trap restaurar_gov EXIT

if [[ "$USAR_PERFORMANCE" -eq 1 ]] && command -v cpupower >/dev/null 2>&1; then
  GOV_PREV="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true)"
  echo "[init] Activando gobernador performance (anterior: ${GOV_PREV:-desconocido})..."
  cpupower frequency-set -g performance || echo "[aviso] No se pudo cambiar gobernador (¿falta sudo?)."
fi

echo "================================================================"
echo " CORRIDA DESATENDIDA — $(date)"
echo " Host: $HOST | nproc: $NPROC | Modo: $MODO | Rondas: $RONDAS | Seg: $SEGUNDOS"
echo " Salida: $SALIDA"
echo "================================================================"

# Guardar snapshot de hardware/entorno
{
  date
  uname -a
  lscpu
  free -h
  cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || true
} > "$SALIDA/entorno_corrida.txt"

# 1. Validación de sanitizers (ASan+UBSan y TSan) sobre v1..v4
echo "=== [1/3] Validando ASan+UBSan y TSan en v1, v2, v3, v4 ==="
CXX="${CXX:-g++-12}"
command -v "$CXX" >/dev/null 2>&1 || CXX="g++"
cd "$SCRIPT_DIR"
for test_cpp in test_correctitud.cpp test_correctitud_v3.cpp test_correctitud_v4.cpp; do
  echo "  -> $test_cpp (ASan+UBSan)..."
  "$CXX" -std=c++20 -O2 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer "$test_cpp" -o /tmp/t_asan_desat
  /tmp/t_asan_desat | tee -a "$SALIDA/sanitizers.log"
  echo "  -> $test_cpp (TSan)..."
  "$CXX" -std=c++20 -O2 -g -pthread -fsanitize=thread "$test_cpp" -o /tmp/t_tsan_desat
  setarch "$(uname -m)" -R /tmp/t_tsan_desat | tee -a "$SALIDA/sanitizers.log"
done
rm -f /tmp/t_asan_desat /tmp/t_tsan_desat
echo "=== Sanitizers OK ==="

# 2. Benchmarks de mejoras v1..v4
echo "=== [2/3] Ejecutando benchmarks de mejoras (v1..v4) ==="
if [[ "$NPROC" -ge 24 ]]; then
  echo "  -> Detectada CPU >= 24 hilos: ejecutando baterías híbridas completas (v1/v2, v3, v4)..."
  python3 "$SCRIPT_DIR/correr_v1_v2.py" --salida "$SALIDA/v1_v2" --rondas-rend "$RONDAS" --segundos "$SEGUNDOS"
  python3 "$SCRIPT_DIR/correr_v3.py"    --salida "$SALIDA/v3"    --rondas "$RONDAS" --segundos "$SEGUNDOS"
  python3 "$SCRIPT_DIR/correr_v4.py"    --salida "$SALIDA/v4"    --rondas-rend "$RONDAS" --rondas-adv 5 --segundos "$SEGUNDOS"
else
  echo "  -> Detectada CPU homogénea/compacta ($NPROC hilos): ejecutando batería portátil..."
  python3 "$SCRIPT_DIR/correr_portatil_maquina.py" \
    --salida "$SALIDA/portatil_${NPROC}h" \
    --max-hilos "$NPROC" \
    --rondas-rend "$RONDAS" \
    --rondas-lat "$RONDAS" \
    --rondas-adv 5 \
    --segundos "$SEGUNDOS"
fi

# 3. Réplica del paper original (solo si --modo completo)
if [[ "$MODO" == "completo" ]]; then
  echo "=== [3/3] Ejecutando réplica oficial del paper original (baterias.py) ==="
  SOLO_REP="fig5,fig6,basura"
  if [[ "$NPROC" -ge 24 ]]; then
    SOLO_REP="fig5,fig6,afinidad,latafin,pares,basura"
  fi
  if [[ "$CON_PARIDAD" -eq 1 ]]; then
    SOLO_REP="${SOLO_REP},paridad"
  fi
  python3 "$REPO_ROOT/replica/completa/baterias.py" \
    --motor oficial \
    --salida "$SALIDA/replica_oficial" \
    --raiz "$ART" \
    --shim "$REPO_ROOT/replica/completa/bin/fijar_hilos.so" \
    --jemalloc "$ART/raiz/usr/lib/x86_64-linux-gnu/libjemalloc.so.2" \
    --biblios "$ART/install/lib/x86_64-linux-gnu:$ART/install/folly/lib:$ART/raiz/usr/lib/x86_64-linux-gnu" \
    --solo "$SOLO_REP" \
    --rondas-fig5 "$RONDAS" \
    --rondas-afinidad "$RONDAS" \
    --segundos "$SEGUNDOS"
else
  echo "=== [3/3] Modo 'mejoras' activo (omitida baterias.py; usa --modo completo para incluirla) ==="
fi

echo "================================================================"
echo " CORRIDA DESATENDIDA TERMINADA CON EXITO — $(date)"
echo " Resultados en: $SALIDA"
echo "================================================================"
