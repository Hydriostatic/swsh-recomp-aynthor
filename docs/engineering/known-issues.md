# Problemas conhecidos

O estado vivo fica em `.agent/backlog.json` (`python tools/agent/supervisor.py status`); aqui fica o resumo com as evidências.

## Abertos
| ID | Prioridade | Sintoma | Evidência / reprodução | Situação |
|---|---|---|---|---|
| VKVAL | P1 | Validation layers ainda não rodadas nos cenários | — | NÃO TESTADO |
| G1 | P1 | Manchas brancas ao entrar andando no quarto | `docs/DIAGNOSTICO-SWSH.md`; `tests/scenarios/room_enter.json` não reproduz (nem com barrier desligado nem no modo antigo de pular draws) | BLOQUEADO: precisa do caminho do jogador |
| G3 | P2 | Arquibancada piscando no título | só no **modo 60 fps**: a plateia (passe 10) troca de quadro de animação a cada quadro (animação contada em quadros, dobrada pelo modo 60); a 30 fps não ocorre. `tests/regression/title60.json` | não é erro do renderer; ajuste específico do modo 60 (60MODE-ANIM) |
| DYNRES | P2 | O jogo desenha a cena a 80% no mundo aberto (1536x864 em alvo 1920x1080) | F9 em `build/logs/sword-20261018-145705.log` | comportamento do jogo; paliativo: render 1440p |
| PERF60 | P2 | 60 fps não ficam lisos | `performance-baseline.md` | a medir a 1440p |
| STAB | P2 | Sessões longas e salvar/carregar sem teste formal | — | NÃO TESTADO |
| FPS120 | P3 | 120 fps | — | depois de 60 estáveis |

## Fechados (com evidência)
| ID | Causa | Correção | Regressão |
|---|---|---|---|
| G2 | `CopyTextureToTexture` com `MIRROR_Y` ignorado | `766239f` | `tests/regression/stairs.json` |
| "posterização 1080p" | Não era bug: a redução da imagem no visualizador | — | — |
| G4 | Programas de shader em cache por **endereço**: o jogo reaproveita a memória de shaders entre áreas e o shader novo recebia a tradução do antigo. Isolado: chave por conteúdo sem receitas = correto; chave por endereço = preto | `80eef24` (programas gráficos e de compute por hash de conteúdo) | `tests/regression/stairs.json` G4-rug-not-black |
| SKIP | Orçamento de 20 ms por quadro e depois pulava; dispatches de compute pulavam sem esperar | `80eef24` (espera com teto por draw + reivindicação do job + receitas) | `SKIP-total` = 0 em stairs e room |
| BARRIER | `CMD_BARRIER` gravado e ignorado | `80eef24` | regressões gerais; A/B sem custo |
| KWAIT | Esperas temporizadas de 1-8 ms duravam ~16 ms (MinGW + Windows) | `27006f3` | `tests/runtime/timed_wait_test.c` |
| TITLE-EXIT | Mortes com código 1: `taskkill /IM lasrecomp.exe` de outro projeto (letsgo `tools/trace_run.sh`) derrubava todo runtime da máquina | `16ee01b` (nosso script) + `scenario.py` repete em EXTERNAL-KILL | — (o script do letsgo-recomp ainda faz isso) |

## Fora do escopo / NÃO TESTADO
- Pokémon Shield (sem dump).
- Wild Area, batalhas, Dynamax, clima, ciclo dia/noite, menus e inventário, cinemáticas.
