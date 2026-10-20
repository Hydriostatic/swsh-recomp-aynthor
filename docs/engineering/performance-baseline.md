# Baseline de desempenho

## Máquina e método
- **Hardware:** RTX 3080 (`[vk] gpu 0: NVIDIA GeForce RTX 3080 (api 1.4.351)`), i5-14600KF, Windows 11 (build 26200).
- **Build:** runtime do commit `dcdeca3` com as mudanças do ciclo 0 (sem o KWAIT); Sword 1.3.2.
- **Execução:** `tools/swsh/scenario.py`, em janela (não em tela cheia), SMAA ligado, FSR1, render thread ligada e `vertex_read = auto`. Cache quente = `build/testcache`, semeada da cache do jogador; cache fria = pasta vazia (`LARECOMP_CACHE_DIR`). A cache do próprio driver da NVIDIA continua quente nas duas.
- **Janela de medição:** frametime só nos presents da janela de cada cenário (`tests/scenarios/*.json`, campo `measure`), depois de o mundo carregar. Percentis por nearest-rank.
- **Ressalva:** são **1 execução por combinação** (20/10, madrugada). A Definition of Done pede 3; o lote de 3 vem por `supervisor.py baseline`.

## Resultados (20/10/2026, `reports/runs/*/2026102000*-base-*`)
| cenário | perfil | cache | status | FPS | mediana | P95 | P99 | máx | >50 ms | draws pulados (rodada / janela) | compilações | tradução | espera dos draws |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| room | 1440p30 | quente | OK | 29,37 | 33,33 | 33,70 | 34,11 | 490 | 1 | 1.164 / 0 | 812 | 759 ms | 174 ms |
| room | 1440p30 | fria | OK | 29,36 | 33,35 | 34,18 | 35,98 | 501 | 1 | 2.858 / 0 | 814 | 981 ms | 526 ms |
| room | 1440p60 | quente | OK | 58,62 | 16,58 | 19,63 | 23,99 | 499 | 1 | 1.261 / 0 | 812 | 788 ms | 181 ms |
| groundfloor | 1440p30 | quente | OK | 29,31 | 33,32 | 36,18 | 42,72 | 532 | 3 | 1.341 / 0 | 1.007 | 958 ms | 194 ms |
| stairs | 1440p30 | quente | OK | 27,25 | 33,33 | 34,33 | 38,86 | 466 | 1 | 1.798 / **16** | 1.388 | 1.134 ms | 269 ms |
| stairs_roundtrip | 1440p30 | fria | OK | 26,82 | 33,35 | 33,80 | 38,59 | 462 | 3 | 3.713 / 0 | 1.349 | 1.231 ms | 574 ms |
| postwick | 1440p30 | quente | OK | **19,02** | 49,35 | 58,36 | 62,80 | 1.023 | **243** | 2.556 / 0 | 1.177 | 1.240 ms | 260 ms |
| postwick | 1440p60 | quente | TIMEOUT (limite do harness curto demais, corrigido) | — | | | | | | | | | |
| title | 1440p30 | quente | EXIT 1 (morte externa, ver known-issues) | — | | | | | | | | | |
| stairs_roundtrip | 1440p30 | quente | EXIT 1 (idem) | — | | | | | | | | | |

O "máx" de ~0,5 s em todas as rodadas é o primeiro present da janela, ainda na carga. O P99 é a medida de suavidade.

## Leitura (fatos)
- Os interiores a 30 fps estão no ritmo, e a 60 fps (quarto) quase: P95 de 19,6 ms e P99 de 24 ms. Isso falha os critérios de P95 <= 16,7 e P99 <= 20.
- **O mundo aberto (Postwick) a 1440p não segura nem 30.** Ainda não está separado se a causa é CPU ou GPU (próximo passo: `LARECOMP_GPUPROF_AT`, `LARECOMP_PROF_SVC` e o perfil por thread).
- Os draws pulados se concentram no boot, nas cargas e nas transições, e dobram com a cache fria. Só na escada aparecem dentro da janela medida.
- Com a cache quente, cada boot ainda traduz todos os programas (0,8-1,4 s por rodada).
- Esperas curtas das threads do jogo levam ~16 ms em vez de 1-8 ms (`tests/runtime/timed_wait_test.c`): é o item KWAIT, e pode pesar no P99.
