# Diagnóstico: Pokémon Sword (v1.0.0, cartucho)

Data: 2026-10-18 · Extraído com hactool 1.4.0 + `prod.keys` do console do usuário (Lockpick_RCM) · Gerador:
`tools/gen.py` do letsgo-recomp, sem nenhuma mudança. Comparação com o Let's Go Eevee 1.0.0 em
[DIAGNOSTICO.md](DIAGNOSTICO.md).

## O dump

| | |
|---|---|
| Arquivo | `Pokemon Sword.xci` (cartucho de 16 GB) |
| Title id | `0100abf008968000` (Shield: `01008db008c2c000`, ainda não testado) |
| Versão | 1.0.0 (NCA de programa `9226d941…`, 9,5 GB) |
| SDK | 7.4.0.0 |
| RomFS | `extracted/romfs.bin`, 9,48 GB |
| Update no cartucho | a partição `update` do XCI só traz firmware (213 arquivos), não traz update do jogo |

## Módulos do ExeFS

| Módulo | O que é | Imagem | bss | Build id | Exports / imports |
|---|---|---|---|---|---|
| `main` | o jogo (engine da Game Freak) | 40,0 MB (text 23,9 MB) | 3,3 MB | `8dafedbb5be81c2c` | 29 / 875 |
| `sdk` | nnSdk (NVN incluso) | 12,8 MB | 0,8 MB | `fcf93a41b08c5193` | 25.077 / 13 |
| `subsdk0` | **multimedia** (`movie::`) | 7,1 MB | 1,1 MB | `93ff8252d975adf0` | 10.872 / 647 |
| `subsdk1` | **glslc** | 8,4 MB | 9 KB | `a36dac0e0a5ed3b4` | 20 / 84 |
| `rtld` | loader dinâmico | 7 KB | | | |

- **Os subsdk estão na ordem inversa da do Let's Go** (lá `subsdk0` = glslc, `subsdk1` = multimedia). Tudo o
  que assume o papel de cada um pelo nome (HLE do movie, `hle_symbols.txt`, comentários de `runtime/rt.h`)
  precisa de uma tabela por jogo.
- **Cabe no layout atual de `runtime/rt.h`:** `main` usa 43,3 MB dos 64 MB antes do `SDK_BASE`; `sdk` (13,6 MB) e
  os dois `subsdk` (8,2 e 8,4 MB) cabem nas janelas de 32 MB. Não é preciso mudar as bases.

## Cobertura do tradutor ARM64 → C

| Módulo | Funções | Não suportadas | Jump tables | `br` por dispatch | Tempo |
|---|---|---|---|---|---|
| `main` | 105.129 | **0** | 1.775 | 7.345 | 44 s |
| `sdk` | 34.472 | **0** | 477 | 9.793 | 10 s |
| `subsdk0` (multimedia) | 13.640 | **0** | 317 | 4.430 | 4 s |
| `subsdk1` (glslc) | 11.613 | **0** | 988 | 440 | 11 s |

100% do código traduzido, conferido também pela ausência no C gerado da marca `rt_udf(...); /* motivo */` que o
lifter deixa nas instruções não suportadas. C gerado: 678 MB em `generated/` (fora do git), contra ~550 MB
do Let's Go. O `main` tem 1,7x as funções do Let's Go (105 mil contra 60 mil), então a compilação da DLL deve
levar proporcionalmente mais.

## Serviços (`main.npdm`)

A mesma lista de 46 serviços do Let's Go (acc:u0, aoc:u, appletOE, audren:u, bcat:u, bsd:u, fsp-srv, hid,
hwopus, ldn:u, nifm:u, nvdrv, pl:u, set, time:u, vi:u, ...). Nenhum serviço novo para implementar só por
permissão; o que muda é **quais comandos** o jogo chama, e isso só aparece em execução.

## Imports do `main` em relação ao Let's Go

875 imports, 126 que o Let's Go não tem e 93 que só o Let's Go tem. Por namespace, o que muda:

| Namespace | Sword | Let's Go | Novos | Observação |
|---|---|---|---|---|
| nn::bluetooth | 0 | 34 | 0 | sumiu (era o Poké Ball Plus) |
| nn::account | 26 | 17 | 11 | Nintendo Account em cache, licença do serviço online (`ShowLicenseRequirementsForNetworkService`) |
| nn::err | 20 | 14 | 9 | `ErrorResultVariant`, `ShowError` (applet de erro) |
| nn::hid | 81 | 82 | 8 | `VibrationPlayer` (arquivos de vibração), cor do controle |
| nn::oe | 19 | 16 | 6 | `SetCpuBoostMode`, `SetPerformanceConfiguration`, seção de pedido de saída, captura de tela |
| nn::time | 23 | 17 | 6 | `StandardSteadyClock::now`, `GetDeviceLocationName`, aritmética de datas |
| nn::web | 5 | 0 | 5 | **applet de página web** (`ShowWebPage`) |
| nn::friends | 6 | 2 | 4 | lista de amigos |
| nn::aoc | 2 | 0 | 2 | **DLC**: `CountAddOnContent`, `ListAddOnContent` |
| nn::pdm | 1 | 0 | 1 | estatísticas de jogo |
| libc / libc++ | | | ~60 | `setjmp`/`longjmp`, `strcoll`, `localeconv`, `random_device`... (sugere um interpretador de script embutido) |

O resto das bibliotecas `nn::` (os, fs, audio, socket, ldn, bcat, fontll, swkbd, crypto) tem praticamente o mesmo
conjunto do Let's Go. NVN entra por um único import (o bootstrap), como no Let's Go: as funções `nvn*` são
resolvidas por nome em execução.

## Primeiro boot (2026-10-18)

`lasgame.dll`: 254 MB, compilado em 18 min com `make -j20` (i5-14600KF), sem nenhum erro. O Makefile usa
`generated/` no Windows (o `generated-<arch>/` é só do Linux).

1. **Primeiro crash:** acesso ao endereço 0 no `sdk` (`f_70041d6b90`, chamado do `main`) logo depois de
   `friend:u` cmd 0. O serviço não tinha implementação, e o stub genérico respondia "ok" sem devolver o objeto
   `IFriendService`, então o jogo usou uma sessão nula. **Correção:** `friend:u` em `runtime/services/offline.c`
   (`CreateFriendService` / `CreateNotificationService` devolvem os objetos; listas vazias, eventos que não
   disparam: um console offline sem amigos).
2. **Depois disso:** carrega e roda a **abertura** (o discurso do Rose no estádio de Wyndon, com legendas) a
   **30 fps estáveis**, ~16 mil draws/s, nenhum shader com falha de tradução (`failed 0`, `pipe-failed 0`) nos
   100 s testados. Os quadros salvos (`LARECOMP_SHOT_AT`) mostram estádio, torcida, iluminação, sombras e os
   modelos do Rose e do Cufant corretos.
3. Stubs que apareceram e não atrapalharam até aqui: `pl:u` cmd 0 (o stub genérico), `ITimeZoneService` cmd 4
   chamado em loop.

Ainda não testado: passar da abertura (entrada pelo controle), tela de título, jogo novo, áudio (não conferido),
save.

## O que isso indica para o port

1. **O recompilador serve como está.** Nenhuma instrução nova, nenhuma mudança de layout de memória.
2. **Primeiro trabalho no runtime:** tornar a ordem dos `subsdk` configurável por jogo e aceitar o title id e o
   build id do Sword no launcher (`runtime/gamesetup.c`, `mods.c`, `services/fs.c`).
3. **Depois, o boot**, no ciclo de sempre (`UNIMPLEMENTED` / crash → implementar). Pontos novos já previstos: aoc
   (responder "sem DLC" na 1.0.0), web e erro (applets que precisam só fechar), `VibrationPlayer`, as funções
   de performance do `oe` (podem ser no-op).
4. **Maior incógnita:** gráficos. O Sword tem muito mais shaders e cenas maiores (Wild Area) que o Let's Go, e o
   tradutor Maxwell → SPIR-V e a camada NVN → Vulkan só foram testados com o Zelda e o Let's Go.
5. **DLC** (Isle of Armor / Crown Tundra) exige um update (1.2.0+) e o romfs da DLC; fica para depois do boot da
   1.0.0.
