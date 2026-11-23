#!/usr/bin/env python3
"""analyze.py — lê results/{host,vm}, calcula medianas de 3 execuções,
gera figuras (figs/*.pdf), tabela e macros LaTeX (../paper/resultados.tex).
Uso: python3 analyze.py   (requer matplotlib)"""
import csv, glob, json, os, statistics as st
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")
FIG = os.path.join(HERE, "..", "paper", "figs")
TEX = os.path.join(HERE, "..", "paper", "resultados.tex")
os.makedirs(FIG, exist_ok=True)
ENVS = [e for e in ("host", "vm") if os.path.isdir(os.path.join(RES, e))]
LABEL = {"host": "macOS nativo", "vm": "Linux em VM (OrbStack)"}
STYLE = {"host": dict(color="#1f5fbf", marker="o"), "vm": dict(color="#d9622b", marker="s")}


def load(env):
    vals = defaultdict(list)
    for f in sorted(glob.glob(os.path.join(RES, env, "run*.csv"))):
        for row in csv.reader(open(f)):
            if len(row) == 3:
                vals[(row[0], row[1])].append(float(row[2]))
    med = {k: st.median(v) for k, v in vals.items()}
    disk = defaultdict(list)
    for f in glob.glob(os.path.join(RES, env, "fio_*_run*.json")):
        name = os.path.basename(f)[4:].rsplit("_run", 1)[0]
        try:
            j = json.load(open(f))["jobs"][0]
        except Exception:
            continue
        side = j["write"] if "write" in name else j["read"]
        disk[(name, "iops")].append(side["iops"])
        disk[(name, "mbs")].append(side["bw"] / 1024)
        disk[(name, "lat_us")].append(side["clat_ns"]["percentile"].get("50.000000", side["clat_ns"]["mean"]) / 1000)
    med.update({("disk_" + k[0], k[1]): st.median(v) for k, v in disk.items()})
    return med


D = {e: load(e) for e in ENVS}


def series(env, test):
    pts = sorted((float(k[1]), v) for k, v in D[env].items() if k[0] == test)
    return [p[0] for p in pts], [p[1] for p in pts]


def amdahl_fit(ns, speedups):
    best = min((sum((s - 1 / ((1 - p) + p / n)) ** 2 for n, s in zip(ns, speedups)), p)
               for p in [i / 1000 for i in range(1001)])
    return best[1]


macros = {}
def M(name, val, fmt="{:.2f}"):
    macros[name] = fmt.format(val).replace(".", "{,}")

# ---------- Figura 1: escada de cache ----------
plt.figure(figsize=(6.4, 3.4))
for e in ENVS:
    x, y = series(e, "cache")
    plt.plot(x, y, label=LABEL[e], lw=1.5, ms=3.5, **STYLE[e])
plt.xscale("log", base=2); plt.yscale("log")
ticks = [4, 16, 64, 256, 1024, 4096, 16384, 65536, 262144]
plt.xticks(ticks, ["4K", "16K", "64K", "256K", "1M", "4M", "16M", "64M", "256M"])
plt.xlabel("Tamanho do conjunto de trabalho (bytes)"); plt.ylabel("Latência por acesso (ns)")
plt.grid(alpha=.3, which="both"); plt.legend(); plt.tight_layout()
plt.savefig(os.path.join(FIG, "cache.pdf"))

# ---------- Figura 2: escalabilidade + Amdahl ----------
fig, axs = plt.subplots(1, 2, figsize=(6.4, 3.0), sharey=True)
for ax, test, title in ((axs[0], "scale_cpu", "Computação (FMA)"), (axs[1], "scale_mem", "Memória (triad)")):
    for e in ENVS:
        n, t = series(e, test)
        if not n or min(t) <= 0:
            print(f"aviso: {test} em {e} tem tempo zero; rode de novo o run.sh")
            continue
        s = [t[0] / ti for ti in t]
        p = amdahl_fit(n, s)
        tag = ("Cpu" if test == "scale_cpu" else "Mem") + e.capitalize()
        M("p" + tag, p); M("speedupMax" + tag, max(s)); M("nMax" + tag, n[s.index(max(s))], "{:.0f}")
        ax.plot(n, s, label=f"{LABEL[e]}", lw=1.5, ms=3.5, **STYLE[e])
        ax.plot(n, [1 / ((1 - p) + p / k) for k in n], ls="--", lw=1, color=STYLE[e]["color"],
                label=f"Amdahl p={p:.2f}")
    ax.set_title(title, fontsize=9); ax.set_xlabel("Threads"); ax.grid(alpha=.3)
    ax.legend(fontsize=6.5)
axs[0].set_ylabel("Speedup")
plt.tight_layout(); plt.savefig(os.path.join(FIG, "scaling.pdf"))

# ---------- Figura 3: roofline ----------
plt.figure(figsize=(6.4, 3.4))
AI = {"triad": 2 / 24, "ikj": 2 / 24, "ijk": 2 / 136}  # FLOP/byte (hipóteses na Seção 3)
xs = [2 ** (i / 4) for i in range(-30, 25)]
for e in ENVS:
    d = D[e]
    peak, bw = d.get(("flops", "1thread_gflops")), d.get(("mem", "triad_gbs"))
    if not peak or not bw:
        continue
    plt.plot(xs, [min(peak, bw * x) for x in xs], lw=1.5, color=STYLE[e]["color"], label=f"Teto {LABEL[e]}")
    pts = {"triad": bw * AI["triad"], "ikj": d.get(("matmul", "ikj_gflops")), "ijk": d.get(("matmul", "ijk_gflops"))}
    for k, v in pts.items():
        if v:
            plt.scatter(AI[k], v, color=STYLE[e]["color"], marker=STYLE[e]["marker"], s=22, zorder=3)
            if e == ENVS[0]:
                plt.annotate(k, (AI[k], v), textcoords="offset points", xytext=(5, -10), fontsize=7)
plt.xscale("log", base=2); plt.yscale("log")
plt.xlabel("Intensidade aritmética (FLOP/byte)"); plt.ylabel("Desempenho (GFLOP/s)")
plt.grid(alpha=.3, which="both"); plt.legend(fontsize=7); plt.tight_layout()
plt.savefig(os.path.join(FIG, "roofline.pdf"))

# ---------- Tabela principal + macros ----------
ROWS = [  # (rótulo, chave, unidade, maior_e_melhor)
    ("Latência L1 (32 KiB)", ("cache", "32"), "ns", False),
    ("Latência 4 MiB", ("cache", "4096"), "ns", False),
    ("Latência DRAM (256 MiB)", ("cache", "262144"), "ns", False),
    ("Laço com desvio, aleatório", ("branch", "unsorted"), "ns/elem", False),
    ("Laço com desvio, ordenado", ("branch", "sorted"), "ns/elem", False),
    ("Page fault (1º toque)", ("mem", "fault_ns_per_kib"), "ns/KiB", False),
    ("Banda de memória (triad)", ("mem", "triad_gbs"), "GB/s", True),
    ("Pico FMA, 1 thread", ("flops", "1thread_gflops"), "GFLOP/s", True),
    ("Matriz ijk", ("matmul", "ijk_gflops"), "GFLOP/s", True),
    ("Matriz ikj", ("matmul", "ikj_gflops"), "GFLOP/s", True),
    ("Disco seq. leitura 1M", ("disk_seqread", "mbs"), "MB/s", True),
    ("Disco seq. escrita 1M", ("disk_seqwrite", "mbs"), "MB/s", True),
    ("Disco aleat. 4K QD1", ("disk_randread_qd1", "iops"), "IOPS", True),
    ("Disco aleat. 4K QD32", ("disk_randread_qd32", "iops"), "IOPS", True),
]
lines = [r"\begin{tabular}{lrrrr}", r"\hline",
         r"\textbf{Métrica} & \textbf{Unid.} & \textbf{macOS} & \textbf{VM Linux} & \textbf{Custo VM} \\ \hline"]
for label, key, unit, higher in ROWS:
    h, v = D.get("host", {}).get(key), D.get("vm", {}).get(key)
    if h is None and v is None:
        continue
    cost = ""
    if h and v:
        c = (h / v - 1) if higher else (v / h - 1)
        cost = f"{c*100:+.1f}\\%".replace(".", "{,}")
    def f(x):
        if x is None: return "--"
        return f"{x:,.0f}".replace(",", ".") if x >= 100 else f"{x:.2f}".replace(".", "{,}")
    lines.append(f"{label} & {unit} & {f(h)} & {f(v)} & {cost} \\\\")
lines += [r"\hline", r"\end{tabular}"]
open(os.path.join(FIG, "..", "tabela.tex"), "w").write("\n".join(lines) + "\n")

for e in ENVS:
    d, E = D[e], e.capitalize()
    for name, key in (("Triad", ("mem", "triad_gbs")), ("Peak", ("flops", "1thread_gflops")),
                      ("Ijk", ("matmul", "ijk_gflops")), ("Ikj", ("matmul", "ikj_gflops")),
                      ("Unsorted", ("branch", "unsorted")), ("Sorted", ("branch", "sorted")),
                      ("Dram", ("cache", "262144")), ("Lone", ("cache", "32")),
                      ("Fault", ("mem", "fault_ns_per_kib")), ("Qdone", ("disk_randread_qd1", "iops")),
                      ("Qdtt", ("disk_randread_qd32", "iops"))):
        if key in d:
            M(name.lower() + E, d[key])
    if ("branch", "sorted") in d:
        M("branchRatio" + E, d[("branch", "unsorted")] / d[("branch", "sorted")])
    if ("mem", "triad_gbs") in d and ("flops", "1thread_gflops") in d:
        M("ridge" + E, d[("flops", "1thread_gflops")] / d[("mem", "triad_gbs")])
    if ("scale", "ncpu") in d:
        M("ncpu" + E, d[("scale", "ncpu")], "{:.0f}")

with open(TEX, "w") as fh:
    fh.write("% gerado por bench/analyze.py — não editar à mão\n")
    for k, v in sorted(macros.items()):
        fh.write(f"\\newcommand{{\\{k}}}{{{v}}}\n")
print("ok:", ENVS, len(macros), "macros")
