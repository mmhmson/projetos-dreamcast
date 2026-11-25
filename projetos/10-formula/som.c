/*
 * som.c — áudio do protótipo de fórmula. TUDO sintetizado em tempo real
 * (nenhum .wav, nenhum som de terceiros), num stream estéreo para o AICA,
 * processado em blocos de 32 amostras (lição do sintetizador do Doom).
 *
 *  - MOTOR V6 1.6 TURBO (F1 atual), por modelo físico do escape:
 *    6 cilindros em 4 tempos = 3 explosões por volta (nota = rpm/20 Hz,
 *    ~480-590 Hz entre 9.600 e 11.800 rpm). O turbo "abafa" o escape: o som
 *    é grave e encorpado, sem o grito agudo dos V10. Cada explosão é um
 *    pulso suave que excita 3 ressonadores fixos (150 / 420 / 1100 Hz), mais
 *    a "meia ordem" (~270 Hz, o ronco grave), dois passa-baixas em série e
 *    saturação suave. Cilindros com forças um pouco diferentes = ronco vivo.
 *    TURBO: assobio discreto que sobe com a pressão; chiado suave da
 *    wastegate ao tirar o pé. Tudo muda de forma CONTÍNUA (acelerador
 *    suavizado) — nada de cortes bruscos, que soavam "picotados".
 *  - RIVAL MAIS PRÓXIMO: um segundo motor, com volume pela distância,
 *    estéreo pela posição e efeito Doppler (agudo chegando, grave indo).
 *  - PNEU: chiado (ruído passa-banda) + um "uivo" tonal que oscila — a
 *    derrapagem com fumacinha. Grama: ruído grave de terra/pedrisco.
 *  - TORCIDA: ruído na faixa da voz, ondulando devagar (perto das arquibancadas).
 *  - EFEITOS CURTOS (bip das luzes, largada, batida): também sintetizados.
 *  - CHUVA: chiado de gotas (ruído agudo) + ronco grave de fundo; pneu no
 *    molhado = "shhh" proporcional à velocidade. TROVÃO: ruído bem grave com
 *    estalo inicial e cauda longa de ~3 s.
 */

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <math.h>
#include <string.h>

#include "som.h"

#define TAXA        22050
#define BLOCO       32
#define BUF_STREAM  8192

static snd_stream_hnd_t stream;
static int16_t buf[BUF_STREAM] __attribute__((aligned(32)));

/* parâmetros vindos do jogo (escritos na thread principal, lidos no callback) */
static volatile float alvo_rpm = 0.3f, alvo_acel, alvo_derrap, alvo_grama, alvo_torcida;
static volatile float alvo_riv_rpm, alvo_riv_vol, alvo_riv_pan = 0.5f, alvo_riv_dop = 1.0f;
static volatile int   pedido_bip, pedido_batida;
static volatile float pedido_batida_vol;
static volatile float alvo_chuva, alvo_agua;
static volatile int   pedido_trovao;

static uint32_t lfsr = 0xACE1u;
static inline float ruido(void) {
    unsigned b = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1;
    lfsr = (lfsr >> 1) | (b << 15);
    return (lfsr & 1) ? 1.0f : -1.0f;
}
/* seno aproximado (parábolas), entrada 0..1 */
static inline float seno_rapido(float f) { float x = f * 2.0f - 1.0f; return -4.0f * x * (1.0f - fabsf(x)); }

typedef struct { float rpm, acel, pf, ph, amp, filtro, f2, rn, pop, inc, env, lo[3], bp[3], tb, tf; int cil; } motor_t;
static motor_t eu, riv;
static float riv_vol, riv_pan = 0.5f, riv_dop = 1.0f, corte, wg, wg_f;
static float derrap, grama, pn_a, pn_b, uivo_fase, uivo_lfo, gr_filtro;
static float tc_vol, tc_a, tc_b, tc_fase;
/* efeitos curtos */
static float bip_env, bip_fase, bip_inc, bat_env, bat_filtro;
static float ch_vol, ag_vol, ch_a, ch_b, ch_grave, tv_env, tv_f1, tv_f2, tv_estalo;

/* rotação do jogo (0..1) -> incremento de fase da nota de explosão.
 * 0,55 (início de cada marcha) ~ 9.600 rpm, 1,0 = 11.800 rpm (V6 turbo). */
static float motor_inc(float rpm) {
    if(rpm < 0.05f) rpm = 0.05f;
    float rpm_real = 3500.0f + 8300.0f * sqrtf(rpm);
    return rpm_real / 20.0f / TAXA;        /* 3 explosões por volta */
}

/* ressonâncias do escape (filtros de estado variável): f = 2 sen(pi fc / taxa) */
static const float RES_F[3] = { 0.04274f, 0.11960f, 0.31236f };    /* 150, 420, 1100 Hz */
static const float RES_Q[3] = { 0.5f, 0.4f, 0.5f };                 /* 1/Q */
static const float AMP_CIL[6] = { 1.00f, 0.86f, 0.97f, 0.84f, 1.00f, 0.88f };

#ifdef SOM_GRAVADO
/* MODO ESTUDO (make ESTUDO=1): em vez do sintetizador, toca em loop uma
 * gravação real fornecida pelo usuário (estudo/motor_gravado.h), mudando a
 * velocidade da reprodução conforme a rotação. Só para comparação local —
 * a gravação não é distribuída nem entra no formula.cdi. */
#include "motor_gravado.h"     /* MG[MG_N] a 22050 Hz; MG_HZ = nota de explosão da gravação */
static inline float gravado_amostra(motor_t *m, float inc, float rpm, float ac) {
    m->ph += inc * ((float)TAXA / MG_HZ);          /* posição na gravação, em amostras */
    while(m->ph >= MG_N) m->ph -= MG_N;
    int i = (int)m->ph, j = i + 1 >= MG_N ? 0 : i + 1;
    float fr = m->ph - i;
    float x = (MG[i] + (MG[j] - MG[i]) * fr) * (1.0f / 32768.0f);
    m->rn += 0.55f * (ruido() - m->rn);
    if(ac < 0.15f && rpm > 0.6f && (lfsr & 4095) == 3) m->pop = 1.0f;
    if(m->pop > 0.01f) { x += m->rn * m->pop * 0.9f; m->pop *= 0.9975f; }
    if(ac < 0.15f) x *= 0.6f;
    float k = 0.35f + 0.45f * ac + 0.2f * rpm; if(k > 1.0f) k = 1.0f;   /* pé fora = mais abafado */
    m->filtro += k * (x - m->filtro);
    return m->filtro;
}
#endif

/* Uma amostra do V6 turbo. inc = fase por amostra; ac = acelerador 0..1 (suave). */
static inline float motor_amostra(motor_t *m, float inc, float rpm, float ac) {
#ifdef SOM_GRAVADO
    return gravado_amostra(m, inc, rpm, ac);
#endif
    m->pf += inc;
    if(m->pf >= 1.0f) {                      /* nova explosão: pulso no escape */
        m->pf -= 1.0f;
        m->env = AMP_CIL[m->cil];
        if(++m->cil >= 6) m->cil = 0;
    }
    m->ph += inc * 0.5f; if(m->ph >= 1.0f) m->ph -= 1.0f;
    m->rn += 0.3f * (ruido() - m->rn);
    float e = m->env * (1.0f + m->rn * 0.15f);
    m->env *= 0.88f;
    float g[3] = { 1.0f, 0.9f, 0.30f + 0.25f * ac };
    float sai = 0;
    for(int k = 0; k < 3; k++) {
        float hi = e - m->lo[k] - RES_Q[k] * m->bp[k];
        m->bp[k] += RES_F[k] * hi;
        m->lo[k] += RES_F[k] * m->bp[k];
        sai += m->bp[k] * g[k];
    }
    sai = sai * 0.5f + seno_rapido(m->pf) * 0.16f + seno_rapido(m->ph) * 0.20f;   /* + meia ordem (ronco) */
    sai *= 0.55f + 0.45f * ac;                                 /* pé fora: mais vazio, sem corte */
    float x = sai * 1.2f;
    x = x / (1.0f + fabsf(x));                                 /* saturação suave */
    float k = 0.16f + 0.22f * ac + 0.10f * rpm;                /* dois passa-baixas: som escuro */
    m->filtro += k * (x - m->filtro);
    m->f2 += k * (m->filtro - m->f2);
    /* turbo: enche devagar, esvazia mais rápido; assobio discreto */
    float alvo = ac * (0.3f + 0.7f * rpm);
    m->tb += (alvo - m->tb) * (alvo > m->tb ? 0.00009f : 0.00025f);
    m->tf += (1400.0f + 2200.0f * m->tb) * (1.0f / TAXA); if(m->tf >= 1.0f) m->tf -= 1.0f;
    return m->f2 * 2.6f + seno_rapido(m->tf) * 0.025f * m->tb;
}

/* LIMITADOR SUAVE: até 0,7 passa direto; acima disso comprime aos poucos e
 * nunca passa de 1,0 — a soma de motor + rival + chuva + trovão não "estala". */
static inline float limitar_suave(float x) {
    float a = fabsf(x);
    if(a <= 0.7f) return x;
    float t = (a - 0.7f) * (1.0f / 0.3f);
    float y = 0.7f + 0.3f * t / (1.0f + t);
    return x < 0 ? -y : y;
}

#ifdef DIAG_SOM
/* -DDIAG_SOM: diagnóstico do "picotado" (amostras saturadas, maior intervalo
 * entre pedidos do AICA, maior pedido e custo do sintetizador). */
static int diag_clip, diag_gap, diag_ped, diag_us;
static uint64_t diag_ult;
void som_diag(int *clip, int *gap_ms, int *pedido, int *custo_us) {
    *clip = diag_clip; *gap_ms = diag_gap; *pedido = diag_ped; *custo_us = diag_us;
    diag_clip = diag_gap = diag_ped = diag_us = 0;
}
#endif

/* Motor do RIVAL, versão leve (custa ~1/3): nota de explosão + meia ordem,
 * com forma de pulso e dois passa-baixas. De longe e passando rápido, o
 * ouvido não distingue os detalhes dos ressonadores. */
static inline float motor_leve(motor_t *m, float inc, float rpm) {
    m->pf += inc; if(m->pf >= 1.0f) m->pf -= 1.0f;
    m->ph += inc * 0.5f; if(m->ph >= 1.0f) m->ph -= 1.0f;
    float pulso = m->pf < 0.25f ? 1.0f - m->pf * 4.0f : 0.0f;
    float x = seno_rapido(m->pf) * 0.30f + seno_rapido(m->ph) * 0.28f + pulso * 0.35f;
    float k = 0.22f + 0.10f * rpm;
    m->filtro += k * (x - m->filtro);
    m->f2 += k * (m->filtro - m->f2);
    return m->f2 * 1.6f;
}

static void sintetizar(int16_t *saida, int n) {
    for(int ini = 0; ini < n; ini += BLOCO) {
        int m = (n - ini < BLOCO) ? n - ini : BLOCO;

        /* ---- 1x por bloco: suaviza parâmetros e dispara efeitos ---- */
        float rpm0 = eu.rpm, ac0 = eu.acel;
        /* troca de marcha: a rotação despenca -> corte de ignição curto */
        if(alvo_rpm < eu.rpm - 0.12f && eu.rpm > 0.8f) corte = 1.0f;
        /* tirou o pé com o turbo cheio: a wastegate alivia a pressão (chiado suave) */
        if(alvo_acel < 0.2f && eu.acel > 0.7f && eu.tb > 0.35f && wg < 0.1f) wg = eu.tb;
        wg *= 0.985f;
        float g_wg = wg;
        eu.rpm  += (alvo_rpm - eu.rpm) * 0.30f;
        eu.acel += (alvo_acel - eu.acel) * 0.05f;          /* ~40 ms: liga-desliga vira rampa */
        float drpm = (eu.rpm - rpm0) / m, dac = (eu.acel - ac0) / m;
        float inc0 = eu.inc ? eu.inc : motor_inc(rpm0);
        eu.inc = motor_inc(eu.rpm);
        float dinc = (eu.inc - inc0) / m;
        riv.rpm += (alvo_riv_rpm - riv.rpm) * 0.2f;
        riv_dop += (alvo_riv_dop - riv_dop) * 0.08f;
        float riv_inc = motor_inc(riv.rpm) * riv_dop;
        float g_corte = 1.0f - 0.3f * corte;              /* troca de marcha: leve queda */
        corte *= 0.85f;
        riv_vol += (alvo_riv_vol - riv_vol) * 0.15f;
        riv_pan += (alvo_riv_pan - riv_pan) * 0.15f;
        derrap  += (alvo_derrap - derrap) * 0.25f;
        grama   += (alvo_grama - grama) * 0.2f;
        tc_vol  += (alvo_torcida - tc_vol) * 0.01f;
        tc_fase += 0.000035f * m; if(tc_fase > 1.0f) tc_fase -= 1.0f;
        float g_torcida = tc_vol * (0.12f + 0.06f * seno_rapido(tc_fase));
        uivo_lfo += 0.004f * m; if(uivo_lfo > 1.0f) uivo_lfo -= 1.0f;
        float uivo_inc = (820.0f + 90.0f * seno_rapido(uivo_lfo)) / TAXA;

        if(pedido_bip) {                /* 1 = bip das luzes, 2 = largada (mais agudo e longo) */
            bip_inc = (pedido_bip == 2 ? 1320.0f : 880.0f) / TAXA;
            bip_env = pedido_bip == 2 ? 1.6f : 1.0f;
            pedido_bip = 0;
        }
        if(pedido_batida) { bat_env = pedido_batida_vol; pedido_batida = 0; }
        if(pedido_trovao) { tv_env = 1.0f; tv_estalo = 1.0f; pedido_trovao = 0; }
        ch_vol += (alvo_chuva - ch_vol) * 0.05f;
        ag_vol += (alvo_agua - ag_vol) * 0.2f;
        float riv_e = riv_vol * (1.0f - riv_pan), riv_d = riv_vol * riv_pan;
        /* fontes em silêncio não são calculadas (o sintetizador é o maior custo de CPU do som) */
        int tem_pneu = derrap > 0.002f, tem_grama = grama > 0.002f, tem_torcida = g_torcida > 0.002f;
        int tem_chuva = ch_vol > 0.002f || ag_vol > 0.002f;

        /* ---- por amostra ---- */
        for(int i = 0; i < m; i++) {
            float rpm = rpm0 + drpm * i, ac = ac0 + dac * i;
            float mot = motor_amostra(&eu, inc0 + dinc * i, rpm, ac) * (0.15f + 0.15f * ac) * g_corte;
            if(g_wg > 0.01f) {                    /* chiado agudo da wastegate */
                float r5 = ruido();
                wg_f += 0.35f * (r5 - wg_f);
                mot += (r5 - wg_f) * g_wg * 0.035f;
            }
            float rv  = riv_vol > 0.01f ? motor_leve(&riv, riv_inc, riv.rpm) * 0.18f : 0.0f;

            /* pneu: passa-banda de ruído + uivo tonal */
            float pneu = 0, gr = 0, tc = 0;
            if(tem_pneu) {
                float r = ruido();
                pn_a += 0.45f * (r - pn_a);
                pn_b += 0.08f * (r - pn_b);
                uivo_fase += uivo_inc; if(uivo_fase >= 1.0f) uivo_fase -= 1.0f;
                pneu = ((pn_a - pn_b) * 0.55f + seno_rapido(uivo_fase) * 0.18f) * derrap * 0.55f;
            }
            /* grama / terra */
            if(tem_grama) {
                gr_filtro += 0.05f * (ruido() - gr_filtro);
                gr = gr_filtro * grama * 0.9f;
            }
            /* torcida */
            if(tem_torcida) {
                float r2 = ruido();
                tc_a += 0.30f * (r2 - tc_a);
                tc_b += 0.04f * (r2 - tc_b);
                tc = (tc_a - tc_b) * g_torcida;
            }

            /* efeitos curtos */
            float fx = 0;
            if(bip_env > 0.001f) {
                bip_fase += bip_inc; if(bip_fase >= 1.0f) bip_fase -= 1.0f;
                fx += seno_rapido(bip_fase) * (bip_env > 1.0f ? 1.0f : bip_env) * 0.22f;
                bip_env -= 1.0f / (TAXA * 0.22f);
            }
            if(bat_env > 0.001f) {
                bat_filtro += 0.12f * (ruido() - bat_filtro);
                fx += bat_filtro * bat_env * 1.6f;
                bat_env *= 0.9993f;
            }

            /* chuva: gotas (agudo) + fundo grave; pneu jogando água */
            float chv = 0;
            if(tem_chuva) {
                float r3 = ruido();
                ch_a += 0.60f * (r3 - ch_a);
                ch_b += 0.15f * (r3 - ch_b);
                ch_grave += 0.02f * (r3 - ch_grave);
                chv = (ch_a - ch_b) * ch_vol * 0.12f + ch_grave * ch_vol * 0.22f + (ch_a - ch_b) * ag_vol * 0.12f;   /* chuva e spray mais baixos */
            }
            /* trovão: dois passa-baixas bem lentos + estalo no começo */
            if(tv_env > 0.001f) {
                float r4 = ruido();
                tv_f1 += 0.010f * (r4 - tv_f1);
                tv_f2 += 0.004f * (tv_f1 - tv_f2);
                fx += (tv_f2 * 3.3f + tv_f1 * tv_estalo * 0.8f) * tv_env;
                tv_env *= 0.99997f;
                tv_estalo *= 0.9995f;
            }
            fx += chv;

            float centro = mot + pneu + gr + fx;
            float e = (centro + rv * riv_e + tc) * 0.82f, d = (centro + rv * riv_d + tc) * 0.82f;   /* volume geral */
#ifdef DIAG_SOM
            if(fabsf(e) > 1.0f || fabsf(d) > 1.0f) diag_clip++;
#endif
            e = limitar_suave(e); d = limitar_suave(d);
            saida[2 * (ini + i)]     = (int16_t)(e * 30000.0f);
            saida[2 * (ini + i) + 1] = (int16_t)(d * 30000.0f);
        }
    }
}

/* FILA DE ÁUDIO PRONTO. O AICA pede ~2048 amostras de uma vez a cada ~93 ms;
 * sintetizar tudo isso num quadro custava 8-10 ms (o quadro perdia o 60 fps).
 * Agora som_atualizar() sintetiza um pouco a cada chamada (2x por quadro)
 * e mantém a fila cheia; o pedido do AICA vira só uma cópia. */
#define FILA_N   8192                     /* amostras estéreo (potência de 2) */
#define FILA_ALVO 2900                    /* nível desejado (pedido de 2048 + folga) */
#define FILA_PASSO 448                    /* máximo sintetizado por chamada */
static int16_t fila[FILA_N * 2] __attribute__((aligned(32)));
static int fila_ini, fila_n;

static void fila_produzir(int n) {
    while(n > 0) {
        int fim = (fila_ini + fila_n) & (FILA_N - 1);
        int k = FILA_N - fim; if(k > n) k = n;
        sintetizar(&fila[fim * 2], k);
        fila_n += k; n -= k;
    }
}

static void *callback(snd_stream_hnd_t h, int bytes, int *entregues) {
    (void)h;
    if(bytes > (int)sizeof(buf)) bytes = sizeof(buf);
    int n = bytes / 4;                            /* estéreo 16 bits */
    /* pega o que já está pronto na fila */
    int tirar = n < fila_n ? n : fila_n, feito = 0;
    while(feito < tirar) {
        int k = FILA_N - fila_ini; if(k > tirar - feito) k = tirar - feito;
        memcpy(&buf[feito * 2], &fila[fila_ini * 2], k * 4);
        fila_ini = (fila_ini + k) & (FILA_N - 1); fila_n -= k; feito += k;
    }
#ifdef DIAG_SOM
    uint64_t t0 = timer_us_gettime64();
    if(diag_ult) { int g = (int)((t0 - diag_ult) / 1000); if(g > diag_gap) diag_gap = g; }
    diag_ult = t0;
    if(n > diag_ped) diag_ped = n;
#endif
    if(feito < n) sintetizar(&buf[feito * 2], n - feito);   /* faltou: completa na hora */
#ifdef DIAG_SOM
    int us = (int)(timer_us_gettime64() - t0); if(us > diag_us) diag_us = us;
#endif
    *entregues = n * 4;
    return buf;
}

void som_iniciar(void) {
    snd_stream_init();
    stream = snd_stream_alloc(callback, BUF_STREAM);
    snd_stream_start(stream, TAXA, 1);
}

void som_atualizar(void) {
    snd_stream_poll(stream);
    int falta = FILA_ALVO - fila_n;
    if(falta > FILA_PASSO) falta = FILA_PASSO;
    if(falta > 0) fila_produzir(falta);
}

void som_motor(float rpm, float acelerador, float derrapagem, float na_grama) {
    alvo_rpm = rpm; alvo_acel = acelerador; alvo_derrap = derrapagem; alvo_grama = na_grama;
}
void som_rival(float rpm, float volume, float pan, float doppler) {
    alvo_riv_rpm = rpm; alvo_riv_vol = volume; alvo_riv_pan = pan; alvo_riv_dop = doppler;
}
void som_torcida(float volume) { alvo_torcida = volume; }
void som_bip(int largada) { pedido_bip = largada ? 2 : 1; }
void som_batida(float volume) { pedido_batida_vol = volume; pedido_batida = 1; }
void som_chuva(float chuva, float agua_pneu) { alvo_chuva = chuva; alvo_agua = agua_pneu; }
void som_trovao(void) { pedido_trovao = 1; }
