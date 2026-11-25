# Classic Racing GT - DC Edition — Documentação do projeto

> Nome até 24/11/2026: "PROTÓTIPO FÓRMULA". Pasta do projeto: `projetos/10-formula`.

Jogo de corrida de Fórmula 1 (era 1998–2000) para **Dreamcast**, na pista de **Interlagos**, feito do zero em C com KallistiOS (DreamSDK).
Este documento reúne o que foi feito até **23/11/2026**: decisões, técnicas, ferramentas, medições e lições. Serve de base para consulta futura.

> Pasta do projeto: `projetos/10-formula/`
> Código principal: `main.c` (≈ 9.500 linhas) + `som.c`
> CDIs: `formula.cdi` (pintura original do jogador) e `formula-sf3.cdi` (variante com pintura inspirada no Stewart SF3)

---

## Sumário

1. [Visão geral e decisões de projeto](#1-visão-geral-e-decisões-de-projeto)
2. [Como compilar, rodar e gerar o CDI](#2-como-compilar-rodar-e-gerar-o-cdi)
3. [Arquivos do projeto](#3-arquivos-do-projeto)
4. [Regras que valem sempre](#4-regras-que-valem-sempre)
5. [Histórico do desenvolvimento](#5-histórico-do-desenvolvimento)
6. [Como o jogo funciona, por área](#6-como-o-jogo-funciona-por-área)
7. [Técnicas do Dreamcast empregadas](#7-técnicas-do-dreamcast-empregadas)
8. [Opções de compilação (-D)](#8-opções-de-compilação--d)
9. [Ferramentas de teste e diagnóstico](#9-ferramentas-de-teste-e-diagnóstico)
10. [Desempenho: medições e orçamento](#10-desempenho-medições-e-orçamento)
11. [Lições aprendidas e armadilhas](#11-lições-aprendidas-e-armadilhas)
12. [Experimentos rejeitados ou revertidos](#12-experimentos-rejeitados-ou-revertidos)
13. [Backups](#13-backups)
14. [Pendências e próximos passos](#14-pendências-e-próximos-passos)

---

## 1. Visão geral e decisões de projeto

- **Gênero:** corrida 3D "arcade com peso", visual low poly com muitas texturas, inspirado nos jogos de corrida do Dreamcast (Speed Devils no céu; Sega GT, Le Mans 24h, F355 e Metropolis Street Racer como referência de detalhe).
- **Era:** F1 1998–2000. Carros sem halo, bico alto, bitola estreita, pneus sulcados, aro 13".
- **Pista:** Interlagos com traçado real (OpenStreetMap) e relevo real (SRTM).
- **Física:** 2,5D em coordenadas da pista (posição ao longo `s` + lateral `x`), com passos fixos de 1/60 s.
- **Câmeras:** atrás do carro e cockpit (com retrovisores e volante animado). Botão Y alterna.
- **Carros:** 8. Jogador com pintura original (azul-petróleo/amarelo, #7). Rivais com as cores reais das equipes de 1999 (McLaren #1, Ferrari #3, Jordan #8, Williams #6, Benetton #9, Prost #18, Sauber #11), **sem logos**.
- **Capacetes:** dos pilotos de 1999, aproximados.
- **Clima:** 7 modos (dinâmico, sol, nublado, chuva, chuva pesada, ensolarado, entardecer). Botão X troca.
- **Detalhe:** START alterna ALTO e NORMAL.
- **Som:** 100% sintetizado (V6 turbo).

Decisões importantes do usuário:

- Sem mecânicas de Road Rash (foi só referência).
- Céu no estilo **Speed Devils** (aprovado). Não mudar o formato das nuvens sem pedido.
- Piso aceitável de **30 fps**; o objetivo é 60 fps.
- Pilotagem v3 aprovada, a ser refinada no futuro.

---

## 2. Como compilar, rodar e gerar o CDI

Ambiente: DreamSDK R4 em `C:\DreamSDK` (GCC 13.2, KOS 2.3) e Flycast 2.7 em `ferramentas\flycast`.

```powershell
.\dc.ps1 build 10-formula        # compila
.\dc.ps1 run 10-formula          # compila e abre o .elf no Flycast
.\dc.ps1 cdi 10-formula -Titulo "CLASSIC RACING GT"     # gera formula.cdi (sempre recompila do zero)
```

Variante Stewart SF3, gerada à mão:

```bash
make OPC="-DPINTURA_STEWART"; cp formula.elf formula-sf3.elf
bash ferramentas/mkcdi.sh projetos/10-formula/formula-sf3.elf "CLASSIC RACING GT SF3"
```

Piloto automático, para assistir ou filmar: `make DEMO=1`. Pode ser combinado com outras opções: `make DEMO=1 OPC="-DCAM_INI=0"`.

Recursos gerados no PC (rodar quando o gerador mudar; os `.bin`/`.h` entram no executável por `.incbin`):

| Script | Gera | Observação |
|---|---|---|
| `gerar_ceu.py [pasta_previa]` | `ceu.bin` (céus em VQ) | ~50 s; usa o cache `serras_perfil.npz` |
| `gerar_texturas.py [pasta_previa]` | `texturas.bin` + `texturas.h` | offsets `TEXB_*`; importa `volante.py` (arte do cockpit) |
| `gerar_relevo.py [previa.png]` | `relevo.h` | relevo SRTM em grade de 40 m e nível das represas |
| `ajustar_altura.py --previa/--aplicar` | 2ª coluna de `pista_interlagos.h` | **não aplicado** (ver §12) |
| `fotos.py` | (módulo) | fotos do usuário; desligado por padrão (`FOTOS=1` liga) |

---

## 3. Arquivos do projeto

| Arquivo | Conteúdo |
|---|---|
| `main.c` | tudo do jogo: pista, física, carros, cenário, céu, clima, efeitos, HUD |
| `som.c` / `som.h` | síntese de áudio (motor V6, rivais, pneus, chuva, trovão, torcida) |
| `pista_interlagos.h` | traçado (rumo e altura por metro), minimapa, curvas, boxes, prédios do OSM |
| `pista_osm.h` | estruturas do formato de pista OSM |
| `relevo.h` | relevo real em volta da pista e `REL_AGUA` (nível das represas) |
| `ceu.bin` | panoramas do céu (limpo 4096, fechado 4096, entardecer 3072) em VQ |
| `texturas.bin` / `texturas.h` | texturas geradas no PC e seus offsets |
| `Makefile` | build; `main.o` depende de `ceu.bin`, `texturas.bin`, `texturas.h` e `relevo.h` |
| `estudo/` | modo estudo com som gravado (nunca entra no CDI) |
| `referencia_video/` | vídeo onboard de referência (só estudo, não distribuir) |
| `fotos_texturas/` | fotos do usuário (céu e prédios) + prévias |
| `previa_tecnicas/` | imagens de comparação das técnicas |
| `comparacao_vq/` | comparação da compressão VQ do céu |
| `ceu_*.bin`, `texturas_antes_fotos.bin`, `gerar_ceu_versao_serra_alta.py` | versões anteriores guardadas para voltar atrás |

---

## 4. Regras que valem sempre

- **Marcas:**
  - Sem marca SEGA no nome do jogo. Nas placas, "FEITO PARA DREAMCAST" só em texto, sem logo.
  - Publicidade só com **marcas fictícias** (NITRAX, ROTA, VOAR, CREDIX, HORIX, GELO+, VELTRO, SINALCOM, RODAX).
  - Cores reais das equipes são permitidas; logos de patrocinadores reais, não.
- **Som:** sempre original (sintetizado). Gravações reais só no modo estudo, nunca no CDI.
- **ROMs e assets de terceiros:** nunca versionar nem distribuir.
- **Edição dos fontes:** nunca com `Get-Content`/`Set-Content` do PowerShell 5.1 (estraga o UTF-8). Usar o editor ou scripts Python.
- **Patches grandes:** escrever um `.py` em arquivo (heredoc com aspas corrompe escapes).
- **Capturas de tela:** só da janela do Flycast (PrintWindow), nunca da tela inteira.
- **Backup:** sempre antes de mudanças grandes (pasta `backups/`).
- **Commit:** só quando o usuário pedir.

---

## 5. Histórico do desenvolvimento

O projeto sucede o `09-corrida` (Super Retro GT DC, pseudo-3D), arquivado em 18/11/2026.

### 18/11 — Protótipo e base
- Pista como malha 3D (fatias de 4 m, 9 faixas, LOD a 200 m, recorte no plano próximo, névoa de tabela).
- Monoposto low poly, 8 carros com IA, câmera atrás e cockpit.
- Derrapagem estilo Super Monaco GP, marcos de Interlagos, retrovisores por render-to-texture, som sintetizado.
- Terreno em grade de 40 m (resolveu objetos cobertos pela grama).
- Clima: dinâmico, sol, nublado e chuva; depois chuva pesada, poças, spray e relâmpagos.
- Carro detalhado com dois níveis (simples e detalhado), estilo F1 1998–2000, pinturas em blocos, rodas que giram com borrão, volante 2D completo.
- Sensação de velocidade: vórtices na asa traseira, campo de visão que abre, trepidação.

### 19/11 — Som, colisão e pilotagem
- Som V10 rejeitado, V6 turbo (v2) aceito.
- Fila de áudio (acabou o picotado) e colisão suave.
- Pilotagem v2 e depois v3 (aprovada).
- Decalques nos carros: faixas, números, capacetes. Cores reais das equipes de 1999 nos rivais.

### 20/11 — Detalhe e primeiras otimizações
- Carro arredondado (lofts octogonais), pneus de 12 lados, sombreamento suave por vértice, detalhes extras.
- Passos fixos de física (o jogo não fica em câmera lenta abaixo de 60 fps).
- Primeira rodada de otimização: matriz do SH4 (XMTRX/ftrv), teste de costas antes de transformar, fsrra, filtro de tela.
- Arquibancadas, casas do OSM, árvores, boxes, fiscais, mecânicos.

### 21/11 — Céu, texturas e técnicas da época
- **Skybox estilo Speed Devils:** céu pintado no PC (`gerar_ceu.py`) com horizonte em camadas e geografia real (cidade ao norte, Serra do Mar ao sul, represas). Compressão VQ (o céu caiu de 4,5 MB para 0,74 MB).
- Modo entardecer com sol pintado atrás de faixas de nuvem.
- Texturas ricas: asfalto com remendos, faixas de corte na grama, brita, zebras.
- Pintura metálica com reflexo no carro do jogador.
- Malha do carro compilada (vértices únicos) e laço em assembly SH4.
- Mipmaps em todas as texturas do cenário.
- Luz pré-calculada (pista e terreno) e árvores em "X".
- Horizonte pintado (cidade), marcas de frenagem, rodas borradas texturizadas, oclusão ambiente nos carros.
- Fachadas de prédios e telhados, brilho do sol no asfalto, sombras de nuvens.

### 22/11 — Relevo real, fotos e correções
- Grade espacial no cenário (desempenho).
- **Relevo real (SRTM)** em volta da pista; represas detectadas; **serras reais** no céu (silhueta até 45 km).
- Sombras projetadas das arquibancadas, tons por clima (color grading), fachadas 128.
- Correções:
  - Placas flutuando, prédios invadindo o muro, casas nos boxes, telhados em prédios do autódromo.
  - Arquibancadas duplicadas (piscavam).
- Gramado com manchas, névoa nos vales, sujeira nos carros, faixas e bandeiras da torcida.
- Arquibancadas 3D com degraus.
- Física na grama: altura interpolada, inclinação e barranco suave.
- Vídeo de referência analisado (pole de 2023): o perfil de altitude confere; quebras arredondadas mantidas.
- Fotos do usuário testadas (céu e prédios) e **revertidas**: serão refeitas com fotos melhores. Serras recoloridas e mais baixas (aprovado).

### 23/11 — Técnicas próprias do Dreamcast e carro híbrido
- **Listas de recorte (PT), volumes modificadores (sombras), relevo por hardware (bump), paleta de 8 bits (torcida).**
- Nuvens em movimento, pássaros, paddock, pódio, torre de controle, torcedores nas grades.
- **Antisserrilhado (FSAA)**, chuva e partículas com **sprites** do hardware.
- **Telão com imagem ao vivo** (RTT) e **reflexo na pista molhada** (RTT com câmera espelhada).
- Fiscais com bandeiras amarela, azul e verde; gotas na tela; safety car, ambulância, reboque.
- **Asfalto 512 em VQ com mipmaps**; grama, brita e concreto 256 em VQ.
- Textura de detalhe; relevo nas zebras e muros.
- Cidade com 3ª camada; entardecer com nuvens acesas; sinalizadores e papel picado.
- **Torcida sem cintilar** (causa real encontrada com sequências de capturas em movimento).
- **Carro híbrido nos rivais:** corpo do modelo simples com textura "assada" do detalhado + peças finas em geometria + reflexo barato. **Aprovado e ligado por padrão.**

### 23/11 (noite) — Os 13 itens: brilho, TV, HUD, replay, título
- **Brilhos ("bloom"):** halos somados no reflexo do sol na pintura dos rivais e na luz traseira na chuva (`brilho_add`, `desenhar_brilhos_tr`).
- **Vinheta de TV** (`desenhar_vinheta`) e **linhas de velocidade** acima de cerca de 225 km/h (`desenhar_velocidade`, o "desfoque de movimento" da época).
- **Luz manchada pelas árvores:** a sombra das copas é assada em `somb_d`/`somb_t`. O carro também escurece na sombra assada da pista.
- **Zebras em relevo:** dentes serrilhados nas 30 fatias perto (`desenhar_zebra_dentes`). **Água escorrendo** nas zebras na chuva (`desenhar_agua_zebra_tr`).
- **Beira da pista:** pneus empilhados nas áreas de brita e cones na entrada do pit.
- **Carros:** viseira refletindo o céu; desgaste dos pneus (escurecem em 6 voltas); calor do escapamento (`desenhar_calor_tr`).
- **Animação por paleta** na torcida PAL8 (`animar_paleta_torcida`); papel picado em retângulos que giram.
- **Híbrido no jogador:** opcional (`-DJOGADOR_HIB`, desligado; o jogador já tem pintura metálica).
- **HUD próprio** (`desenhar_hud_novo`):
  - Fonte 5×7 desenhada no código, em itálico e com contorno (`FONTE`, `hud_texto`).
  - Caixas de posição e tempos, conta-giros em 16 segmentos, marcha, velocidade e minimapa (textura 128 gerada das fatias, `criar_hud`).
  - `-DHUD_ANTIGO` volta ao HUD anterior.
  - Ajuste: HUD cerca de 20% menor. Fonte em 512×128 (células de 32 px, supersampling 4×4, contorno suave) e minimapa em 256×256 com linha suavizada. Os marcadores dos carros ficam por cima do mapa.
  - Ajuste 2: mais 10% menor. A fonte virou traços suavizados (cápsulas ligando os pontos 5×7, borda analítica) em 256×256 com mipmap; o minimapa também ganhou mipmap. FPS no canto superior direito (verde ≥58, amarelo ≥30, vermelho abaixo).
- **Replay com câmeras de TV:**
  - Grava os últimos 26 s a 30 Hz (`rp_buf`, cerca de 0,6 MB); toca 20 s interpolando as amostras.
  - Câmeras: postos fixos a cada 200 m do lado de dentro da curva (com zoom), helicóptero e câmera em cima do carro.
  - Botão BAIXO durante a corrida; A/B/START volta. No DEMO, o replay entra sozinho a cada volta. `-DREPLAY_TESTE=s` dispara depois de s segundos.
- **Tela de título:**
  - Câmera girando em volta do carro no grid; esquerda/direita escolhem o clima; START começa.
  - No DEMO começa sozinha após 7 s; `-DSEM_TITULO` pula; testes com `S_INI` também pulam.
- **Medido (Flycast):** folga E de 3–13 ms na chuva e no entardecer com 8 carros.

### 23/11 (noite) — Cockpit 3D
- **Motivo:** a arte 2D colada na tela parecia chapada, sem profundidade, e a ordem das camadas punha os braços atrás do volante.
- **Olho do piloto na posição real:** dentro do capacete, 0,765 m do chão e 0,42 m à frente da base (antes: 1,02 m e atrás do capacete). Com FOCO 290, o próprio carro 3D forma o "V" das referências. Não há mais 2D na visão interna (só o HUD).
- **`desenhar_cockpit3d()` (lista opaca) e `desenhar_cockpit3d_pt()` (face do volante):**
  - geometria 3D no espaço do carro, iluminada por `ck_luz()` (luz da cena, céu e especular), como o carro de fora;
  - **volante:** centro (0; 0,585; 0,745), inclinado 17°, girando em volta da própria normal. Tem contorno de carbono com 2,4 cm de espessura, face em textura ARGB1555 256×128 **sem compressão** (`TEXB_VOLANTE_FACE`, de `cockpit_arte.face_1555`), manoplas cilíndricas de camurça, coluna de direção, LEDs e dígitos como polígonos sobre a face;
  - **luvas:** dorso em casca do lado de fora, gomos claros, dedos só nos trechos visíveis, pontas de camurça, polegar cilíndrico apontando para o centro e punho;
  - **antebraços:** cone de 8 lados do cotovelo (0,15; 0,585; 0,56) até o pulso, **na frente** do volante (resolvido pelo z-buffer). Tecido tingido com a cor da equipe e faixa na cor secundária. O cotovelo acompanha 40% do pulso;
  - **retrovisores retangulares:** caixa na cor do carro em (±0,405; 0,648; 0,875), moldura escura, vidro com o render-to-texture e haste;
  - **bordas acolchoadas** da abertura do cockpit.
- **Texturas repetidas 64×64 com mipmap** (tecido, camurça, luva), geradas no boot (`ck_tile`): detalhe nítido de perto, como os jogos da época.
- **Desempenho:**
  - cilindros com luz e transformação uma vez por "anel" e enviados como faixa de triângulos (strip);
  - dedos desenhados só onde aparecem;
  - custo de cerca de 1,4 ms;
  - 55–60 FPS na corrida e 50 na largada (Flycast).
- `-DCOCKPIT_2D` volta ao cockpit pintado em 2D (o v2 abaixo).
- **Ajuste pela referência de simulador** (olho pouco acima das laterais, câmera olhando para o volante):
  - **Câmera:** olho a 0,728 m e 0,40 m à frente da base; olha cerca de 7° para baixo; FOCO 270.
  - **Laterais altas do cockpit** (só na visão interna): seção de 6 pontos (parede interna acolchoada, quina arredondada, topo envernizado na cor da equipe com reflexo do céu e filete na cor secundária). A borda desce perto da frente.
  - **Arco escuro da frente** (carbono + borda de camurça) logo acima do volante; o bico aparece por cima.
  - **Chassi maciço:** as faces do modelo dentro da abertura (|x| < 0,168, z −0,1..0,86, y 0,30..0,64) saem do cockpit (`atras = 2`). No lugar aparece o interior escuro.
  - **Volante** mais alto (centro y 0,612). Cotovelos baixos (0,155; 0,47; 0,47): braços longos subindo de baixo. Pernas mais baixas e na sombra.
  - **Retrovisores:** contorno de 12 pontos com cantos arredondados (superelipse), 17,6 × 8 cm, carenagem com profundidade, moldura e vidro octogonal com a imagem de trás. Ficam no alto, ao lado dos pneus, com haste dupla.
  - **Luvas** cerca de 15% maiores; punho como anel curto alinhado ao antebraço.
  - **Desempenho:** 50–60 FPS na corrida, cerca de 49 na largada.
- **Refinamentos do cockpit 3D:**
  - **Verniz (`ck_verniz`)**, por vértice, nas laterais e na carenagem dos retrovisores. Reflete o céu, o horizonte e o asfalto conforme a direção refletida, com Fresnel (mais reflexo de lado) e brilho do sol. As laterais usam malha mais fina (7 fatias) para o degradê.
  - **Volante:** chanfro iluminado na borda; 3 seletores e 7 botões saltados em 3D; brilho dos LEDs vazando na lista translúcida (`desenhar_cockpit3d_tr`).
  - **Luvas:** punho em duas cores; sombra das mãos sobre a face do volante.
  - **Movimento:** a cabeça vai para fora da curva (força G pelo esterço × velocidade) e o olhar procura a tangência. O cockpit escurece sob passarelas, arquibancadas e árvores (`ck_brilho`).
  - **Chuva:** gotas escorrendo para trás no verniz das laterais; vidro dos retrovisores embaçado.
  - **Largada:** retrovisor a 10 Hz com o carro abaixo de 8 m/s.
- **Mãos com dedos individuais:**
  - 4 dedos de cada lado, com espessura e posição próprias e o médio mais comprido;
  - cada dedo tem a falange de cima (do nó, por fora, até trás da manopla) e a de dentro (volta pelo lado de dentro, com a junta e só a pontinha de camurça);
  - protetor de borracha sobre cada nó;
  - polegar com duas falanges e junta;
  - dorso abaulado com painéis claros.
- **Detalhes técnicos:** macro `PG(altura, ângulo, raio)` posiciona pontos em volta do eixo da manopla. A parte de trás nunca é desenhada.
- **Texturas do cockpit 3D em 128×128, pintadas no PC** (`cockpit_arte.tiles_cockpit`, `TEXB_CK_TILES`, com mipmap; 128 KB):
  - tecido em sarja com rugas e costura com pesponto;
  - camurça com fibras direcionais;
  - luva com poros, painéis costurados, furos de ventilação e vincos;
  - carbono em sarja 2×2 com brilho por fio.
  
  O relevo vem de mapas de alturas periódicos (sem emenda). `-DCK_TILE_64` volta às texturas 64×64 antigas, geradas no boot.
- **Pintura das laterais do cockpit:**
  - 5ª textura (`hdr_ck_pintura`): "casca de laranja" do verniz, micro-riscos de polimento e poucos pontinhos suaves de borracha, com cerca de 25 cm por repetição;
  - **sujeira de pista** por vértice nos segmentos de baixo e de fora, mais forte na frente (spray dos pneus), crescendo com `carro_t.sujeira`;
  - o verniz (`ck_verniz`) continua por cima.
- **Cockpit, mais vida:**
  - dedos com 7 lados e dorso da luva com 9;
  - **visão de dentro do capacete** (`desenhar_capacete`): borda da viseira, cantos e reflexo. **Removida a pedido do usuário**; `-DCK_CAPACETE` religa;
  - **troca de marcha:** o número pisca invertido no visor por 0,3 s e a borboleta do lado acende (verde = sobe, laranja = desce);
  - **vibração própria do volante e das mãos** (`ck_trep`, vindo da trepidação da câmera) em zebras, grama e toques.
- **Dedos legíveis do lado de dentro:** a falange de dentro fica mais afastada da manopla e 8% mais grossa, numa cor da luva um pouco mais clara, com menos sombra de contato e junta arredondada. Só os 12% finais são a ponta (mistura de luva com camurça), porque era a ponta que ficava de frente para o piloto e deixava tudo cinza.
- **Cockpit 3D, detalhes e profundidade:**
  - **Interior de verdade:** a "tampa" escura da abertura do carro (necessária na vista de fora) sai da visão de cockpit (`atras = 2`). No lugar entram paredes internas, painel da frente e assoalho em fibra de carbono texturizada, escurecendo para baixo.
  - **Pernas do piloto:** coxas subindo até os joelhos, que ficam altos, logo abaixo do volante (posição de pilotagem de F1). Canelas descem para os pedais.
  - **Antebraço com forma de braço:** dois cones, grosso no cotovelo, com a "barriga" do músculo no terço e fino no pulso.
  - **Luvas:** dorso afinando no pulso; nós dos dedos em relevo (protetor de borracha); polegar curto.
  - **Sombra de contato** nos cilindros (`ck_ka`/`ck_kb`): escurece onde as peças entram no cockpit, sob a luva e na dobra do joelho.
  - **Texturas repetidas pintadas com relevo:**
    - tecido com dobras e costura de painel;
    - luva com painéis, pesponto, furos de ventilação e vincos;
    - camurça;
    - carbono em sarja com brilho.

### 23/11 (noite) — Cockpit v2, a partir de referências reais
- **Referências** em `referencia_cockpit/`: fotos de volantes e cockpits de 1999 e capturas de simuladores. Só estudo: nada é copiado e nenhum nome ou logo é reproduzido.
- **Prévia aprovada antes de mexer no jogo:** `previa_cockpit.py` monta a arte por cima de uma captura limpa (build com `-DSEM_VOLANTE -DPREVIA_COCKPIT`). Resultado em `previa_tecnicas/cockpit_comparacao.png`.
- **`cockpit_arte.py`** pinta em coordenadas da tela, numa "janela" com escala própria, e reduz para a região da textura. Gera duas texturas VQ ARGB1555 por pintura (original e SF3):
  - `TEXB_VOLANTE`: volante e luvas (janela da tela 170..470 × 344..504), cerca de 1,7 × 3,2 texels por pixel;
  - `TEXB_COCKPIT`: lateral da carroceria, retrovisor com haste, manga do macacão e vão + borda da frente.
- **Composição nova:**
  - laterais da carroceria em "V" na cor da equipe, com verniz, filete e acolchoado na borda de dentro;
  - vão escuro com joelhos e coluna de direção;
  - volante menor e central (VOL_CY 408), com aros vazados, manoplas de camurça, LEDs, visor, botões com etiquetas e seletores com anel colorido;
  - luvas na cor da equipe segurando o topo das manoplas;
  - braços subindo de baixo, com sombra translúcida sobre a carroceria.
- **Retrovisores:** carenagem em gota no alto, ao lado dos pneus, com haste, junção e parafuso. O vidro fica embutido, com a imagem real (RTT) e brilho translúcido por cima (`desenhar_brilho_espelhos`).
- **Na lista opaca** ficam só os LEDs acesos, os dígitos do visor (marcha, velocidade e barra de rotação).
- `-DVOLANTE_ANTIGO` ainda volta ao cockpit de polígonos.

### 23/11 (noite) — Cockpit pintado
- **`volante.py`** pinta no PC, com supersampling 2×, a arte do cockpit:
  - volante de carbono (trama em sarja, verniz, bisel), manoplas de borracha com sulcos, borboletas de alumínio;
  - botões em domo brilhante, seletores serrilhados, cubo metálico, visor e moldura dos LEDs;
  - luvas acolchoadas (dedos cilíndricos, costuras, protetor dos nós, polegar);
  - tecido do macacão para os antebraços, em cinza que o jogo tinge com a cor da equipe;
  - carenagem de carbono dos retrovisores.
- **Técnica:** a iluminação vem do "relevo" das máscaras: a máscara borrada vira altura, e dela saem as normais, a sombra e o brilho especular.
- **Formato:** compressão **VQ ARGB1555** sem mipmap (512×512 em 66 KB) feita no gerador (`volante.vq_1555`), entrando em `texturas.bin` como `TEXB_VOLANTE`.
- **No jogo:**
  - o volante é **1 quad** na lista PT que gira com o esterço (`desenhar_volante_pt`);
  - os antebraços são quads texturizados;
  - a carenagem dos retrovisores tem o miolo transparente (`desenhar_espelhos_pt`);
  - na lista opaca ficam só os LEDs acesos, os dígitos do visor, os punhos e a faixa do macacão.
- `-DVOLANTE_ANTIGO` volta ao volante de polígonos.
- **Cockpit v2:**
  - **Volante:** sombras de contato (`sombra()`); placa central rebaixada com 6 parafusos; etiquetas em Arial Bold rasterizadas no PC (N, PIT, RAD, OK, BEB, LIM, MIX, DIF, escalas 1–6, "+/−" nas borboletas); LEDs com lente; visor com retroiluminação; anel amarelo de engate rápido; adesivo do nº 7; borracha das manoplas em losango.
  - **Luvas:** pontas dos dedos e polegar em camurça, vincos, protetor com furos de ventilação, painel acolchoado costurado e faixa de velcro com aba.
  - **Tecido:** painéis com costura e pesponto, punho canelado, dobra do cotovelo e sombra entrando no cockpit.
  - **Bordas laterais do cockpit** (capitonê com friso de carbono) na área livre da textura (384..511 × 256..511).
  - **Luz:** toda a arte 2D é modulada por `luz_cockpit()` (brilho da cena e tom dourado no entardecer).
- **Cockpit v3:**
  - **Mãos na posição certa:** dorso da luva do lado de fora da manopla, nós dos dedos ao longo da manopla, pontas dos dedos aparecendo na borda de dentro e polegar deitado na frente, apontando para o centro.
  - **Braço ligado à luva:** o antebraço começa por baixo do pulso; o punho na cor da equipe fica no pulso (x ±194).
  - **Área da arte:** x −226..226, y −118..108.
  - **Capitonê** das bordas laterais mais claro.
  - **Pneus dianteiros do jogador com 16 lados** (`carro_disco16`); os traseiros continuam com 12.
  - **Risco pontilhado corrigido:** era a borda do quad do volante, onde o filtro bilinear puxava a linha vizinha da textura (tecido opaco). As coordenadas agora ficam a 1–2 texels das divisas entre as artes.
  - A sombra por volume do carro a menos de 3 m da câmera é pulada (no cockpit ela nem aparece).
- **Cockpit v4:**
  - **Duas texturas VQ 512×512**, cada uma com seu próprio livro de códigos:
    - `TEXB_VOLANTE`: volante e luvas ocupando a textura inteira (desenho 1024×512, reduzido 2:1 só na horizontal), com o dobro da resolução vertical;
    - `TEXB_COCKPIT`: tecido 512×256 em cima, retrovisor em (0..255, 256..383) e lateral do cockpit 256×256 em (256..511, 256..511).
    - Total de 132 KB de memória de vídeo.
  - **Dedos:** cada um é uma cápsula de 2 falanges saindo da borda de dentro da manopla e apontando para a palma, com junta, vão entre os dedos e ponta de camurça.
  - **Braço:** o cotovelo acompanha 60% do deslocamento do pulso (e 45% na vertical), então o braço inteiro se move em vez de só esticar. O punho virou parte do tecido (canelado tingido); acabaram os retângulos presos ao volante.

### 24/11 — Carro em .obj (modelo externo, licença gratuita)
- **Origem:** `modelo_obj/Car.obj` + `Car.mtl` + `Team01..11.png`. São 433 triângulos com UV e normais e 11 pinturas fictícias de 256×256 (rodas, capacete e número na mesma textura).
- **`conv_obj.py`** gera `carro_obj.h`:
  - escala 0,85 e largura × 0,80 (bitola de 1998, entre-eixos 3,1 m), chão em y = 0, origem entre os eixos, z para frente;
  - separa a carroceria, as rodas 1..4 (padrão do jogo) e o capacete pelos componentes conexos;
  - gera **292 vértices únicos** (`OBJ_V`), triângulos indexados (`OBJ_T`) e UVs por canto (`OBJ_UV`);
  - corrige a ordem dos cantos pela normal;
  - extrai as 2 cores principais de cada pintura (`OBJ_COR`, para o HUD, os reflexos e o cockpit).
  
  `obj_texturas()` comprime as pinturas em VQ com mipmap (cerca de 24 KB cada, `TEXB_OBJ_TEX0..10`).
- **No jogo** (`desenhar_carro_obj`):
  - **Desenho:** uma matriz carro→câmera; cada vértice único é transformado e iluminado uma vez; luz por vértice no espaço do carro, sujeira embaixo; descarte de costas no espaço da câmera (a conversão inverte a orientação, então o teste é `n·A <= 0`).
  - **Rodas:** giram (estroboscópicas acima de 14 m/s) e as dianteiras esterçam.
  - **Rivais:** todos usam o `.obj` com as equipes que sobram. O híbrido não é mais "assado" (boot mais rápido, mais memória de vídeo livre).
  - **Jogador:** escolhe na tela de título, com cima/baixo, entre "CLASSIC GT" (o carro original) e as equipes 01–11.
  - **Cockpit com o `.obj`:** o capacete e a região em volta da câmera somem (`obj_ck_pula`).
- **Opções:** `-DJOGADOR_OBJ=n` (teste) e `-DSEM_CARRO_OBJ` (volta tudo ao modelo anterior).
- **Desempenho:** custo dos carros 6,7–7,4 ms (híbrido 4,4–6,2); largada 50 FPS, corrida 57–60.
- **Armadilha:** gerar código em Python via heredoc quebra os escapes `
`; usar a ferramenta de escrita ou `chr(10)`.

---

## 6. Como o jogo funciona, por área

### 6.1 Pista
- `montar_pista()` integra o **rumo real** de cada metro (OSM) para obter a planta. O erro de fechamento da volta é espalhado e o traçado é suavizado.
- **Fatias** de 4 m (`F[k]`): posição, eixos, curvatura, `vlim` (velocidade boa para a IA), zebra, brita, boxes.
- Pontos laterais `LAT[]`:
  - `±7` asfalto, `±7,3` e `±8,6` zebra, `±26` fim da grama.
  - A faixa de grama desce até o terreno (`borda_y`, no máximo 3 m) e, onde a pista está em aterro, um talude (`desenhar_saias`) desce até o chão.
- `montar_visiveis_dir()`: fatias visíveis (LOD de 12 m após 200 m) e bits de fora da tela (`vis_t.oc`).
- **Texturas da pista:**
  - Asfalto 512 em VQ com mipmap e especular (brilho do sol).
  - Grama cortada em listras (`hdr_corte`), brita, zebra, tom do asfalto por trecho.
  - Por cima: relevo por hardware, textura de detalhe, reflexo, marcas de frenagem, borracha solta, borda irregular da grama, grelhas, marcas de pneu na grama.
- **Luz pré-calculada** (`assar_luz`): oclusão no pé dos muros e boxes, sombra de contato das construções e sombras projetadas das arquibancadas (dia e entardecer, misturadas pelo clima). Multiplicada no vértice por `cao()`, junto com as sombras de nuvens (`nuv_f`).

### 6.2 Física (`fisica()`)
- Passos fixos de 1/60 s com acumulador de tempo real; 1 a 4 passos por quadro.
- **Motor e arrasto:** `motor = min(11, 1000/(v+1))` e `arrasto = 0,15 + 0,00142 v²`, o que dá cerca de 315 km/h em 7ª.
- **Aderência e derrapagem:**
  - Faixa de derrapagem até 1,5 × o limite de aderência.
  - Sobre-esterço com potência abaixo de cerca de 115 km/h (só o jogador).
  - Círculo de aderência: frear forte tira aderência lateral.
- **Superfícies:** vácuo (até 45 m), zebra (−8%), grama (aderência cai gradualmente em 1,5 m), brita (freia forte).
- **Grama:** usa a inclinação da própria grama; o carro desliza no barranco a no máximo 1 m/s; muro com batida só acima de 3 m/s.
- **Chuva:** aderência −32% e freio −28%. A pista molha em cerca de 10 s e seca em cerca de 2 min.
- **Altura e inclinação:** `base_carro()` usa a altura interpolada (`chao_preciso`) e inclina o carro para a frente e para os lados conforme a grama.

### 6.3 Carros
- **Geometria gerada por código** (`montar_carro_simples` / `montar_carro_detalhado`): caixas, troncos, lofts octogonais, discos, decalques. Limite `MAX_FACES_CARRO` 3000.
- **Níveis** (`face_t.miudo`): jogador completo, rival completo, médio e simples. Listas compiladas `LST_*` e vértices únicos (`compilar_malha`).
- **Desenho** (`desenhar_faces`):
  - Uma matriz carro→câmera na XMTRX, laço em assembly `vu_lote`.
  - Rodas com matriz própria (giro). Cabeça com deslocamento lateral e de freada.
  - Luz por vértice nas faces suaves.
- **Jogador:** pintura metálica (textura de ambiente e Blinn) e tinta com textura; carbono texturizado.
- **Oclusão ambiente assada** (`assar_oclusao_carro`): voxels de 6 cm e 14 raios por vértice. Mais a sujeira acumulada na parte de baixo.
- **Rivais = CARRO HÍBRIDO** (`CARRO_TEX`):
  - No boot, `assar_carros_tex()` **rasteriza por software** (no processador) as 5 vistas do modelo detalhado numa textura 256×256 por carro. Escala de 46 px/m, rasterizado em 512 e reduzido 2×2. Entram a cor × oclusão × forma, mais faixa, número e capacete copiados da RAM.
  - Na corrida, o corpo usa o **modelo simples** e cada face pega a vista da sua normal (`ctex_uv`), com reflexo barato (Fresnel do céu e brilho do sol por vértice).
  - Até 110 m, rodas, asas, placas e suspensão vêm em **geometria** (`hib_lista`, lista de vértices `lista_v[5]`).
- **Sombra:** volume modificador com o contorno do carro e as 4 rodas, mais mancha suave na lista translúcida.
- **Efeitos:** luz traseira piscando na chuva, disco de freio aceso, vórtices, faíscas, spray, fumaça e poeira (sprites), marcas na grama.

### 6.4 Cenário
- **Prédios do OSM** (`montar_cenario`):
  - Filtrados por distância real à pista: nada a menos de 30 m; casas pequenas a mais de 210 m; arquibancadas duplicadas removidas.
  - 6 fachadas por tipo, telhado de duas águas nas casas, janelas acesas no entardecer, vidro com reflexo.
  - Silhueta texturizada até 460 m.
- **Grade espacial** de 100 m: visita só as células visíveis, da mais perto para a mais longe.
- **Arquibancadas:**
  - Rampa texturizada de longe; perto de 38 m, **degraus 3D** com torcida sentada, corredores, guarda-corpo e pilares.
  - Bandeiras de países (3×2 m), faixas da torcida, sinalizadores e papel picado.
  - Torcida em paleta de 8 bits; versão suavizada de longe (sem cintilar).
- **Boxes:** garagens com cores das equipes, andar envidraçado, mecânicos, pódio, torre de controle e paddock (caminhões e carros).
- **Árvores** em "X" com textura vazada (folhosa, pinheiro, ipês de 4 cores) na lista de recorte; arbustos.
- **Pessoas e estruturas:** fiscais com bandeiras, fotógrafos, torcedores nas grades, postes (acesos no entardecer), torres de TV, passarela com painel, pórtico com luzes da largada, telão com imagem ao vivo, safety car, ambulância e reboque.
- **Terreno:**
  - Relevo real (SRTM) a partir de 60–180 m da pista; perto, acompanha a pista.
  - Represas reais; lago com textura animada.
  - Gramado com manchas e névoa nos vales; luz assada que acompanha o clima.
- **Muros:** concreto texturizado (VQ, com relevo do lado do sol), painéis de publicidade fictícia e alambrado.

### 6.5 Céu e horizonte (de longe para perto)
1. **Céu pintado** (`ceu.bin`, cilindro de 4 km):
   - Nuvens na fórmula aprovada e serras reais (SRTM até 45 km, exagero moderado), com cidade e morros pintados.
   - Três variantes com transição suave: limpo, fechado e entardecer.
2. **Nuvens em movimento:** anel translúcido de 2 camadas, douradas do lado do sol no entardecer.
3. **Anel de morros** a 1,7 km (altura real), mais aviões para Congonhas e pássaros.
4. **Cidade:** 3 camadas (a do fundo em contraluz no entardecer), névoa urbana e faixa de névoa entre as camadas, janelas acesas.
5. **Anel de bairros** a 1,1 km: telhados, árvores, luzes e trânsito no entardecer.
6. **Sol:** halo, raios, clarão, reflexo de lente e feixes de luz.

### 6.6 Clima e luz
- **Variáveis contínuas:** `nuvens`, `chuva`, `pesada`, `manha`, `tarde` e `molhado`.
- **Dinâmico:** ciclo de 6 minutos com pontos-chave interpolados.
- **Luz:** direção `LUZ` da manhã (leste) para o entardecer (oeste, baixa); `luz_base`, `luz_dir` e `brilho`.
- **Névoa** de tabela pela cor do horizonte; com céu limpo, vai mais longe.
- **Tons por clima:** quad multiplicativo na tela inteira (entardecer dourado, chuva fria).
- **Chuva:** sprites, gotas na tela no cockpit, poças, reflexo na pista, spray, relâmpago e trovão, vapor quando o sol volta.

### 6.7 Som (`som.c`)
- V6 turbo sintetizado: pulso por explosão excitando ressonadores, saturação suave, turbo e wastegate.
- Rival mais próximo com Doppler.
- Pneu, grama, chuva, trovão, torcida e bips da largada.
- **Fila de áudio:** a síntese acontece em pedaços dentro do quadro, e o callback só copia.

### 6.8 HUD
- Posição, volta, tempo, velocidade, marcha, nome do trecho e clima.
- Painel atualizado a 15 Hz. Volante com LCD e luzes de troca no cockpit; retrovisores a 20 Hz.

---

## 7. Técnicas do Dreamcast empregadas

| Técnica | Onde | Observação |
|---|---|---|
| Matriz do SH4 (XMTRX, `ftrv`) + `fsrra` | todo o 3D | transformação e 1/z sem divisão |
| Laço em assembly SH4 | vértices do carro | ganho pequeno (3–4%) |
| Envio direto pela store queue | todo o 3D | `pvr_dr_target` |
| Mipmaps (cadeia montada na RAM) | texturas do cenário | some o chuvisco de longe |
| Compressão VQ | céu, asfalto 512, grama/brita/concreto 256 | VQ **com** mipmap funcionou |
| Paleta de 8 bits (PAL8) | torcida | metade da memória |
| Lista de recorte (punch-through) | árvores, faixas | sem custo de ordenação |
| Volumes modificadores (cheap shadow) | sombra dos carros e estruturas | trabalho do chip de vídeo |
| Relevo por hardware (bump) | asfalto, zebras, muros | no Flycast o resultado sai no alfa |
| Render-to-texture | retrovisor, telão ao vivo, reflexo molhado | **não** serve para "assar" texturas (no Flycast saiu em blocos) |
| Especular (cor somada) | pintura, vidro, asfalto, reflexo dos rivais | |
| Sprites do PVR | chuva, fumaça, spray, poeira | 1 vértice por partícula |
| Antisserrilhado horizontal (FSAA) | tela | todo X enviado é multiplicado por `XS = 2` |
| Névoa de tabela | tudo | cor do horizonte |
| Luz e oclusão pré-calculadas | pista, terreno, carros | custo zero em tempo real |
| Texturas pintadas no PC e embutidas (`.incbin`) | céu, texturas | |
| Níveis de detalhe | carros (4 níveis), cenário (silhuetas), torcida | |
| Grade espacial | prédios e árvores | |
| **Textura "assada" por software no boot** | carros rivais (híbrido) | técnica central da última fase |
| DMA de vértices | — | tentado; tela preta no Flycast; desligado |

---

## 8. Opções de compilação (-D)

Uso: `make OPC="-DOPCAO1 -DOPCAO2"`.

| Grupo | Opções |
|---|---|
| **Teste / câmera** | `DEMO=1` (piloto automático, no make), `CAM_INI=0/1`, `CAM_FIXA`, `S_INI=metro`, `PARADO` (carro parado em S_INI, rivais longe), `CLIMA_INI=CLIMA_SOL/NUBLADO/CHUVA/PESADA/ENSOLARADO/ENTARDECER`, `T_CLIMA_INI=s`, `CAM_ORBITA=r` + `CAM_ALVO=n` + `CAM_MIRA=y`, `CAM_BOX=s`, `CAM_PLACA=s`, `OLHAR_SOL`, `TESTE_LENTO` |
| **Diagnóstico (HUD)** | `PERFIL` (tempos por etapa), `DIAG_CEN`, `DIAG_CARRO`, `DIAG_LOD`, `DIAG_VRAM`, `DIAG_ARQ`, `DIAG_ARV`, `DIAG_SOM`, `DBG_MIP`, `DBG_CTEX` (mostra a textura assada) |
| **Desligar recursos** | `SEM_CARRO_TEX` (rivais no modelo antigo), `SEM_HIBRIDO`, `SEM_REFLEXO_RIVAL`, `SEM_FSAA`, `SEM_RELEVO`, `SEM_DETALHE`, `SEM_REFLEXO`, `SEM_SPRITE`, `SEM_ESPELHO`, `SEM_SOMBRA_CEN`, `SEM_PINT`, `SEM_BRILHO_ASF`, `SEM_VOLANTE`, `ARV_X=0`, `SKYBOX=0`, `TORCIDA_16BIT`, `ASFALTO_256`, `VQ_ANTIGO` |
| **Carro .obj** | `JOGADOR_OBJ=n`, `SEM_CARRO_OBJ` |
| **Novos (23/11 noite)** | `VOLANTE_ANTIGO`, `HUD_ANTIGO`, `SEM_TITULO`, `REPLAY_TESTE=s`, `JOGADOR_HIB`, `MAPA_ESPELHO=1` |
| **Variantes** | `PINTURA_STEWART`, `PINT_RIVAIS`, `DMA_VERTICES` (não funciona no Flycast), `OPB32` |
| **Geradores (variáveis de ambiente)** | `FOTOS=1`, `CEU_FOTO_BRUTA=1`, `CONTRASTE=x` |

---

## 9. Ferramentas de teste e diagnóstico

- **Medir:** `-DPERFIL` mostra no HUD o tempo de cada etapa em ms×10:
  - `E` folga (espera do chip de vídeo), `R` retrovisor, `T` terreno, `P` pista, `C` cenário, `K` carros;
  - `V` volante, `X` HUD + translúcidos, `M` fatias, `F` física, `c` câmera, `b` início da cena, `l` céu.
- **FPS real no Flycast:** `rend.ShowFPS = yes` no `emu.cfg` (canto inferior esquerdo, `F:xx`). O FPS do HUD do jogo é tempo **emulado**.
- **Capturas automáticas:** script `testar.ps1` (na pasta temporária de testes): compila com opções, abre o Flycast, captura a janela nos tempos pedidos e fecha.
- **Teste de cintilação:** sempre **sequências** de capturas a cada 0,15 s com a câmera em movimento. Uma captura parada não mostra o problema.

---

## 10. Desempenho: medições e orçamento

- **Gargalo principal:** carros (K). Antes do híbrido: 6,6–8,0 ms nos trechos pesados. Com o híbrido: 4,4–6,2 ms.
- **Folga por quadro:** de 1 a 3 ms na corrida (câmera de trás, detalhe ALTO). A largada com os 8 carros juntos é o pior caso: cerca de 50 fps antes do híbrido e por volta de 60 com ele (medições no Flycast).
- **Memória de vídeo:** livre cerca de 600 KB com as 7 texturas dos rivais (170 KB cada, com mipmap).
- **Custos de referência:**
  - Pintura metálica em rival: cerca de 0,8 ms.
  - Especular nos rivais (modelo antigo): cerca de 1 ms.
  - Casas até 420 m: até 2,8 ms (por isso `DIST_CASAS` 250).
  - Rival completo a menos de 14 m: cerca de 5 fps.
- **Flycast neste PC** (Xeon 2,2 GHz, fraco por núcleo): o emulador pode não rodar em tempo real em cenas pesadas, mesmo quando o jogo está dentro do orçamento. **O teste no console real é o que vale.**

---

## 11. Lições aprendidas e armadilhas

- `sinf`/`cosf` da libm são caros no SH4: usar tabelas, `fsin`/`fcos` ou valores pré-calculados.
- `-O3` e `-ffast-math` causam erro interno no GCC do DreamSDK.
- Ultrapassar `MAX_FACES_CARRO` ou `MAX_PREDIOS` gera tela preta no boot.
- No `sh-elf`, símbolos de C têm sublinhado no assembly (`_ceu_bin`, `_tex_bin`).
- O skybox a 15.000 m some (1/w pequeno demais); usar 4.000 m.
- Triângulos 2D translúcidos com o giro invertido são descartados.
- **Mipmap com textura multicolorida fina** (torcida) cintila de longe: usar uma versão suavizada e viés de mipmap alto.
- **Objetos sobrepostos no mesmo lugar** (arquibancadas duplicadas, OSM + jogo) piscam: remover as duplicatas.
- **Traçado que se dobra:** objetos colocados a uma distância fixa da pista podem cair em cima de outro trecho. Usar a distância real a **qualquer** trecho (`checar_perna`, filtro de prédios do OSM).
- **Atlas de texturas com alfa (PT):** as coordenadas UV nas bordas de cada arte precisam ficar a 1–2 texels da divisa. O filtro bilinear puxa a arte vizinha e, no recorte, isso aparece como um risco pontilhado ao longo da borda do polígono.
- **Topo do céu:** a pintura das nuvens terminava numa borda reta no alto do cilindro, visível no céu fechado. `ceu_topo_tr()` desenha uma faixa translúcida (cor do zênite opaca em cima, transparente embaixo, de 70% a 100% da altura) que dissolve as nuvens no céu de cima. O formato das nuvens não mudou (24/11).
- **Textura repetida em anel:** o número de repetições em volta do anel precisa ser inteiro. A 2ª camada das nuvens em movimento usava 4,3; no fecho do anel (ângulo 0°) a textura saltava e a nuvem aparecia cortada por uma borda vertical. Agora usa 4. Na mesma correção, o descarte de segmentos do anel passou a testar o meio e as duas pontas (24/11).
- **Tubos com junção:** dois cilindros separados geram anéis diferentes no ponto de encontro (cada um projeta o `ref` no plano do seu próprio eixo) e deixam uma fresta. Isso aparecia no meio do antebraço e no joelho. `ck_tubo3()` desenha A→M→B com o anel do meio compartilhado, que usa a direção média (24/11).
- **Limite do skybox:** o cilindro pintado vai só até cerca de 29° acima do horizonte. O cockpit novo mostra até cerca de 35°, e as nuvens apareciam cortadas no alto. `teto_tr()` desenha uma **camada de nuvens sobre a cabeça**: um plano a 1.500 m, em anéis de 23° a 90°, com a textura `TEXB_TETO_NUVENS` (256×256 periódica, ARGB4444 com mipmap, `teto_nuvens()` no gerador) em coordenadas do mundo e andando com o vento. O alfa sobe de 23° a 33° para emendar com a pintura; cor e cobertura seguem o tempo. `-DSEM_TETO` desliga (24/11).
- **Teto do céu na transição de tempo:** a tampa opaca do zênite usava o azul fixo do céu limpo. Com o céu mudando (limpo por baixo, fechado translúcido por cima), aparecia uma mancha azul no alto. Agora a tampa usa a cor do zênite misturada pelo tempo do momento (`sky_mistura`/`sky_tarde`); `ceu_topo_tr` também fecha o teto inteiro. `-DDBG_MIX=0.5f` fixa a mistura para teste (24/11).
- **Junção em "T" no céu:** a tampa do zênite tinha metade dos segmentos do cilindro. As cordas deixavam uma fresta, que aparecia como uma linha tracejada entre as nuvens e o azul de cima. Peças que se encostam devem usar exatamente os mesmos vértices na borda (corrigido em 24/11).
- **Relevo por hardware no Flycast:** a cor sai branca e o efeito vai para o alfa. Misturar com `ZERO`/`SRCALPHA`.
- **RTT para "assar" texturas no boot** saiu em blocos no Flycast: rasterizar por software.
- `dc.ps1 cdi` pode pegar o `formula-sf3.elf` se ele estiver na pasta: apague-o antes, ou gere com `ferramentas/mkcdi.sh` indicando o .elf.
- Testar efeitos com o clima e o tempo certos. A pista só molha depois de cerca de 10 s de chuva; os efeitos da torcida dependem do carro perto.
- Fotos para texturas: céu fotografado para cima gera nuvens enormes; prédios fotografados de baixo ficam tortos.

---

## 12. Experimentos rejeitados ou revertidos

| Experimento | Motivo |
|---|---|
| Som V10 sintetizado | "muito ruim"; trocado pelo V6 |
| Nuvens translúcidas (billboards) e céu v4 | artificiais |
| Cúmulos de "bolhas" com sol pintado no céu limpo | fora do padrão Speed Devils |
| Textura de pneus nos muros | não gostou; muros de concreto |
| Linha de borracha na trajetória | "ficou horrível" (código desligado com `if(0)`) |
| Fotos do usuário no céu e nos prédios | nuvens grandes demais, fachadas de baixa qualidade; serão refeitas |
| Ajuste automático do perfil de altitude (`ajustar_altura.py`) | criava ondulações falsas nas retas |
| Serra alta com pouca névoa | grande demais; reduzida 40% |
| Animação da torcida por troca de quadros | "picotava" de longe |
| DMA de vértices | tela preta no Flycast |
| Reorganizar os carros em tiras (strips) | substituído pela abordagem do carro híbrido |

---

## 13. Backups e versionamento

**Git** (desde 24/11/2026): o repositório local fica na raiz `Projetos Dreamcast/`, no ramo `main`.
- **Versionado:** fontes, scripts geradores, `DOCUMENTACAO.md`, o modelo `.obj` e os `.bin` necessários para compilar.
- **Fora (`.gitignore`):** builds, backups, emulador e material de estudo de terceiros (vídeo, referências, fotos pessoais, áudio gravado).
- `.gitattributes` mantém o fim de linha LF (os scripts rodam no bash do DreamSDK).
- Commits só quando o usuário pedir.

**Backups em zip:**

Pasta `backups/` (zips do projeto; os mais novos sem os CDIs, que são regeráveis):

- `10-formula_2026-11-21_antes_pintura_metalica.zip`
- `10-formula_2026-11-21_antes_malha_compilada.zip`
- `10-formula_2026-11-21_antes_luz_precalculada.zip`
- `10-formula_2026-11-21_antes_horizonte_pintado.zip`
- `10-formula_2026-11-21_antes_otimizacao_e_predios.zip`
- `10-formula_2026-11-22_antes_serras_reais.zip`
- `10-formula_2026-11-22_antes_gramado_nevoa.zip`
- `10-formula_2026-11-22_antes_fotos.zip`
- `10-formula_2026-11-23_antes_tecnicas_dc.zip`
- `10-formula_2026-11-23_antes_9_melhorias.zip`
- `10-formula_2026-11-23_antes_detalhe_vq.zip`
- `10-formula_2026-11-23_antes_vq2_horizonte.zip`
- `10-formula_2026-11-23_antes_13_itens.zip`

Para restaurar: extrair e copiar `10-formula` por cima de `projetos/10-formula`.

---

## 14. Pendências e próximos passos

**Prioritário**
- **Testar no Dreamcast de verdade** (GDEMU/CD-R). Confirma o desempenho real, decide o DMA de vértices e mostra quanto sobra para efeitos pesados.

**Ideias levantadas e ainda não feitas**
- Animação por paleta também na água do lago e nas luzes.
- Mais câmeras no replay (câmera no box, câmera de frente) e escolha do carro seguido.
- Menu de seleção mais completo (pintura, número de voltas).
- Sombra das nuvens também sobre carros e prédios (volumes modificadores).
- Fotos novas do usuário: céu em 360° no horizonte e fachadas de frente (o processo `fotos.py` já está pronto).
- Refinar a pilotagem (o usuário quer mexer de novo).
- Ajuste manual das quebras de relevo (crista antes do S do Senna, Mergulho), se o usuário pedir.
- Mais pistas no futuro (o gerador OSM já suporta outros circuitos).

**Problemas conhecidos**
- A torcida muito de longe vira uma faixa colorida borrada (troca consciente para não cintilar).
- O boot leva alguns segundos a mais (preparo das texturas dos rivais).
- O boot tem cerca de 12 s de tela preta (texturas dos rivais e minimapa).
- Os controles do replay e do título ainda não foram testados com controle real, só no piloto automático.
