# Auditoria gráfica (NVN → Vulkan, Maxwell → SPIR-V)

## Fatos

### Caminho de um draw
1. O jogo grava comandos NVN, e o runtime os registra numa sombra (`runtime/nvn/cmdbuf.c`).
2. No envio, `exec_list` (`vulkan/exec.c`) percorre os registros e mantém o estado (`st`).
3. Em `draw_`, a chave do programa (atributos, formatos dos alvos, origem da janela, modo de profundidade) busca o programa traduzido (`get_program`) e o pipeline. Se um deles ainda não está pronto, o draw espera dentro de um orçamento por quadro e, quando o orçamento acaba, **é pulado** (`SKIP`).
4. Os motivos dos pulos são contados na linha `[vk]`:

| Motivo | Quando |
|---|---|
| `noprog` | sem programa ligado |
| `compiling` | programa em tradução ou compilação |
| `failed` | a tradução ou a compilação falhou |
| `noobj` | não há objeto de programa |
| `mode` | modo de primitiva sem suporte |
| `pipe-building` | pipeline em construção |
| `pipe-failed` | a construção do pipeline falhou |

### Shaders e pipelines
- **Tradução e compilação:** a tradução Maxwell → GLSL (`maxwell/translate.c`) e a compilação GLSL → SPIR-V (shaderc, `vulkan/shaders.c`) rodam em worker threads (`exec.c`, seção "worker threads").
- **Caches persistentes:**
  - SPIR-V por hash do GLSL em `<cache>/spirv/*.spv`;
  - `VkPipelineCache` em `<cache>/vk_pipelines.bin`, salvo periodicamente num worker.
  `<cache>` é `cache/` ou `LARECOMP_CACHE_DIR` (ciclo 0).
- **Espera por draw:** `wait_built`, com `LARECOMP_SHADER_WAIT_MS` = 20 ms por quadro. Esgotado o orçamento, os draws seguintes que dependem de algo não pronto são pulados.
- **Medido no ciclo 0** (`reports/runs/stairs/20261020-004147-1440p30-warm-1`, 1440p30, cache quente):
  - 2.037 draws pulados no total: 1.260 `compiling` e 777 `pipe-building`;
  - 1.388 compilações, com 1,25 s de tradução, 1,28 s de SPIR-V (leitura da cache), 0,17 s de pipelines e 0,3 s de espera dos draws;
  - os pulos se concentram no boot (quadro 121), no título (362), na entrada no mundo (722) e na chegada ao térreo pela escada (2757 e 2806).
  - **Com a cache quente, o boot ainda traduz todos os programas:** a tradução Maxwell → GLSL não é cacheada, só o SPIR-V e o pipeline.

### Validação
- `LARECOMP_VK_VALIDATION=1` carrega `VK_LAYER_KHRONOS_validation` (MSYS2 `vulkan-validation-layers` 1.4.363, via `VK_ADD_LAYER_PATH`). `VK_LAYER_VALIDATE_SYNC=1` liga a validação de sincronização e `VK_LAYER_GPUAV_ENABLE=1` a validação assistida pela GPU.
- As mensagens são contadas por severidade (`| vkval E n W n` na linha `[vk]`), e cada ID é logado por inteiro nas 5 primeiras vezes (`[vkval]`).

### Bugs gráficos corrigidos (com regressão)
- **G2:** flags de cópia `MIRROR_X`/`MIRROR_Y` ignoradas (`766239f`). Regressão: `tests/regression/stairs.json` (`G2-snapshot-upright`); margem de orientação +0,78 corrigido contra −0,78 no bug.
- **`.FMZ`:** multiplicações do Maxwell (`cddab04`).
- **Swizzle de viewport `NEGATIVE_Y`:** implementado (`d99a99e`); o Sword não usa.

## Hipóteses
- **G4 (tapete preto):** coincide com draws pulados na chegada ao térreo (quadros 2757 e 2806). Um passe que pinta o tapete, ou que monta um dado que ele lê, pode ter sido pulado ou usado com o estado de outro quadro. A conferir no ciclo do G4.
- **G1 (brilho estourado):** some quando os draws são separados (cada draw no seu passe, salvo e esperado). Indica sincronização entre envios ou reuso de dados; a validação de sincronização deve dizer algo.

## Não investigado
- Execução completa das validation layers nos cenários (item VKVAL).
- RenderDoc nos cenários do Sword: a integração existe (`vulkan/renderdoc.c`, `LARECOMP_RENDERDOC_AT`); o Nsight não está instalado.
- Equivalência funcional do SPIR-V: `spirv-val` valida a forma, não o comportamento. Comparar com referência do hardware não é possível aqui.
- Formatos raros, como ASTC (`runtime/nvn/astc.c`), e texturas 3D/cube nos cenários fora de casa.

## Atualização de 20/10 (ciclos 1-2)
- **Identidade dos programas:** antes, o endereço do código. Agora é o hash do bloco de código (tamanho em
  `control + 0x704`), calculado **na render thread, no draw**, em cache por endereço e invalidado por cópias que
  escrevem sobre o código (`CopyBufferToBuffer`) e por `nvnProgramSetShaders`. Motivo: o jogo reaproveita a memória de
  shaders entre áreas (G4) e pode copiar o código com uma cópia de GPU que só roda depois do SetShaders. Um hash tirado
  no SetShaders falhou em 1 de 9 rodadas. Também une cópias idênticas do mesmo shader: 649 → 204 pipelines na escada.
- **Draws pulados:** zero nos cenários da escada e do quarto. O draw ou dispatch espera, com teto de 3 s, e
  reivindica o job ainda não iniciado. As receitas de pipeline pré-aquecem o que a sessão anterior usou: na segunda
  rodada, 187 pré-aquecidos e 187 adotados, com 18 esperas que somaram 4 ms.
- **Barriers NVN:** respeitados (fim do passe, barreira completa). O jogo pede ~70 por quadro com bits de
  invalidação; sem custo medido.
- **Validation layers** (`--validation`, camada do MSYS2 1.4.363):
  - **ERROR** `VUID-vkCmdDrawIndexed-format-07753` e `-magFilter-04553`: um shader amostrava uma textura
    R16G16B16A16_UINT (alvo 0x2C do passe de composição de luz) por `sampler2D` de float com filtro linear.
    Corrigido (TEXINT): por draw, os slots com formato inteiro entram na chave do programa (`tex_uint` / `tex_sint`),
    o tradutor declara `usampler` / `isampler` e repassa os bits crus (como o TEX do Maxwell), e o sampler vira
    NEAREST. Depois da correção: E = 0 na escada até o ponto da queda da camada.
  - **ERROR** `SYNC-HAZARD-PRESENT-AFTER-WRITE` (validação de sincronização): o present da swapchain depois de uma
    transição de layout de `vkCmdEndRenderPass` (o render pass da apresentação ou da interface). A corrigir.
  - **WARNING** `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering`: o fragment shader escreve c0 num passe
    sem alvo de cor (passes só de profundidade). Inofensivo.
  - **BLOQUEIO:** a camada do MSYS2 cai na própria thread (violação de acesso em libwinpthread dentro de
    `VkLayer_khronos_validation.dll`, depois de `INTERNAL-ERROR-VkSemaphore-state-timeout`) por volta da transição
    da escada. A validação completa precisa da camada oficial do Vulkan SDK da LunarG (o instalador pede
    administrador).
