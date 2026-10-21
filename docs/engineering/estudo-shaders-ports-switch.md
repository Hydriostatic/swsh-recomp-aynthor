# Estudo: como os ports recompilados do Switch tratam a compilação de shaders

_21/10/2026. Fonte: lista de recompilações estáticas do Switch em recompsoftware.com/games e o código / documentos
de cada projeto (clones rasos em `reference/`, ignorado pelo git). Só conta o que está no código ou num documento
com medição._

## O problema
Numa instalação nova, cada shader e cada combinação de estado (pipeline) aparece pela primeira vez durante o jogo e
o driver precisa compilá-los: no nosso teste de "primeira vez" (`build/jogar-sword-primeira-vez.bat`), 9,9 s de
travadas em 68 s de jogo (carregamentos de 0,3-1,4 s e dezenas de 50-150 ms a cada objeto / efeito novo).

## O que cada projeto faz

| Projeto (jogo) | Commit | Backend | Estratégia de shaders / pipelines |
|---|---|---|---|
| **ssbu-recomp / OpenSmash** (Smash Ultimate) | `a61cf6f` | Vulkan | **A mais completa.** Extrai os contêineres BNSH dos arquivos do jogo (`tools_vk/bnsh_extract.py`, depois o próprio runtime: `nvn_vk_store.c`); uma **preparação em segundo plano** (`nvn_vk_prep`, sessão 10) traduz cada estágio e compila uma **pipeline library por estágio** (`SSBU_VK_GPL=2`, com um layout de descritores "universal" que serve a qualquer par), na prioridade mais baixa, pausando em luta; no draw só **liga** as partes. Grava o cache de pipelines do driver ao fim. Medido no README: **0 holds em quadros de luta** na primeira execução (NVIDIA e Intel) depois da preparação; a preparação leva ~15 min de jogo na primeira vez e ~2,5 GB de disco. Também compila no `nvnProgramSetShaders` (antes do draw) e prevê chaves de pipeline. |
| **SuperMarioWonderRecomp** | `665b2f9` | Vulkan (MoltenVK / Android) | **Tradução AOT de tudo**: `tools/maxwell/containers.py` acha todos os shaders do romfs (SARC / BFSHA / BNSH / VFXB / BAHS: 47.797 estágios, 12.717 blocos distintos) e o tradutor do Ryujinx (MIT), rodado offline, gera SPIR-V de todos; o runtime usa um cache por hash do bloco. As variantes que dependem do draw (tipo dos atributos de vértice, tipo das saídas, alpha test...) são assadas com valores padrão; as outras são registradas e assadas depois. Os **pipelines** ainda são criados durante o jogo, com receitas pré-aquecidas na execução seguinte e cache de pipelines em disco ("a pipeline cache on disk halves the course's hitches"). |
| **WindowSmash** (Smash, D3D11) | `06f5498` | Direct3D 11 | Compila na primeira vez que cada shader é usado; um passo opcional **`launchers\Prepare Shaders.bat`** compila todos antes ("a new effect can hitch once"). Cache AOT com data de uso (`tools/shaderaot.c`). |
| **HWDER** (Hyrule Warriors DE) | `2a85d50` | Vulkan | Emula o comando da GPU Maxwell (pushbuffer) e tem tradutor SASS → SPIR-V próprio **com grafo de fluxo de controle** (não um laço de despacho); "pipelines are created asynchronously and cached on disk". Sem pré-compilação a partir dos arquivos. |
| **mk8-recomp** (Mario Kart 8 DX) | `b312299` | (base suyu) | Receitas de pipeline persistentes; modo de diagnóstico "skip-and-collect" para shaders que faltam. |
| **LASRecomp / SkyRecomp / EoWRecomp** (família do nosso) | `da35b83` / `c8de7ab` / `e022e55` | Vulkan | Tradução e pipelines no primeiro uso, cache em disco; as melhorias de receitas / pré-aquecimento que temos vieram do mk8 / OpenSmash. |

## Comparação com o nosso estado (commit `0ab836a`)
| Técnica | Quem tem | Nós |
|---|---|---|
| Receitas: lista dos pipelines já vistos, pré-aquecidos | OpenSmash, mk8, SMW | **sim** (`pipeline_recipes_v2`) |
| Compilar no SetShaders, antes do draw | OpenSmash | parcial (receitas no SetShaders; programas sem receita só no draw) |
| Pré-varredura do quadro (programas / pipelines em paralelo) | — | **sim** (`0ab836a`) |
| Achar todos os shaders nos arquivos do jogo | OpenSmash, SMW | **não** |
| Traduzir tudo antes (AOT) | SMW (offline), OpenSmash (em segundo plano) | **não** |
| Pipeline library por estágio + ligação no draw | OpenSmash (`GPL=2`) | caminho por programa feito (`07c39e7`), desligado; **sem layout universal** |
| Tradutor com fluxo de controle estruturado | HWDER, Ryujinx (SMW) | **não** (laço `switch (pc)`; `b0d0238` reduziu os rótulos do BRX) |
| Cache de pipelines do driver persistente | todos | sim (`vk_pipelines.bin`) |

## Caminho recomendado
O do OpenSmash é o único medido que resolve a primeira execução **sem pular draws** (a exigência do usuário), e
encaixa no que já temos:
1. **Inventário dos shaders do jogo** (etapa de viabilidade): achar os BNSH no romfs (577 soltos; dentro dos `.gfpak`,
   265 de 300 pacotes amostrados têm BNSH sem compressão), extrair cada estágio (controle + código), calcular o
   mesmo hash de conteúdo do runtime e medir quantos dos programas de uma sessão real eles cobrem.
2. **Preparação em segundo plano**: traduzir cada estágio (nosso tradutor) e compilar uma pipeline library por
   estágio, na prioridade mais baixa, com progresso no log / na tela de título; gravar o cache do driver.
3. **Layout de descritores universal** (pré-requisito da biblioteca por estágio: duas partes só se ligam se
   concordarem no layout) e ligação no draw (vertex input + estágios + fragment output).
4. **Variantes que dependem do draw** (tipo dos atributos de vértice, alvos inteiros, texturas inteiras): assar o
   padrão, registrar as outras e assá-las na preparação seguinte (o "1." da recomendação do SMW).
5. Em paralelo e independente: **estruturar o fluxo de controle no tradutor** (HWDER, Ryujinx), que deixa cada
   shader mais barato de compilar e de executar.
Custo esperado (referência OpenSmash): minutos de preparação na primeira execução e alguns GB de disco.
