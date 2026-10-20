# Ciclo 3 (concluído): mundo aberto a 60 fps

Relatório completo: `reports/cycles/003.md`. Parado às 08:15 e retomado às 12:20 de 20/10/2026.

## Estado do código
Tudo commitado: `2016be2` (FP via MXCSR), `8a642f1` (cópias inline + readbacks adiados), `e2ef82f` (RT_PROF + sampler),
`434d823` (cenários). Não rastreados e sem importância: `reports/baseline-dod-run.txt`, `reports/stab60-1.txt`.

## Números atuais (1440p60, mundo aberto parado, antes da batalha)
| versão | fps |
|---|---|
| 805c64e | ~33 |
| + cópias inline / readbacks adiados (8a642f1) | ~47 |
| + modo de arredondamento via MXCSR (2016be2) | **60** (3 rodadas: 60,00 / 59,99 / 59,82; P95 18,9 ms, P99 20,2 ms) |

## Como continuar
0. (FEITO 12:20-13:05: passos 1 e 2 abaixo; ver reports/cycles/003.md "Retomada".) Próximo de verdade: medir o
   mundo aberto em movimento (bicicleta / andando) e o P95; render thread ~90% ocupada agora.
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
