# Comparativo com outros projetos de recompilação estática do Switch

_Lido em 20/10/2026, clones rasos em `reference/` (ignorado pelo git). Só conta o que foi lido no código ou em documento com medição; o README sozinho não conta._

| Projeto | Commit | Licença | Base |
|---|---|---|---|
| ssbu-recomp ("OpenSmash", Smash Ultimate) | `90cb8a2` (20/10) | **GPL-3**: só ideias, reimplementadas | runtime próprio, NVN → Vulkan (`runtime/nvn_vk_*.c`) |
| mk8-recomp | `e846588` (19/10) | **sem licença** (`LEGAL.md`): só ideias | fork de emulador (suyu) com recompilador; runtime "native" por patches |
| SkyRecomp (Skyward Sword HD) | `e9ea020` (19/10) | MIT | fork do LASRecomp (mesma família do nosso) |
| LASRecomp (upstream) | `da35b83` (19/10) | MIT | origem do nosso port |
| WindowSmash | `0dc9f44` (19/10) | sem licença declarada para o código próprio (`NOTICE.md`) | backend **D3D11** |
| SuperMarioWonderRecomp | `665b2f9` (18/10) | MIT | runtime C++ próprio, NVN → Vulkan |
| EoWRecomp | `bc95ad8` (19/10) | MIT | família LASRecomp |

## Matriz técnica
Coluna "aqui": **ausente**, **inferior**, **igual**, **superior** ou **inconclusivo**, em relação ao nosso port antes desta pesquisa.

| Técnica | Onde (arquivo) | Medido lá | Aqui | Decisão |
|---|---|---|---|---|
| Lista persistente das chaves de pipeline usadas, pré-aquecida em threads de fundo; o draw adota ou espera, **nunca pula** | ssbu `runtime/nvn_vk_m4.c` (`pipes.bin`, `pw_main`, `piperec_append`); mk8 `docs/native-pipeline-recipes.md` (`pipeline_recipes_v1.bin`); SMW `runtime/src/nvn/nvn_pipeline.cpp` (`canonical_key`, `.keys`) | mk8: pipelines no caminho do draw ~950 → 10-141; pior present da carga 1,1-1,35 s → 0,49-0,8 s. SMW: o modo que pula draws deixou 5.367 draws faltando numa janela e ficou **desligado** | **ausente**: a gente pula depois de 20 ms por quadro (2.037 draws pulados por rodada com a cache quente) | **adotar** (item RECIPES), reimplementado: chave por valor, estágio identificado pelo hash do conteúdo, arquivo versionado e escrito de forma atômica |
| Compilar no `nvnProgramSetShaders`, antes do primeiro draw | ssbu `nvn_vk_m4.c` (cabeçalho: "a copy of the program's machine code goes to the worker queue at once") | holds raros ("holds so far") | **ausente**: traduzimos no primeiro draw (`get_program`) | adotar junto com RECIPES: o casamento da receita acontece no SetShaders |
| Resultado da tradução Maxwell → SPIR-V persistente (sem traduzir de novo a cada boot) | ssbu `StageRec` / `VarRec` (`cache/vk/stage/*.var`) | — | **inferior**: só o SPIR-V é cacheado, por hash do GLSL; cada boot traduz tudo (1,25 s por rodada) | avaliar depois das receitas |
| Graphics pipeline libraries (ligação rápida no draw) | ssbu `nvn_vk_m4.c` (sessão 9, `PairLib`) | o relink com LTO foi **medido pior** e ficou desligado | ausente | adiado: muda o modelo de pipeline; receitas primeiro (o mk8 adiou pelo mesmo motivo) |
| Estado dinâmico estendido para reduzir a chave | ssbu (EDS3); SMW (chave canônica: zera o que a create info não lê; 18% das chaves duplicadas) | SMW: 47 de 256 chaves eram o mesmo pipeline | inconclusivo | medir duplicatas da nossa `PipeKey` antes |
| Barrier NVN respeitado (fim do passe + barreira de memória) | SkyRecomp `runtime/nvn/vulkan/exec.c` (`case CMD_BARRIER`) | — | **ausente**: `CMD_BARRIER` é gravado e cai em `default: break` | **adotar** (item BARRIER); suspeito do G1 |
| Esperas temporizadas do kernel com timer de alta resolução | SkyRecomp `runtime/platform/windows.c` (`plat_waiter_*`), `runtime/kernel.c` (`kwait`, `wake`) | lá: 3 ms viravam 15,6 ms; depois 3,29 ms | **inferior**, medido **aqui**: 1/3/8 ms → 16,0/16,3/16,4 ms (`tests/runtime/timed_wait_test.c`) | **adotar** (item KWAIT), com atribuição MIT |
| `SetDepthClamp` aplicado | SkyRecomp `cmdbuf.c`, `exec.c` | efeito nas sombras não verificado lá | ausente (no-op em `cmdbuf.c`) | adotar depois (DEPTHCLAMP) |
| Blending dual-source (SRC1) em location 0 / index 1 | SkyRecomp `docs/render-fixes-validation.md` | pipeline real criado sem erro de validação | a verificar no nosso `translate.c` | verificar quando a validação apontar |
| Alpha test do NVN por discard | SkyRecomp | 36 combinações compiladas | **igual**: temos `CMD_SET_ALPHA_REF` (a conferir no uso) | — |
| Teste diferencial do lifter (instruções reais × oráculo) | ssbu `tools/lifter/difftest.py` + `difftest_host.c` (Unicorn); mk8 `tests/differential_*.cpp` (dynarmic) | mk8: compara 31 GPRs, SP, 32 SIMD, NZCV, FPCR/FPSR e a memória inteira | **ausente** | adotar (DIFFTEST) com o Unicorn (pacote Python, só como oráculo de teste, sem ir para o produto) |
| Watchdog de travamento (registro de esperas sem timeout) | ssbu `runtime/nvn_vk_wd.c`; WindowSmash `debug_watchdog.c` | — | ausente | adotar quando houver um travamento a diagnosticar (STAB) |
| A/B intercalado com intervalo bootstrap | mk8 `docs/native-performance.md`; o nosso `tools/perf_ab.py` (Let's Go) | — | **igual** em método, ainda sem cenários do Sword | portar para `tools/swsh` (PERF60) |
| Cache de shaders AOT (D3D11/DXBC) com data de uso | WindowSmash `runtime/gpu_shader_cache.h`, `tools/shaderaot.c` | — | não se aplica (Vulkan) | — |
| Áudio: registros `SNDI` do splitter, bypass de SRC, relógio do renderer | SkyRecomp `docs/render-fixes-validation.md` | medidos lá (picos de clipping, tempo das fanfarras) | **não investigado aqui** | abrir item se houver relato de áudio |

## Ordem de adoção (ciclos)
1. **KWAIT:** correção de tempo com impacto direto no ritmo dos quadros; mudança contida no kernel e na plataforma.
2. **BARRIER:** correção de sincronização, barata, candidata ao G1.
3. **RECIPES:** elimina a causa dos draws pulados sem esperas indefinidas. A espera passa a ter teto e contagem.
4. DEPTHCLAMP, DIFFTEST e o watchdog quando os itens P1 estiverem verdes.

Cada técnica entra com A/B (`tools/swsh/scenario.py`, mesmo cenário, execuções intercaladas) e com as regressões (`supervisor.py regress`). O que não melhorar com evidência é revertido.
