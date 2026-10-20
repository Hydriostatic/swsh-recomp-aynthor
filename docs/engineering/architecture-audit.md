# Auditoria de arquitetura

_Base: commit `dcdeca3` e as mudanças do ciclo 0. Detalhes mais antigos: [../technical-audit.md](../technical-audit.md) e [../ANALISE-LASRecomp.md](../ANALISE-LASRecomp.md)._

## Fatos (lidos no código ou medidos)

### Recompilação estática ARM64 → C
- **Entrada:** os NSO do jogo (`extracted-132/exefs`: `main`, `sdk`, `subsdk0`, `subsdk1`), lidos por `tools/nso.py`.
- **Gerador:** `tools/gen.py` com `tools/lifter.py` (ARM64 → C) e `tools/simd_c.py` (NEON → intrínsecas x86-64 em `runtime/simd.h` e `simd_fp.h`).
- **Saída:** `generated-132/<módulo>/` (~700 MB de C), compilada no `lasgame.dll` de `build/v132/`. O build 1.0.0 vai em `generated/` → `build/`.
- **Despacho:** cada função do guest vira `f_<endereço>`; chamadas indiretas passam por `rt_dispatch` (`runtime/dispatch.c`), e um endereço sem função é `FATAL: no function at ...`.
- **HLE:** substitui imports por nome (`hle_symbols.txt`, `HLE_FUNC`). Ganchos em endereços do guest usam `runtime/fnhook.c` (ex.: `swsh60.c`, `setjmp.c`).
- **Memória:** o guest vive num espaço reservado com commit sob demanda (`plat_reserve_lazy`, `runtime/platform/windows.c`); acesso a página não comprometida é tratado no handler de exceção.

### Runtime (Horizon)
- **Núcleo:** `runtime/kernel.c` (threads, svc, sincronização), `runtime/ipc.c` (sessões, CMIF) e serviços em `runtime/services/` (acc, am, audio, fs, hid/set, hwopus, misc, network, offline, time).
- **Serviço sem implementação:** recebe o `stub_handler` (`ipc.c:19`), que loga `[ipc] STUB` e responde sucesso com saída zerada. Ver [runtime-audit.md](runtime-audit.md).

### Gráficos
- **API NVN:** os objetos ficam em `runtime/nvn/objects.c`; os command buffers viram uma sombra de registros (`cmdbuf.c`, `nvn.h`, `CmdRec`).
- **Execução:** a sombra é executada em Vulkan por `runtime/nvn/vulkan/exec.c` (~4 mil linhas), em geral numa render thread própria (`renderthread.c`).
- **Recursos:** texturas e envios em `vulkan/textures.c`; instância, dispositivo, swapchain, cache de pipelines e apresentação em `vulkan/device.c`.
- **Shaders:** Maxwell → GLSL em `runtime/nvn/maxwell/translate.c`; GLSL → SPIR-V pelo shaderc, com cache em disco, em `vulkan/shaders.c`.
- **Resolução de saída:** `runtime/nvn/output_scale.c`; SMAA e FSR1 na pós-composição, em `exec.c`.

### Build
- `make` (1.0.0) e `make B=build/v132 GEN=generated-132` (1.3.2).
- MSYS2 UCRT64 com clang. O link do DLL usa um arquivo de resposta (`Makefile`).
- Não há suíte de testes de unidade; existe `tools/test_output_scale.c`. Os testes reais são os cenários (`tests/scenarios`) e as regressões (`tests/regression`).

## Hipóteses
- O renderer está isolado do runtime só por funções (`renderer.h`), sem uma camada de API formal. Isso basta para um só backend; não justifica refatoração sem um ganho medido.

## Não investigado
- Testes diferenciais da recompilação (ARM64 original × C gerado). Não há referência executável do código original aqui (sem hardware nem emulador no ciclo); o caminho viável é comparar funções puras isoladas (matemática, RNG) contra uma execução de referência. Fica como item a abrir quando houver suspeita concreta.
- Precisão de ponto flutuante NEON → x86 (`simd_fp.h`) contra o comportamento do ARM (FPCR, denormais, NaN padrão).
