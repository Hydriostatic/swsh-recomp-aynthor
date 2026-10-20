# Ciclo 3 (interrompido): mundo aberto a 60 fps

Relatório completo: `reports/cycles/003.md`. Parado em 20/10/2026 ~08:15 porque o usuário precisou usar o PC.

## Estado do código (IMPORTANTE)
Último commit: `805c64e`. **Não commitado** (no working tree, compila; o build em `build/v132` já contém tudo):
- `runtime/nvn/vulkan/exec.c`: cópias de buffer inline (`gpu_may_touch`), readbacks adiados (`g_rb`,
  `readback_sync_range`, `readbacks_done`, superseded), estatísticas novas.
- `runtime/nvn/vulkan/textures.c`: `readback_sync_range` no `upload_texture`.
- `runtime/nvn/vulkan/renderthread.c`: `LARECOMP_RT_PROF`.
- `runtime/sampler.c` (novo, não rastreado) + `runtime/kernel.c`: `LARECOMP_SAMPLE`.
- `tests/scenarios/openworld.json` e `stairs_rug.json` (não rastreados), `.agent/test-matrix.json`.
- `reports/baseline-dod-run.txt`, `reports/stab60-1.txt` (não rastreados, saídas de rodadas).

Antes de commitar: rodar as regressões e a checagem visual abaixo (passos 1-2).

## Números atuais (1440p60, mundo aberto, antes da batalha)
| versão | fps |
|---|---|
| 805c64e (flush em toda CopyBufferToBuffer) | ~33 |
| + cópias inline | ~46-47 |
| + readbacks adiados / superseded | ~46-49 (espera da GPU 545 -> 5 ms a cada 2 s) |

Render thread ocupada ~40%; o jogo quase não espera por ela (`[rtprof]`). O gargalo agora é a CPU das threads do
jogo (16/17/18 trabalham em cadeia, pelo `LARECOMP_PROF_SVC`).

## Como continuar
1. **Correção das mudanças:** `python tools/agent/supervisor.py regress` e comparar o quadro do openworld (shot 1030)
   com `--env LARECOMP_COPY_FLUSH=1 --env LARECOMP_READBACK_SYNC=1` (imgcheck / SSIM, relógio fixo). Se ok, commitar
   em partes: `perf(vulkan): ...copies inline`, `perf(vulkan): deferred readbacks`, `feat(tools): rt prof + sampler`.
2. **Perfil da CPU do jogo:** 
   `python tools/swsh/scenario.py openworld --profile 1440p60 --label ow-sample --env LARECOMP_SAMPLE=1450:1650 --env LARECOMP_SAMPLE_OUT=D:/projects/swsh-recomp/reports/profiling/ow-sample.txt`
   (a janela 1450:1800 não terminou antes de o harness sair: renderer_game_frame anda atrás do contador de presents).
   Melhoria a fazer no sampler: escrever o relatório também na saída do processo. Depois, olhar as funções f_<addr>
   do topo inclusivo das threads 16/17/18 (HLE caro? spinlocks? funções do lifter lentas?).
3. **G3 / 60MODE-ANIM:** o usuário ainda vê a plateia piscando a 60 fps. Fazer a animação da plateia andar a 30 Hz
   no modo 60 (achar o contador de quadros da animação - FNPROF / ganchos em runtime/fnhook.c).
4. Pendências antigas: LEAK (~4-5 MB/min), PRESENT-SYNC, VKVAL (camada do LunarG SDK), difftest de desvios e sdk,
   STAB com sessões reais do jogador (não com loop em quadrado - o usuário descartou).

## Cuidados
- Ao medir, conferir pela captura que o quadro é o overworld e não uma batalha (um Pokémon selvagem inicia batalha
  ~45 s depois do load; no modo 60 por volta do present 2200).
- `"contaminated": true` = o letsgo-recomp estava rodando junto; repetir A/Bs com a máquina livre.
- Heredocs do Git Bash transformam `\\n` dentro de scripts Python em quebra de linha real em strings C: editar C com
  a ferramenta Edit ou scripts gravados no scratchpad.
- `make ... | grep` não para o script quando a compilação falha: confira se não houve `error` antes de medir.
