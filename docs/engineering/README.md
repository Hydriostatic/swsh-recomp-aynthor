# Engenharia do port: como o ciclo funciona

Escopo: **Pokémon Sword 1.3.2 com as DLCs** (Isle of Armor e Crown Tundra). O Shield está **NÃO TESTADO**, porque não há dump do jogo.

## Documentos
- [architecture-audit.md](architecture-audit.md): como o port é montado (recompilador, runtime, renderer).
- [runtime-audit.md](runtime-audit.md): o HLE do Horizon, stubs e fallbacks.
- [graphics-audit.md](graphics-audit.md): NVN → Vulkan, shaders, pipelines e validação.
- [performance-baseline.md](performance-baseline.md): números medidos e o método.
- [known-issues.md](known-issues.md): bugs abertos e fechados, com evidência.

Em cada documento, **fato** quer dizer medido ou lido no código, com o caminho da evidência; **hipótese** quer dizer ainda não comprovado; **não investigado** fica explícito como tal.

## Ferramentas do ciclo
| Ferramenta | Para quê |
|---|---|
| `tools/swsh/scenario.py <cenário>` | Roda um cenário de `tests/scenarios/` e grava log, frametimes, capturas, `meta.json` (hashes, commit, GPU) e `summary.json` em `reports/runs/<cenário>/<data>-<rótulo>/`. Perfis: `--profile 1440p30/1440p60/1080p30/...`. Cache: `--cache warm` (`build/testcache`) ou `--cache cold` (pasta vazia, via `LARECOMP_CACHE_DIR`). Validação: `--validation [sync,gpuav]` |
| `tools/swsh/stats.py` | Percentis por nearest-rank sobre os presents da janela; lows de FPS derivados do frametime (1% low = 1000/P99); contadores da linha `[vk]` |
| `tools/swsh/imgcheck.py` | Checagens de imagem para as regressões: orientação (G2), fração preta numa região (G4), luma e SSIM |
| `tools/agent/supervisor.py` | Estado em `.agent/`. Subcomandos: `regress`, `baseline`, `next`, `record` e `agent` (execuções do Claude Code sem interface, com timeout, limite de tentativas e detecção de estagnação) |
| `tests/regression/*.json` | Checagens por cenário. Uma checagem com `"expect": "fail"` documenta um bug aberto e passa a ser exigida quando o bug é fechado |

## Regras
- **Saves e cache do jogador:** os testes nunca usam `save-sword-132` nem `cache/` diretamente. Copiam o save para cada execução, e a cache quente é uma cópia própria.
- **Saves de cenário:** ficam em `build/scenario-saves/`, fora do git, porque são dados do jogo. As evidências com imagens ficam em `reports/runs/`, também fora do git; os relatórios citam os caminhos.
- **Processos:** os testes encerram só o processo que iniciaram, e esperam se outra instância do Sword estiver aberta, porque o jogador pode estar jogando.
- **Formato:** relatório de ciclo em `reports/cycles/NNN.md`; commits em inglês, Conventional Commits, sem linha de coautoria.
