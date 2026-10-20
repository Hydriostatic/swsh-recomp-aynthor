# Relatório de aceitação: Pokémon Sword 1.3.2 + DLCs (port nativo)

_Atualizado a cada ciclo. Escopo: Sword 1.3.2. **Pokémon Shield: NÃO TESTADO** (não há dump do jogo)._
_Máquina: RTX 3080, i5-14600KF, Windows 11 26200. Build de referência: o último commit em `feat/swsh-setup` citado em cada linha._

Status de cada critério: **PASSOU**, **FALHOU**, **NÃO TESTADO** ou **BLOQUEADO**, sempre com evidência.

## Estabilidade
| Critério | Status | Evidência |
|---|---|---|
| Zero crashes, deadlocks ou GPU device lost nos cenários aprovados | PASSOU (nos cenários rodados) | dezenas de rodadas de room / groundfloor / stairs / stairs_roundtrip / postwick / room_enter em 20/10 sem crash (`reports/runs/*/summary.json`, status OK). As mortes com código 1 eram externas (taskkill do letsgo-recomp) |
| Três sessões de 60 minutos sem falhas | NÃO TESTADO | — |
| Uma sessão contínua de 4 horas (vazamentos e degradação) | NÃO TESTADO | — |
| Salvar e carregar em cópias dos saves | PASSOU parcial | o jogo salva sozinho na troca de cômodo nas cópias (`build/t132-save-E2` / `build/save-e2` recarregado no térreo); falta o salvar pelo menu |

## Renderização
| Critério | Status | Evidência |
|---|---|---|
| Zero draws pulados por falha ou ausência de pipelines | PASSOU (room, stairs) | `SKIP-total = 0` (`tests/regression/*.json`); baseline DoD em andamento para os outros cenários |
| Zero shaders essenciais com falha não tratada | PASSOU (cenários rodados) | `failed 0`, `pipe-failed 0` nas linhas `[vk]` |
| Zero bugs gráficos relevantes nos cenários aprovados | FALHOU | G1 (não reproduzido, BLOQUEADO), G3 (aberto) |
| G1 corrigido | BLOQUEADO | não reproduz pelo caminho automatizado; precisa do caminho do jogador |
| G2 corrigido | PASSOU | `766239f`; `stairs/G2-snapshot-upright` +0,80 |
| G3 corrigido | FALHOU | aberto |
| G4 corrigido | PASSOU | `80eef24` + `3001d97`; 0 de 8 rodadas com o tapete preto; regressão `stairs/G4-rug-not-black` |
| Zero erros Vulkan ERROR nas operações testadas | FALHOU / BLOQUEADO | TEXINT corrigido (`3001d97`); resta `SYNC-HAZARD-PRESENT-AFTER-WRITE`; a camada do MSYS2 cai sozinha (precisa da camada do Vulkan SDK) |
| Capturas e evidências vinculadas ao build | PASSOU | `meta.json` de cada rodada (hash do exe e do DLL, commit, GPU) |

## Shaders e pipelines
| Critério | Status | Evidência |
|---|---|---|
| 3 execuções com cache fria e 3 com quente por cenário obrigatório | EM ANDAMENTO | `supervisor.py baseline --runs 3` (20/10, 03:37) |
| Compilação assíncrona comprovada por traces | PASSOU parcial | workers + receitas: 187 pipelines pré-aquecidos e 187 adotados na 2ª rodada; contadores de holds e claims na linha `[vk]`. Falta um trace de linha do tempo (Nsight Systems ou WPR) |
| Zero deadlocks ou corrupção de cache | PASSOU (até aqui) | arquivo de receitas com registro verificado (magic, versão, tamanhos, checksum) e layout no nome; nenhum travamento nas rodadas |
| Cache persistente e invalidação funcionando | PASSOU parcial | receitas por hash de conteúdo, hash refeito no draw, invalidação por cópias e por SetShaders (13 por rodada) |
| Stutters de compilação identificados e medidos | PASSOU parcial | holds: 1.391 / 1.335 ms (antes) → 18 / 4 ms (com receitas) na escada |

## Desempenho, 1440p / RTX 3080 / i5-14600KF (meta: 60 fps)
| Critério | Status | Evidência |
|---|---|---|
| FPS médio ≥ 59 | EM ANDAMENTO | baseline DoD |
| Mediana do frametime ≤ 16,7 ms | EM ANDAMENTO | |
| P95 ≤ 16,7 ms | EM ANDAMENTO | quarto a 60 (1 rodada antes): P95 17,2-17,9 ms → provavelmente FALHA por pouco |
| P99 ≤ 20 ms | EM ANDAMENTO | quarto a 60: P99 18,3-19,8 ms com o build do ciclo 1 |
| Zero picos > 50 ms no jogo estabilizado | EM ANDAMENTO | |
| 3 execuções reproduzíveis por cenário | EM ANDAMENTO | |
| Velocidade da lógica intacta | NÃO TESTADO formalmente | o modo 60 (swsh60) muda só o intervalo; falta medir a velocidade do jogo pelo relógio |

## Qualidade e regressões
| Critério | Status | Evidência |
|---|---|---|
| 100% dos testes automatizados obrigatórios passando | PASSOU | `supervisor.py regress`: room e stairs, todas PASSOU (20/10 03:17); difftest main 0 diferenças |
| Zero regressões críticas conhecidas | PASSOU | — |
| Toda correção com teste reproduzível e evidência | PASSOU parcial | G2, G4, SKIP, TEXINT (via validação), KWAIT (timed_wait_test); BARRIER sem teste dedicado |

## Classificação (provisória)
| Área | Classificação |
|---|---|
| Estabilidade funcional | APROVADO COM RESTRIÇÕES (sessões longas NÃO TESTADAS) |
| Fidelidade gráfica | REPROVADO por ora (G1 / G3 abertos; validação incompleta) |
| Correção do runtime e da recompilação | APROVADO COM RESTRIÇÕES (difftest sem memória / desvios; esperas do kernel corrigidas) |
| Shaders e pipelines | APROVADO COM RESTRIÇÕES (falta 3x3 e trace) |
| Desempenho a 60 fps | INCONCLUSIVO (baseline em andamento) |
| Cobertura de testes | APROVADO COM RESTRIÇÕES (poucos cenários: casa e Postwick) |

**Resultado geral: INCONCLUSIVO.** Não há afirmação de jogo sem bugs: Wild Area, batalhas, Dynamax, clima, menus e cinemáticas estão NÃO TESTADOS.
