# Quanto custa a camada de virtualização? — MacBook Air (Apple Silicon)

Perfil de desempenho em 6 eixos, macOS nativo × VM Linux (OrbStack).

```
paper/   artigo no template SBC (abrir no Overleaf)
bench/   bench.c (microbenchmarks), run.sh (coleta), analyze.py (medianas, figuras, tabelas)
```

## 1. Host (macOS) — ~6 min

Feche os outros apps e deixe o Mac na tomada.

```bash
xcode-select --install        # só se ainda não tiver o clang
brew install fio
cd artigo/bench
./run.sh host
```

## 2. VM Linux (OrbStack) — ~6 min

```bash
orb create ubuntu bench
orb -m bench                  # entra na VM; /Users do Mac já aparece montado
sudo apt update && sudo apt install -y clang fio python3-matplotlib
cd /Users/$USER/caminho/para/artigo/bench
./run.sh vm
```

## 3. Análise (ainda dentro da VM)

```bash
python3 analyze.py
```

Gera `paper/figs/*.pdf`, `paper/tabela.tex` e `paper/resultados.tex`.
Os números entram sozinhos no artigo. Depois é só substituir os trechos
vermelhos `[PREENCHER: ...]` pela interpretação.

## 4. Overleaf

Compacte a pasta `paper/` e use "New Project → Upload Project".
Arquivo principal: `main.tex`.

## Protocolo

- 3 execuções por medida e relato da mediana
- mesmo compilador (`clang -O2 -pthread`) nos dois ambientes
- `fio --direct=1`, arquivo de 512 MiB no disco nativo de cada ambiente
- dados brutos em `bench/results/{host,vm}/`
