# Problemas conhecidos

O estado vivo fica em `.agent/backlog.json` (`python tools/agent/supervisor.py status`); aqui fica o resumo com as evidências.

## Abertos
| ID | Prioridade | Sintoma | Evidência / reprodução | Situação |
|---|---|---|---|---|
| TITLE-EXIT | P0 | O cenário `title` sai com código 1 por volta do quadro 1966, sem FATAL nem exceção no log | `reports/runs/title/20261020-004409-base-1440p30-warm-1` | a investigar |
| SKIP | P1 | Draws pulados enquanto o programa ou o pipeline compila (2.037 numa rodada com cache quente) | `graphics-audit.md`; `tests/regression/stairs.json` SKIP-window | a investigar |
| VKVAL | P1 | Validation layers ainda não rodadas nos cenários | — | NÃO TESTADO |
| G4 | P1 | Tapete preto no térreo ao chegar pela escada; sai certo quando carregado direto | `tests/regression/stairs.json` G4-rug-not-black (fração preta 1,0 na região do tapete) | aberto |
| G1 | P1 | Manchas brancas ao entrar andando no quarto | `docs/DIAGNOSTICO-SWSH.md` | aberto, sem reprodução automática |
| G3 | P1 | Arquibancada piscando no título (o desfoque alterna) | `docs/DIAGNOSTICO-SWSH.md` | aberto |
| DYNRES | P2 | O jogo desenha a cena a 80% no mundo aberto (1536x864 em alvo 1920x1080) | F9 em `build/logs/sword-20261018-145705.log` | comportamento do jogo; paliativo: render 1440p |
| PERF60 | P2 | 60 fps não ficam lisos | `performance-baseline.md` | a medir a 1440p |
| STAB | P2 | Sessões longas e salvar/carregar sem teste formal | — | NÃO TESTADO |
| FPS120 | P3 | 120 fps | — | depois de 60 estáveis |

## Fechados (com evidência)
| ID | Causa | Correção | Regressão |
|---|---|---|---|
| G2 | `CopyTextureToTexture` com `MIRROR_Y` ignorado | `766239f` | `tests/regression/stairs.json` |
| "posterização 1080p" | Não era bug: a redução da imagem no visualizador | — | — |

## Fora do escopo / NÃO TESTADO
- Pokémon Shield (sem dump).
- Wild Area, batalhas, Dynamax, clima, ciclo dia/noite, menus e inventário, cinemáticas.
