# PS2 Showdown (prototipo)

Battle royale 2D top-down de 10 jogadores, visual de arena colorida: personagens humanos,
gas que fecha, caixas que soltam cubos de poder, arbustos para se esconder, municao com
recarga e super. Personagens e nomes sao originais.

## Compilar
1. Instale o toolchain: https://github.com/ps2dev/ps2dev (PS2DEV e PS2SDK no ambiente)
2. `make`
3. PCSX2: File > Run ELF > battle_royale.elf. Em PS2 real, via uLaunchELF.

## Controles
- Analogico esquerdo: mover (com inercia)
- Analogico direito: mirar; ao soltar, atira (tudo manual, sem mira automatica)
- QUADRADO: super (quando o botao de super no canto inferior direito estiver amarelo)
- START: pausa | CIMA/BAIXO/X: menus

## Classes (originais)
- Estilhaco (laranja): 3 projeteis, curto alcance
- Falcao (roxo): 1 tiro forte, longo alcance, pouca vida
- Muralha (verde): muita vida, 2 projeteis

## Tela
- Sem HUD de barras: vida (com numero), 3 segmentos de municao, nome e cubos ficam
  em cima de cada personagem.
- Canto superior esquerdo: "RESTAM: N" e o aviso de quem matou quem (retrato do
  assassino, revolver, retrato da vitima com caveira; "GAS" quando morre no gas).
- Canto inferior direito: botao de super (mostra a porcentagem e vira caveira amarela
  quando pronto).

## Proximos passos
1. Sprites e texturas de verdade; mais animacoes de andar/atirar
2. Mais classes e supers, modo 3v3 com bots
3. Audio (audsrv)
4. Multiplayer local para 2 jogadores (padPortOpen na porta 1)
