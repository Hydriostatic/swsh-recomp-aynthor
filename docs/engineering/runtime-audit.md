# Auditoria do runtime (HLE do Horizon)

## Fatos
- **Serviços sem HLE:** respondem pelo stub genérico (`runtime/ipc.c:19`), que devolve sucesso com 0x40 bytes zerados e loga `[ipc] STUB <serviço> cmd <n>`.
  - Uso real medido numa sessão de 24 min do jogador (`build/logs/sword-20261018-150115.log`, 1.3.2): só `pl:u` cmd 0 (4 vezes) e `prepo:u` cmd 10103 (1 vez, relatório de uso).
  - Nenhum outro serviço caiu no stub.
- **Serviços com HLE próprio:** acc, am, audio, fs (saves, romfs com patch BKTR, add-on content), hid/set, hwopus, misc (inclui `aoc:u`), network, offline (`friend:u` sem amigos) e time.
- **Correções específicas do Sword:**
  - `setjmp`/`longjmp` do jogo em HLE (`runtime/hle/setjmp.c`);
  - DLCs servidas por `OpenDataStorageByDataId` (`services/fs.c`);
  - modo 60 fps por gancho no intervalo de apresentação (`runtime/hle/swsh60.c`). Fica desligado por padrão: `fps = 30` no ini.
- **Memória:** commit sob demanda. A corrida no commit preguiçoso foi corrigida (HISTORICO 61-66).

## Hipóteses
- `pl:u` cmd 0 (`RequestLoad`, fontes compartilhadas) pode estar coberto pelo HLE de `runtime/hle/pl.c`, chamado direto por símbolo, e o stub só atender a sessão IPC. A conferir se algum texto fica sem fonte.

## Não investigado
- Os demais caminhos de serviço fora dos cenários testados: batalhas online, Pokémon HOME, raides com rede (a rede está desligada no HLE).
- Comportamento do relógio do jogo (time zone, passagem de dias) em sessões longas.
- Atomics e modelo de memória: o lifter traduz LDXR/STXR e LDAR/STLR; não foi auditado contra a semântica do ARM em código com muitas threads.
