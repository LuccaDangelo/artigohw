#!/usr/bin/env bash
# run.sh — roda a suíte 3x no ambiente atual e salva em results/<ambiente>/
# Uso:  ./run.sh host      (no macOS)
#       ./run.sh vm        (dentro da VM Linux do OrbStack)
set -euo pipefail
ENV_NAME="${1:?informe o nome do ambiente: host ou vm}"
cd "$(dirname "$0")"
OUT="results/$ENV_NAME"
mkdir -p "$OUT"

# ---------- Eixo 1: identificação do ambiente ----------
{
  echo "date: $(date -u +%FT%TZ)"
  echo "uname: $(uname -a)"
  echo "compiler: $(clang --version | head -1)"
  echo "flags: -O2 -pthread"
  if [ "$(uname)" = "Darwin" ]; then
    sysctl -n machdep.cpu.brand_string | sed 's/^/cpu: /'
    for k in hw.ncpu hw.physicalcpu hw.perflevel0.physicalcpu hw.perflevel1.physicalcpu \
             hw.perflevel0.l1dcachesize hw.perflevel0.l2cachesize \
             hw.perflevel1.l1dcachesize hw.perflevel1.l2cachesize \
             hw.cachelinesize hw.pagesize hw.memsize; do
      echo "$k: $(sysctl -n $k 2>/dev/null || echo n/a)"
    done
    sw_vers | sed 's/^/os_/'
  else
    lscpu
    echo "nproc: $(nproc)"
    grep MemTotal /proc/meminfo
    echo "pagesize: $(getconf PAGESIZE)"
    for f in /sys/devices/system/cpu/cpu0/cache/index*/size; do echo "$f: $(cat $f)"; done 2>/dev/null || true
    cat /etc/os-release | head -2
  fi
} > "$OUT/env.txt"

clang -O2 -pthread -o bench bench.c -lm

# ---------- Eixos 2, 3, 4 e 6: microbenchmarks (3 execuções) ----------
for r in 1 2 3; do
  echo ">> [$ENV_NAME] execução $r/3 (≈1 min)"
  ./bench all > "$OUT/run$r.csv"
done

# ---------- Eixo 5: disco (fio, --direct=1) ----------
if [ "${SKIP_DISK:-0}" = "1" ]; then
  echo "SKIP_DISK=1: mantendo os resultados de disco já coletados"
elif command -v fio >/dev/null; then
  if [ "$(uname)" = "Darwin" ]; then ENG=posixaio; DIR="${TMPDIR:-/tmp}"; else ENG=libaio; DIR=/var/tmp; fi
  FILE="$DIR/fio_bench.dat"
  for r in 1 2 3; do
    echo ">> [$ENV_NAME] disco $r/3 (≈1 min)"
    for job in "seqread read 1M 1" "seqwrite write 1M 1" "randread_qd1 randread 4k 1" "randread_qd32 randread 4k 32"; do
      set -- $job
      fio --name="$1" --filename="$FILE" --rw="$2" --bs="$3" --iodepth="$4" \
          --ioengine=$ENG --direct=1 --size=512M --runtime=15 --time_based \
          --output-format=json > "$OUT/fio_${1}_run$r.json" 2>/dev/null \
      || { echo "   aviso: $1 falhou com direct=1; repetindo sem" ;
           fio --name="$1" --filename="$FILE" --rw="$2" --bs="$3" --iodepth="$4" \
               --ioengine=$ENG --direct=0 --size=512M --runtime=15 --time_based \
               --output-format=json > "$OUT/fio_${1}_run$r.json"; echo "direct0: $1" >> "$OUT/env.txt"; }
    done
  done
  rm -f "$FILE"
else
  echo "fio não encontrado: eixo de disco pulado (instale com 'brew install fio' ou 'apt install fio')"
fi

echo "Pronto: $OUT"
