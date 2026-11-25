/*
 * doomgeneric_dc.c — CAMADA DE PLATAFORMA do Doom para Sega Dreamcast (KallistiOS).
 *
 * Este é o único arquivo "do Dreamcast" do port. Todo o resto (~95 arquivos
 * do doomgeneric) é o jogo original, compilado sem alterações para o SH-4.
 * É assim que se porta qualquer jogo: isolar o que depende da plataforma
 * (vídeo, entrada, tempo, arquivos, som) e reescrever só essa parte.
 *
 *   Função pedida pelo jogo      Implementação no Dreamcast
 *   ------------------------     ------------------------------------------------
 *   DG_Init()                    inicia a PVR, cria a textura da tela
 *   DG_DrawFrame()               converte 320x200 ARGB8888 -> RGB565, sobe para a
 *                                VRAM e desenha um retângulo em tela cheia 4:3
 *   DG_GetKey()                  traduz o controle do Dreamcast em "teclas" do Doom
 *   DG_GetTicksMs/DG_SleepMs     timer do KOS / thd_sleep
 *   main()                       chama doomgeneric_Create() e o laço de Tick
 *
 * Dados do jogo: Freedoom (freedoom1.wad, licença BSD) lido de /cd/ — o WAD
 * tem 27,5 MB e NÃO cabe nos 16 MB de RAM, então precisa vir do disco.
 *
 * Licença: o doomgeneric é GPL v2; este arquivo, por ser parte do port,
 * também é distribuído sob a GPL v2.
 */

#include <kos.h>
#include <stdio.h>
#include <string.h>

#include "doomgeneric.h"
#include "doomkeys.h"

/* vmu_saves_dc.c */
void VMU_Iniciar(void);
void VMU_Sincronizar(void);
void VMU_ImprimirResumo(void);

#ifdef TESTE_VMU
void G_SaveGame(int slot, char *description);   /* g_game.c */
#endif

/* ------------------------------------------------------------------------ */
/* Vídeo                                                                     */
/* ------------------------------------------------------------------------ */
/* A tela do Doom é 320x200 (DOOMGENERIC_RESX/RESY definidos no Makefile).
 * Texturas da PVR precisam de largura potência de 2: usamos 512x256 e só a
 * parte 320x200 aparece (UVs 320/512 e 200/256). Formato "não-twiddled"
 * (linear), porque o reescrevemos inteiro a cada quadro. */

#define TEX_W 512
#define TEX_H 256

static uint16_t       tela565[TEX_W * DOOMGENERIC_RESY] __attribute__((aligned(32)));
static pvr_ptr_t      tex_vram;
static pvr_poly_hdr_t hdr_tela;

/* Painel de FPS: texto da BIOS numa textura ARGB4444 translúcida (como no 06).
 * Ligue/desligue no Makefile com -DMOSTRAR_FPS. */
#define PAINEL_W 256
#define PAINEL_H 32
static uint16_t       painel[PAINEL_W * PAINEL_H] __attribute__((aligned(32)));
static pvr_ptr_t      painel_vram;
static pvr_poly_hdr_t hdr_painel;
extern uint32_t I_MusicDC_TempoUs(void);

static void atualizar_painel(void) {
    static uint64_t inicio;
    static int quadros;
    uint64_t agora = timer_us_gettime64();

    if(inicio == 0) inicio = agora;
    quadros++;
    if(agora - inicio < 500000)            /* atualiza 2x por segundo */
        return;

    float seg = (agora - inicio) / 1e6f;
    float fps = quadros / seg;
    float musica = I_MusicDC_TempoUs() / (seg * 1e4f);   /* % da CPU */
    char txt[24];
    snprintf(txt, sizeof(txt), "FPS %4.1f mus %4.1f%%", (double)fps, (double)musica);

    memset(painel, 0, sizeof(painel));
    bfont_set_foreground_color(0xFFFF);
    bfont_set_background_color(0xA000);
    bfont_draw_str(painel + 4 * PAINEL_W, PAINEL_W, true, txt);
    pvr_txr_load(painel, painel_vram, sizeof(painel));

    inicio = agora;
    quadros = 0;
}

void DG_Init(void) {
    pvr_init_defaults();
    pvr_set_bg_color(0, 0, 0);

    tex_vram = pvr_mem_malloc(TEX_W * TEX_H * 2);
    memset(tela565, 0, sizeof(tela565));

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     TEX_W, TEX_H, tex_vram, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_tela, &cxt);

    painel_vram = pvr_mem_malloc(sizeof(painel));
    memset(painel, 0, sizeof(painel));
    pvr_txr_load(painel, painel_vram, sizeof(painel));
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                     PAINEL_W, PAINEL_H, painel_vram, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr_painel, &cxt);
}

void DG_DrawFrame(void) {
    /* 1) ARGB8888 (o que o doomgeneric produz) -> RGB565 (o que a PVR lê) */
    const uint32_t *src = (const uint32_t *)DG_ScreenBuffer;
    for(int y = 0; y < DOOMGENERIC_RESY; y++) {
        uint16_t *dst = tela565 + y * TEX_W;
        for(int x = 0; x < DOOMGENERIC_RESX; x++) {
            uint32_t c = *src++;
            *dst++ = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
        }
    }

    /* 2) Copia para a VRAM (pvr_txr_load usa as store queues: rápido) */
    pvr_txr_load(tela565, tex_vram, sizeof(tela565));

    /* 3) Desenha a tela: 320x200 esticado para 640x480 = proporção 4:3,
     *    igual a um monitor CRT da época (o Doom foi feito para isso). */
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_tela, sizeof(hdr_tela));

    const float u1 = (float)DOOMGENERIC_RESX / TEX_W, v1 = (float)DOOMGENERIC_RESY / TEX_H;
    pvr_vertex_t *v;
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX;     v->x = 0;   v->y = 480; v->z = 1; v->u = 0;  v->v = v1; v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX;     v->x = 0;   v->y = 0;   v->z = 1; v->u = 0;  v->v = 0;  v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX;     v->x = 640; v->y = 480; v->z = 1; v->u = u1; v->v = v1; v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX_EOL; v->x = 640; v->y = 0;   v->z = 1; v->u = u1; v->v = 0;  v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);

    pvr_list_finish();

#ifdef MOSTRAR_FPS
    /* Painel no canto superior direito, por cima do jogo (lista translúcida) */
    pvr_list_begin(PVR_LIST_TR_POLY);
    pvr_prim(&hdr_painel, sizeof(hdr_painel));
    const float px = 640 - PAINEL_W - 4, py = 4;
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX;     v->x = px;            v->y = py + PAINEL_H; v->z = 2; v->u = 0; v->v = 1; v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX;     v->x = px;            v->y = py;            v->z = 2; v->u = 0; v->v = 0; v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX;     v->x = px + PAINEL_W; v->y = py + PAINEL_H; v->z = 2; v->u = 1; v->v = 1; v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);
    v = pvr_dr_target(); v->flags = PVR_CMD_VERTEX_EOL; v->x = px + PAINEL_W; v->y = py;            v->z = 2; v->u = 1; v->v = 0; v->argb = 0xFFFFFFFF; v->oargb = 0; pvr_dr_commit(v);
    pvr_list_finish();
#endif

    pvr_scene_finish();

#ifdef MOSTRAR_FPS
    atualizar_painel();
#endif

    /* ~1x por segundo: o jogador salvou? Então grava na VMU. */
    static int quadros_vmu, primeiro_quadro = 1;
    if(primeiro_quadro) { primeiro_quadro = 0; VMU_ImprimirResumo(); }
    if(++quadros_vmu >= 35) {
        quadros_vmu = 0;
        VMU_Sincronizar();
    }

#if defined(TESTE_VMU) && TESTE_VMU == 1
    /* Teste automático: depois de ~8 s de jogo, salva no slot 0 sozinho */
    static int quadros_teste;
    if(++quadros_teste == 280) {
        printf("TESTE_VMU: salvando no slot 0...\n");
        G_SaveGame(0, "TESTE AUTOMATICO VMU");
    }
#endif
}

/* ------------------------------------------------------------------------ */
/* Entrada: controle do Dreamcast -> teclas do Doom                         */
/* ------------------------------------------------------------------------ */
/* O Doom espera EVENTOS de teclado (tecla X apertada / solta). O controle
 * nos dá ESTADO (o que está apertado agora). Então comparamos o estado com
 * o do quadro anterior e geramos um evento para cada mudança.
 *
 *   D-pad / analógico  setas (andar e virar)     A      atirar + Enter (menus)
 *   B                  usar (portas) + 'y'       X      próxima arma (1..7)
 *   Y                  mapa (Tab)                L / R  andar de lado
 *   START              menu (Esc)                analógico no talo: correr  */

typedef struct { int ativo; } estado_tecla_t;

enum {
    T_CIMA, T_BAIXO, T_ESQ, T_DIR, T_FIRE, T_ENTER, T_USE, T_Y, T_MAPA,
    T_STRAFE_L, T_STRAFE_R, T_ESC, T_CORRER, T_ARMA, N_TECLAS
};
static const unsigned char CODIGO[N_TECLAS] = {
    KEY_UPARROW, KEY_DOWNARROW, KEY_LEFTARROW, KEY_RIGHTARROW, KEY_FIRE, KEY_ENTER,
    KEY_USE, 'y', KEY_TAB, KEY_STRAFE_L, KEY_STRAFE_R, KEY_ESCAPE, KEY_RSHIFT, '1'
};
static int estado_ant[N_TECLAS];

/* fila circular de eventos */
#define FILA 64
static struct { int pressed; unsigned char key; } fila[FILA];
static int fila_ini, fila_fim;

static void empilhar(int pressed, unsigned char key) {
    int prox = (fila_fim + 1) % FILA;
    if(prox == fila_ini) return;          /* cheia: descarta */
    fila[fila_fim].pressed = pressed;
    fila[fila_fim].key = key;
    fila_fim = prox;
}

static int arma_atual = 0;               /* para o botão X: 1,2,...,7,1... */

static void ler_controle(void) {
    int agora[N_TECLAS] = {0};
    maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if(cont) {
        cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
        if(st) {
            uint32_t b = st->buttons;
            agora[T_CIMA]     = (b & CONT_DPAD_UP)    || st->joyy < -40;
            agora[T_BAIXO]    = (b & CONT_DPAD_DOWN)  || st->joyy >  40;
            agora[T_ESQ]      = (b & CONT_DPAD_LEFT)  || st->joyx < -40;
            agora[T_DIR]      = (b & CONT_DPAD_RIGHT) || st->joyx >  40;
            agora[T_CORRER]   = st->joyy < -110 || st->joyy > 110;
            agora[T_FIRE]     = agora[T_ENTER] = (b & CONT_A) != 0;
            agora[T_USE]      = agora[T_Y]     = (b & CONT_B) != 0;
            agora[T_ARMA]     = (b & CONT_X) != 0;
            agora[T_MAPA]     = (b & CONT_Y) != 0;
            agora[T_ESC]      = (b & CONT_START) != 0;
            agora[T_STRAFE_L] = st->ltrig > 64;
            agora[T_STRAFE_R] = st->rtrig > 64;
        }
    }

    for(int i = 0; i < N_TECLAS; i++) {
        if(agora[i] == estado_ant[i]) continue;
        unsigned char k = CODIGO[i];
        if(i == T_ARMA) {                      /* X: cicla armas 1..7 */
            if(agora[i]) arma_atual = arma_atual % 7 + 1;
            k = (unsigned char)('0' + (arma_atual ? arma_atual : 1));
        }
        empilhar(agora[i], k);
        estado_ant[i] = agora[i];
    }
}

int DG_GetKey(int *pressed, unsigned char *key) {
    if(fila_ini == fila_fim)
        ler_controle();                        /* fila vazia: lê o controle de novo */
    if(fila_ini == fila_fim)
        return 0;
    *pressed = fila[fila_ini].pressed;
    *key = fila[fila_ini].key;
    fila_ini = (fila_ini + 1) % FILA;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Tempo                                                                     */
/* ------------------------------------------------------------------------ */
/* O Doom roda a lógica a 35 "tics" por segundo e usa estes dois para medir. */

uint32_t DG_GetTicksMs(void) {
    return (uint32_t)timer_ms_gettime64();
}

void DG_SleepMs(uint32_t ms) {
    thd_sleep(ms);
}

void DG_SetWindowTitle(const char *title) {
    (void)title;                               /* não há janela no Dreamcast */
}

/* ------------------------------------------------------------------------ */
/* Programa principal                                                        */
/* ------------------------------------------------------------------------ */

static void mostrar_erro(const char *msg) {
    /* tela de erro simples com a fonte da BIOS (sem PVR) */
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_clear(0, 0, 80);
    bfont_draw_str(vram_s + 100 * 640 + 40, 640, 1, msg);
    bfont_draw_str(vram_s + 140 * 640 + 40, 640, 1, "Rode a partir do .cdi (WAD em /cd).");
    for(;;) thd_sleep(1000);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* printf() sai pela porta SERIAL do Dreamcast. No Flycast, ative
     * "Debug.SerialConsoleEnabled = yes" no emu.cfg para ver numa janela;
     * no console real, use um cabo serial (Coders Cable) ou BBA + dcload. */
    /* stdout sem buffer: o Doom imprime progresso ("....") sem quebrar a
     * linha, e com buffer nada apareceria até o fim do carregamento. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Doom para Dreamcast - inicializando...\n");

    /* Onde está o WAD? Num disco (.cdi), em /cd/. */
    static const char *CANDIDATOS[] = { "/cd/freedoom1.wad", "/cd/FREEDOOM1.WAD", "/pc/freedoom1.wad" };
    const char *wad = NULL;
    for(unsigned i = 0; i < sizeof(CANDIDATOS) / sizeof(CANDIDATOS[0]) && !wad; i++) {
        FILE *f = fopen(CANDIDATOS[i], "rb");
        if(f) { fclose(f); wad = CANDIDATOS[i]; }
    }
    if(!wad)
        mostrar_erro("freedoom1.wad nao encontrado!");

    printf("Usando IWAD: %s\n", wad);

    /* Saves: traz os saves da VMU para /ram/ antes de o Doom procurá-los */
    VMU_Iniciar();

    /* Argumentos como se fosse a linha de comando do PC:
     *   -iwad  : qual arquivo de dados usar
     *   -mb 6  : tamanho da "zona" de memória do Doom (6 MB dos 16 MB do DC) */
#if defined(TESTE_VMU) && TESTE_VMU == 1
    char *args[] = { "doom", "-iwad", (char *)wad, "-mb", "6", "-warp", "1", "1", NULL };
#elif defined(TESTE_VMU) && TESTE_VMU == 2
    char *args[] = { "doom", "-iwad", (char *)wad, "-mb", "6", "-loadgame", "0", NULL };
#else
    char *args[] = { "doom", "-iwad", (char *)wad, "-mb", "6", NULL };
#endif
    int n_args = 0;
    while(args[n_args]) n_args++;
    doomgeneric_Create(n_args, args);

    for(;;)
        doomgeneric_Tick();

    return 0;
}
