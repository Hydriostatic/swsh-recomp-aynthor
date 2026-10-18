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

4. **Jogado à mão** (sessão com log ao vivo): abertura, criação do personagem, casa em Postwick, Wedgehurst e a
   chegada do Leon. **Segundo crash** na cena dos iniciais: o pool de memória de comandos do jogo esgotou, porque o
   runtime gravava 80 bytes por comando NVN (HISTORICO, item 62). Corrigido com a sombra dos registros; depois disso,
   jogado até a apresentação dos iniciais sem crash. O save do jogo (`save-sword/`) funciona: o jogo voltou do ponto
   salvo.

Ainda não testado: escolher o inicial e a primeira batalha, áudio (não conferido), o resto da história.

## Sword 1.3.2 com o Expansion Pass (18/10/2026)

- **Atualização** (`0100abf008968800`, v1.3.2, NSP com ticket comum): extraída sobre a NCA de programa do cartucho
  com `hactool --basenca`. O hactool não grava a imagem de um RomFS com patch (BKTR), só os arquivos
  (`--romfsdir`, 50.494 arquivos, 13 GB); a imagem sai do `tools/romfs_pack.c` (o montador do launcher).
- **`main` 1.3.2:** build `a3b75bcd3311385a` (o mesmo do mod de 60 fps), imagem 41,5 MB + bss 3,3 MB (cabe), SDK
  7.7.0. `sdk`, `subsdk0` e `subsdk1` idênticos aos da 1.0.0. Lifter: 109.846 funções, 0 não suportadas.
- **DLCs** (`...9001` e `...9002`, Expansion Pass): só um `keyfile.dat` cada; as áreas estão na atualização.
  Montadas como `aoc/<data id>.bin` e servidas pelo `aoc:u` / `OpenDataStorageByDataId` (commit 516a242).
- **Primeiro boot:** crash do `longjmp` (HISTORICO, item 63). Corrigido: título, save da 1.0.0 carregado e
  convertido, Armor Pass entregue.
- **60 fps:** os endereços do mod na 1.3.2 confirmam a leitura da 1.0.0 (intervalo 1 no invólucro do
  `SetPresentInterval`, entrada de 1/30 s da tabela de durações trocada por 1/60 s): `runtime/hle/swsh60.c`.
- **Como rodar:** `build/v132/lasrecomp.exe extracted-132` (DLL próprio em `build/v132/`, compilado com
  `make B=build/v132 GEN=generated-132`). Usar uma pasta de save à parte (`LARECOMP_SAVE_DIR`): a 1.3.2 converte
  o save que abre, e a 1.0.0 não lê mais o convertido.

## Bugs gráficos conhecidos (abertos)

Vistos jogando (sessão de 18/10, `build/play5.log`, por volta de t=1100 s / quadro ~32000). As capturas do jogador
ficam em `build/bugs/` (fora do git: são imagens do jogo).

| # | Sintoma | Onde | Hipóteses a conferir |
|---|---|---|---|
| G1 | **Brilho estourado:** manchas brancas grandes e borradas sobre objetos e perto das fontes de luz (abajur, cômoda, janela), atravessando paredes. Cena correta por baixo | Quarto do jogador e sala, Postwick (`brilho-estourado-quarto.webp`) | Halos/flares de luz desenhados sem oclusão: o jogo deve decidir a visibilidade por **consulta de oclusão** (`nvnCommandBufferReportCounter`, amostras que passaram) ou lendo a profundidade; se o runtime responde "visível" sempre (ou o contador errado), todo flare aparece, inclusive atrás de paredes. Outra possibilidade: bloom com alvo HDR (R11G11B10/RGBA16F) sem clamp, ou mistura aditiva com fator errado. Conferir com `LARECOMP_DEBUG_FRAME`/F9 qual passe desenha as manchas |
| G1 (estado em 18/10) | — | — | **Não é o shader nem o bloom:** o branco já sai na cena (passe 7, alvo RGBA16F) em revistas, livros e caixas; com o despejo por draw (`LARECOMP_DEBUG_PASS_DRAWS`, cada draw no seu passe, salvo e esperado) o quadro sai **limpo**. Não é a thread de renderização (`LARECOMP_RENDER_THREAD=0`: continua). Não se repete carregando o save já dentro do quarto (4 rodadas automáticas, 720p e 1080p, com e sem as cópias de vértices / constantes): aparece entrando no quarto andando. Suspeitas abertas: dados de constantes / vértices desses materiais reaproveitados entre envios quando o cômodo carrega, ou um recurso atualizado na entrada (iluminação, ambiente). A correção do `.FMZ` (item 65) é certa pela semântica do Maxwell, mas não era a causa |
| G2 (estado em 18/10) | — | — | A imagem guardada (textura 1920x1080 RGBA8) é gravada por um blit com draw (`vs 72be62a000` / `fs 72be62a200`, coordenada de textura vinda dos vértices) e mostrada por `vs 728b1fcf00`, que **inverte o Y de propósito** com uma constante do jogo (`y * -cb[33].y`). O swizzle `NEGATIVE_Y` (implementado, item 66) não é usado pelo jogo. Próximo passo: salvar origem e destino do blit no instante da gravação (`LARECOMP_WATCH_TEX` já acha o quadro) |
| G3 | **Arquibancada piscando / pulando** na tela de título (estádio, desfoque de fundo); o fundo desfocado também sai "posterizado", em manchas de cor | Título da 1.3.2 (captura F9 dos quadros 1967-1970, `build/play132c.log`) | Entre quadros seguidos o desfoque de profundidade (DOF) alterna: 1967 bem desfocado, 1968 quase nítido. Suspeita: um efeito com buffers alternados por quadro (ping-pong) ou com dados do quadro anterior, um dos lados desatualizado no runtime. **A posterização (faixas de cor) aparece em toda rodada a 1080p e em nenhuma a 720p** (mesma cena, quarto, `build/t132/R0hd` x `R0sd`): ligada à resolução interna acima da nativa, reproduzível sem jogador |
| G2 | **Cena de ponta-cabeça** por um momento ao trocar de cena ou cenário | Escada da casa, ao descer (`cena-de-ponta-cabeca-escada.webp`) | A transição usa uma cópia do quadro anterior (captura do framebuffer para o fade ou a troca): uma cópia ou um blit (`CopyTextureToTexture`, `Downsample`, ou a textura amostrada depois) com a origem Y invertida em relação ao resto. O Let's Go não usa esse caminho, por isso nunca apareceu lá. Conferir com `LARECOMP_DEBUG_FRAMES` na troca de cena |

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
