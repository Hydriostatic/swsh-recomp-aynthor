# Ciclo 2: teste diferencial do lifter, validação Vulkan, TEXINT, identidade de shader no draw, relógio fixo

```
CICLO: 2

SUBSISTEMA: recompilador (lifter), renderer (texturas inteiras, identidade de programa), testes (relógio, re-exec)
PROBLEMA:
  DIFFTEST  sem teste diferencial das instruções
  VKVAL     validação nunca rodada nos cenários
  TEXINT    texturas inteiras amostradas como float (achado pela validação)
  G4        tapete preto voltou em 1 de 9 rodadas
  TESTES    iluminação dependente da hora real e filho re-executado invisível ao harness
SEVERIDADE: P1 (fidelidade) / P2 (infraestrutura de teste)

EVIDÊNCIA INICIAL:
  - validação (escada): ERROR VUID-vkCmdDrawIndexed-format-07753 e -magFilter-04553 (R16G16B16A16_UINT amostrado por
    sampler2D com filtro linear); SYNC-HAZARD-PRESENT-AFTER-WRITE; a camada MSYS2 cai na própria thread
  - G4 preto em reports/runs/stairs/20261020-024611-regress-1 com o build que já usava chave por conteúdo
  - quarto: luma 123,0 (00:45 e 14:00) x 125,4 (03:20), mesma cena

CAUSA RAIZ:
  TEXINT  o código Maxwell não diz o tipo da textura; o tradutor declarava tudo float
  G4      hash do conteúdo tirado no nvnProgramSetShaders (thread do jogo) antes de a cópia de GPU do código rodar
          (CopyBufferToBuffer = memmove na render thread, em ordem): hash do código antigo -> tradução antiga
  luma    o jogo ilumina pela hora do dia (relógio do host)
  re-exec main.c reserve_guest_space relança o processo; o pai sai com 0 e o filho segue

ALTERAÇÕES REALIZADAS:
  - tests/runtime/difftest/difftest.py (Unicorn como oráculo; implementação própria, técnica OpenSmash / mk8)
  - TEXINT: máscaras tex_uint / tex_sint na ShaderKey por draw, usampler / isampler, bits crus, sampler NEAREST
    (LARECOMP_TEXINT=0)
  - identidade do programa: hash do código na render thread, no draw; cache por endereço invalidado por cópias sobre
    o código e por SetShaders; índice por página de 64 KB
  - arquivo de receitas com o layout no nome
  - LARECOMP_FIXED_TIME (scenario.py usa 20/10/2026 14:00); harness acompanha o filho re-executado
  commits: 3001d97, 5d22d4a

BUILD: make B=build/v132 GEN=generated-132 (sem avisos)

TESTES EXECUTADOS:
  - difftest main: 2.995 formatos x 8 estados = 0 diferenças (controle negativo --inject-mismatch detectado 4/4)
  - validação básica na escada depois do TEXINT: E = 0 até a queda da camada (antes: format-07753, magFilter-04553)
  - escada 8x (stairs_rug, quadros 2754-2799 a cada 3): tapete preto em 0 de 8 (antes 1 de 9); 13 hashes de código
    invalidados por rodada (código trocado no mesmo endereço durante o jogo)
  - quarto com relógio fixo: SSIM entre rodadas 0,9994-0,9995, luma 123,0 = baseline
  - regressões room + stairs: todas PASSOU

RESULTADOS:
  - lifter: nenhuma divergência nas instruções sem memória do main (loads / stores / desvios: NÃO TESTADO)
  - G4 corrigido (com a ressalva estatística: 0/8 x 1/9 ainda pode ser acaso em ~39%; o mecanismo foi observado)
RENDERIZAÇÃO: sem regressão visual nos cenários com relógio fixo
SHADERS: texturas inteiras com tipo correto; zero draws pulados
FPS / FRAMETIME: em medição (baseline 3x3 da DoD em andamento)
CPU / GPU: não medido
MEMÓRIA: não medido
VALIDATION ERRORS: ERROR restante: SYNC-HAZARD-PRESENT-AFTER-WRITE (a investigar); camada MSYS2 instável (BLOQUEIO
  parcial: precisa da camada do Vulkan SDK)

REGRESSÕES: nenhuma conhecida
STATUS: CORRIGIDO (TEXINT, G4) / PARCIAL (VKVAL)

PRÓXIMA AÇÃO: baseline DoD (48 rodadas) -> PERF60; G3 (título); difftest com memória e sdk; SYNC-HAZARD do present.
```
