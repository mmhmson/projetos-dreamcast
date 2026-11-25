/*
 * i_music_dc.c — MÚSICA do Doom no Dreamcast: tocador de MIDI + sintetizador.
 *
 * As músicas do Freedoom estão em MIDI padrão (Standard MIDI File). As do Doom
 * original estão em MUS (um "MIDI compacto" da id Software); essas convertemos
 * para MIDI com o mus2mid.c que já vem no doomgeneric. Assim um único tocador
 * serve para os dois formatos.
 *
 * MIDI não é áudio: é uma PARTITURA ("toque a nota 60 no canal 2 com força
 * 100"). Quem transforma isso em som é um SINTETIZADOR. No PC de 1993 era o
 * chip FM OPL da placa de som; aqui escrevemos um sintetizador simples em C:
 *
 *   - 20 vozes (polifonia); cada nota ocupa uma voz;
 *   - TIMBRE pela família do instrumento General MIDI (piano, órgão, baixo,
 *     cordas, metais...): forma de onda + envelope ADSR;
 *   - BATERIA (canal 10 do MIDI): bumbo, caixa, pratos e tons feitos com
 *     seno com queda de frequência + ruído (LFSR);
 *   - volume, pan, expressão e pitch bend por canal.
 *
 * O áudio gerado (22.050 Hz estéreo) vai para o AICA por STREAM, como no
 * 08-audio. A geração roda numa THREAD própria: durante o carregamento de uma
 * fase (segundos lendo o CD) o jogo não chama nada, e sem a thread a música
 * engasgaria. As store queues usadas pelo stream e pela PVR são protegidas
 * pelo próprio KOS com sq_lock().
 *
 * Licença: GPL v2 (parte do port do doomgeneric).
 */

#include <kos.h>
#include <kos/mutex.h>
#include <kos/thread.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "memio.h"
#include "mus2mid.h"

#define TAXA        22050
#define BUF_STREAM  16384           /* bytes por canal (~370 ms de folga) */
#define N_VOZES     20
#define MAX_TRILHAS 32

/* ======================================================================== */
/* SINTETIZADOR                                                              */
/* ======================================================================== */

enum { F_SENO, F_TRIANGULAR, F_QUADRADA, F_PULSO, F_SERRA };
static const float GANHO_FORMA[] = { 1.0f, 1.0f, 0.5f, 0.5f, 0.6f };

/* Timbre por família General MIDI (programa / 8). Taxas por amostra a 22 kHz. */
typedef struct { uint8_t forma; float ataque, decai, sustenta, solta; } timbre_t;
static const timbre_t TIMBRE[16] = {
    /* forma         ataque   decai     sustenta solta */
    { F_TRIANGULAR, 1.0f,    0.99990f, 0.00f, 0.9990f },   /*  0 pianos           */
    { F_SENO,       1.0f,    0.99975f, 0.00f, 0.9990f },   /*  1 percussão cromát.*/
    { F_QUADRADA,   0.010f,  1.00000f, 0.80f, 0.9985f },   /*  2 órgãos           */
    { F_SERRA,      1.0f,    0.99985f, 0.35f, 0.9990f },   /*  3 guitarras        */
    { F_TRIANGULAR, 1.0f,    0.99990f, 0.60f, 0.9980f },   /*  4 baixos           */
    { F_SERRA,      0.002f,  0.99995f, 0.80f, 0.9996f },   /*  5 cordas           */
    { F_SERRA,      0.002f,  0.99995f, 0.80f, 0.9996f },   /*  6 conjuntos        */
    { F_SERRA,      0.005f,  0.99990f, 0.70f, 0.9990f },   /*  7 metais           */
    { F_PULSO,      0.010f,  0.99995f, 0.80f, 0.9990f },   /*  8 palhetas         */
    { F_TRIANGULAR, 0.005f,  0.99995f, 0.80f, 0.9990f },   /*  9 flautas          */
    { F_QUADRADA,   0.050f,  0.99995f, 0.80f, 0.9990f },   /* 10 synth lead       */
    { F_SERRA,      0.0008f, 0.99998f, 0.80f, 0.9997f },   /* 11 synth pad        */
    { F_TRIANGULAR, 0.001f,  0.99995f, 0.70f, 0.9995f },   /* 12 synth efeitos    */
    { F_PULSO,      1.0f,    0.99985f, 0.30f, 0.9990f },   /* 13 étnicos          */
    { F_SENO,       1.0f,    0.99970f, 0.00f, 0.9990f },   /* 14 percussivos      */
    { F_TRIANGULAR, 0.010f,  0.99980f, 0.40f, 0.9990f },   /* 15 efeitos sonoros  */
};

/* OTIMIZAÇÃO: processamos em BLOCOS de 32 amostras. Envelope e ganhos são
 * calculados 1x por bloco (com rampa linear dentro dele, sem "estalos");
 * o laço interno, por amostra, só avança a fase, gera a onda e mistura.
 * Na 1ª versão (tudo por amostra) a música usava ~40% da CPU. */
#define BLOCO 32

typedef struct {
    uint8_t ativa, canal, nota, forma, bateria, solto;
    float   fase, inc;              /* fase 0..1, incremento por amostra */
    float   env, sustenta;
    float   ataque_b, decai_b, solta_b;   /* taxas já elevadas a BLOCO */
    int     subindo;                /* ainda no ataque? */
    float   amp;                    /* velocidade × ganho da forma */
    float   queda_b, ruido_mix;     /* só bateria */
    float   ruido_ant;
    float   ge_ant, gd_ant;         /* ganhos do bloco anterior (para a rampa) */
} voz_t;

typedef struct {
    int   programa;
    float volume, expressao, pan, bend;
} canal_t;

static voz_t    voz[N_VOZES];
static canal_t  canal[16];
static float    freq_nota[128];
static uint32_t lfsr = 0xBEEFu;

static inline float seno_rapido(float f) { float x = f * 2.0f - 1.0f; return -4.0f * x * (1.0f - fabsf(x)); }

static inline float ruido(void) {
    unsigned b = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1;
    lfsr = (lfsr >> 1) | (b << 15);
    return (lfsr & 1) ? 1.0f : -1.0f;
}

static void reset_canais(void) {
    for(int c = 0; c < 16; c++) {
        canal[c].programa = 0;
        canal[c].volume = 100.0f / 127.0f;
        canal[c].expressao = 1.0f;
        canal[c].pan = 0.5f;
        canal[c].bend = 1.0f;
    }
    memset(voz, 0, sizeof(voz));
}

static voz_t *alocar_voz(void) {
    voz_t *melhor = &voz[0];
    for(int i = 0; i < N_VOZES; i++) {
        if(!voz[i].ativa) return &voz[i];
        /* rouba a voz mais fraca, de preferência uma já solta */
        float pv = voz[i].env * (voz[i].solto ? 0.5f : 1.0f);
        float pm = melhor->env * (melhor->solto ? 0.5f : 1.0f);
        if(pv < pm) melhor = &voz[i];
    }
    return melhor;
}

static void config_bateria(voz_t *v, int nota) {
    float f = 0.0f, decai = 0.998f, queda = 1.0f;
    v->bateria = 1; v->forma = F_SENO; v->subindo = 0; v->env = 1.0f; v->sustenta = 0.0f;
    v->ruido_mix = 0.0f;
    switch(nota) {
        case 35: case 36: f = 120; queda = 0.99985f; decai = 0.9993f; v->ruido_mix = 0.05f; break; /* bumbo */
        case 38: case 40: f = 190; queda = 0.9999f;  decai = 0.9990f; v->ruido_mix = 0.70f; break; /* caixa */
        case 42: case 44:          decai = 0.9960f;  v->ruido_mix = 1.0f; break;                   /* chimbal fechado */
        case 46:                   decai = 0.9993f;  v->ruido_mix = 1.0f; break;                   /* chimbal aberto */
        case 49: case 52: case 55: case 57:
                                   decai = 0.99975f; v->ruido_mix = 1.0f; break;                   /* pratos */
        case 51: case 53: case 59: f = 600; decai = 0.9996f; v->ruido_mix = 0.8f; break;          /* ride */
        case 41: case 43: case 45: case 47: case 48: case 50:
            f = 80.0f + (nota - 41) * 14.0f; queda = 0.99993f; decai = 0.9994f; v->ruido_mix = 0.1f; break; /* tons */
        default:                   f = 300; decai = 0.998f; v->ruido_mix = 0.5f; break;
    }
    v->inc = f / TAXA;
    v->decai_b = powf(decai, BLOCO);    /* powf só na nota nova, não por amostra */
    v->queda_b = powf(queda, BLOCO);
}

static void nota_on(int c, int nota, int vel) {
    if(vel == 0) return;
    voz_t *v = alocar_voz();
    memset(v, 0, sizeof(*v));
    v->ativa = 1; v->canal = c; v->nota = nota;
    v->amp = vel / 127.0f;
    if(c == 9) {                                  /* canal 10 do MIDI = bateria */
        config_bateria(v, nota);
        v->amp *= 0.9f;
        return;
    }
    const timbre_t *t = &TIMBRE[(canal[c].programa >> 3) & 15];
    v->forma = t->forma;
    v->ataque_b = t->ataque * BLOCO;
    v->decai_b  = powf(t->decai, BLOCO);
    v->solta_b  = powf(t->solta, BLOCO);
    v->sustenta = t->sustenta;
    v->subindo = 1;
    v->env = t->ataque >= 1.0f ? 1.0f : 0.0f;
    if(v->env >= 1.0f) v->subindo = 0;
    v->inc = freq_nota[nota & 127] / TAXA;
    v->amp *= GANHO_FORMA[v->forma];
}

static void nota_off(int c, int nota) {
    for(int i = 0; i < N_VOZES; i++)
        if(voz[i].ativa && voz[i].canal == c && voz[i].nota == nota && !voz[i].bateria)
            voz[i].solto = 1;
}

static void todas_off(int c) {
    for(int i = 0; i < N_VOZES; i++)
        if(voz[i].ativa && (c < 0 || voz[i].canal == c)) voz[i].solto = 1;
}

static void controle(int c, int num, int val) {
    switch(num) {
        case 7:   canal[c].volume = val / 127.0f; break;
        case 10:  canal[c].pan = val / 127.0f; break;
        case 11:  canal[c].expressao = val / 127.0f; break;
        case 120: case 123: todas_off(c); break;
        case 121: canal[c].expressao = 1.0f; canal[c].bend = 1.0f; break;
    }
}

static float mestre = 1.0f;             /* volume da música (menu do Doom) */
static uint64_t us_sintese;              /* tempo de CPU gasto sintetizando */

static float mix_e[BLOCO], mix_d[BLOCO];

/* Laço interno de UMA forma de onda: só fase + onda + mistura com rampa.
 * Uma macro gera uma versão do laço por forma, para não ter switch dentro. */
#define LACO_ONDA(EXPR)                                         \
    for(int i = 0; i < n; i++) {                                \
        fase += inc; if(fase >= 1.0f) fase -= 1.0f;             \
        float s = (EXPR);                                       \
        mix_e[i] += s * ce; mix_d[i] += s * cd;                 \
        ce += dge; cd += dgd;                                   \
    }

/* Gera n (<= BLOCO) amostras de todas as vozes em mix_e/mix_d. */
static void sintetizar_bloco(int n) {
    memset(mix_e, 0, sizeof(float) * n);
    memset(mix_d, 0, sizeof(float) * n);

    for(int k = 0; k < N_VOZES; k++) {
        voz_t *v = &voz[k];
        if(!v->ativa) continue;
        canal_t *ch = &canal[v->canal];

        /* --- envelope: 1x por bloco --- */
        if(v->bateria) {
            v->env *= v->decai_b;
            v->inc *= v->queda_b;
        }
        else if(v->solto)   v->env *= v->solta_b;
        else if(v->subindo) { v->env += v->ataque_b; if(v->env >= 1.0f) { v->env = 1.0f; v->subindo = 0; } }
        else if(v->env > v->sustenta) { v->env *= v->decai_b; if(v->env < v->sustenta) v->env = v->sustenta; }
        if(v->env < 0.0005f && !v->subindo) { v->ativa = 0; continue; }

        /* --- ganhos esquerda/direita: 1x por bloco, com rampa --- */
        float g  = v->env * v->amp * ch->volume * ch->expressao * 0.25f * mestre;
        float ge = g * (ch->pan < 0.5f ? 1.0f : 2.0f * (1.0f - ch->pan));
        float gd = g * (ch->pan > 0.5f ? 1.0f : 2.0f * ch->pan);
        float ce = v->ge_ant, cd = v->gd_ant;
        float dge = (ge - ce) / n, dgd = (gd - cd) / n;
        v->ge_ant = ge; v->gd_ant = gd;

        float fase = v->fase;
        float inc = v->bateria ? v->inc : v->inc * ch->bend;

        if(v->bateria) {
            float tom = 1.0f - v->ruido_mix, rm = 0.5f * v->ruido_mix, ant = v->ruido_ant;
            for(int i = 0; i < n; i++) {
                fase += inc; if(fase >= 1.0f) fase -= 1.0f;
                float r = ruido(), brilho = r - ant;    /* "agudo" = diferença */
                ant = r;
                float s = seno_rapido(fase) * tom + brilho * rm;
                mix_e[i] += s * ce; mix_d[i] += s * cd;
                ce += dge; cd += dgd;
            }
            v->ruido_ant = ant;
        }
        else switch(v->forma) {
            case F_SENO:       LACO_ONDA(seno_rapido(fase)); break;
            case F_TRIANGULAR: LACO_ONDA(4.0f * fabsf(fase - 0.5f) - 1.0f); break;
            case F_QUADRADA:   LACO_ONDA(fase < 0.5f ? 1.0f : -1.0f); break;
            case F_PULSO:      LACO_ONDA(fase < 0.25f ? 1.0f : -1.0f); break;
            default:           LACO_ONDA(2.0f * fase - 1.0f); break;
        }
        v->fase = fase;
    }
}

/* ======================================================================== */
/* TOCADOR DE MIDI (Standard MIDI File, formatos 0 e 1)                      */
/* ======================================================================== */

typedef struct {
    const uint8_t *p, *fim;
    uint32_t prox_tick;
    uint8_t  status;                /* "running status" do MIDI */
    uint8_t  ativa;
} trilha_t;

typedef struct {
    uint8_t *dados;
    int      tam;
    int      divisao;               /* ticks por semínima */
    int      n_trilhas;
    trilha_t trilha[MAX_TRILHAS];
} musica_t;

static musica_t *tocando;
static int       repetir, pausado;
static uint32_t  tick_atual;
static float     ticks_por_amostra, acum_ticks;

static uint32_t ler_varlen(const uint8_t **p, const uint8_t *fim) {
    uint32_t v = 0;
    while(*p < fim) {
        uint8_t b = *(*p)++;
        v = (v << 7) | (b & 0x7F);
        if(!(b & 0x80)) break;
    }
    return v;
}

static inline uint32_t be32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

static void definir_tempo(uint32_t us_por_seminima) {
    /* ticks por amostra = divisão × semínimas por segundo / amostras por segundo */
    ticks_por_amostra = (float)tocando->divisao * 1000000.0f / ((float)us_por_seminima * TAXA);
}

static int iniciar_trilhas(musica_t *m) {
    const uint8_t *p = m->dados, *fim = m->dados + m->tam;
    if(m->tam < 14 || memcmp(p, "MThd", 4) != 0) return 0;
    uint32_t tam_cab = be32(p + 4);
    int n = be16(p + 10);
    int div = (int16_t)be16(p + 12);
    m->divisao = div > 0 ? div : 96;            /* SMPTE (negativo) é raro: aproxima */
    p += 8 + tam_cab;

    m->n_trilhas = 0;
    for(int i = 0; i < n && p + 8 <= fim && m->n_trilhas < MAX_TRILHAS; i++) {
        uint32_t len = be32(p + 4);
        if(memcmp(p, "MTrk", 4) == 0) {
            trilha_t *t = &m->trilha[m->n_trilhas++];
            t->p = p + 8;
            t->fim = (p + 8 + len <= fim) ? p + 8 + len : fim;
            t->status = 0;
            t->ativa = 1;
            t->prox_tick = ler_varlen(&t->p, t->fim);
        }
        p += 8 + len;
    }
    return m->n_trilhas > 0;
}

static void processar_evento(trilha_t *t) {
    if(t->p >= t->fim) { t->ativa = 0; return; }
    uint8_t b = *t->p;
    if(b & 0x80) { t->status = b; t->p++; }
    else if(t->status == 0) { t->ativa = 0; return; }   /* dado sem status: corrompido */

    uint8_t st = t->status;
    if(st == 0xFF) {                                       /* meta-evento */
        uint8_t tipo = *t->p++;
        uint32_t len = ler_varlen(&t->p, t->fim);
        if(tipo == 0x51 && len == 3)
            definir_tempo((t->p[0] << 16) | (t->p[1] << 8) | t->p[2]);
        else if(tipo == 0x2F) { t->ativa = 0; return; }   /* fim da trilha */
        t->p += len;
        t->status = 0;
    }
    else if(st == 0xF0 || st == 0xF7) {                    /* sysex: ignora */
        uint32_t len = ler_varlen(&t->p, t->fim);
        t->p += len;
        t->status = 0;
    }
    else {
        int c = st & 0x0F;
        switch(st & 0xF0) {
            case 0x80: nota_off(c, t->p[0]); t->p += 2; break;
            case 0x90: if(t->p[1]) nota_on(c, t->p[0], t->p[1]); else nota_off(c, t->p[0]); t->p += 2; break;
            case 0xA0: t->p += 2; break;
            case 0xB0: controle(c, t->p[0], t->p[1]); t->p += 2; break;
            case 0xC0: canal[c].programa = t->p[0]; t->p += 1; break;
            case 0xD0: t->p += 1; break;
            case 0xE0: {                                   /* pitch bend: ±2 semitons */
                int val = (t->p[0] | (t->p[1] << 7)) - 8192;
                canal[c].bend = powf(2.0f, (val / 8192.0f) * 2.0f / 12.0f);
                t->p += 2; break;
            }
        }
    }
    if(t->p >= t->fim) t->ativa = 0;
    else t->prox_tick += ler_varlen(&t->p, t->fim);
}

static void reiniciar_musica(void) {
    reset_canais();
    iniciar_trilhas(tocando);
    tick_atual = 0;
    acum_ticks = 0.0f;
    definir_tempo(500000);                                 /* 120 BPM padrão */
}

static void avancar_um_tick(void) {
    tick_atual++;
    int alguma = 0;
    for(int i = 0; i < tocando->n_trilhas; i++) {
        trilha_t *t = &tocando->trilha[i];
        while(t->ativa && t->prox_tick <= tick_atual)
            processar_evento(t);
        alguma |= t->ativa;
    }
    if(!alguma) {                                          /* acabou a música */
        if(repetir) reiniciar_musica();
        else { todas_off(-1); tocando = NULL; }
    }
}

/* ======================================================================== */
/* STREAM + THREAD                                                           */
/* ======================================================================== */

static int16_t           buf_cb[BUF_STREAM] __attribute__((aligned(32)));
static snd_stream_hnd_t  stream = SND_STREAM_INVALID;
static mutex_t           trava = MUTEX_INITIALIZER;
static kthread_t        *thread_musica;
static volatile int      rodando;

static void *callback_stream(snd_stream_hnd_t hnd, int bytes, int *entregues) {
    (void)hnd;
    if(bytes > (int)sizeof(buf_cb)) bytes = sizeof(buf_cb);
    int n = bytes / 4;                                     /* amostras estéreo 16 bits */
    uint64_t t0 = timer_us_gettime64();

    for(int ini = 0; ini < n; ini += BLOCO) {
        int m = (n - ini < BLOCO) ? n - ini : BLOCO;
        int16_t *out = buf_cb + 2 * ini;

        if(!tocando || pausado) {
            memset(out, 0, m * 4);
            continue;
        }
        /* eventos MIDI: avança os ticks do bloco inteiro (precisão de ~1,5 ms) */
        acum_ticks += ticks_por_amostra * m;
        while(acum_ticks >= 1.0f && tocando) { acum_ticks -= 1.0f; avancar_um_tick(); }
        if(!tocando) { memset(out, 0, m * 4); continue; }

        sintetizar_bloco(m);
        for(int i = 0; i < m; i++) {                       /* corta picos e converte */
            float e = mix_e[i], d = mix_d[i];
            e = e > 1.0f ? 1.0f : (e < -1.0f ? -1.0f : e);
            d = d > 1.0f ? 1.0f : (d < -1.0f ? -1.0f : d);
            out[2 * i]     = (int16_t)(e * 32000.0f);
            out[2 * i + 1] = (int16_t)(d * 32000.0f);
        }
    }

    us_sintese += timer_us_gettime64() - t0;
    *entregues = n * 4;
    return buf_cb;
}

static void *laco_musica(void *arg) {
    (void)arg;
    while(rodando) {
        mutex_lock(&trava);
        snd_stream_poll(stream);                           /* chama o callback se precisar */
        mutex_unlock(&trava);
        thd_sleep(8);
    }
    return NULL;
}

/* ======================================================================== */
/* MÓDULO DE MÚSICA DO DOOM                                                  */
/* ======================================================================== */

static boolean Mus_Init(void) {
    for(int n = 0; n < 128; n++)
        freq_nota[n] = 440.0f * powf(2.0f, (n - 69) / 12.0f);
    reset_canais();

    stream = snd_stream_alloc(callback_stream, BUF_STREAM);
    if(stream == SND_STREAM_INVALID) {
        printf("I_MusicDC: sem stream disponível\n");
        return false;
    }
    snd_stream_start(stream, TAXA, 1);
    rodando = 1;
    thread_musica = thd_create(0, laco_musica, NULL);
    printf("I_MusicDC: sintetizador MIDI pronto (%d Hz, %d vozes, thread própria)\n", TAXA, N_VOZES);
    return true;
}

static void Mus_Shutdown(void) {
    rodando = 0;
    if(thread_musica) thd_join(thread_musica, NULL);
    if(stream != SND_STREAM_INVALID) { snd_stream_stop(stream); snd_stream_destroy(stream); }
}

static void Mus_SetVolume(int vol) {                       /* 0..127 */
    mutex_lock(&trava);
    mestre = vol / 127.0f;
    mutex_unlock(&trava);
}

static void Mus_Pause(void)  { mutex_lock(&trava); pausado = 1; mutex_unlock(&trava); }
static void Mus_Resume(void) { mutex_lock(&trava); pausado = 0; mutex_unlock(&trava); }

static void *Mus_Register(void *dados, int tam) {
    musica_t *m = calloc(1, sizeof(musica_t));
    const uint8_t *d = dados;

    if(tam >= 4 && memcmp(d, "MUS\x1a", 4) == 0) {
        /* MUS (Doom original) -> MIDI, com o conversor do próprio doomgeneric */
        MEMFILE *in = mem_fopen_read(dados, tam);
        MEMFILE *out = mem_fopen_write();
        void *mid; size_t tam_mid;
        if(!mus2mid(in, out)) {                            /* retorna true se ERRO */
            mem_get_buf(out, &mid, &tam_mid);
            m->dados = malloc(tam_mid);
            memcpy(m->dados, mid, tam_mid);
            m->tam = (int)tam_mid;
        }
        mem_fclose(in);
        mem_fclose(out);
    }
    else {
        /* já é MIDI: copiamos, porque o Doom pode liberar o lump depois */
        m->dados = malloc(tam);
        memcpy(m->dados, dados, tam);
        m->tam = tam;
    }

    if(!m->dados || !iniciar_trilhas(m)) {
        printf("I_MusicDC: formato de música não reconhecido\n");
        free(m->dados); free(m);
        return NULL;
    }
    printf("I_MusicDC: música registrada (%d trilhas, %d ticks/semínima, %d bytes)\n",
           m->n_trilhas, m->divisao, m->tam);
    return m;
}

static void Mus_Unregister(void *h) {
    musica_t *m = h;
    if(!m) return;
    mutex_lock(&trava);
    if(tocando == m) tocando = NULL;
    mutex_unlock(&trava);
    free(m->dados);
    free(m);
}

static void Mus_Play(void *h, boolean loop) {
    mutex_lock(&trava);
    tocando = h;
    repetir = loop;
    if(tocando) reiniciar_musica();
    mutex_unlock(&trava);
}

static void Mus_Stop(void) {
    mutex_lock(&trava);
    tocando = NULL;
    reset_canais();
    mutex_unlock(&trava);
}

static boolean Mus_IsPlaying(void) { return tocando != NULL; }

static snddevice_t dispositivos_mus[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_GENMIDI, SNDDEVICE_AWE32,
};

music_module_t DG_music_module = {
    dispositivos_mus,
    sizeof(dispositivos_mus) / sizeof(dispositivos_mus[0]),
    Mus_Init,
    Mus_Shutdown,
    Mus_SetVolume,
    Mus_Pause,
    Mus_Resume,
    Mus_Register,
    Mus_Unregister,
    Mus_Play,
    Mus_Stop,
    Mus_IsPlaying,
    NULL,                       /* Poll: não precisamos, temos a thread */
};

/* Para o painel de FPS: microssegundos gastos sintetizando desde a última
 * chamada (e zera o contador). */
uint32_t I_MusicDC_TempoUs(void) {
    uint32_t t = (uint32_t)us_sintese;
    us_sintese = 0;
    return t;
}
