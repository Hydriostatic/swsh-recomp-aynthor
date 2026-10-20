# Bloqueios

Itens que o supervisor bloqueou (limite de tentativas, estagnação) ou que dependem do jogador.

## G1 (20/10)
Nao reproduz pelo caminho automatizado (tests/scenarios/room_enter.json) em nenhuma combinacao. Precisa do jogador: um save no terreo ao pe da escada, para subir e entrar no quarto como ele fez, ou o F9 no momento.

## G3 (2026-10-20T06:32:38, commit 38ea613)
Arquibancada piscando no título (desfoque alterna entre quadros)

- evidência: passes de 4 quadros a 60 fps: so o passe 10 (plateia) alterna (SSIM vizinhos 0,717, a dois 0,995); passes 0-9 identicos. A plateia troca de quadro de animacao a cada quadro: animacao contada em quadros, dobrada pelo modo 60
- nota: nao e bug do renderer; limitacao do modo 60 (mod). Correcao possivel: patch especifico do modo 60 para a animacao da plateia (item 60MODE-ANIM)
