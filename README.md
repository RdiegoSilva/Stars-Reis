# PS2 Showdown (prototipo)

Battle royale 2D top-down de 10 jogadores, estilo arena colorida: gas que fecha, caixas que
soltam cubos de poder, arbustos para se esconder, municao com recarga e super.
Personagens e nomes sao originais.

## Compilar
1. Instale o toolchain: https://github.com/ps2dev/ps2dev (PS2DEV e PS2SDK no ambiente)
2. `make`
3. PCSX2: File > Run ELF > battle_royale.elf. Em PS2 real, via uLaunchELF.

## Controles
- Analogico esquerdo: mover | Analogico direito: mirar (solte para atirar)
- R1: mira automatica | QUADRADO: super | SELECT: trocar classe | START: reiniciar

## Classes (originais)
- Estilhaco (laranja): 3 projeteis, curto alcance
- Falcao (roxo): 1 tiro forte, longo alcance, pouca vida
- Muralha (verde): muita vida, 2 projeteis

## HUD (sem texto por enquanto)
Barra verde = vida | 3 barras laranja = municao | barra azul = super (amarela quando pronta)
Quadrados roxos = cubos | quadrados brancos (canto) = jogadores vivos

## Proximos passos
1. Fonte/texto (gsKit_fontm) e menu de selecao de classe
2. Sprites e texturas; animacao de andar/atirar
3. Mais classes e supers, modo 3v3 com bots
4. Audio (audsrv)
5. Multiplayer local para 2 jogadores (padPortOpen na porta 1)
