# Classic Racing GT - DC Edition (e port do Doom) — Dreamcast

Homebrew para o **Sega Dreamcast** feito com **KallistiOS** (DreamSDK), testado no emulador
Flycast. Projeto pessoal de estudo, sem fins comerciais.

| Pasta | Conteúdo |
|---|---|
| `projetos/10-formula/` | **Classic Racing GT - DC Edition**: corrida de fórmula (era 1998–2000) em Interlagos |
| `ports/doom-dc/` | Port do Doom (doomgeneric + Freedoom) com som, música MIDI e saves na VMU |
| `dc.ps1`, `ferramentas/` | Compilar, gerar `.cdi` e capturar a tela do emulador |

## Classic Racing GT - DC Edition

Traçado real de Interlagos (OpenStreetMap) com relevo real (SRTM), 8 carros com IA, câmera
de trás e **cockpit 3D**, clima dinâmico com chuva, céu pintado no estilo Speed Devils, HUD
próprio, replay com câmeras de TV e tela de título. Usa as técnicas típicas do Dreamcast:
VQ com mipmap, paleta de 8 bits, punch-through, *modifier volumes* (sombras), *bump mapping*,
FSAA, sprites, *render-to-texture* (retrovisores, telão, reflexo da pista molhada), matriz
do SH4 (`ftrv`) e laço em assembly.

**Toda a história do desenvolvimento, técnicas, opções de compilação, medições e lições:**
[`projetos/10-formula/DOCUMENTACAO.md`](projetos/10-formula/DOCUMENTACAO.md).

Controles: A ou R = acelerar · B ou L = frear · Y = câmera (trás/cockpit) · X = clima ·
START = detalhe · BAIXO = replay. Na tela de título: esquerda/direita = clima, START = correr.

### Compilar

Requer o **DreamSDK** (`C:\DreamSDK`) e **Python 3** com `numpy`, `scipy` e `Pillow`.

```powershell
.\dc.ps1 build 10-formula        # na 1ª vez gera as texturas (gerar_texturas.py)
.\dc.ps1 run 10-formula          # compila e abre no Flycast (ferramentas\flycast\flycast.exe)
.\dc.ps1 cdi 10-formula -Titulo "CLASSIC RACING GT"
```

Piloto automático (demonstração): `make DEMO=1` no shell do DreamSDK.

## Ambiente

| Item | Onde | Versão |
|---|---|---|
| DreamSDK (toolchain SH-4 + KallistiOS) | `C:\DreamSDK` | R4 (GCC 13.2.0) |
| Flycast (emulador, não incluído) | `ferramentas\flycast\flycast.exe` | 2.7 |

Para o console real com GDEMU: copie o `.cdi` para uma pasta numerada do cartão SD,
renomeado para `disc.cdi`.

## Créditos e licenças de terceiros

- **Traçado de Interlagos:** © colaboradores do **OpenStreetMap**, licença **ODbL**
  (https://www.openstreetmap.org/copyright). Crédito também exibido na tela de título.
- **Relevo:** **SRTM** (NASA, domínio público) via *Terrain Tiles* (formato Terrarium,
  Mapzen / Amazon Web Services).
- Céu, texturas, carros, cockpit e sons do jogo são **gerados por código** neste repositório.
  Equipes rivais com as cores de 1999 e marcas **fictícias** (sem logos reais). Sem marcas
  da SEGA.
- Um modelo opcional de carro em `.obj` (de terceiros) **não está no repositório**; sem ele
  o jogo usa o carro original automaticamente.
- **Doom:** o código do jogo é o **doomgeneric** (GPL v2) e os dados são do **Freedoom**
  (BSD) — **não incluídos**: baixe-os das fontes originais. Os arquivos do port em
  `ports/doom-dc/` derivam do doomgeneric e seguem a **GPL v2**.

## Port: Doom

### ports/doom-dc — Doom (doomgeneric + Freedoom)

- Código do jogo: **doomgeneric** (GPL v2), em `ports\fontes-baixadas\doomgeneric\`,
  compilado **sem alterações**. Dados: **Freedoom 0.13.0** (BSD), `freedoom1.wad` no CD.
- Arquivos do port (GPL v2): `doomgeneric_dc.c` (vídeo/controle/tempo) e `w_file_dc.c`
  (leitura do WAD com cache de blocos, no lugar de `w_file_stdc.c`).
- Build: no shell do DreamSDK, `make` na pasta `ports\doom-dc`, depois
  `bash ../../ferramentas/mkcdi.sh doom.elf "DOOM DC PORT"` → `doom.cdi`.
  Precisa rodar do **.cdi** (o WAD de 27,5 MB não cabe na RAM).
- Controles: setas/analógico andar · A atirar/Enter · B usar/'y' · X próxima arma ·
  Y mapa · L/R andar de lado · START menu · analógico no talo corre.
- Lições:
  - **Depuração:** `Debug.SerialConsoleEnabled = yes` no `emu.cfg` do Flycast abre a
    janela "Flycast Serial Output" com o `printf` do programa. Use
    `setvbuf(stdout, NULL, _IONBF, 0)` para ver progresso sem quebra de linha.
    `.\ferramentas\screenshot.ps1 x.png -Titulo Serial` captura essa janela.
  - **CD é lento para leituras pequenas:** o Doom faz milhares de fseek+fread.
    Cache de blocos de 64 KB (LRU) resolveu o número de buscas.
  - **DMA do GD-ROM:** o `fs_iso9660` do KOS só usa o caminho rápido (stream/DMA) com
    buffer **alinhado em 32 bytes** e leitura no início de setor → `memalign(32)` +
    `fs_read` (não `fread`). Com isso o carregamento caiu de vários minutos para ~25 s.
  - **Som (`i_sound_dc.c`):** implementa o `DG_sound_module` do doomgeneric. Sons do WAD
    (formato DMX: PCM 8 bits sem sinal, 11/22 kHz) são convertidos para 8 bits com sinal
    (`x ^ 0x80`) e enviados ao AICA com `snd_sfx_load_raw_buf()` na 1ª vez que tocam.
    Volume/pan ao vivo via comando `AICA_CH_CMD_UPDATE` (como o `snd_stream_volume` do KOS).
    `compat/SDL_mixer.h` vazio evita editar o `i_sound.c` (include sem uso).
    `use_libsamplerate`/`libsamplerate_scale` precisam ser definidos no módulo.
  - **Música (`i_music_dc.c`):** o Freedoom guarda as 41 músicas em **MIDI** (o Doom
    original usa MUS → convertido com o `mus2mid.c` do próprio doomgeneric). Tocador de
    Standard MIDI File (trilhas, running status, tempo) + sintetizador de 20 vozes
    (timbre por família General MIDI, bateria no canal 10) → stream 22 kHz no AICA,
    numa **thread** própria (a música não engasga durante loading). As store queues são
    protegidas pelo KOS (`sq_lock`), então stream e PVR convivem.
  - **Otimização medida:** sintetizar tudo por amostra custava ~40% da CPU (FPS 26–28).
    Processando em **blocos de 32 amostras** (envelope/ganhos 1x por bloco com rampa,
    um laço por forma de onda, `if(fase>=1)` em vez de `(int)fase`): **5–12%**, FPS ~29–32.
  - **Painel de FPS** (`-DMOSTRAR_FPS` no Makefile): FPS e % de CPU da música.
  - **Saves na VMU (`vmu_saves_dc.c`):** o Doom salva em `/ram/` (RAM disk do KOS) e uma
    camada nossa sincroniza com a VMU (`/vmu/a1/DOOMSAV0..5`).
    - `--wrap` do linker (sem editar o jogo): `M_GetSaveGameDir` → `/ram/` (RAM disk não
      tem `mkdir`) e `rename` → cópia + `unlink` dentro de `/ram` (RAM disk não tem `rename`).
    - Cabeçalho VMS (descrição + ícone 32x32/16 cores): no KOS 2.3 passa-se com
      `fs_ioctl(fd, IOCTL_VMU_SET_HDR, &pkg)` e escreve-se só os dados; `fs_read` devolve os
      dados sem o cabeçalho. (Montar o pacote na mão gera o aviso "written without header".)
    - Medido: save de E1M1 = 61 KB → **LZSS** → 18 KB (122 → **36 blocos**). Gravar na VMU
      leva ~6 s no Flycast → feito numa **thread** (o 1º teste congelou o jogo 11 s).
    - Teste automático: `make TESTE=1` (entra em E1M1 e salva sozinho) e `make TESTE=2`
      (boot com `-loadgame 0`). Resumo da VMU repetido no log no 1º quadro.
- Pendências: medir FPS e testar saves no console real.

## Ferramentas de modelos baixados

- `ferramentas\inspecionar_glb.py modelo.glb` — partes, materiais, faces, tamanho.
- `ferramentas\previa_modelo.py modelo.glb saida.png` — vistas cima/lado/frente.
- `ferramentas\conv_glb_lod.py modelo.glb pasta prefixo N1 N2 ...` — limpa, reduz, gera .dms.
- Dependências Python (Windows): `py -m pip install trimesh fast-simplification scipy`.

## Aviso legal

Não versionar nem distribuir ROMs, BIOS ou assets de jogos comerciais. Assets devem ser
extraídos da sua própria cópia na hora do build.
