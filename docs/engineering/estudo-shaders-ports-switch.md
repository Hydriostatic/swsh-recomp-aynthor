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

## Resultado (21/10/2026, etapas 1 a 4)
- **Inventário** (`tools/swsh/shader_store.py`): 68.204 contêineres BNSH em 50.495 arquivos (`.gfpak` com LZ4,
  `.ptcl`, `.arc`, `.bnsh*`), 18.587 estágios distintos (11.921 de vértice, 6.666 de fragmento, 104 MB). Cobrem 97,5%
  dos estágios (3.221 de 3.303) e 97,9% dos programas das receitas de uma sessão real. 4.092 estágios de fragmento
  ficam em variações sem estágio de vértice: o jogo escolhe o par na hora do draw.
- **Preparação** (`runtime/nvn/vulkan/prep.c`): na primeira abertura, threads em prioridade ociosa leem os arquivos
  (~30 s), traduzem cada estágio com as chaves dos draws do jogo e constroem a parte de pipeline dele. 11.921 partes de
  vértice e 18.631 de fragmento em **4,8 min** (8 threads, i5-14600KF + RTX 3080). Disco: 1,3 GB de partes
  (`vk_parts.bin`, cache próprio) + 0,6 GB de SPIR-V (agora comprimido com zstd).
- **Layout universal → "independent sets"**: em vez de um layout único (64 texturas por set encheriam os pools), cada
  parte usa só o set do seu estágio (`VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT`), e o layout de cada set
  vem da reflexão do próprio estágio: a parte é a mesma em qualquer programa.
- **Variantes**: vértice com as posições 7, 8 e 11 inteiras (onde o estágio as lê); fragmento com todas as saídas do
  vértice e com as de cada par das variações que deixe entradas de fora (só os bits lidos mudam a tradução).
  Na entrada da cidade: GLSL igual ao do draw em 46 de 46 estágios de vértice e 183 de 199 de fragmento preparados.
- **Medido como instalação nova** (cache vazia + `LARECOMP_DRIVER_CACHE_NONCE`; a rodada "com" usa a cache que a
  preparação deixou):

  | Cenário | Sem preparação | Com preparação |
  |---|---|---|
  | Entrada na cidade | 10-11 quadros > 50 ms, 3,5-4,0 s | 5, 2,2 s (o que resta são os carregamentos do jogo, presentes em toda rodada) |
  | Mundo aberto (`openworld2`) | 14, 2,8 s | 6, 1,5 s |
  | Batalha selvagem | 50, 6,4 s, P99 97 ms | 9-12, 2,1-2,5 s, P99 34-37 ms |

- Custos: com cache quente e sem limite de fps, mundo aberto 75,5 → 73,6 fps (pipeline library ligada; com a trava de
  60 fps, nada). Imagens sem diferença sistemática. Durante a preparação a tela de título cai para ~20 fps.
- Erros no caminho: gravar 1 GB de cache de pipelines bloqueava toda criação de pipeline por 6,4 s (cache separado,
  gravado só no fim e ao sair); a flag `RETAIN_LINK_TIME_OPTIMIZATION` era inútil (só há ligação rápida); 8
  variantes por estágio de vértice davam 7,4 GB e travadas de 16 s no boot.
- Falta: a etapa 5 (fluxo de controle estruturado no tradutor); o DLC fora do RomFS principal (`aoc/*.bin`) ainda não
  é lido pela preparação; um aviso de progresso na tela.
- **Depois do primeiro teste no jogo** (log de 8 min do usuário: 98 quadros > 50 ms, 14,4 s): as partes acertavam o
  cache (~1-5 ms cada), mas eram criadas e ligadas uma a uma na render thread (205 partes, ~1 s seguido numa área
  nova), e os pipelines inteiros disputavam os workers e a CPU (16 s de driver no mesmo momento). Agora a ligação é um
  trabalho dos workers pedido assim que o pipeline é conhecido (draw ou pré-varredura), e os pipelines inteiros vão
  para 2 threads em prioridade ociosa. Mesma cache preparada: cidade 2,2 → 0,86 s (pior quadro 1.423 → 664 ms), mundo
  aberto 1,5 → 1,05 s, batalha 2,1-2,5 → 2,0 s (pior 731 → 522 ms; os 522 ms são o carregamento da batalha). E uma
  preparação concluída não roda de novo (`shader_prep_complete`): antes, toda abertura relia e retraduzia tudo (38 s).
