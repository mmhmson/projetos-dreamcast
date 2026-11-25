/* CLASSIC RACING GT - DC EDITION (pasta 10-formula): jogo de fórmula em Interlagos, 1999.
 *
 * A ideia em uma frase: o MUNDO é 3D de verdade (malha low poly desenhada
 * pelo PowerVR, com relevo e neblina), mas a FÍSICA é 2.5D: cada carro só
 * conhece duas coordenadas da pista,
 *
 *     s = metros percorridos ao longo do traçado (0 .. comprimento da volta)
 *     x = posição lateral em metros (0 = centro, + = direita)
 *
 * mais a velocidade v e o ângulo psi entre o carro e a pista. Na curva, a
 * pista "gira embaixo do carro" (psi' = omega - curvatura*v): se você não
 * esterçar, o carro vai para fora, como no Super Monaco GP. A aderência
 * (que cresce com a velocidade, pela asa) limita quanto dá para esterçar.
 *
 * Do 3D, só o desenho: a cada quadro transformamos (s, x) em (X, Y, Z) do
 * mundo usando a linha central da pista, montada com o traçado e o relevo
 * reais de Interlagos (OpenStreetMap ODbL + SRTM, gerados no 09-corrida).
 *
 * Controles: analógico/direcional = volante, A ou R = acelerar,
 *            B ou L = frear, Y = trocar câmera (atrás do carro / cockpit).
 */

#include <kos.h>
#ifndef SEM_CARRO_TEX
#define CARRO_TEX        /* rivais: modelo híbrido (corpo texturizado + peças em geometria); -DSEM_CARRO_TEX volta ao antigo */
#endif
#include <dc/pvr/pvr_regs.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <malloc.h>
#include <dc/matrix.h>
#include <dc/fmath.h>
#ifdef DMA_VERTICES
/* VÉRTICES POR DMA: em vez de mandar cada vértice ao chip pela store queue,
 * ele é escrito num buffer na RAM e o hardware transfere tudo por DMA (o
 * processador não espera o chip). Os cabeçalhos (pvr_prim) e o envio direto
 * escrevem no MESMO buffer, na ordem certa. */
static pvr_list_t dma_lista;
static uint8_t *dma_p;
static size_t dma_n;
static inline void dma_flush(void) { if(dma_n) { pvr_vertbuf_written(dma_lista, dma_n); dma_n = 0; } dma_p = NULL; }
static inline void *dma_target(void) { if(!dma_p) dma_p = pvr_vertbuf_tail(dma_lista); return dma_p + dma_n; }
#undef pvr_dr_target
#undef pvr_dr_commit
#define pvr_dr_target(...) ((pvr_vertex_t *)dma_target())
#define pvr_dr_commit(a) (dma_n += 32)
static inline int prim_dma(const void *d, size_t sz) { dma_flush(); return (pvr_prim)(d, sz); }
#define pvr_prim(d, sz) prim_dma((d), (sz))
static inline int list_begin_dma(pvr_list_t l) { dma_flush(); dma_lista = l; return (pvr_list_begin)(l); }
#define pvr_list_begin(l) list_begin_dma(l)
static inline int list_finish_dma(void) { dma_flush(); return (pvr_list_finish)(); }
#define pvr_list_finish() list_finish_dma()
#endif
#include "pista_interlagos.h"
#include "som.h"
#ifndef SKYBOX
#define SKYBOX 1          /* 1 = céu pintado (skybox); 0 = céu antigo (degradê + polígonos) */
#endif

#define PI_F    3.14159265f
#define TELA_W  640.0f
#define TELA_H  480.0f
/* centro da projeção: variável porque o retrovisor renderiza numa textura menor */
static float CX = 320.0f, CY = 240.0f;
static float ESPELHAR = 1.0f;     /* -1 = imagem invertida (retrovisor) */
static float XS = 1.0f;           /* 2 = antisserrilhado (o chip desenha com o dobro da largura e reduz) */
static pvr_ptr_t tex_telao, tex_refl;
static int8_t muro_lado[120];
static int n_muro;
static pvr_poly_hdr_t hdr_telao, hdr_refl;
static int telao_pronto, refl_ativo;
static float refl_cy, refl_P[3], refl_R[3], refl_U[3], refl_F[3], refl_foco;
static float telao_q[4][3];
#define TELAO_S 230.0f
#define TELAO_L 36.0f
static float dist2_cam(const float p[3]);

#ifndef CAM_INI
#define CAM_INI 0           /* 0 = atrás do carro, 1 = cockpit */
#endif
#ifndef S_INI
#define S_INI   -1.0f       /* >= 0: começa já correndo nesse metro (testes) */
#endif

/* ======================================================================== */
/* Utilidades                                                                */
/* ======================================================================== */

static inline uint32_t h32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
static inline float aleat(uint32_t semente) { return (h32(semente) & 0xFFFF) / 65535.0f; }

static inline float limitar(float v, float a, float b) { return v < a ? a : v > b ? b : v; }

static uint32_t cor_escala(uint32_t c, float k) {
    /* uma conversão float->int só (k em ponto fixo 8.8), o resto em inteiros */
    int ki = (int)(k * 256.0f);
    if(ki < 0) ki = 0;
    int r = (int)((c >> 16) & 0xFF) * ki >> 8, g = (int)((c >> 8) & 0xFF) * ki >> 8, b = (int)(c & 0xFF) * ki >> 8;
    if(r > 255) r = 255;
    if(g > 255) g = 255;
    if(b > 255) b = 255;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}
static uint32_t cor_mistura(uint32_t a, uint32_t b, float t) {
    int r = (int)(((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t);
    int g = (int)(((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t);
    int bb = (int)((a & 0xFF) * (1 - t) + (b & 0xFF) * t);
    return 0xFF000000 | (r << 16) | (g << 8) | bb;
}

/* Clima de São Paulo: céu azul meio esbranquiçado pela poluição/umidade. */
#define COR_CEU_TOPO    0xFF4A80C0
#define COR_NEBLINA     0xFFC6D0D6
#define NEBLINA_INI     70.0f
#define NEBLINA_FIM     640.0f

/* ---------------- CLIMA ----------------
 * Interlagos é famosa pelo tempo instável: nublado quase sempre e chuva que
 * chega no meio da corrida. O estado do clima muda devagar e mexe em tudo:
 * cor do céu e da neblina, alcance da visão, luz do sol, cor do asfalto
 * (molhado = mais escuro), aderência e freio, gotas, spray e som. */
enum { CLIMA_DINAMICO, CLIMA_SOL, CLIMA_NUBLADO, CLIMA_CHUVA, CLIMA_PESADA, CLIMA_ENSOLARADO, CLIMA_ENTARDECER, N_MODOS_CLIMA };
#ifndef CLIMA_INI
#define CLIMA_INI CLIMA_DINAMICO
#endif
static int   modo_clima = CLIMA_INI;
static float nuvens = 0.7f;     /* 0 = céu limpo .. 1 = fechado */
static float chuva = 0.0f;      /* intensidade da chuva 0..1 */
static float tarde = 0.0f;      /* ENTARDECER: sol baixo a oeste, céu laranja 0..1 */
static float manha = 0.0f;      /* ENSOLARADO: manhã clara, sol baixo com raios de luz 0..1 */
static float pesada = 0.0f;     /* CHUVA PESADA (temporal) por cima da chuva 0..1: céu quase noite,
                                   visão ~170 m, cortina de spray, mais gotas, poças e raios */
static float molhado = 0.0f;    /* água sobre a pista 0..1 (molha e seca com atraso) */
static float relampago = 0.0f;  /* clarão 0..1 */
static float brilho = 1.0f;     /* luz geral da cena */
static float luz_base = 0.55f, luz_dir = 0.5f;
static float fator_grip = 1.0f, fator_freio = 1.0f;
static float dist_visao = 620.0f;
#ifndef T_CLIMA_INI
#define T_CLIMA_INI 0.0f
#endif
static float t_clima = T_CLIMA_INI;
float t_clima_pub;            /* relógio do clima (s) */
static float cam_s_ref;          /* metro da pista onde está a câmera */
static uint32_t cor_ceu = COR_CEU_TOPO, cor_neb = COR_NEBLINA;

/* Sol de fim de tarde vindo do noroeste (vetor JÁ normalizado na init). */
static float LUZ[3] = { -0.45f, 0.75f, -0.50f };

/* ======================================================================== */
/* A PISTA: linha central em fatias de 4 m                                   */
/* ======================================================================== */

#define PASSO       4               /* metros por fatia */
#define MAX_FATIAS  1200
#define MEIA_LARG   7.0f            /* meia largura do asfalto (pista ~14 m) */
#ifndef EXAGERO_RELEVO
#define EXAGERO_RELEVO 1.0f         /* 1 = desnível real (~40 m em Interlagos) */
#endif

typedef struct {
    float x, y, z;          /* centro da pista no mundo (X = leste, Z = sul, Y = cima) */
    float fx, fz;           /* frente horizontal (unitário) */
    float rx, rz;           /* direita horizontal (unitário) */
    float curva;            /* curvatura em 1/m (+ = curva para a DIREITA) */
    float rampa;            /* inclinação dY/ds */
    float larg_esq, larg_dir;   /* até onde vai a grama "de longe" de cada lado */
    float vlim;             /* velocidade máxima "boa" (m/s), usada pela IA */
    uint8_t zebra;          /* 1 = trecho de curva: zebra vermelha/branca */
    uint8_t amb;            /* bits AMB_* (mata/água reais do OSM) */
    uint8_t box;            /* 1 = pit lane do lado esquerdo */
    uint8_t brita;          /* caixa de brita na área de escape: bit 1 = esquerda, 2 = direita */
} fatia_t;

static fatia_t F[MAX_FATIAS];
/* altura da BORDA de fora da faixa de grama (lateral 26 m) de cada fatia,
 * esquerda [0] e direita [1]: acompanha o terreno real (vira barranco onde a
 * pista corre num aterro). Antes era sempre a pista - 2 m, e no Bico de Pato
 * a faixa ficava flutuando, com muros, placas e árvores em cima. */
static float borda_y[MAX_FATIAS][2];
static float ter_saia[MAX_FATIAS][2];      /* terreno logo depois da faixa (barranco até ele) */
static inline float faixa_y(int k, float lat) {        /* altura da faixa de grama numa lateral */
    float a = fabsf(lat);
    if(a <= 8.6f) return F[k].y;
    float b = borda_y[k][lat > 0];
    float t = (a - 8.6f) / 17.4f;
    return F[k].y + (b - F[k].y) * (t > 1 ? 1 + (t - 1) * 0.3f : t);
}
static int     NF;          /* número de fatias */
static float   COMPR;       /* comprimento da volta em m */
static float   CENTRO_X, CENTRO_Z;

static float angulo_dif(float a, float b) {      /* a - b em (-pi, pi] */
    float d = a - b;
    while(d >  PI_F) d -= 2 * PI_F;
    while(d < -PI_F) d += 2 * PI_F;
    return d;
}

static void suavizar_ciclico(float *v, int n, int raio, int passadas) {
    float *t = malloc(n * sizeof(float));
    for(int p = 0; p < passadas; p++) {
        for(int i = 0; i < n; i++) {
            float s = 0;
            for(int k = -raio; k <= raio; k++) s += v[(i + k + n) % n];
            t[i] = s / (2 * raio + 1);
        }
        memcpy(v, t, n * sizeof(float));
    }
    free(t);
}

/* Aderência lateral (m/s²) do carro em função da velocidade: base mecânica
 * + pressão aerodinâmica (cresce com v²). A ~300 km/h passa de 4 g. */
#define GRIP_BASE 16.5f
#define GRIP_AERO 0.0040f
#define FAIXA_DERRAPAGEM 1.50f      /* quanto além do limite dá para "escorregar" */
static inline float aderencia(float v) { return (GRIP_BASE + GRIP_AERO * v * v) * fator_grip; }
/* Freio (m/s²): também melhora com a asa. */
static inline float desaceleracao(float v) { return (20.0f + 0.0018f * v * v) * fator_freio; }

static uint32_t tinta_tarde(uint32_t c, float d) {      /* d = N·L (0..1) */
    if(tarde < 0.05f || d <= 0) return c;
    float k = tarde * d * 0.65f;
    int r = (int)((c >> 16) & 255), g = (int)((c >> 8) & 255), b = (int)(c & 255);
    int r2 = r * 13 / 10 + 6, g2 = g * 176 / 255, b2 = b * 112 / 255;      /* luz laranja MULTIPLICA (preto continua preto) */
    if(r2 > 255) r2 = 255;
    r = r + (int)((r2 - r) * k); g = g + (int)((g2 - g) * k); b = b + (int)((b2 - b) * k);
    return 0xFF000000 | r << 16 | g << 8 | b;
}
static float borracha_lat[MAX_FATIAS];     /* lateral da trajetória ideal (linha de borracha) */
static void montar_pista(void) {
    const pista_osm_t *P = &PISTA_INTERLAGOS;
    int n = P->n_seg;
    float *px = malloc(n * sizeof(float)), *pz = malloc(n * sizeof(float)), *py = malloc(n * sizeof(float));

    /* 1) Integra o RUMO real (1 m por segmento) para obter o desenho em planta. */
    float x = 0, z = 0;
    for(int i = 0; i < n; i++) {
        px[i] = x; pz[i] = z;
        /* a coluna de altura do 09 está em "unidades" pseudo-3D (200/m) com
         * exagero 1,6x: converte para METROS reais do SRTM */
        py[i] = P->pista[i][1] / (200.0f * 1.6f) * EXAGERO_RELEVO;
        float th = P->rumo[i] * (PI_F / 1800.0f);       /* 0 = leste, anti-horário */
        x += cosf(th);
        z -= sinf(th);                                  /* norte = -Z */
    }
    /* 2) O OSM não fecha a volta exatamente: espalha o erro ao longo dela. */
    for(int i = 0; i < n; i++) { px[i] -= x * i / n; pz[i] -= z * i / n; }
    /* 3) Suaviza (o traçado do OSM é uma poligonal com "quinas"). */
    suavizar_ciclico(px, n, 4, 3);
    suavizar_ciclico(pz, n, 4, 3);
    suavizar_ciclico(py, n, 8, 2);

    NF = n / PASSO;
    if(NF > MAX_FATIAS) NF = MAX_FATIAS;
    COMPR = (float)(NF * PASSO);
    CENTRO_X = CENTRO_Z = 0;

    for(int k = 0; k < NF; k++) {
        int i = k * PASSO;
        F[k].x = px[i]; F[k].y = py[i]; F[k].z = pz[i];
        F[k].amb = P->ambiente[i];
        CENTRO_X += px[i] / NF; CENTRO_Z += pz[i] / NF;
    }
    float rumo[MAX_FATIAS];
    for(int k = 0; k < NF; k++) {
        fatia_t *a = &F[(k + NF - 1) % NF], *b = &F[(k + 1) % NF];
        float dx = b->x - a->x, dz = b->z - a->z, d = sqrtf(dx * dx + dz * dz);
        F[k].fx = dx / d; F[k].fz = dz / d;
        F[k].rx = -F[k].fz; F[k].rz = F[k].fx;          /* direita = frente x cima */
        F[k].rampa = (b->y - a->y) / (2 * PASSO);
        rumo[k] = atan2f(-F[k].fz, F[k].fx);
    }
    /* curvatura = quanto o rumo gira por metro (sinal: + = direita) */
    float curva[MAX_FATIAS], larga[MAX_FATIAS];
    for(int k = 0; k < NF; k++)
        curva[k] = -angulo_dif(rumo[(k + 1) % NF], rumo[(k + NF - 1) % NF]) / (2 * PASSO);
    suavizar_ciclico(curva, NF, 1, 2);
    memcpy(larga, curva, sizeof(float) * NF);
    suavizar_ciclico(larga, NF, 6, 2);                  /* curvatura "média" de ~50 m */

    for(int k = 0; k < NF; k++) {
        F[k].curva = curva[k];
        F[k].zebra = fabsf(curva[k]) > 1.0f / 220.0f;
        F[k].brita = 0;
        /* A grama de longe vai até 150 m; do lado de DENTRO da curva, ela
         * é encurtada para não "dobrar" por cima da própria pista. */
        float dentro = fabsf(larga[k]) > 1e-4f ? 0.7f / fabsf(larga[k]) : 150.0f;
        dentro = limitar(dentro, 30.0f, 120.0f);
        F[k].larg_esq = larga[k] < 0 ? dentro : 120.0f;
        F[k].larg_dir = larga[k] > 0 ? dentro : 120.0f;

        /* velocidade-limite da curva: v²·curva <= aderência(v) */
        float c = fabsf(curva[k]);
        float vmax = 92.0f;
        if(c > GRIP_AERO) { float v2 = GRIP_BASE / (c - GRIP_AERO); if(v2 < vmax * vmax) vmax = sqrtf(v2); }
        F[k].vlim = vmax;
    }
    /* Passagem para trás: dá tempo de frear antes de cada curva (2 voltas p/ fechar o ciclo). */
    for(int p = 0; p < 2; p++)
        for(int k = NF - 1; k >= 0; k--) {
            float vn = F[(k + 1) % NF].vlim;
            float vmax = sqrtf(vn * vn + 2.0f * 0.8f * desaceleracao(vn) * PASSO);
            if(F[k].vlim > vmax) F[k].vlim = vmax;
        }
    /* CAIXAS DE BRITA: por fora das curvas fortes (e um pouco antes/depois) */
    for(int k = 0; k < NF; k++)
        if(fabsf(F[k].curva) > 1.0f / 110.0f)
            for(int d = -5; d <= 3; d++) F[(k + d + NF) % NF].brita |= F[k].curva > 0 ? 1 : 2;   /* curva à direita: fora = esquerda */
    /* pit lane real (lado esquerdo) */
    for(int b = 0; b < P->n_boxes; b++) {
        /* o trecho pode "passar" da linha de chegada (fim > n): normaliza */
        int ini = P->boxes[b].ini % n, fim = P->boxes[b].fim % n;
        for(int i = ini; i != fim; i = (i + 1) % n)
            if(i % PASSO == 0 && i / PASSO < NF) F[i / PASSO].box = 1;
    }
    for(int k = 0; k < NF; k++) if(F[k].box) F[k].brita &= ~1;      /* nada de brita no pit */
    /* TRAJETÓRIA IDEAL: curvatura suavizada (±60 m) -> lateral (ápice por dentro) */
    for(int k = 0; k < NF; k++) {
        float sm = 0;
        for(int d = -15; d <= 15; d++) sm += F[(k + d + NF) % NF].curva;
        borracha_lat[k] = limitar(sm / 31.0f * 520.0f, -4.6f, 4.6f);
    }
    free(px); free(pz); free(py);
}

/* Ponto da pista no metro s, deslocado "lat" metros para a direita.
 * Devolve posição e (opcional) a base local frente/direita. */
static void ponto_pista(float s, float lat, float out[3], float fr[3], float dir[3]) {
    s = fmodf(s, COMPR);
    if(s < 0) s += COMPR;
    float fk = s / PASSO;
    int k = (int)fk;
    float t = fk - k;
    const fatia_t *a = &F[k % NF], *b = &F[(k + 1) % NF];
    float fx = a->fx + (b->fx - a->fx) * t, fz = a->fz + (b->fz - a->fz) * t;
    float d = 1.0f / sqrtf(fx * fx + fz * fz);
    fx *= d; fz *= d;
    out[0] = a->x + (b->x - a->x) * t - fz * lat;
    out[1] = a->y + (b->y - a->y) * t;
    out[2] = a->z + (b->z - a->z) * t + fx * lat;
    if(fr) {
        float rampa = a->rampa + (b->rampa - a->rampa) * t;
        float n = 1.0f / sqrtf(1 + rampa * rampa);
        fr[0] = fx * n; fr[1] = rampa * n; fr[2] = fz * n;
    }
    if(dir) { dir[0] = -fz; dir[1] = 0; dir[2] = fx; }
}

static inline const fatia_t *fatia_em(float s) {
    int k = (int)(fmodf(s + COMPR, COMPR) / PASSO);
    return &F[k % NF];
}

/* ======================================================================== */
/* CÂMERA, PROJEÇÃO E RECORTE (near plane)                                   */
/* ======================================================================== */

typedef struct { float x, y, z; uint32_t c; } vv_t;     /* vértice no espaço da câmera */

static float cam_p[3], cam_r[3], cam_u[3], cam_f[3];
static float FOCO = 415.0f;     /* distância focal em pixels (define o FOV) */
static float PERTO = 0.5f;      /* plano de recorte próximo (m) */

/* Matriz mundo -> câmera na unidade de matrizes do SH4 (XMTRX): para_vista()
 * transforma cada vértice com UMA instrução ftrv. desenhar_faces() troca pela
 * matriz do carro e, no fim, recarrega esta. */
static matrix_t mat_cam __attribute__((aligned(32)));
static void camera_matriz(void) {
    for(int j = 0; j < 3; j++) { mat_cam[j][0] = cam_r[j]; mat_cam[j][1] = cam_u[j]; mat_cam[j][2] = -cam_f[j]; mat_cam[j][3] = 0; }
    mat_cam[3][0] = -(cam_r[0] * cam_p[0] + cam_r[1] * cam_p[1] + cam_r[2] * cam_p[2]);
    mat_cam[3][1] = -(cam_u[0] * cam_p[0] + cam_u[1] * cam_p[1] + cam_u[2] * cam_p[2]);
    mat_cam[3][2] =  (cam_f[0] * cam_p[0] + cam_f[1] * cam_p[1] + cam_f[2] * cam_p[2]);
    mat_cam[3][3] = 1;
    mat_load(&mat_cam);
}
static void camera_olhar(const float pos[3], const float alvo[3]) {
    float f[3] = { alvo[0] - pos[0], alvo[1] - pos[1], alvo[2] - pos[2] };
    float n = 1.0f / sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    f[0] *= n; f[1] *= n; f[2] *= n;
    /* direita = frente x cima(0,1,0) ; cima = direita x frente */
    float r[3] = { -f[2], 0, f[0] };
    n = 1.0f / sqrtf(r[0] * r[0] + r[2] * r[2]);
    r[0] *= n; r[2] *= n;
    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    memcpy(cam_p, pos, 12); memcpy(cam_f, f, 12); memcpy(cam_r, r, 12); memcpy(cam_u, u, 12);
    camera_matriz();
}

static inline void para_vista(float x, float y, float z, uint32_t c, vv_t *o) {
    mat_trans_single3_nodiv(x, y, z);      /* XMTRX = mat_cam (câmera olha para -Z) */
    o->x = x; o->y = y; o->z = z;
    o->c = c;
}

static int vert_quadro;     /* contador de vértices enviados (orçamento) */

/* Projeta e escreve direto na store queue (direct render). */
static inline void enviar(const vv_t *p, int fim) {
    float iz = __frsqrt(p->z * p->z);   /* = 1/(-z) com a instrução fsrra (sem divisão) */
    pvr_vertex_t *v = pvr_dr_target();
    v->flags = fim ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = (CX + ESPELHAR * FOCO * p->x * iz) * XS;
    v->y = CY - FOCO * p->y * iz;
    v->z = iz;                  /* o PVR usa 1/w para profundidade e neblina */
    v->u = v->v = 0;
    v->argb = p->c; v->oargb = 0;
    pvr_dr_commit(v);
    vert_quadro++;
}

/* vértice com coordenada de textura (decalques dos carros) */
typedef struct { float x, y, z, u, v; uint32_t c; uint32_t o; } vvt_t;   /* o: brilho (cor somada, só em cabeçalhos com especular) */
static vvt_t muro_q[120][4];
static pvr_poly_hdr_t hdr_mundo, hdr_longe, hdr_2d, hdr_tr2d;
static inline void brilho_add(const float p[3], float r, uint32_t c);
static pvr_poly_hdr_t hdr_fumaca;
#define hdr_tr2d_3d hdr_tr2d
/* texturas do cenário: asfalto, grama, torcida das arquibancadas, placas */
static pvr_poly_hdr_t hdr_asfalto, hdr_grama, hdr_torcida, hdr_placas, hdr_janelas, hdr_agua, hdr_bandeiras;
static pvr_poly_hdr_t hdr_torcida2, hdr_torcida_longe;
static uint16_t pal_torcida[256];
/* ANIMAÇÃO POR PALETA (truque clássico): algumas cores da torcida pulsam
 * devagar, como gente se mexendo — sem trocar a textura (não cintila) */
static void animar_paleta_torcida(float t) {
#ifdef TORCIDA_16BIT
    (void)t; return;
#endif
    for(int i = 3; i < 256; i += 7) {
        uint16_t c = pal_torcida[i];
        float k = 1.0f + 0.09f * fsin(t * 1.6f + i * 0.9f);
        int r = (int)((c >> 11) * k), g = (int)(((c >> 5) & 63) * k), b = (int)((c & 31) * k);
        if(r > 31) r = 31;
        if(g > 63) g = 63;
        if(b > 31) b = 31;
        pvr_set_pal_entry(i, (uint32_t)(r << 11 | g << 5 | b));
    }
}
static inline const pvr_poly_hdr_t *torcida_quadro(float fase, float agito) {
#ifdef TORCIDA_16BIT
    (void)fase; (void)agito; return &hdr_torcida;
#else
    extern float t_clima_pub;
    (void)fase; (void)agito;
    return &hdr_torcida;          /* a troca de quadros "picotava" as pessoas nessa resolução: desligada */
#endif
}
static pvr_poly_hdr_t hdr_pneus, hdr_concreto, hdr_placas_muro, hdr_nuvem, hdr_cerca;
#include "texturas.h"
extern const uint8_t tex_bin[];
static pvr_poly_hdr_t hdr_cidade, hdr_cidade_luz, hdr_marcas, hdr_roda_borr, hdr_sombra_carro, hdr_nuvem, hdr_brilho_luz;
static int mip_4444, mip_spec;
static int caixa_vidro;           /* desenhar_caixa: fachada de vidro (reflete o céu) */
static pvr_poly_hdr_t hdr_nuv_mov;
static pvr_poly_hdr_t hdr_fach[6], hdr_jan_luz, hdr_bairros, hdr_borda, hdr_grelha, hdr_bairros_luz, hdr_transito, hdr_faixas;
static pvr_ptr_t tex_subir_mip(int n);
static pvr_poly_hdr_t hdr_brilho, hdr_tinta, hdr_corte, hdr_brita, hdr_zebra, hdr_folhas, hdr_carbono, hdr_pneu_letras, hdr_sombra, hdr_digitos;
static uint8_t caixa_tex;          /* desenhar_caixa: 1 = torcida, 2..5 = placa de publicidade, 6 = janelas */
static uint8_t caixa_lote;         /* 1: o cabeçalho de janelas já está ativo (desenho em lote, sem trocas) */
static inline void enviar_tex(const vvt_t *p, int fim) {
    float iz = __frsqrt(p->z * p->z);
    pvr_vertex_t *v = pvr_dr_target();
    v->flags = fim ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = (CX + ESPELHAR * FOCO * p->x * iz) * XS;
    v->y = CY - FOCO * p->y * iz;
    v->z = iz;
    v->u = p->u; v->v = p->v;
    v->argb = p->c; v->oargb = p->o;
    pvr_dr_commit(v);
    vert_quadro++;
}

static inline void enviar_2d(float x, float y, float z, uint32_t c, int fim) {
    pvr_vertex_t *v = pvr_dr_target();
    v->flags = fim ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = x * XS; v->y = y; v->z = z; v->u = v->v = 0; v->argb = c; v->oargb = 0;
    pvr_dr_commit(v);
    vert_quadro++;
}

/* Polígono convexo (ordem cíclica, até 8 vértices): recorta contra o plano
 * próximo (Sutherland-Hodgman) e envia como triangle strip em "zigue-zague". */
static int sem_envio;           /* teste de medição: não envia ao PVR */
static void poligono(const vv_t *in, int n) {
    if(sem_envio) return;
    vv_t out[12];
    int m = 0, todos = 1;
    for(int i = 0; i < n; i++) if(in[i].z > -PERTO) { todos = 0; break; }
    if(todos) {
        /* caso comum (nada a recortar): envia direto, sem copiar */
        int lo = 1, hi = n - 1, k = 1;
        enviar(&in[0], 0);
        while(lo <= hi) {
            enviar(&in[lo++], k == n - 1); k++;
            if(lo <= hi) { enviar(&in[hi--], k == n - 1); k++; }
        }
        return;
    }
    else {
        for(int i = 0; i < n; i++) {
            const vv_t *a = &in[i], *b = &in[(i + 1) % n];
            int da = a->z <= -PERTO, db = b->z <= -PERTO;
            if(da) out[m++] = *a;
            if(da != db) {
                float t = (-PERTO - a->z) / (b->z - a->z);
                vv_t *o = &out[m++];
                o->x = a->x + (b->x - a->x) * t;
                o->y = a->y + (b->y - a->y) * t;
                o->z = -PERTO;
                o->c = da ? a->c : b->c;
            }
        }
        if(m < 3) return;
    }
    /* zigue-zague: 0, 1, m-1, 2, m-2, ... vira um strip que cobre o polígono */
    int lo = 1, hi = m - 1, k = 1;
    enviar(&out[0], 0);
    while(lo <= hi) {
        enviar(&out[lo++], k == m - 1); k++;
        if(lo <= hi) { enviar(&out[hi--], k == m - 1); k++; }
    }
}

/* igual a poligono(), mas leva a coordenada de textura junto no recorte */
static void poligono_tex(const vvt_t *in, int n) {
    if(sem_envio) return;
    vvt_t out[12];
    int m = 0;
    int todos = 1;
    for(int i = 0; i < n; i++) if(in[i].z > -PERTO) { todos = 0; break; }
    if(todos) {               /* caso comum (nada a recortar): envia direto, sem copiar */
        int lo = 1, hi = n - 1, k = 1;
        enviar_tex(&in[0], 0);
        while(lo <= hi) {
            enviar_tex(&in[lo++], k == n - 1); k++;
            if(lo <= hi) { enviar_tex(&in[hi--], k == n - 1); k++; }
        }
        return;
    }
    for(int i = 0; i < n; i++) {
        const vvt_t *a = &in[i], *b = &in[(i + 1) % n];
        int da = a->z <= -PERTO, db = b->z <= -PERTO;
        if(da) out[m++] = *a;
        if(da != db) {
            float t = (-PERTO - a->z) / (b->z - a->z);
            vvt_t *o = &out[m++];
            o->x = a->x + (b->x - a->x) * t; o->y = a->y + (b->y - a->y) * t; o->z = -PERTO;
            o->u = a->u + (b->u - a->u) * t; o->v = a->v + (b->v - a->v) * t;
            o->c = da ? a->c : b->c; o->o = da ? a->o : b->o;
        }
    }
    if(m < 3) return;
    int lo = 1, hi = m - 1, k = 1;
    enviar_tex(&out[0], 0);
    while(lo <= hi) {
        enviar_tex(&out[lo++], k == m - 1); k++;
        if(lo <= hi) { enviar_tex(&out[hi--], k == m - 1); k++; }
    }
}

/* ======================================================================== */
/* DESENHO DA PISTA (malha gerada a cada quadro a partir das fatias)         */
/* ======================================================================== */

/* Corte transversal: 10 pontos laterais -> 9 faixas
 *   grama longe | grama perto | zebra | linha | ASFALTO | linha | zebra | grama perto | grama longe */
#define NPTS 10
static const float LAT[NPTS] = { 0, -26.0f, -8.6f, -7.3f, -7.0f, 7.0f, 7.3f, 8.6f, 26.0f, 0 };

#define MAX_VIS 140
typedef struct { int k; int frente; vv_t p[NPTS]; uint8_t oc[NPTS]; float uv[NPTS][2]; float st[NPTS][2]; } vis_t;   /* oc: fora da tela (bits) */
static vis_t vis[MAX_VIS];
static int nvis;

#define DIST_VISAO 620.0f

static uint32_t cor_faixa_seca(int faixa, const fatia_t *f, int k) {
    int par = k & 1;
    switch(faixa) {
        case 0: if(f->amb & AMB_AGUA_ESQ) return par ? 0xFF3F7290 : 0xFF3A6C8A;
                return par ? 0xFF4F8A36 : 0xFF4A8432;
        case 8: if(f->amb & AMB_AGUA_DIR) return par ? 0xFF3F7290 : 0xFF3A6C8A;
                return par ? 0xFF4F8A36 : 0xFF4A8432;
        case 1: if(f->box) return par ? 0xFF64676C : 0xFF5F6267;   /* pit lane */
                /* fallthrough */
        case 7: return par ? 0xFF62A23E : 0xFF5B993A;
        case 3: case 5: return 0xFFE6E6E6;
        case 4: return par ? 0xFF5C5F65 : 0xFF575A60;
        default: return 0;
    }
}

/* cor da faixa com o clima: asfalto escurece molhado; tudo segue a luz geral */
static uint32_t cor_faixa(int faixa, const fatia_t *f, int k) {
    uint32_t c = cor_faixa_seca(faixa, f, k);
    if(faixa == 4) c = cor_mistura(c, 0xFF383C42, molhado * 0.75f);
    else if(faixa != 3 && faixa != 5) c = cor_mistura(c, cor_escala(c, 0.8f), molhado * 0.5f);
    return cor_escala(c, brilho);
}

/* FILTRO DE TELA: cada ponto ganha bits dizendo de que lado da tela ele está
 * (1 direita, 2 esquerda, 4 acima, 8 abaixo, 16 atrás do plano próximo).
 * Um pedaço cujos 4 cantos têm um bit EM COMUM está todo fora -> não é
 * enviado (ex.: o trecho da pista que some pela lateral numa curva). */
static float tela_tx, tela_tc, tela_tb;      /* tangentes: meia-largura, acima, abaixo */
static void tela_limites(void) {
    tela_tx = CX / FOCO * 1.05f;
    tela_tc = CY / FOCO * 1.05f;
    tela_tb = CY / FOCO * 1.50f;              /* abaixo do centro (o retrovisor tem CY a 42%) */
}
static inline uint8_t fora_tela(const vv_t *p) {
    float w = -p->z;
    if(w < PERTO) return 16;                  /* atrás: nada de bits laterais (seriam enganosos) */
    uint8_t oc = 0;
    if(p->x >  w * tela_tx) oc |= 1;
    if(p->x < -w * tela_tx) oc |= 2;
    if(p->y >  w * tela_tc) oc |= 4;
    if(p->y < -w * tela_tb) oc |= 8;
    return oc;
}

static void montar_visiveis_dir(float s_cam, int sentido, float dist) {
    tela_limites();
    int k0 = (int)(fmodf(s_cam + COMPR, COMPR) / PASSO) + (sentido > 0 ? -1 : 2);
    int desl = 0;
    nvis = 0;
    while(nvis < MAX_VIS && desl * PASSO < dist) {
        int k = ((k0 + sentido * desl) % NF + NF) % NF;
        const fatia_t *f = &F[k];
        vis_t *v = &vis[nvis++];
        v->k = k;
        v->frente = 1;
        for(int i = 0; i < NPTS; i++) {
            float lat = i == 0 ? -f->larg_esq : i == NPTS - 1 ? f->larg_dir : LAT[i];
            /* a grama de longe CAI em rampa (~7°): vista de baixo, numa subida,
             * parece um barranco e não uma "placa" voando acima da pista */
            float y = (i == 1 || i == NPTS - 2) ? borda_y[k][i > 1] :
                      (i == 0 || i == NPTS - 1) ? borda_y[k][i > 1] - 0.12f * (fabsf(lat) - 26.0f) : f->y;
            float wx = f->x + f->rx * lat, wz = f->z + f->rz * lat;
            v->uv[i][0] = wx * 0.25f; v->uv[i][1] = wz * 0.25f;   /* textura a cada 4 m, alinhada ao mundo */
            v->st[i][0] = lat * 0.0625f; v->st[i][1] = k * (float)PASSO * 0.0625f;   /* alinhada à pista (16 m) */
            para_vista(wx, y, wz, 0, &v->p[i]);
            if(v->p[i].z > -PERTO) v->frente = 0;
            v->oc[i] = fora_tela(&v->p[i]);
        }
        desl += nvis < 50 ? 1 : 3;          /* LOD: fatias de 12 m depois de 200 m */
    }
}

static void montar_visiveis(float s_cam) { montar_visiveis_dir(s_cam, 1, dist_visao); }

static vv_t buf_strip[2 * MAX_VIS];
static vvt_t buf_strip_t[2 * MAX_VIS];
static void flush_strip_t(int n) {
    for(int i = 0; i < n; i++) enviar_tex(&buf_strip_t[i], i == n - 1);
}
/* OCLUSÃO PRÉ-CALCULADA da pista (0..255 = escuro..normal), por fatia e ponto lateral */
static uint8_t ao_p[MAX_FATIAS][NPTS];
static uint8_t nuv_f[MAX_FATIAS];          /* sombra de nuvem passando (255 = sol) */
static uint8_t somb_d[MAX_FATIAS][NPTS], somb_t[MAX_FATIAS][NPTS], somb[MAX_FATIAS][NPTS];   /* sombra projetada: dia, entardecer, atual */
static inline uint32_t cao(uint32_t c, int k, int i) {
    uint32_t a = (((ao_p[k][i] * (nuv_f[k] + 1)) >> 8) * (somb[k][i] + 1)) >> 8;
    if(a >= 255) return c;
    return 0xFF000000 | ((((c >> 16) & 255) * a >> 8) << 16) | ((((c >> 8) & 255) * a >> 8) << 8) | ((c & 255) * a >> 8);
}
/* BRILHO DO SOL NO ASFALTO (Sega Rally / Le Mans): reflexo do sol baixo ou
 * da pista molhada, como cor SOMADA no vértice (o asfalto tem especular) */
static int asf_brilho_on;
static float asf_Lv[3], asf_Nv[3], asf_k;
static uint32_t asf_cor;
static uint32_t brilho_asf(const vv_t *p) {
    float il = __frsqrt(p->x * p->x + p->y * p->y + p->z * p->z);
    float vx = p->x * il, vy = p->y * il, vz = p->z * il;
    float nd = 2.0f * (asf_Nv[0] * vx + asf_Nv[1] * vy + asf_Nv[2] * vz);
    float s = (vx - nd * asf_Nv[0]) * asf_Lv[0] + (vy - nd * asf_Nv[1]) * asf_Lv[1] + (vz - nd * asf_Nv[2]) * asf_Lv[2];
    if(s <= 0.55f) return 0;
    s = (s - 0.55f) / 0.45f; s *= s; s *= s;
    float k = s * asf_k;
    if(k < 0.02f) return 0;
    return cor_escala(asf_cor, k) & 0x00FFFFFF;
}
static inline void vt_de(vvt_t *o, const vv_t *p, const float *uv, float esc, uint32_t c) {
    o->x = p->x; o->y = p->y; o->z = p->z; o->u = uv[0] * esc; o->v = uv[1] * esc; o->c = c;
    o->o = asf_brilho_on ? brilho_asf(p) : 0;
}
/* SOMBRAS DE NUVENS: manchas grandes e suaves andando com o vento */
static uint8_t nuv_tab[32][32];
static void montar_nuv_tab(void) {
    for(int y = 0; y < 32; y++)
        for(int x = 0; x < 32; x++) {
            float v = 0;
            for(int o = 0; o < 3; o++) {
                int c = 8 >> o;
                int gx = x / c, gy = y / c, n = 32 / c;
                float fx = (x % c) / (float)c, fy = (y % c) / (float)c;
                fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
                float a = aleat(o * 977 + gy * 61 + gx), b = aleat(o * 977 + gy * 61 + (gx + 1) % n);
                float cc = aleat(o * 977 + ((gy + 1) % n) * 61 + gx), d = aleat(o * 977 + ((gy + 1) % n) * 61 + (gx + 1) % n);
                v += ((a * (1 - fx) + b * fx) * (1 - fy) + (cc * (1 - fx) + d * fx) * fy) * (0.55f / (o + 1));
            }
            float m = limitar((v - 0.52f) / 0.14f, 0, 1);         /* ~40% de cobertura, borda macia */
            nuv_tab[y][x] = (uint8_t)(m * 255);
        }
}
static float nuv_forca;
static float nuv_dx, nuv_dz;
static inline float nuv_amostra(float x, float z) {
    float u = (x + nuv_dx) * (1.0f / 45.0f), v = (z + nuv_dz) * (1.0f / 45.0f);
    int iu = (int)floorf(u), iv = (int)floorf(v);
    float fu = u - iu, fv = v - iv;
    int a = iu & 31, b = (iu + 1) & 31, c = iv & 31, d = (iv + 1) & 31;
    return ((nuv_tab[c][a] * (1 - fu) + nuv_tab[c][b] * fu) * (1 - fv) + (nuv_tab[d][a] * (1 - fu) + nuv_tab[d][b] * fu) * fv) * (1.0f / 255.0f);
}
static void nuv_quadro(void) {
    float sol = limitar(luz_dir / 0.5f, 0, 1) * (1.0f - 0.9f * nuvens) * (1.0f - chuva);
    nuv_forca = 0.30f * sol;
    nuv_dx = t_clima * 7.0f; nuv_dz = t_clima * 3.0f;
}
static inline float fator_nuvem(float x, float z) {
    if(nuv_forca < 0.01f) return 1.0f;
    return 1.0f - nuv_forca * nuv_amostra(x, z);
}

static void flush_strip(int n) {
    for(int i = 0; i < n; i++) enviar(&buf_strip[i], i == n - 1);
}

/* As cores das faixas da pista só dependem de (faixa, par/ímpar, boxes) e do
 * clima do quadro: tabela calculada UMA vez por quadro, em vez de misturar e
 * escalar cores para cada pedaço de pista (eram ~1000 cálculos por quadro). */
static uint32_t tab_faixa[NPTS][2][2];
static void cores_pista_quadro(void) {
    fatia_t fb; memset(&fb, 0, sizeof(fb));
    for(int faixa = 1; faixa < NPTS - 2; faixa++)
        for(int par = 0; par < 2; par++)
            for(int box = 0; box < 2; box++) {
                fb.box = box;
                tab_faixa[faixa][par][box] = cor_faixa(faixa, &fb, par);
            }
}
static inline uint32_t cor_faixa_rapida(int faixa, const fatia_t *f, int k) {
    return tab_faixa[faixa][k & 1][f->box ? 1 : 0];
}

static inline uint32_t cor_fx(int pb, uint32_t cb, int faixa, const fatia_t *f, int k) {
    return pb ? cb : cor_faixa_rapida(faixa, f, k);
}
/* BARRANCO: onde o terreno real é mais baixo que a borda da faixa de grama
 * (pista em aterro), um talude de grama desce da borda até o terreno */
static void desenhar_saias(void) {
    uint32_t cs = cor_escala(0xFF457A32, brilho * (luz_base + luz_dir * 0.45f) * 1.1f);
    uint32_t cb = cor_escala(cs, 0.72f);
    int nj = nvis < 70 ? nvis : 70;
    for(int j = 0; j + 1 < nj; j++) {
        int ka = vis[j].k, kb = vis[j + 1].k;
        if(((ka + 1) % NF) != kb) continue;
        for(int l = 0; l < 2; l++) {
            float ga = borda_y[ka][l] - ter_saia[ka][l], gb = borda_y[kb][l] - ter_saia[kb][l];
            if(ga < 0.25f && gb < 0.25f) continue;
            if(F[ka].box && !l) continue;
            float lat0 = l ? 25.9f : -25.9f, lat1 = l ? 26.8f + (ga > gb ? ga : gb) * 0.6f : -(26.8f + (ga > gb ? ga : gb) * 0.6f);
            const fatia_t *A = &F[ka], *B = &F[kb];
            vv_t q[4];
            para_vista(A->x + A->rx * lat0, borda_y[ka][l], A->z + A->rz * lat0, cs, &q[0]);
            para_vista(B->x + B->rx * lat0, borda_y[kb][l], B->z + B->rz * lat0, cs, &q[1]);
            para_vista(B->x + B->rx * lat1, ter_saia[kb][l] - 0.3f, B->z + B->rz * lat1, cb, &q[2]);
            para_vista(A->x + A->rx * lat1, ter_saia[ka][l] - 0.3f, A->z + A->rz * lat1, cb, &q[3]);
            poligono(q, 4);
        }
    }
}
/* RELEVO DE VERDADE NO ASFALTO (bump mapping do PowerVR — o chip calcula a
 * luz em cada texel a partir de um mapa de normais): rachaduras, remendos e
 * pedrinhas ganham sombra e brilho conforme a posição do sol. Uma 2ª passada
 * só nas fatias perto (~100 m), que MULTIPLICA o asfalto já desenhado. */
static pvr_poly_hdr_t hdr_relevo;
static int relevo_pronto;
static void desenhar_telao(void) {
    if(!telao_pronto || dist2_cam(telao_q[0]) > 320.0f * 320.0f) return;
    static const float U[4] = { 0, 1, 1, 0 }, V[4] = { 1, 1, 0, 0 };
    vvt_t t[4]; int fora = 0;
    uint32_t c = 0xFF000000 | (uint32_t)(limitar(0.85f + 0.15f * brilho, 0, 1) * 255) * 0x010101u;
    for(int v = 0; v < 4; v++) {
        vv_t q;
        para_vista(telao_q[v][0], telao_q[v][1], telao_q[v][2], 0, &q);
        t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = V[v]; t[v].c = c; t[v].o = 0;
        fora |= q.z > -PERTO;
    }
    if(fora) return;
    pvr_prim(&hdr_telao, sizeof(hdr_telao));
    poligono_tex(t, 4);
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
}
/* TEXTURA DE DETALHE: perto da câmera, uma textura fina (grão, fibra da
 * grama) multiplicada 2x sobre o asfalto e a grama — média 0,5 = não muda a
 * cor, só acrescenta grão. Os níveis pequenos do mipmap são cinza liso, então
 * o efeito some sozinho com a distância. */
static pvr_poly_hdr_t hdr_detalhe;
static uint16_t tex_px[256 * 256];
static int detalhe_pronto;
static void desenhar_detalhe_tr(void) {
#ifdef SEM_DETALHE
    return;
#endif
    if(!detalhe_pronto) {
        memcpy(tex_px, tex_bin + TEXB_DETALHE, 64 * 64 * 2);
        pvr_ptr_t t = tex_subir_mip(64);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 64, 64, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
        cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
        cxt.blend.src = PVR_BLEND_DESTCOLOR; cxt.blend.dst = PVR_BLEND_DESTCOLOR;    /* no destino, "DESTCOLOR" = cor da textura: 2 x cena x detalhe */
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL; cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&hdr_detalhe, &cxt);
        detalhe_pronto = 1;
    }
    pvr_prim(&hdr_detalhe, sizeof(hdr_detalhe));
    int nj = nvis < 22 ? nvis : 22;
    static const uint8_t FX[3][2] = { { 4, 5 }, { 1, 2 }, { 7, 8 } };      /* asfalto, grama esq., grama dir. */
    static const float ESC[3] = { 1.2f, 2.0f, 2.0f };
    for(int j = 0; j + 1 < nj; j++) {
        vis_t *a = &vis[j], *b = &vis[j + 1];
        for(int f = 0; f < 3; f++) {
            int i0 = FX[f][0], i1 = FX[f][1];
            const vv_t *P4[4] = { &a->p[i0], &a->p[i1], &b->p[i1], &b->p[i0] };
            const float *U4[4] = { a->uv[i0], a->uv[i1], b->uv[i1], b->uv[i0] };
            vvt_t t[4]; int fora = 0;
            for(int v = 0; v < 4; v++) {
                fora |= P4[v]->z > -PERTO;
                t[v].x = P4[v]->x; t[v].y = P4[v]->y; t[v].z = P4[v]->z;
                t[v].u = U4[v][0] * 4.0f * ESC[f]; t[v].v = U4[v][1] * 4.0f * ESC[f]; t[v].c = 0xFFFFFFFF; t[v].o = 0;
            }
            if(!fora) poligono_tex(t, 4);
        }
    }
}
static void desenhar_reflexo_tr(void) {
    if(!refl_ativo) return;
#ifdef DBG_REFL2
    {
        pvr_prim(&hdr_refl, sizeof(hdr_refl));
        pvr_vertex_t v;
        const float X[4] = { 0, 0, 256, 256 }, Y[4] = { 256, 0, 256, 0 }, U[4] = { 0, 0, 1, 1 }, V[4] = { 1, 0, 1, 0 };
        for(int k = 0; k < 4; k++) {
            v.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = X[k] * XS; v.y = Y[k] + 60; v.z = 50.0f; v.u = U[k]; v.v = V[k]; v.argb = 0xFFFFFFFF; v.oargb = 0;
            pvr_prim(&v, sizeof(v));
        }
        return;
    }
#endif
#ifdef DBG_REFL
    uint32_t c = 0xFFFFFFFF;
#else
    uint32_t c = ((uint32_t)(limitar(molhado, 0, 1) * 0.32f * 255) << 24) | 0xFFFFFF;
#endif
    pvr_prim(&hdr_refl, sizeof(hdr_refl));
    int nj = nvis < 60 ? nvis : 60;
    for(int j = 0; j + 1 < nj; j++) {
        vis_t *a = &vis[j], *b = &vis[j + 1];
        /* Fresnel: quase nada perto (olhando para baixo), forte de longe (rasante) */
        float fa = limitar((-a->p[4].z - 25.0f) / 35.0f, 0, 1), fb = limitar((-b->p[4].z - 25.0f) / 35.0f, 0, 1);
        if(fa <= 0 && fb <= 0) continue;
        const vv_t *P4[4] = { &a->p[3], &a->p[6], &b->p[6], &b->p[3] };
        const float FR[4] = { fa, fa, fb, fb };
        vvt_t t[4]; int fora = 0;
        for(int v = 0; v < 4; v++) {
            const vv_t *q = P4[v];
            if(q->z > -PERTO) { fora = 1; break; }
            /* ponto da pista no mundo, projetado pela CÂMERA ESPELHADA: num plano
             * de reflexo, é exatamente ali que a textura guarda o reflexo */
            float w[3];
            for(int e = 0; e < 3; e++) w[e] = cam_p[e] + q->x * cam_r[e] + q->y * cam_u[e] - q->z * cam_f[e] - refl_P[e];
            float xr = w[0] * refl_R[0] + w[1] * refl_R[1] + w[2] * refl_R[2];
            float yr = w[0] * refl_U[0] + w[1] * refl_U[1] + w[2] * refl_U[2];
            float zr = w[0] * refl_F[0] + w[1] * refl_F[1] + w[2] * refl_F[2];
            if(zr < 0.5f) { fora = 1; break; }
            t[v].x = q->x; t[v].y = q->y; t[v].z = q->z;
            t[v].u = (128.0f + refl_foco * xr / zr) / 256.0f;
            t[v].v = (refl_cy - refl_foco * yr / zr) / 256.0f;
            t[v].c = ((uint32_t)((c >> 24) * FR[v]) << 24) | (c & 0xFFFFFF); t[v].o = 0;
        }
        if(!fora) poligono_tex(t, 4);
    }
}
static void desenhar_relevo_tr(void) {
#ifdef SEM_RELEVO
    return;
#endif
    if(!relevo_pronto) {
        pvr_ptr_t t = pvr_mem_malloc(256 * 256 * 2);
        pvr_txr_load(tex_bin + TEXB_RELEVO_ASFALTO, t, 256 * 256 * 2);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_BUMP | PVR_TXRFMT_TWIDDLED, 256, 256, t, PVR_FILTER_BILINEAR);
        cxt.gen.specular = PVR_SPECULAR_ENABLE;
        cxt.txr.env = PVR_TXRENV_DECAL;
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
        /* o resultado do relevo sai no ALFA (a cor fica branca): cena x alfa */
        cxt.blend.src = PVR_BLEND_ZERO; cxt.blend.dst = PVR_BLEND_SRCALPHA;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL; cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
#ifdef DBG_RELEVO3
        cxt.blend.src = PVR_BLEND_ONE; cxt.blend.dst = PVR_BLEND_ZERO;
#endif
#ifdef DBG_RELEVO2
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
        cxt.blend.src = PVR_BLEND_DESTCOLOR; cxt.blend.dst = PVR_BLEND_ZERO;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL; cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
#endif
        pvr_poly_compile(&hdr_relevo, &cxt);
        relevo_pronto = 1;
    }
    float sol = limitar(luz_dir / 0.5f, 0, 1) * (1.0f - 0.8f * nuvens) * (1.0f - 0.7f * chuva);
    if(sol < 0.1f) return;
    /* luz no espaço da textura (u = x do mundo, v = z do mundo) */
    float Q = atan2f(LUZ[2], LUZ[0]); if(Q < 0) Q += 2 * PI_F;
    float T = 60.0f * PI_F / 180.0f;              /* luz "de cima" (plano ~ 0,93): só as reentrâncias escurecem */
    {   /* ZEBRAS e MURO de concreto: relevo com a luz no espaço da textura de cada um */
        static pvr_poly_hdr_t hz, hm; static int ok2;
        if(!ok2) {
            pvr_poly_cxt_t cxt;
            pvr_ptr_t tz = pvr_mem_malloc(64 * 64 * 2), tm = pvr_mem_malloc(128 * 128 * 2);
            pvr_txr_load(tex_bin + TEXB_RELEVO_ZEBRA, tz, 64 * 64 * 2);
            pvr_txr_load(tex_bin + TEXB_RELEVO_CONCRETO, tm, 128 * 128 * 2);
            for(int k2 = 0; k2 < 2; k2++) {
                pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_BUMP | PVR_TXRFMT_TWIDDLED, k2 ? 128 : 64, k2 ? 128 : 64,
                                 k2 ? tm : tz, PVR_FILTER_BILINEAR);
                cxt.gen.specular = PVR_SPECULAR_ENABLE; cxt.txr.env = PVR_TXRENV_DECAL;
                cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
                cxt.blend.src = PVR_BLEND_ZERO; cxt.blend.dst = PVR_BLEND_SRCALPHA;
                cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL; cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
                pvr_poly_compile(k2 ? &hm : &hz, &cxt);
            }
            ok2 = 1;
        }
        const fatia_t *fc = &F[vis[0].k];
        float Lr = LUZ[0] * fc->rx + LUZ[2] * fc->rz, Lf = LUZ[0] * fc->fx + LUZ[2] * fc->fz;
        /* zebra: u = lateral (+direita), v = ao longo (+frente) */
        uint32_t oz = pvr_pack_bump(0.6f * sol, 50.0f * PI_F / 180.0f, fmodf(atan2f(Lf, Lr) + 2 * PI_F, 2 * PI_F));
        pvr_prim(&hz, sizeof(hz));
        int nz = nvis < 26 ? nvis : 26;
        for(int j = 0; j + 1 < nz; j++) {
            vis_t *a = &vis[j], *b = &vis[j + 1];
            if(!F[a->k].zebra) continue;
            for(int fx = 2; fx <= 6; fx += 4) {
                const vv_t *P4[4] = { &a->p[fx], &a->p[fx + 1], &b->p[fx + 1], &b->p[fx] };
                static const float U[4] = { 0, 1, 1, 0 }, V[4] = { 0, 0, 1, 1 };
                vvt_t t[4]; int fora = 0;
                for(int v = 0; v < 4; v++) {
                    fora |= P4[v]->z > -PERTO;
                    t[v].x = P4[v]->x; t[v].y = P4[v]->y; t[v].z = P4[v]->z; t[v].u = U[v]; t[v].v = V[v]; t[v].c = 0xFFFFFFFF; t[v].o = oz;
                }
                if(!fora) poligono_tex(t, 4);
            }
        }
        /* muro: normal para a pista (-lado * direita); u = ao longo (sinal pelo lado), v = para baixo */
        if(n_muro) {
            pvr_prim(&hm, sizeof(hm));
            for(int lado = -1; lado <= 1; lado += 2) {
                float Ln = -lado * Lr, Lu = lado < 0 ? Lf : -Lf, Lv = -LUZ[1];
                if(Ln < 0.05f) continue;                       /* sol por trás do muro: sem relevo */
                float T = asinf(limitar(Ln, 0, 1)); if(T < 0.35f) T = 0.35f;
                uint32_t om = pvr_pack_bump(0.5f * sol * limitar(Ln * 1.5f, 0, 1), T, fmodf(atan2f(Lv, Lu) + 2 * PI_F, 2 * PI_F));
                for(int i = 0; i < n_muro; i++) {
                    if(muro_lado[i] != lado) continue;
                    if(-muro_q[i][0].z > 70.0f) continue;
                    vvt_t t[4];
                    for(int v = 0; v < 4; v++) { t[v] = muro_q[i][v]; t[v].c = 0xFFFFFFFF; t[v].o = om; t[v].u *= 2.0f; }
                    poligono_tex(t, 4);
                }
            }
        }
    }
    pvr_prim(&hdr_relevo, sizeof(hdr_relevo));
    int nj = nvis < 26 ? nvis : 26;
    for(int j = 0; j + 1 < nj; j++) {
        vis_t *a = &vis[j], *b = &vis[j + 1];
        const vv_t *P4[4] = { &a->p[4], &a->p[5], &b->p[5], &b->p[4] };
        const float *U4[4] = { a->uv[4], a->uv[5], b->uv[5], b->uv[4] };
        int fora = 0;
        for(int v = 0; v < 4; v++) fora |= P4[v]->z > -PERTO;
        if(fora) continue;
        float dist = -a->p[4].z;
        float h = 0.55f * sol * limitar((110.0f - dist) / 50.0f, 0, 1);   /* some aos poucos de longe */
        if(h < 0.02f) continue;
#ifdef DBG_RELEVO
        h = 1.0f; T = 15.0f * PI_F / 180.0f;
#endif
        uint32_t o = pvr_pack_bump(h, T, Q);
        vvt_t t[4];
        for(int v = 0; v < 4; v++) {
            t[v].x = P4[v]->x; t[v].y = P4[v]->y; t[v].z = P4[v]->z;
            t[v].u = U4[v][0] * 0.25f; t[v].v = U4[v][1] * 0.25f; t[v].c = 0xFFFFFFFF; t[v].o = o;
#ifdef DBG_RELEVO2
            t[v].c = 0xFFFF6060;
#endif
        }
        poligono_tex(t, 4);
    }
}
/* ZEBRAS EM RELEVO: dentes serrilhados na borda de fora (só perto) */
static void desenhar_zebra_dentes(void) {
    float up[3] = { cam_r[1], cam_u[1], -cam_f[1] };
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
    int nj = nvis < 30 ? nvis : 30;
    for(int j = 0; j + 1 < nj; j++) {
        vis_t *a = &vis[j], *b = &vis[j + 1];
        if(!F[a->k].zebra) continue;
        uint32_t c = cor_escala((a->k & 1) ? 0xFFC8322D : 0xFFEDEDED, brilho * (luz_base + luz_dir * 0.7f) * 1.2f);
        uint32_t ce = cor_escala(c, 0.62f);
        for(int lado = 0; lado < 2; lado++) {
            int ix = lado ? 7 : 2;
            const vv_t *A = &a->p[ix], *B = &b->p[ix];
            if(A->z > -PERTO || B->z > -PERTO) continue;
            for(int d = 0; d < 4; d++) {
                float t0 = d / 4.0f, t1 = (d + 1) / 4.0f, tm = (d + 0.5f) / 4.0f;
                vv_t q[3];
                float h = 0.07f;
                q[0].x = A->x + (B->x - A->x) * t0; q[0].y = A->y + (B->y - A->y) * t0; q[0].z = A->z + (B->z - A->z) * t0;
                q[1].x = A->x + (B->x - A->x) * t1; q[1].y = A->y + (B->y - A->y) * t1; q[1].z = A->z + (B->z - A->z) * t1;
                q[2].x = A->x + (B->x - A->x) * tm + up[0] * h; q[2].y = A->y + (B->y - A->y) * tm + up[1] * h; q[2].z = A->z + (B->z - A->z) * tm + up[2] * h;
                q[0].c = q[1].c = ce; q[2].c = c;
                poligono(q, 3);
            }
        }
    }
}
/* ÁGUA ESCORRENDO pelas zebras na chuva (faixas claras descendo) */
static void desenhar_agua_zebra_tr(void) {
    if(molhado < 0.30f) return;
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    uint32_t cb = cor_mistura(cor_ceu, 0xFFFFFFFF, 0.5f) & 0xFFFFFF;
    int nj = nvis < 32 ? nvis : 32;
    for(int j = 0; j + 1 < nj; j++) {
        vis_t *a = &vis[j], *b = &vis[j + 1];
        if(!F[a->k].zebra) continue;
        float sa = 0.5f + 0.5f * fsin(t_clima * 4.0f - a->k * 0.9f), sb = 0.5f + 0.5f * fsin(t_clima * 4.0f - b->k * 0.9f);
        uint32_t ca = ((uint32_t)(molhado * 110 * sa) << 24) | cb, cbb = ((uint32_t)(molhado * 110 * sb) << 24) | cb;
        for(int f = 2; f <= 6; f += 4) {
            vv_t q[4] = { a->p[f], a->p[f + 1], b->p[f + 1], b->p[f] };
            int fora = 0;
            for(int v = 0; v < 4; v++) fora |= q[v].z > -PERTO;
            if(fora) continue;
            q[0].c = q[1].c = ca; q[2].c = q[3].c = cbb;
            poligono(q, 4);
        }
    }
}
static void desenhar_pista(void) {
    cores_pista_quadro();
    nuv_quadro();
    static int par;
    par ^= 1;
#ifdef SEM_NUV_PISTA
    par = nvis;
#endif
    float tts = limitar(tarde, 0, 1); tts = tts * tts * (3 - 2 * tts);
    int it = (int)(tts * 256);
    float sol_s = limitar(luz_dir / 0.5f, 0, 1) * (1.0f - 0.9f * nuvens) * (1.0f - chuva) * 0.55f;
    int fs = (int)(sol_s * 256);
    for(int j = par; j < nvis; j += 2) {           /* metade por quadro: as nuvens andam devagar */
        int k = vis[j].k;
        const fatia_t *f = &F[k];
        nuv_f[k] = (uint8_t)(fator_nuvem(f->x, f->z) * 255);
        for(int i = 1; i < NPTS - 1; i++) {        /* sombra das arquibancadas: dia <-> entardecer */
            int m = somb_d[k][i] + (((somb_t[k][i] - somb_d[k][i]) * it) >> 8);
            somb[k][i] = (uint8_t)(255 - (((255 - m) * fs) >> 8));
        }
    }
    {   /* brilho do sol: luz e "chão" levados para o espaço da câmera */
        float tt = limitar(tarde, 0, 1);
        float sol = (LUZ[1] > 0.02f) * (1.0f - 0.85f * nuvens) * (1.0f - 0.5f * chuva);
        asf_k = (0.60f * tt + 0.55f * molhado) * sol;        /* meio-dia seco: desligado (sem custo) */
        asf_cor = cor_mistura(0xFFFFF4E0, 0xFFFFB060, tt);
        asf_Lv[0] = cam_r[0] * LUZ[0] + cam_r[1] * LUZ[1] + cam_r[2] * LUZ[2];
        asf_Lv[1] = cam_u[0] * LUZ[0] + cam_u[1] * LUZ[1] + cam_u[2] * LUZ[2];
        asf_Lv[2] = -(cam_f[0] * LUZ[0] + cam_f[1] * LUZ[1] + cam_f[2] * LUZ[2]);
        asf_Nv[0] = cam_r[1]; asf_Nv[1] = cam_u[1]; asf_Nv[2] = -cam_f[1];
    }
    uint32_t cor_brita = cor_escala(cor_mistura(0xFFD2C4A6, 0xFF8E8676, molhado * 0.6f), brilho);
    /* faixas 0 e 8 (grama de longe) não são mais desenhadas: o TERRENO faz esse papel */
    for(int faixa = 1; faixa < NPTS - 2; faixa++) {
        int zebra = (faixa == 2 || faixa == 6);
        int nb = 0;
        /* asfalto e grama perto da pista com TEXTURA (granulado), modulada pela cor */
        int tx = faixa == 4 ? 1 : (faixa == 1 || faixa == 7) ? 2 : 0;
        int bit_brita = faixa == 1 ? 1 : 2;
        for(int pb = 0; pb < (tx == 2 ? 2 : 1); pb++) {   /* grama: 2 passes (gramado cortado / brita) */
        float esc = tx == 2 ? (pb ? 4.0f : 1.0f) : 0.25f;
        if(tx) pvr_prim(tx == 1 ? &hdr_asfalto : pb ? &hdr_brita : &hdr_corte, sizeof(pvr_poly_hdr_t));
        asf_brilho_on = tx == 1 && asf_k > 0.02f;
        if(zebra) pvr_prim(&hdr_zebra, sizeof(hdr_zebra));
        nb = 0;
        for(int j = 0; j + 1 < nvis; j++) {
            vis_t *a = &vis[j], *b = &vis[j + 1];
            const fatia_t *fa = &F[a->k];
            if(tx == 2 && ((fa->brita & bit_brita) != 0) != pb) {
                if(nb) { flush_strip_t(nb); nb = 0; }
                continue;
            }
            const float (*UVA)[2] = tx == 2 ? a->st : a->uv, (*UVB)[2] = tx == 2 ? b->st : b->uv;
            if(a->oc[faixa] & a->oc[faixa + 1] & b->oc[faixa] & b->oc[faixa + 1]) {
                if(nb) { if(tx) flush_strip_t(nb); else flush_strip(nb); nb = 0; }   /* todo fora da tela */
                continue;
            }
            if(zebra) {
                /* zebra: cor vermelho/branco (vértice) x textura de tinta gasta com relevo */
                uint32_t c = cor_escala(fa->zebra ? ((a->k & 1) ? 0xFFC8322D : 0xFFEDEDED) : 0xFF6C6F74, brilho);
                vvt_t q[4];
                const vv_t *pp[4] = { &a->p[faixa], &a->p[faixa + 1], &b->p[faixa + 1], &b->p[faixa] };
                const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 0, 0, 1, 1 };
                for(int i = 0; i < 4; i++) { q[i].x = pp[i]->x; q[i].y = pp[i]->y; q[i].z = pp[i]->z; q[i].u = us[i]; q[i].v = vs[i]; q[i].c = c; }
                poligono_tex(q, 4);
                continue;
            }
            int ok = a->p[faixa].z <= -PERTO && a->p[faixa + 1].z <= -PERTO &&
                     b->p[faixa].z <= -PERTO && b->p[faixa + 1].z <= -PERTO;
            if(ok && tx) {
                if(nb == 0) {
                    uint32_t c = cor_fx(pb, cor_brita, faixa, fa, a->k);
                    vt_de(&buf_strip_t[nb++], &a->p[faixa], UVA[faixa], esc, cao(c, a->k, faixa));
                    vt_de(&buf_strip_t[nb++], &a->p[faixa + 1], UVA[faixa + 1], esc, cao(c, a->k, faixa + 1));
                }
                uint32_t c = cor_fx(pb, cor_brita, faixa, &F[b->k], b->k);
                vt_de(&buf_strip_t[nb++], &b->p[faixa], UVB[faixa], esc, cao(c, b->k, faixa));
                vt_de(&buf_strip_t[nb++], &b->p[faixa + 1], UVB[faixa + 1], esc, cao(c, b->k, faixa + 1));
            }
            else if(ok) {
                /* caminho rápido: strip contínuo (2 vértices por fatia) */
                if(nb == 0) {
                    uint32_t c = cor_fx(pb, cor_brita, faixa, fa, a->k);
                    buf_strip[nb] = a->p[faixa];     buf_strip[nb++].c = cao(c, a->k, faixa);
                    buf_strip[nb] = a->p[faixa + 1]; buf_strip[nb++].c = cao(c, a->k, faixa + 1);
                }
                uint32_t c = cor_fx(pb, cor_brita, faixa, &F[b->k], b->k);
                buf_strip[nb] = b->p[faixa];     buf_strip[nb++].c = cao(c, b->k, faixa);
                buf_strip[nb] = b->p[faixa + 1]; buf_strip[nb++].c = cao(c, b->k, faixa + 1);
            }
            else {
                /* trecho que cruza o plano da câmera: recorta como polígono */
                if(nb) { if(tx) flush_strip_t(nb); else flush_strip(nb); nb = 0; }
                uint32_t c = cor_fx(pb, cor_brita, faixa, fa, a->k);
                if(tx) {
                    vvt_t q[4];
                    vt_de(&q[0], &a->p[faixa], UVA[faixa], esc, cao(c, a->k, faixa));         vt_de(&q[1], &a->p[faixa + 1], UVA[faixa + 1], esc, cao(c, a->k, faixa + 1));
                    vt_de(&q[2], &b->p[faixa + 1], UVB[faixa + 1], esc, cao(c, b->k, faixa + 1)); vt_de(&q[3], &b->p[faixa], UVB[faixa], esc, cao(c, b->k, faixa));
                    poligono_tex(q, 4);
                } else {
                    vv_t q[4] = { a->p[faixa], a->p[faixa + 1], b->p[faixa + 1], b->p[faixa] };
                    for(int i = 0; i < 4; i++) q[i].c = c;
                    poligono(q, 4);
                }
            }
        }
        if(nb) { if(tx) flush_strip_t(nb); else flush_strip(nb); }
        nb = 0;
        asf_brilho_on = 0;
        if(0 && tx == 1) {          /* (removida a pedido do usuário) */
            /* LINHA DE BORRACHA: faixa mais escura na trajetória ideal (só perto) */
            uint32_t cb = cor_escala(tab_faixa[4][0][0], 0.86f + 0.08f * molhado);
            int nmax = nvis < 60 ? nvis : 60;
            vvt_t pa[2], pb[2];
            int tem = 0;
            for(int j = 0; j < nmax; j++) {
                const fatia_t *f = &F[vis[j].k];
                float lat = borracha_lat[vis[j].k];
                for(int e = 0; e < 2; e++) {
                    float la = lat + (e ? 0.85f : -0.85f);
                    float wx = f->x + f->rx * la, wz = f->z + f->rz * la;
                    vv_t q; para_vista(wx, f->y + 0.02f, wz, 0, &q);
                    pb[e].x = q.x; pb[e].y = q.y; pb[e].z = q.z; pb[e].c = cb; pb[e].o = 0;
                    pb[e].u = wx * 0.0625f; pb[e].v = wz * 0.0625f;
                }
                if(tem) {
                    vvt_t qd[4] = { pa[0], pa[1], pb[1], pb[0] };
                    poligono_tex(qd, 4);
                }
                pa[0] = pb[0]; pa[1] = pb[1]; tem = 1;
            }
        }
        if(tx || zebra) pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
        }
    }
}

/* Muros de pneus na beira da área de escape (vermelho/branco nas curvas,
 * concreto nas retas). No trecho dos boxes, o lado esquerdo vira o muro
 * do pit, colado na pista. Só nas 60 fatias mais próximas. */
/* MUROS: nas curvas, pilhas de pneus pintados (vermelho/branco); nas retas,
 * muro de concreto com um painel de publicidade a cada 5 trechos; o muro do
 * pit é concreto branco. Cada pedaço tem a face da frente com textura e,
 * de perto, o TOPO (0,6 m de espessura). Desenhados em 3 lotes (um
 * cabeçalho de textura por lote). */
static vvt_t cerca_q[120][4];
static int n_cerca;
static void desenhar_barreiras(void) {
    int n = nvis < 60 ? nvis : 60;
    n_cerca = 0;
    static vvt_t lote[3][120][4];
    static int8_t lote_lado[3][120];
    static vv_t topo[120][4];
    int nl[3] = { 0, 0, 0 }, nt = 0;
    for(int lado = -1; lado <= 1; lado += 2) {
        vv_t ant_b, ant_t, ant_tt;
        float ant_u = 0;
        int tem = 0;
        for(int j = 0; j < n; j++) {
            const fatia_t *f = &F[vis[j].k];
            int k = vis[j].k;
            float lat = lado * 24.5f, alt = 1.2f;
            int pit = lado < 0 && f->box;
            if(pit) { lat = -9.8f; alt = 1.1f; }
            vv_t b, t, tt;
            float y0 = faixa_y(k, lat);
            float xm = f->x + f->rx * lat, zm = f->z + f->rz * lat;
            float lo = lado * 0.6f;                               /* espessura para fora */
            para_vista(xm, y0 - 0.3f, zm, 0, &b);
            para_vista(xm, y0 + alt, zm, 0, &t);
            para_vista(xm + f->rx * lo, y0 + alt, zm + f->rz * lo, 0, &tt);
            float u = k * (float)PASSO;
            if(tem && !(fora_tela(&ant_b) & fora_tela(&b) & fora_tela(&t) & fora_tela(&ant_t))) {
                int tipo;                                          /* 0 pneus, 1 concreto, 2 publicidade */
                uint32_t c;
                if(pit) { tipo = 1; c = 0xFFF0F0EC; }
                else if(f->zebra) { tipo = 0; c = ((k >> 1) & 1) ? 0xFFD03028 : 0xFFF0F0F0; }
                else if((k / 2) % 5 == 0) { tipo = 2; c = 0xFFFFFFFF; }
                else { tipo = 1; c = 0xFFB4B8BE; }
                c = cor_escala(c, brilho);
                if(nl[tipo] < 120) {
                    lote_lado[tipo][nl[tipo]] = (int8_t)lado;
                    vvt_t *q = lote[tipo][nl[tipo]++];
                    const vv_t *pp[4] = { &ant_b, &b, &t, &ant_t };
                    float uu0, uu1, vv0 = 1, vv1 = 0;
                    if(tipo != 2) { uu0 = ant_u / 4.0f; uu1 = u / 4.0f; }          /* concreto: placas de 4 m */
                    else {
                        /* publicidade: 8 marcas fictícias (2 colunas x 4 linhas na textura) */
                        int d = (int)((h32((uint32_t)k * 2654435761u) >> 8) % 7), col = d & 1, lin = d >> 1;
                        if(d == 6) {                       /* 1 em cada 7: "FEITO PARA DREAMCAST" */
                            uu0 = 0.004f; uu1 = 0.996f; vv0 = 0.746f; vv1 = 0.502f;
                        } else {
                            d = (int)(h32((uint32_t)k * 977u) & 7); col = d & 1; lin = d >> 1;
                            uu0 = col * 0.5f + 0.004f; uu1 = col * 0.5f + 0.496f;
                            vv0 = lin * 0.125f + 0.123f; vv1 = lin * 0.125f + 0.002f;
                        }
                        if(lado > 0) { float tmp = uu0; uu0 = uu1; uu1 = tmp; }   /* lido certo dos dois lados */
                    }
                    if(tipo != 2 && lado > 0) { float tmp = uu0; uu0 = -uu0; uu1 = -uu1; (void)tmp; }
                    const float us[4] = { uu0, uu1, uu1, uu0 }, vs[4] = { vv0, vv0, vv1, vv1 };
                    for(int v = 0; v < 4; v++) {
                        q[v].x = pp[v]->x; q[v].y = pp[v]->y; q[v].z = pp[v]->z;
                        q[v].u = us[v]; q[v].v = vs[v]; q[v].c = v < 2 ? cor_escala(c, 0.70f) : c;   /* pé do muro mais escuro */
                    }
                }
                if(j < 28 && n_cerca < 120) {                  /* alambrado de 2,5 m sobre o muro */
                    vv_t c1, c2;
                    float lm = lado * 0.3f;
                    const fatia_t *fa2 = &F[vis[j - 1].k];
                    float la = (pit ? -9.8f : lado * 24.5f);
                    para_vista(fa2->x + fa2->rx * (la + lm), faixa_y(vis[j - 1].k, la) + alt + 2.5f, fa2->z + fa2->rz * (la + lm), 0, &c1);
                    para_vista(xm + f->rx * lm, y0 + alt + 2.5f, zm + f->rz * lm, 0, &c2);
                    uint32_t cc = 0xFFFFFFFF;
                    cc = (cor_escala(cc, brilho) & 0xFFFFFF) | 0xFF000000;
                    vvt_t *q = cerca_q[n_cerca++];
                    const vv_t *pp[4] = { &ant_t, &t, &c2, &c1 };
                    const float us[4] = { 0, 1, 1, 0 }, vs[4] = { 1, 1, 0, 0 };
                    for(int v = 0; v < 4; v++) {
                        q[v].x = pp[v]->x; q[v].y = pp[v]->y; q[v].z = pp[v]->z; q[v].u = us[v]; q[v].v = vs[v]; q[v].c = cc;
                    }
                }
                if(j < 28 && nt < 120) {                       /* topo, só perto */
                    uint32_t ct = cor_escala(f->zebra && !pit ? 0xFF2A2C30 : 0xFFA0A4AA, brilho);
                    vv_t *q = topo[nt++];
                    q[0] = ant_t; q[1] = t; q[2] = tt; q[3] = ant_tt;
                    for(int v = 0; v < 4; v++) q[v].c = ct;
                }
            }
            ant_b = b; ant_t = t; ant_tt = tt; ant_u = u; tem = 1;
        }
    }
    n_muro = nl[1] < 120 ? nl[1] : 120;              /* guarda o muro de concreto para o relevo */
    memcpy(muro_q, lote[1], sizeof(vvt_t) * 4 * n_muro);
    memcpy(muro_lado, lote_lado[1], n_muro);
    static pvr_poly_hdr_t *H[3] = { &hdr_concreto, &hdr_concreto, &hdr_placas_muro };
    for(int tp = 0; tp < 3; tp++) {
        if(!nl[tp]) continue;
        pvr_prim(H[tp], sizeof(pvr_poly_hdr_t));
        for(int i = 0; i < nl[tp]; i++) poligono_tex(lote[tp][i], 4);
    }
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
    for(int i = 0; i < nt; i++) poligono(topo[i], 4);
}

/* Linha de largada/chegada: xadrez sobre o asfalto em s = 0. */
static void desenhar_largada(void) {
    for(int i = 0; i < 14; i++)
        for(int j = 0; j < 2; j++) {
            float x0 = -MEIA_LARG + i * 1.0f, s0 = j * 1.0f;
            float p[4][3];
            ponto_pista(s0,     x0,     p[0], 0, 0);
            ponto_pista(s0,     x0 + 1, p[1], 0, 0);
            ponto_pista(s0 + 1, x0 + 1, p[2], 0, 0);
            ponto_pista(s0 + 1, x0,     p[3], 0, 0);
            uint32_t c = ((i + j) & 1) ? 0xFF202020 : 0xFFF0F0F0;
            vv_t q[4];
            for(int v = 0; v < 4; v++) para_vista(p[v][0], p[v][1] + 0.03f, p[v][2], c, &q[v]);
            poligono(q, 4);
        }
}

/* ======================================================================== */
/* OBJETOS LOW POLY: faces planas com sombreamento chapado (flat shading)    */
/* ======================================================================== */

typedef struct { int n; float v[8][3]; float nrm[3]; uint8_t cor, atras, roda, modo, miudo, tex, suave;
                 float uv[4][2];
                 float vn[4][3]; } face_t;   /* suave: luz por vértice (normais vn) — superfícies curvas */   /* tex: 0 = cor lisa; 1 = tartan; 2 = números (decalques) */   /* atras: inteira atrás do piloto */

static const int8_t CUBO[6][4][3] = {
    {{ 1,-1,-1},{ 1, 1,-1},{ 1, 1, 1},{ 1,-1, 1}},
    {{-1,-1, 1},{-1, 1, 1},{-1, 1,-1},{-1,-1,-1}},
    {{-1, 1,-1},{-1, 1, 1},{ 1, 1, 1},{ 1, 1,-1}},
    {{-1,-1,-1},{ 1,-1,-1},{ 1,-1, 1},{-1,-1, 1}},
    {{-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}},
    {{ 1,-1,-1},{-1,-1,-1},{-1, 1,-1},{ 1, 1,-1}},
};
static const int8_t NCUBO[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

static inline float sombra_luz(const float n[3]) {
    float d = n[0] * LUZ[0] + n[1] * LUZ[1] + n[2] * LUZ[2];
    return (luz_base + luz_dir * (d > 0 ? d : 0)) * brilho;
}

static uint8_t pular_atras;     /* 1 = cockpit: nem processa o que fica atrás da câmera */

/* Desenha uma lista de faces com base (P; R, U, Fr) = posição e eixos locais. */
/* giro atual de cada roda (cos, sen) e qual modo ESCONDER (1 nítido / 2 borrado) */
static float giro_c[6] = { 1, 1, 1, 1, 1, 1 }, giro_s[6];
static float cab_dx, cab_dz;      /* deslocamento da cabeça (curvas/freadas) */
static float giro_cy[6], giro_cz[6];
static uint8_t modo_oculto = 2;

static int pint_on;             /* pintura texturizada (só carros grandes na tela) */
static int spec_on;            /* 1: carroceria curva com brilho; 2: também as faces retas (jogador) */
static float spec_forca;
static uint8_t pular_miudo = 2;   /* bits de face_t.miudo a pular (1 = nível médio, 2 = completo) */
/* texturas dos decalques dos carros e cabeçalhos de polígono */
#define N_TEX_CARRO 8                      /* = N_CARROS (definido mais abaixo) */
static pvr_poly_hdr_t hdr_faixa[N_TEX_CARRO], hdr_num[N_TEX_CARRO], hdr_capacete[N_TEX_CARRO];
static int tex_carro_ativo;
static const uint8_t marca_carro[8] = { 2, 0, 3, 5, 6, 1, 7, 4 };   /* patrocinador fictício de cada carro */                 /* 0 = sem decalques; senão índice do carro + 1 */
enum { C_PRINC, C_SEC, C_PNEU, C_CAPACETE, C_CARBONO, C_ARO,
       C_VISOR, C_HALO, C_LUZ, C_FAIXA_PNEU, C_ESCURO, C_SULCO,
       /* blocos da pintura */
       C_TERC, C_NARIZ, C_MOTOR, C_ASA, C_PLACA, C_PAINEL,
       /* rodas girando */
       C_MARCA, C_BORRAO_PNEU, C_BORRAO_ARO, C_DISCO, N_CORES };
static inline int e_pintura(int cor) {
    return cor == C_PRINC || cor == C_SEC || cor == C_TERC || cor == C_NARIZ || cor == C_MOTOR ||
           cor == C_ASA || cor == C_PLACA || cor == C_CAPACETE;
}
/* brilho (Blinn) num vértice, no espaço do carro: (N·H)^16 */
static inline uint32_t brilho_v(const float *nv, const float *pv, const float *Ll, const float *Cl) {
    float vx = Cl[0] - pv[0], vy = Cl[1] - pv[1], vz = Cl[2] - pv[2];
    float iv = __frsqrt(vx * vx + vy * vy + vz * vz);
    float hx = Ll[0] + vx * iv, hy = Ll[1] + vy * iv, hz = Ll[2] + vz * iv;
    float d = (nv[0] * hx + nv[1] * hy + nv[2] * hz) * __frsqrt(hx * hx + hy * hy + hz * hz + 1e-6f);
    if(d <= 0.40f) return 0;
    d *= d; d *= d; d *= d;                          /* (N·H)^8: reflexo mais largo */
    int a = (int)(d * spec_forca * 255.0f);
    if(a > 255) a = 255;
    return (uint32_t)(a << 16 | a << 8 | a);
}
#define MAX_FACES_CARRO 3000
static face_t carro_faces[MAX_FACES_CARRO];
/* ======================================================================== */
/* MALHA COMPILADA DOS CARROS                                               */
/* No boot: (1) cada canto de face ganha o índice de um VÉRTICE ÚNICO       */
/* (posição + grupo de matriz: carroceria, roda 1..4, cabeça); (2) listas   */
/* prontas de faces por nível de detalhe. No desenho, cada vértice único é  */
/* transformado e projetado UMA vez por carro (cache com carimbo), e o laço */
/* só percorre as faces daquele nível.                                      */
/* ======================================================================== */
#define MAX_VU 9000
static uint16_t face_vid[MAX_FACES_CARRO][8];
/* vértices únicos: posição local (x,y,z,1) e resultado do quadro
 * (x,y,z na câmera, sx,sy na tela, 1/z), em blocos contíguos para o laço em assembly */
static float vl[MAX_VU][4] __attribute__((aligned(32)));
static float vo[MAX_VU][8] __attribute__((aligned(32)));
static uint16_t lista_v[6][MAX_VU];      /* [5] = peças em geometria do carro híbrido */      /* vértices da CARROCERIA (grupo 0) de cada nível */
static int n_lista_v[6];

/* LAÇO EM ASSEMBLY (SH4): para cada vértice da lista, ftrv com a matriz já
 * carregada na XMTRX, 1/z com fsrra e projeção na tela; grava x,y,z,sx,sy,iz.
 * k4 = { ESPELHAR*FOCO, -FOCO, CX, CY }. */
static void vu_lote(const uint16_t *ids, int n, const float *k4) {
    if(n <= 0) return;
    const float *pl = &vl[0][0];
    float *po = &vo[0][0];
    __asm__ __volatile__(
        "fmov.s  @%[k]+,fr12\n\t"
        "fmov.s  @%[k]+,fr13\n\t"
        "fmov.s  @%[k]+,fr14\n\t"
        "fmov.s  @%[k]+,fr15\n"
        "1:\n\t"
        "mov.w   @%[ids]+,r0\n\t"
        "extu.w  r0,r0\n\t"
        "shll2   r0\n\t"
        "shll2   r0\n\t"           /* id*16: linha de vl */
        "mov     %[pl],r1\n\t"
        "add     r0,r1\n\t"
        "fmov.s  @r1+,fr4\n\t"
        "fmov.s  @r1+,fr5\n\t"
        "fmov.s  @r1,fr6\n\t"
        "fldi1   fr7\n\t"
        "ftrv    xmtrx,fv4\n\t"
        "shll    r0\n\t"           /* id*32: linha de vo */
        "mov     %[po],r2\n\t"
        "add     r0,r2\n\t"
        "add     #24,r2\n\t"
        "fmov    fr6,fr8\n\t"
        "fmul    fr6,fr8\n\t"
        "fsrra   fr8\n\t"          /* 1/|z| */
        "fmov    fr4,fr9\n\t"
        "fmul    fr12,fr9\n\t"
        "fmul    fr8,fr9\n\t"
        "fadd    fr14,fr9\n\t"     /* sx */
        "fmov    fr5,fr10\n\t"
        "fmul    fr13,fr10\n\t"
        "fmul    fr8,fr10\n\t"
        "fadd    fr15,fr10\n\t"    /* sy */
        "fmov.s  fr8,@-r2\n\t"
        "fmov.s  fr10,@-r2\n\t"
        "fmov.s  fr9,@-r2\n\t"
        "fmov.s  fr6,@-r2\n\t"
        "fmov.s  fr5,@-r2\n\t"
        "fmov.s  fr4,@-r2\n\t"
        "dt      %[n]\n\t"
        "bf      1b\n"
        : [ids] "+r" (ids), [n] "+r" (n), [k] "+r" (k4)
        : [pl] "r" (pl), [po] "r" (po)
        : "r0", "r1", "r2", "t", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10",
          "fr12", "fr13", "fr14", "fr15", "memory");
}
static int lista_atual;
/* luz/reflexo por VÉRTICE único (faces suaves compartilham a mesma normal média
 * no vértice): calculados uma vez por carro, reaproveitados pelas faces vizinhas */
static int assando_carro;          /* 1: desenhando o carro para a textura (sem rodas, sem luz) */
static float vl_luz[MAX_VU], vl_u[MAX_VU], vl_v[MAX_VU], vl_dd[MAX_VU];
static float ao_v[MAX_VU];                 /* OCLUSÃO pré-calculada por vértice (1 = aberto) */
static float ao_f[MAX_FACES_CARRO];        /* média por face (faces de luz chapada) */
static float baixo_v[MAX_VU], baixo_f[MAX_FACES_CARRO];   /* quanto o ponto está "embaixo" (suja primeiro) */
static float suj_atual;                    /* sujeira do carro sendo desenhado */
static uint32_t vl_o[MAX_VU];
static uint16_t vl_st[MAX_VU], vl_st_env[MAX_VU];
static uint16_t vu_st[MAX_VU], vu_carimbo = 1;
static int n_vu;
enum { LST_SIMPLES, LST_JOGADOR, LST_COCKPIT, LST_RIVAL, LST_MEDIO, N_LST };
static uint16_t lista_f[N_LST][MAX_FACES_CARRO];
static int n_lista[N_LST];
#ifdef DIAG_CARRO
static uint64_t car_ac[N_LST]; static int car_n[N_LST], car_q; static char car_txt[64];
#endif

/* ordem de envio em strip (zigue-zague 0,1,n-1,2,n-2...) para 3..8 vértices */
static const uint8_t ZZ[9][8] = { {0}, {0}, {0}, { 0, 1, 2 }, { 0, 1, 3, 2 }, { 0, 1, 4, 2, 3 }, { 0, 1, 5, 2, 4, 3 },
                                   { 0, 1, 6, 2, 5, 3, 4 }, { 0, 1, 7, 2, 6, 3, 5, 4 } };
static float face_pj[MAX_FACES_CARRO][8][2];     /* projeção fixa (eixo da normal): uv de carbono/tinta */
static inline void enviar_vu_t(int id, float u, float v2, uint32_t c, uint32_t o, int fim) {
    pvr_vertex_t *v = pvr_dr_target();
    v->flags = fim ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = vo[id][3]; v->y = vo[id][4]; v->z = vo[id][5];
    v->u = u; v->v = v2;
    v->argb = c; v->oargb = o;
    pvr_dr_commit(v);
    vert_quadro++;
}
/* envia um vértice já projetado (cache) */
static inline void enviar_vu(int id, uint32_t c, int fim) {
    pvr_vertex_t *v = pvr_dr_target();
    v->flags = fim ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = vo[id][3]; v->y = vo[id][4]; v->z = vo[id][5];
    v->u = v->v = 0;
    v->argb = c; v->oargb = 0;
    pvr_dr_commit(v);
    vert_quadro++;
}

static void desenhar_faces(const uint16_t *lista, int n, const float P[3], const float R[3],
                           const float U[3], const float Fr[3], const uint32_t *cores, uint32_t pular) {
    (void)pular;
    if(++vu_carimbo == 0) { memset(vu_st, 0, sizeof(vu_st)); memset(vl_st, 0, sizeof(vl_st)); memset(vl_st_env, 0, sizeof(vl_st_env)); vu_carimbo = 1; }
    const float near = -PERTO;
    static int nvis_f; (void)nvis_f;
    /* Uma matriz só por carro: local do carro -> espaço da câmera (antes eram
     * duas transformações por vértice: carro -> mundo -> câmera). A luz é
     * calculada no espaço do carro (a direção do sol é levada para lá uma vez). */
    float M[3][3], T[3], dP[3] = { P[0] - cam_p[0], P[1] - cam_p[1], P[2] - cam_p[2] };
    const float *eixo[3] = { cam_r, cam_u, cam_f };
    for(int l = 0; l < 3; l++) {
        const float *e = eixo[l];
        float sg = l == 2 ? -1.0f : 1.0f;                 /* a câmera olha para -Z */
        M[l][0] = sg * (e[0] * R[0] + e[1] * R[1] + e[2] * R[2]);
        M[l][1] = sg * (e[0] * U[0] + e[1] * U[1] + e[2] * U[2]);
        M[l][2] = sg * (e[0] * Fr[0] + e[1] * Fr[1] + e[2] * Fr[2]);
        T[l]    = sg * (e[0] * dP[0] + e[1] * dP[1] + e[2] * dP[2]);
    }
    float Ll[3] = { R[0] * LUZ[0] + R[1] * LUZ[1] + R[2] * LUZ[2],
                    U[0] * LUZ[0] + U[1] * LUZ[1] + U[2] * LUZ[2],
                    Fr[0] * LUZ[0] + Fr[1] * LUZ[1] + Fr[2] * LUZ[2] };
    /* posição da câmera no espaço do carro: o teste de "face de costas" é
     * feito ANTES de transformar qualquer vértice (metade das faces some aqui) */
    float Cl[3] = { -(R[0] * dP[0] + R[1] * dP[1] + R[2] * dP[2]),
                    -(U[0] * dP[0] + U[1] * dP[1] + U[2] * dP[2]),
                    -(Fr[0] * dP[0] + Fr[1] * dP[1] + Fr[2] * dP[2]) };
    /* a matriz vai para a unidade de matrizes do SH4 (XMTRX): cada vértice é
     * transformado por UMA instrução (ftrv) em vez de 9 multiplicações */
    static matrix_t mt __attribute__((aligned(32)));
    for(int l = 0; l < 3; l++) {
        mt[0][l] = M[l][0]; mt[1][l] = M[l][1]; mt[2][l] = M[l][2]; mt[3][l] = T[l];
    }
    mt[0][3] = mt[1][3] = mt[2][3] = 0; mt[3][3] = 1;
    mat_load(&mt);
#ifndef SEM_ASM
    {
        const float k4[4] __attribute__((aligned(8))) = { ESPELHAR * FOCO * XS, -FOCO, CX * XS, CY };
        vu_lote(lista_v[lista_atual], n_lista_v[lista_atual], k4);
    }
    const int lote = 1;
#else
    const int lote = 0;
#endif
    /* RODAS: o giro de cada roda (em volta do eixo x, passando por cy,cz) vai
     * embutido numa matriz própria (M x giro); troca-se a XMTRX quando muda
     * a roda, e os vértices não precisam ser girados um a um */
    static matrix_t mr[6] __attribute__((aligned(32)));
    uint8_t pronta = 0;                       /* bits: matriz da roda k já montada */
    int carregada = 0;                        /* 0 = carro; k = roda k */
    int hdr_atual = 0;                        /* 0 = hdr_mundo; 1..3 = textura do decalque */
    for(int i = 0; i < n; i++) {
        const face_t *fc = &carro_faces[lista[i]];
        if(fc->modo && fc->modo == modo_oculto) continue;
        if(assando_carro && (fc->roda >= 1 && fc->roda <= 4)) continue;
        const float *lv0 = fc->v[0];
        const float *ln = fc->nrm;
        float rv0[3], rn[3];
        int k = fc->roda;
        if(k) {                   /* só o 1º vértice e a normal girados (teste e luz) */
            float gc = giro_c[k], gs = giro_s[k], cy = giro_cy[k], cz = giro_cz[k];
            float dy = lv0[1] - cy, dz = lv0[2] - cz;
            rv0[0] = lv0[0]; rv0[1] = cy + dy * gc - dz * gs; rv0[2] = cz + dy * gs + dz * gc;
            if(k == 5) { rv0[0] += cab_dx; rv0[2] += cab_dz; }
            rn[0] = ln[0]; rn[1] = ln[1] * gc - ln[2] * gs; rn[2] = ln[1] * gs + ln[2] * gc;
            lv0 = rv0; ln = rn;
        }
        /* face de costas? (no espaço do carro: normal · (vértice - câmera)) */
        if(ln[0] * (lv0[0] - Cl[0]) + ln[1] * (lv0[1] - Cl[1]) + ln[2] * (lv0[2] - Cl[2]) >= 0) continue;
#ifdef TESTE_SO_LACO
        nvis_f++; continue;
#endif
        if(k != carregada) {
            if(k) {
                if(!(pronta & (1 << k))) {
                    /* v' = A v + b (giro em x); matriz = M·A, translação = M·b + T */
                    float gc = giro_c[k], gs = giro_s[k], cy = giro_cy[k], cz = giro_cz[k];
                    float by = cy - cy * gc + cz * gs, bz = cz - cy * gs - cz * gc;
                    for(int l = 0; l < 3; l++) {
                        mr[k][0][l] = M[l][0];
                        mr[k][1][l] = M[l][1] * gc + M[l][2] * gs;
                        mr[k][2][l] = -M[l][1] * gs + M[l][2] * gc;
                        mr[k][3][l] = M[l][1] * by + M[l][2] * bz + T[l];
                        if(k == 5) mr[k][3][l] += M[l][0] * cab_dx + M[l][2] * cab_dz;
                    }
                    mr[k][0][3] = mr[k][1][3] = mr[k][2][3] = 0; mr[k][3][3] = 1;
                    pronta |= 1 << k;
                }
                mat_load(&mr[k]);
            } else mat_load(&mt);
            carregada = k;
        }
        vv_t q[8];
        const uint16_t *fid = face_vid[lista[i]];
        int frente = 1;
        for(int v = 0; v < fc->n; v++) {
            int id = fid[v];
            if((k || !lote) && vu_st[id] != vu_carimbo) {   /* rodas/cabeça: transforma sob demanda */
                float x = fc->v[v][0], y = fc->v[v][1], z = fc->v[v][2];
                mat_trans_single3_nodiv(x, y, z);
                vo[id][0] = x; vo[id][1] = y; vo[id][2] = z;
                if(z <= near) {
                    float iz = __frsqrt(z * z);
                    vo[id][3] = (CX + ESPELHAR * FOCO * x * iz) * XS; vo[id][4] = CY - FOCO * y * iz; vo[id][5] = iz;
                }
                vu_st[id] = vu_carimbo;
            }
            q[v].x = vo[id][0]; q[v].y = vo[id][1]; q[v].z = vo[id][2];
            frente &= q[v].z <= near;
        }
        float d = ln[0] * Ll[0] + ln[1] * Ll[1] + ln[2] * Ll[2];
        float luz = (luz_base + luz_dir * (d > 0 ? d : 0)) * brilho * ao_f[lista[i]] * (1.0f - suj_atual * baixo_f[lista[i]]);
        if(fc->tex) {             /* decalque: textura modulada pela luz da face */
            uint32_t cl = cor_escala(0xFFFFFFFF, luz);
            vvt_t t[4];
            float du = 0, dv = 0;
            if(fc->tex == 5) { int m = marca_carro[tex_carro_ativo - 1]; du = (m & 1) * 0.5f; dv = (m >> 1) * 0.125f; }
            for(int v = 0; v < 4; v++) {
                t[v].x = q[v].x; t[v].y = q[v].y; t[v].z = q[v].z;
                t[v].u = fc->uv[v][0] + du; t[v].v = fc->uv[v][1] + dv; t[v].c = cl;
                if(fc->suave) {           /* capacete redondo: luz por vértice */
                    const float *nv = fc->vn[v];
                    float dd = nv[0] * Ll[0] + nv[1] * Ll[1] + nv[2] * Ll[2];
                    t[v].c = cor_escala(0xFFFFFFFF, (luz_base + luz_dir * (dd > 0 ? dd : 0)) * brilho);
                }
            }
            /* troca o cabeçalho só quando muda (o capacete tem dezenas de faces seguidas) */
            if(hdr_atual != fc->tex) {
                if(fc->tex == 1)      pvr_prim(&hdr_faixa[tex_carro_ativo - 1], sizeof(pvr_poly_hdr_t));
                else if(fc->tex == 2) pvr_prim(&hdr_num[tex_carro_ativo - 1], sizeof(pvr_poly_hdr_t));
                else if(fc->tex == 4) pvr_prim(&hdr_pneu_letras, sizeof(pvr_poly_hdr_t));
                else if(fc->tex == 5) pvr_prim(&hdr_placas_muro, sizeof(pvr_poly_hdr_t));
                else if(fc->tex == 6) pvr_prim(&hdr_roda_borr, sizeof(pvr_poly_hdr_t));
                else                  pvr_prim(&hdr_capacete[tex_carro_ativo - 1], sizeof(pvr_poly_hdr_t));
                hdr_atual = fc->tex;
            }
            poligono_tex(t, 4);
            continue;
        }
        if(hdr_atual && !(pint_on && !k && e_pintura(fc->cor)) &&
           (fc->suave || fc->cor != C_CARBONO)) { pvr_prim(&hdr_mundo, sizeof(hdr_mundo)); hdr_atual = 0; }
        if(pint_on && !k && e_pintura(fc->cor)) {
            /* PINTURA: no carro do jogador as superfícies curvas são METÁLICAS
             * (reflexo do ambiente que desliza + brilho do sol); nas outras
             * faces e nos rivais, tinta com textura sutil (emendas, rebites).
             * Parte de baixo mais suja; na chuva a pintura fica mais escura. */
            int env = spec_on && fc->suave;
            int h = env ? 10 : 11;
            if(hdr_atual != h) { pvr_prim(env ? &hdr_brilho : &hdr_tinta, sizeof(pvr_poly_hdr_t)); hdr_atual = h; }
            uint32_t base = cores[fc->cor];
            const float (*pj)[2] = face_pj[lista[i]];
            vvt_t t[8];
            for(int v = 0; v < fc->n; v++) {
                const float *nv = (fc->suave && v < 4) ? fc->vn[v] : fc->nrm;
                const float *pv = fc->v[v];
                int idv = fid[v];
                if(fc->suave && v < 4 && vl_st_env[idv] == vu_carimbo) {     /* vértice já calculado */
                    float suj = 0.80f + 0.20f * limitar(pv[1] / 0.38f, 0, 1);
                    t[v].x = q[v].x; t[v].y = q[v].y; t[v].z = q[v].z;
                    t[v].c = tinta_tarde(cor_escala(base, vl_luz[idv] * suj * (1.0f - 0.12f * molhado)), vl_dd[idv]);
                    t[v].u = vl_u[idv]; t[v].v = vl_v[idv]; t[v].o = env ? vl_o[idv] : 0;
                    if(!env) { t[v].u = pj[v][0]; t[v].v = pj[v][1]; }
                    continue;
                }
                float dd = nv[0] * Ll[0] + nv[1] * Ll[1] + nv[2] * Ll[2];
                float lv = fc->suave ? (luz_base + luz_dir * (dd > 0 ? dd : 0)) * brilho * ao_v[idv] * (1.0f - suj_atual * baixo_v[idv]) : luz;
                float suj = 0.80f + 0.20f * limitar(pv[1] / 0.38f, 0, 1);      /* poeira embaixo */
                uint32_t cc = tinta_tarde(cor_escala(base, lv * suj * (1.0f - 0.12f * molhado)), dd);
                t[v].x = q[v].x; t[v].y = q[v].y; t[v].z = q[v].z; t[v].c = cc; t[v].o = 0;
                if(env) {
                    /* reflexo: R = V - 2(N.V)N, no espaço do carro */
                    float vx = pv[0] - Cl[0], vy = pv[1] - Cl[1], vz = pv[2] - Cl[2];
                    float iv = __frsqrt(vx * vx + vy * vy + vz * vz);
                    vx *= iv; vy *= iv; vz *= iv;
                    float nd = 2.0f * (nv[0] * vx + nv[1] * vy + nv[2] * vz);
                    float rx = vx - nd * nv[0], ry = vy - nd * nv[1], rz = vz - nd * nv[2];
                    t[v].u = 0.5f + 0.30f * rx + 0.18f * rz; t[v].v = 0.5f - 0.46f * ry;
                    uint32_t o = brilho_v(nv, pv, Ll, Cl);
                    if(molhado > 0.2f && o) o = cor_escala(o | 0xFF000000, 1.0f + 0.4f * molhado) & 0xFFFFFF;
                    t[v].o = o;
                    if(v < 4) { vl_luz[idv] = lv; vl_dd[idv] = dd; vl_u[idv] = t[v].u; vl_v[idv] = t[v].v; vl_o[idv] = o; vl_st_env[idv] = vu_carimbo; }
                } else {
                    t[v].u = pj[v][0]; t[v].v = pj[v][1];
                    if(fc->suave && v < 4) { vl_luz[idv] = lv; vl_dd[idv] = dd; vl_o[idv] = 0; vl_st_env[idv] = vu_carimbo; }
                }
            }
            if(frente && !sem_envio) {
                const uint8_t *zz = ZZ[fc->n];
                for(int v = 0; v < fc->n; v++) { int w = zz[v]; enviar_vu_t(fid[w], t[w].u, t[w].v, t[w].c, t[w].o, v == fc->n - 1); }
                continue;
            }
            poligono_tex(t, fc->n);
            continue;
        }
        if(fc->suave) {
            /* SOMBREAMENTO SUAVE (Gouraud): luz calculada em cada vértice com a
             * normal "média" daquele ponto -> a superfície parece curva */
            uint32_t base = cores[fc->cor];
            for(int v = 0; v < 4; v++) {
                int id = fid[v];
                if(vl_st[id] != vu_carimbo) {
                    const float *nv = fc->vn[v];
                    float n1 = nv[1], n2 = nv[2];
                    if(k) { float gc = giro_c[k], gs = giro_s[k]; n1 = nv[1] * gc - nv[2] * gs; n2 = nv[1] * gs + nv[2] * gc; }
                    float dd = nv[0] * Ll[0] + n1 * Ll[1] + n2 * Ll[2];
                    vl_luz[id] = (luz_base + luz_dir * (dd > 0 ? dd : 0)) * brilho * ao_v[id] * (1.0f - suj_atual * baixo_v[id]);
                    vl_st[id] = vu_carimbo;
                }
                q[v].c = cor_escala(base, vl_luz[id]);
            }
            if(frente && !sem_envio) {
                enviar_vu(fid[0], q[0].c, 0); enviar_vu(fid[1], q[1].c, 0);
                enviar_vu(fid[3], q[3].c, 0); enviar_vu(fid[2], q[2].c, 1);
                continue;
            }
            poligono(q, 4);
            continue;
        }
        uint32_t c = tinta_tarde(cor_escala(cores[fc->cor], luz), d);
        if(fc->cor == C_CARBONO) {
            /* FIBRA DE CARBONO: trama projetada pelo eixo dominante da normal */
            if(hdr_atual != 9) { pvr_prim(&hdr_carbono, sizeof(hdr_carbono)); hdr_atual = 9; }
            const float (*pj)[2] = face_pj[lista[i]];
            if(frente && !sem_envio) {
                const uint8_t *zz = ZZ[fc->n];
                for(int v = 0; v < fc->n; v++) { int w = zz[v]; enviar_vu_t(fid[w], pj[w][0] * 6.0f, pj[w][1] * 6.0f, c, 0, v == fc->n - 1); }
                continue;
            }
            vvt_t t[8];
            for(int v = 0; v < fc->n; v++) {
                t[v].x = q[v].x; t[v].y = q[v].y; t[v].z = q[v].z; t[v].c = c; t[v].o = 0;
                t[v].u = pj[v][0] * 6.0f; t[v].v = pj[v][1] * 6.0f;
            }
            poligono_tex(t, fc->n);
            continue;
        }
        if(hdr_atual) { pvr_prim(&hdr_mundo, sizeof(hdr_mundo)); hdr_atual = 0; }
        if(frente && !sem_envio) {                 /* caso comum: vértices já projetados (cache) */
            const uint8_t *zz = ZZ[fc->n];
            for(int v = 0; v < fc->n; v++) enviar_vu(fid[zz[v]], c, v == fc->n - 1);
            continue;
        }
        for(int v = 0; v < fc->n; v++) q[v].c = c;
        poligono(q, fc->n);
    }
    if(hdr_atual) pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
    mat_load(&mat_cam);                       /* volta a matriz da câmera */
}

/* Caixa orientada (prédios, arquibancadas, arranha-céus), sem a face de baixo. */
static void desenhar_caixa(const float C[3], const float R[3], const float Fr[3],
                           float hx, float hy, float hz, uint32_t cor, uint32_t cor_topo) {
    /* Face visível? n·(centro da face - câmera) < 0, com n = ±R, ±U ou ±Fr:
     * dá para testar as 5 faces só com 3 produtos escalares. Os 8 cantos são
     * calculados e transformados (ftrv) uma vez só, e apenas os usados. */
    float dx = C[0] - cam_p[0], dy = C[1] - cam_p[1], dz = C[2] - cam_p[2];
    float dR = R[0] * dx + R[1] * dy + R[2] * dz, dF = Fr[0] * dx + Fr[1] * dy + Fr[2] * dz;
    float RL = R[0] * LUZ[0] + R[1] * LUZ[1] + R[2] * LUZ[2], FL = Fr[0] * LUZ[0] + Fr[1] * LUZ[1] + Fr[2] * LUZ[2];
    float ax[3] = { R[0] * hx, R[1] * hx, R[2] * hx }, az[3] = { Fr[0] * hz, Fr[1] * hz, Fr[2] * hz };
    vv_t cv[8];
    unsigned feito = 0;
    for(int f = 0; f < 6; f++) {
        if(f == 3) continue;
        const int8_t *nn = NCUBO[f];
        float nd, nl, h;
        if(nn[0])      { nd = nn[0] * dR; h = hx; nl = nn[0] * RL; }
        else if(nn[1]) { nd = dy;         h = hy; nl = LUZ[1]; }          /* só a de cima (f == 2) */
        else           { nd = nn[2] * dF; h = hz; nl = nn[2] * FL; }
        if(nd + h >= 0) continue;
        float lz = (luz_base + luz_dir * (nl > 0 ? nl : 0)) * brilho;
        uint32_t c = cor_escala(caixa_tex == 8 ? 0xFF505458 : f == 2 ? cor_topo : cor, lz);
        c = tinta_tarde(c, nl);
        uint32_t c_pe = cor_escala(c, 0.68f);
        int com_tex = (caixa_tex == 6 || caixa_tex == 7) ? 1 : (caixa_tex && f >= 4);   /* janelas/concreto: todas as faces */
        vv_t q[4];
        for(int v = 0; v < 4; v++) {
            const int8_t *s3 = CUBO[f][v];
            int id = (s3[0] > 0) | ((s3[1] > 0) << 1) | ((s3[2] > 0) << 2);
            if(!(feito & (1u << id))) {
                para_vista(C[0] + s3[0] * ax[0] + s3[2] * az[0],
                           C[1] + s3[0] * ax[1] + s3[1] * hy + s3[2] * az[1],
                           C[2] + s3[0] * ax[2] + s3[2] * az[2], 0, &cv[id]);
                feito |= 1u << id;
            }
            q[v] = cv[id]; q[v].c = (f != 2 && s3[1] < 0) ? c_pe : c;   /* pé da parede mais escuro */
        }
        if(com_tex) {
            vvt_t t[4];
            uint32_t cl = cor_escala(0xFFFFFFFF, lz);
            uint32_t ov = 0;
            if(caixa_vidro && f != 2) {
                /* vidro: reflete mais o céu quanto mais rasante a visão (Fresnel) */
                float D = sqrtf(dx * dx + dy * dy + dz * dz) + 1.0f;
                float cv = limitar(-(nd + h) / D, 0, 1), fr = (1 - cv) * (1 - cv);
                ov = cor_escala(cor_mistura(cor_ceu, cor_neb, 0.45f), (0.10f + 0.50f * fr) * brilho) & 0x00FFFFFF;
            }
            for(int v = 0; v < 4; v++) {
                const int8_t *s3 = CUBO[f][v];
                t[v].x = q[v].x; t[v].y = q[v].y; t[v].z = q[v].z; t[v].c = cl; t[v].o = ov;
                float a = (s3[0] + 1) * 0.5f, b = (1 - s3[1]) * 0.5f;
                if(caixa_tex == 7) {                 /* concreto: placas de 4 m x 2 m */
                    float larg = f < 2 ? hz : hx;
                    if(f == 2) { t[v].u = 0.3f; t[v].v = 0.3f; }
                    else { t[v].u = a * larg * 2 / 4.0f; t[v].v = b * hy * 2 / 2.0f; }
                }
                else if(caixa_tex == 6) {            /* janelas: uma a cada 3 m (largura) e um andar a cada 3 m */
                    float larg = f < 2 ? hz : hx;
                    if(f == 2) { t[v].u = 0.01f; t[v].v = 0.01f; }        /* telhado: canto liso da textura */
                    else { t[v].u = a * larg * 2 / 3.0f * 0.25f; t[v].v = b * hy * 2 / 3.0f * 0.25f; }
                }
                else if(caixa_tex == 8) {
                    int d = (int)(cor_topo & 15), col = d & 1, lin = d >> 1;
                    float aa = f == 5 ? a : 1 - a;            /* lido certo de frente (as duas faces) */
                    if(d == 8) { t[v].u = 0.004f + aa * 0.992f; t[v].v = 0.502f + b * 0.244f; }
                    else { t[v].u = col * 0.5f + 0.004f + aa * 0.492f; t[v].v = lin * 0.125f + 0.002f + b * 0.121f; }
                }
                else if(caixa_tex == 1) { t[v].u = a * hx * 2 / 3.0f; t[v].v = b * hy * 2 / 1.5f; }   /* 3 m x 1,5 m por quadro */
                else { int d = caixa_tex - 2; t[v].u = a; t[v].v = d * 0.25f + 0.01f + b * 0.23f; }
            }
            if(caixa_tex == 6 || caixa_tex == 7) {  /* parede colorida: a cor vem do vértice */
                uint32_t cp = cor_escala(f == 2 ? cor_topo : cor, lz), cpe = cor_escala(cp, 0.68f);
                for(int v = 0; v < 4; v++) t[v].c = (f != 2 && CUBO[f][v][1] < 0) ? cpe : cp;
            } else if(f != 2) {
                uint32_t cle = cor_escala(cl, 0.72f);
                for(int v = 0; v < 4; v++) if(CUBO[f][v][1] < 0) t[v].c = cle;
            }
            if(!caixa_lote) pvr_prim(caixa_tex == 1 ? &hdr_torcida : caixa_tex == 6 ? &hdr_janelas :
                                     caixa_tex == 7 ? &hdr_concreto : caixa_tex == 8 ? &hdr_placas_muro : &hdr_placas, sizeof(pvr_poly_hdr_t));
            poligono_tex(t, 4);
            if(!caixa_lote) pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
            continue;
        }
        poligono(q, 4);
    }
}

/* ------------------------------------------------------------------------ */
/* O MONOPOSTO (design original, montado com caixas e rodas octogonais)     */
/* ------------------------------------------------------------------------ */

/* (enum das cores do carro movido para antes de desenhar_faces) */

/* Dois modelos no mesmo vetor: o SIMPLES (longe) em [0, nf_simples) e o
 * DETALHADO (perto da câmera) em [nf_simples, carro_nf). */
static int    carro_nf, nf_simples, nf_cockpit;
static int    tampa_ini = -1, tampa_fim = -1;   /* faces da "tampa" escura da abertura (fora da visão de cockpit) */
static uint8_t marca_lod, face_lod[MAX_FACES_CARRO];   /* nível de detalhe de cada face (ver carro_face) */

/* ---------------- TEXTURAS DOS CARROS (decalques) ----------------
 * O carro continua com cores lisas; só alguns DECALQUES (quads 1 cm acima da
 * carroceria) usam textura: a FAIXA nas laterais do cockpit e o NÚMERO no bico
 * e nas placas da asa traseira. Cada carro tem as suas duas texturas, geradas
 * por código com as cores da pintura (criar_texturas_carro, mais abaixo). */
/* (cabeçalhos declarados antes de desenhar_faces) */

static uint16_t rgb565(uint32_t c) {
    return (uint16_t)((((c >> 16) & 0xFF) >> 3) << 11 | (((c >> 8) & 0xFF) >> 2) << 5 | ((c & 0xFF) >> 3));
}
static uint16_t tex_px[256 * 256] __attribute__((aligned(32)));
static pvr_ptr_t tex_subir(int w, int h) {
    pvr_ptr_t t = pvr_mem_malloc(w * h * 2);
    pvr_txr_load_ex(tex_px, t, w, h, PVR_TXRLOAD_16BPP);
    return t;
}

/* Tartan do clã Stewart (versão simplificada do "sett"): metade da sequência
 * de fios, espelhada; trama de sarja 2/2 (diagonais, como tecido de verdade). */
static void gerar_tartan(void) {
    enum { TR = 0, TB, TK, TY, TW, TG };
    static const uint32_t COR[6] = { 0xB41628, 0x1A265E, 0x121214, 0xE8C028, 0xF0F0F0, 0x1E5A34 };
    static const uint8_t SEQ[][2] = { { TR, 12 }, { TB, 5 }, { TK, 1 }, { TY, 1 }, { TK, 1 },
                                      { TW, 1 }, { TK, 1 }, { TG, 5 }, { TR, 5 } };   /* 32 fios */
    uint8_t fio[64];
    int k = 0;
    for(unsigned i = 0; i < sizeof(SEQ) / 2; i++) for(int j = 0; j < SEQ[i][1]; j++) fio[k++] = SEQ[i][0];
    for(int i = 0; i < 32; i++) fio[63 - i] = fio[i];                 /* espelha: sett completo */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++)
            tex_px[y * 64 + x] = rgb565(COR[((x + y) & 3) < 2 ? fio[x] : fio[y]]);
}

/* faixa "de corrida" das outras pinturas: listras horizontais ao longo do
 * cockpit (filete claro, faixa larga na cor secundária, filete) */
static void gerar_faixa(uint32_t fundo, uint32_t faixa, uint32_t filete) {
    for(int y = 0; y < 64; y++) {
        uint32_t c = fundo;
        /* a faixa usa só as linhas 0..33 da textura (altura 10,5 cm / quadro 20 cm) */
        if((y >= 3 && y < 6) || (y >= 28 && y < 31)) c = filete;
        else if(y >= 8 && y < 26) c = faixa;
        for(int x = 0; x < 64; x++) tex_px[y * 64 + x] = rgb565(c);
    }
}

/* números em blocos (caixa 22 x 40 px, traço 6 px), na textura 128 x 64 */
static void num_ret(int x0, int y0, int x1, int y1, uint16_t c) {
    for(int y = y0; y < y1; y++) for(int x = x0; x < x1; x++) tex_px[y * 128 + x] = c;
}
static void num_digito(int d, int ox, int oy, uint16_t c, uint16_t fundo) {
    if(d == 1) {
        num_ret(ox + 9, oy, ox + 16, oy + 40, c);           /* haste */
        num_ret(ox + 3, oy + 4, ox + 9, oy + 10, c);        /* bandeirinha */
        num_ret(ox + 5, oy + 34, ox + 20, oy + 40, c);      /* base */
        return;
    }
    /* sete segmentos:          a  b  c  d  e  f  g */
    static const uint8_t SEG[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    uint8_t m = SEG[d];
    if(m & 0x01) num_ret(ox + 2, oy, ox + 20, oy + 6, c);                 /* a: topo */
    if(m & 0x02) num_ret(ox + 16, oy + 2, ox + 22, oy + 21, c);           /* b */
    if(m & 0x04) num_ret(ox + 16, oy + 19, ox + 22, oy + 38, c);          /* c */
    if(m & 0x08) num_ret(ox + 2, oy + 34, ox + 20, oy + 40, c);           /* d: base */
    if(m & 0x10) num_ret(ox, oy + 19, ox + 6, oy + 38, c);                /* e */
    if(m & 0x20) num_ret(ox, oy + 2, ox + 6, oy + 21, c);                 /* f */
    if(m & 0x40) num_ret(ox + 2, oy + 17, ox + 20, oy + 23, c);           /* g: meio */
    if(d == 7) num_ret(ox + 16, oy, ox + 22, oy + 6, c);                  /* 7: canto cheio */
    /* cantos chanfrados */
    (void)fundo;
}
static void num_escrever(int n, int ox, uint16_t c, uint16_t fundo) {
    if(n >= 10) { num_digito(n / 10, ox + 6, 12, c, fundo); num_digito(n % 10, ox + 34, 12, c, fundo); }
    else          num_digito(n, ox + 21, 12, c, fundo);
}
/* metade esquerda: placa do bico (preto no branco); direita: placas da asa
 * traseira (número claro ou escuro sobre a cor da asa, o que contrastar) */
static void gerar_numeros(int n, uint32_t cor_asa) {
    uint32_t r = (cor_asa >> 16) & 0xFF, g = (cor_asa >> 8) & 0xFF, b = cor_asa & 0xFF;
    int claro = r * 3 + g * 6 + b > 1300;               /* asa clara -> número preto */
    uint16_t BRANCO = 0xFFFF, PRETO = 0x0841, ASA = rgb565(cor_asa);
    num_ret(0, 0, 64, 64, BRANCO);
    num_ret(64, 0, 128, 64, ASA);
    num_escrever(n, 0, PRETO, BRANCO);
    num_escrever(n, 64, claro ? PRETO : BRANCO, ASA);
}

/* ---------------- CAPACETES ----------------
 * Textura 64x64 por piloto, em 4 quadrantes de 32x32:
 *   [0,0] lateral   [1,0] topo   [0,1] traseira   [1,1] faixa acima da viseira
 * O desenho é feito por uma "receita" simples: cor de base, coroa (o topo),
 * até 3 faixas e um estilo (faixas retas, faixa curva que sobe até a viseira,
 * ou duas linhas cruzadas em X). */
enum { CAP_RETAS, CAP_CURVA, CAP_X };
typedef struct {
    uint32_t base, coroa, f[3], filete;   /* filete: linha fina entre as faixas (0 = nenhuma) */
    float fv, fl;                           /* centro (v) e meia-largura de cada faixa */
    uint8_t estilo, nf;
} capacete_t;

/* lado do capacete: u = 0 na frente .. 1 atrás; v = 0 no topo .. 1 embaixo */
static uint32_t capacete_lado(const capacete_t *k, float u, float v) {
    if(u < 0.14f && v > 0.30f && v < 0.56f) return 0xFF14161A;      /* borda da viseira */
    if(v < 0.17f + 0.05f * u) return k->coroa;
    uint32_t c = k->base;
    if(k->estilo == CAP_X) {
        /* duas linhas cruzadas; losango na cruz (f[2]) */
        float a = fabsf(v - (0.32f + 0.50f * u)), b = fabsf(v - (0.82f - 0.50f * u));
        float dc = fabsf(u - 0.5f) + fabsf(v - 0.57f);
        if(dc < 0.11f) return k->f[2];
        if(a < k->fl) c = k->f[0];
        if(b < k->fl) c = k->f[1];
        return c;
    }
    float centro = k->fv;
    if(k->estilo == CAP_CURVA) centro -= 0.42f * (1.0f - u) * (1.0f - u);   /* sobe para a frente */
    for(int i = 0; i < k->nf; i++) {
        float ci = centro + (i - (k->nf - 1) * 0.5f) * (2.0f * k->fl + (k->filete ? 0.05f : 0.0f));
        float d = fabsf(v - ci);
        if(d < k->fl) return k->f[i];
        if(k->filete && d < k->fl + 0.05f && i < k->nf - 1 && v > ci) return k->filete;
    }
    return c;
}
/* topo visto de cima: a = -1..1 (lado), b = -1 atrás .. 1 na frente */
static uint32_t capacete_topo(const capacete_t *k, float a, float b) {
    float ex = a / 0.72f, ez = (b + 0.05f) / 0.88f;
    if(b < -0.1f) ex = a / (0.72f * (1.0f + 0.9f * (b + 0.1f)) + 0.001f);   /* "gota": afina atrás */
    if(ex * ex + ez * ez < 1.0f) return k->coroa;
    return capacete_lado(k, 0.5f, 0.12f + 0.25f * (1.0f - fabsf(a)));      /* bordas = alto da lateral */
}
static void gerar_capacete(const capacete_t *k) {     /* 128x128: o dobro do detalhe */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            float u = (x + 0.5f) / 64.0f, v = (y + 0.5f) / 64.0f;
            tex_px[y * 128 + x]             = rgb565(capacete_lado(k, u, v));
            tex_px[y * 128 + 64 + x]        = rgb565(capacete_topo(k, u * 2 - 1, 1 - v * 2));
            tex_px[(64 + y) * 128 + x]      = rgb565(capacete_lado(k, 0.75f, v));
            tex_px[(64 + y) * 128 + 64 + x] = rgb565(capacete_lado(k, 0.02f + 0.0f * u, v * 0.25f));
        }
}

/* decalque: quad com normal e coordenadas de textura dadas */
static void carro_decal(const float pts[4][3], const float uv[4][2], const float nrm[3], int tex) {
    face_t *fc = &carro_faces[carro_nf++];
    memset(fc, 0, sizeof(*fc));
    fc->n = 4; fc->cor = C_PRINC; fc->tex = (uint8_t)tex;
    for(int i = 0; i < 4; i++) {
        for(int e = 0; e < 3; e++) fc->v[i][e] = pts[i][e];
        fc->uv[i][0] = uv[i][0]; fc->uv[i][1] = uv[i][1];
    }
    for(int e = 0; e < 3; e++) fc->nrm[e] = nrm[e];
}

#define CHANF_MONO 0.25f              /* chanfro de cima do monocoque (a faixa fica abaixo dele) */
/* Decalques: faixa dos dois lados do cockpit e o número (bico e asa traseira) */
static void carro_decalques(void) {
    /* faixa tartan seguindo a lateral do monocoque (afina para a frente) */
    const float SETT = 0.20f;                     /* tamanho de um "quadro" do xadrez (m) */
    float z0 = -0.45f, z1 = 1.30f;
    for(int l = -1; l <= 1; l += 2) {
        float x[2], yt[2];
        float zz[2] = { z0, z1 };
        for(int i = 0; i < 2; i++) {
            float t = (zz[i] + 0.55f) / 1.93f;
            x[i] = 0.40f + (0.22f - 0.40f) * t + 0.012f;                  /* lateral + 1,2 cm */
            float hx = 0.40f + (0.22f - 0.40f) * t, hy = 0.22f - 0.05f * t;
            yt[i] = (0.33f + 0.03f * t) + hy                                  /* topo do monocoque... */
                  - CHANF_MONO * (hx < hy ? hx : hy) + 0.004f;                /* ...menos o chanfro */
        }
        float h = 0.105f;
        float pts[4][3] = { { l * x[0], yt[0] - 0.01f - h, z0 }, { l * x[1], yt[1] - 0.01f - h, z1 },
                            { l * x[1], yt[1] - 0.01f, z1 },     { l * x[0], yt[0] - 0.01f, z0 } };
        float uv[4][2] = { { 0, h / SETT }, { (z1 - z0) / SETT, h / SETT }, { (z1 - z0) / SETT, 0 }, { 0, 0 } };
        float n[3] = { (float)l, 0, 0 };
        carro_decal(pts, uv, n, 1);
    }
    /* "16" na placa do bico (preto no branco), lido DE FRENTE como nos carros reais */
    {
        float za = 1.575f, zb = 1.83f, xa = 0.085f;
        float ya = 0.535f + (0.503f - 0.535f) * (za - 1.55f) / 0.30f + 0.012f;
        float yb = 0.535f + (0.503f - 0.535f) * (zb - 1.55f) / 0.30f + 0.012f;
        float pts[4][3] = { { xa, ya, za }, { -xa, ya, za }, { -xa, yb, zb }, { xa, yb, zb } };
        float uv[4][2] = { { 0, 0 }, { 0.5f, 0 }, { 0.5f, 1 }, { 0, 1 } };   /* topo do número p/ trás */
        float n[3] = { 0, 0.995f, 0.1f };
        carro_decal(pts, uv, n, 2);
    }
    /* "16" nas placas laterais da asa traseira (branco no azul), por fora */
    for(int l = -1; l <= 1; l += 2) {
        float xp = l * 0.548f, ya = 0.91f, yb = 0.69f, zf = -2.06f, zt = -2.38f;
        /* lado esquerdo: a leitura vai para trás (-z); lado direito: para a frente */
        float ua = 0.5f, ub = 1.0f;
        float zi = l < 0 ? zf : zt, zo = l < 0 ? zt : zf;
        float pts[4][3] = { { xp, ya, zi }, { xp, ya, zo }, { xp, yb, zo }, { xp, yb, zi } };
        float uv[4][2] = { { ua, 0 }, { ub, 0 }, { ub, 1 }, { ua, 1 } };
        float n[3] = { (float)l, 0, 0 };
        carro_decal(pts, uv, n, 2);
    }
    /* patrocinador em cima da asa traseira (lido de trás, na câmera de corrida) */
    {
        float pts[4][3] = { { -0.42f, 0.826f, -2.32f }, { 0.42f, 0.826f, -2.32f }, { 0.42f, 0.837f, -2.12f }, { -0.42f, 0.837f, -2.12f } };
        float uv[4][2] = { { 0.004f, 0.123f }, { 0.496f, 0.123f }, { 0.496f, 0.002f }, { 0.004f, 0.002f } };
        float n[3] = { 0, 0.998f, 0.055f };
        carro_decal(pts, uv, n, 5);
    }
    /* PAINÉIS DE PATROCINADOR (marcas fictícias, 1 por carro) nas laterais
     * dos sidepods, coladas na parede inclinada */
    for(int l = -1; l <= 1; l += 2) {
        float za = -0.50f, zb = 0.15f;
        float ta = (za + 1.15f) / 1.53f, tb = (zb + 1.15f) / 1.53f;
        float xa = 0.48f + 0.28f * ta + 0.014f, xb = 0.48f + 0.28f * tb + 0.014f;
        float ya = 0.24f + 0.10f * ta, yb = 0.24f + 0.10f * tb, ha = 0.058f, hb = 0.074f;
        float pts[4][3] = { { l * xa, ya + ha, za }, { l * xb, yb + hb, zb }, { l * xb, yb - hb, zb }, { l * xa, ya - ha, za } };
        float uf = l < 0 ? 0.004f : 0.496f, ut = l < 0 ? 0.496f : 0.004f;   /* lê da frente p/ trás à esquerda */
        float uv[4][2] = { { ut, 0.002f }, { uf, 0.002f }, { uf, 0.123f }, { ut, 0.123f } };
        float n[3] = { (float)l, 0, 0 };
        carro_decal(pts, uv, n, 5);
    }
    /* CAPACETE (caixa x ±0,125, y 0,56..0,82, z 0,25..0,55): laterais, topo,
     * traseira e a faixa acima da viseira. cor = C_CAPACETE para sumir junto
     * com o capacete na visão de dentro do cockpit. */
    {
        const float m = 1.0f / 256.0f;          /* meio texel: não pega o quadrante vizinho */
        const float X = 0.137f, Y0 = 0.56f, Y1 = 0.82f, Z0 = 0.25f, Z1 = 0.55f;
        for(int l = -1; l <= 1; l += 2) {       /* laterais: u = frente->trás, v = topo->baixo */
            float pts[4][3] = { { l * X, Y1, Z1 }, { l * X, Y1, Z0 }, { l * X, Y0, Z0 }, { l * X, Y0, Z1 } };
            float uv[4][2] = { { m, m }, { 0.5f - m, m }, { 0.5f - m, 0.5f - m }, { m, 0.5f - m } };
            float n[3] = { (float)l, 0, 0 };
            carro_decal(pts, uv, n, 3);
            carro_faces[carro_nf - 1].cor = C_CAPACETE;
            face_lod[carro_nf - 1] = 2;          /* no completo, o capacete é arredondado */
        }
        {   /* topo: u = lado (x), v = frente->trás */
            float yt = 0.838f, xt = 0.128f;
            float pts[4][3] = { { -xt, yt, Z1 + 0.005f }, { xt, yt, Z1 + 0.005f }, { xt, yt, Z0 - 0.005f }, { -xt, yt, Z0 - 0.005f } };
            float uv[4][2] = { { 0.5f + m, m }, { 1 - m, m }, { 1 - m, 0.5f - m }, { 0.5f + m, 0.5f - m } };
            float n[3] = { 0, 1, 0 };
            carro_decal(pts, uv, n, 3);
            carro_faces[carro_nf - 1].cor = C_CAPACETE;
            face_lod[carro_nf - 1] = 2;          /* no completo, o capacete é arredondado */
        }
        {   /* traseira */
            float zb = Z0 - 0.012f, xb = 0.125f;
            float pts[4][3] = { { -xb, Y1, zb }, { xb, Y1, zb }, { xb, Y0, zb }, { -xb, Y0, zb } };
            float uv[4][2] = { { m, 0.5f + m }, { 0.5f - m, 0.5f + m }, { 0.5f - m, 1 - m }, { m, 1 - m } };
            float n[3] = { 0, 0, -1 };
            carro_decal(pts, uv, n, 3);
            carro_faces[carro_nf - 1].cor = C_CAPACETE;
            face_lod[carro_nf - 1] = 2;          /* no completo, o capacete é arredondado */
        }
        {   /* frente, acima da viseira */
            float zf = Z1 + 0.012f, xf = 0.125f, yv = 0.757f;
            float pts[4][3] = { { xf, Y1, zf }, { -xf, Y1, zf }, { -xf, yv, zf }, { xf, yv, zf } };
            float uv[4][2] = { { 0.5f + m, 0.5f + m }, { 1 - m, 0.5f + m }, { 1 - m, 1 - m }, { 0.5f + m, 1 - m } };
            float n[3] = { 0, 0, 1 };
            carro_decal(pts, uv, n, 3);
            carro_faces[carro_nf - 1].cor = C_CAPACETE;
            face_lod[carro_nf - 1] = 2;          /* no completo, o capacete é arredondado */
        }
    }
}

/* face genérica; a normal sai pelo método de Newell (ordem = a mesma do CUBO) */
/* NÍVEL de cada face do modelo detalhado: 0 = sempre; 1 = só no completo
 * (formas arredondadas); 2 = só no médio (a versão "caixa", mais leve) */
static void carro_face(int n, const float pts[][3], int cor) {
    face_lod[carro_nf] = marca_lod;
    face_t *fc = &carro_faces[carro_nf++];
    fc->n = n; fc->cor = cor;
    float nx = 0, ny = 0, nz = 0;
    for(int i = 0; i < n; i++) {
        const float *a = pts[i], *b = pts[(i + 1) % n];
        fc->v[i][0] = a[0]; fc->v[i][1] = a[1]; fc->v[i][2] = a[2];
        nx += (a[1] - b[1]) * (a[2] + b[2]);
        ny += (a[2] - b[2]) * (a[0] + b[0]);
        nz += (a[0] - b[0]) * (a[1] + b[1]);
    }
    float l = sqrtf(nx * nx + ny * ny + nz * nz);
    l = l > 1e-6f ? 1.0f / l : 0;
    fc->nrm[0] = nx * l; fc->nrm[1] = ny * l; fc->nrm[2] = nz * l;
}

/* Tronco: "caixa" cuja face traseira (z-) e dianteira (z+) têm tamanho e
 * centro próprios — bico afinando, sidepod "garrafa de Coca", tampa do motor
 * descendo. Sem a face de baixo (a câmera nunca fica sob o carro). */
static void carro_tronco(float xr, float yr, float zr, float hxr, float hyr,
                         float xf, float yf, float zf, float hxf, float hyf, int cor) {
    for(int f = 0; f < 6; f++) {
        if(f == 3) continue;
        float pts[4][3];
        for(int v = 0; v < 4; v++) {
            int fr = CUBO[f][v][2] > 0;
            pts[v][0] = (fr ? xf : xr) + CUBO[f][v][0] * (fr ? hxf : hxr);
            pts[v][1] = (fr ? yf : yr) + CUBO[f][v][1] * (fr ? hyf : hyr);
            pts[v][2] = fr ? zf : zr;
        }
        carro_face(4, pts, cor);
    }
}

static void carro_caixa(float cx, float cy, float cz, float hx, float hy, float hz, int cor) {
    carro_tronco(cx, cy, cz - hz, hx, hy, cx, cy, cz + hz, hx, hy, cor);
}

/* LOFT ARREDONDADO: uma sequência de seções (de trás para a frente), cada uma
 * um retângulo com os cantos CHANFRADOS (octógono) — dá o aspecto arredondado
 * do bico, do monocoque e dos sidepods sem gastar muitas faces.
 * cs/ci = tamanho do chanfro de cima/de baixo, em fração do menor semieixo.
 * Sem a face de baixo; tampas opcionais atrás/na frente. */
typedef struct { float x, y, z, hx, hy; } secao_t;
static void secao_pontos(const secao_t *s, float cs, float ci, float p[8][3]) {
    float m = s->hx < s->hy ? s->hx : s->hy, a = cs * m, b = ci * m;
    float q[8][2] = { {  s->hx, -s->hy + b }, {  s->hx,  s->hy - a }, {  s->hx - a,  s->hy }, { -s->hx + a,  s->hy },
                      { -s->hx,  s->hy - a }, { -s->hx, -s->hy + b }, { -s->hx + b, -s->hy }, {  s->hx - b, -s->hy } };
    for(int k = 0; k < 8; k++) { p[k][0] = s->x + q[k][0]; p[k][1] = s->y + q[k][1]; p[k][2] = s->z; }
}
static int loft_suave = 1;          /* 0: faces chapadas (asas, peças de quinas vivas) */
static void carro_loft(const secao_t *s, int n, float cs, float ci, int tampa_tras, int tampa_frente, int cor) {
    float A[8][3], B[8][3];
    int id[8][8];                                     /* face de cada (segmento, lado); -1 = não existe */
    for(int i = 0; i < 8; i++) for(int k = 0; k < 8; k++) id[i][k] = -1;
    for(int i = 0; i + 1 < n; i++) {
        secao_pontos(&s[i], cs, ci, A);
        secao_pontos(&s[i + 1], cs, ci, B);
        float cx = (s[i].x + s[i + 1].x) * 0.5f, cy = (s[i].y + s[i + 1].y) * 0.5f;
        for(int k = 0; k < 8; k++) {
            if(k == 6) continue;                              /* fundo */
            int k2 = (k + 1) & 7;
            float la = fabsf(A[k][0] - A[k2][0]) + fabsf(A[k][1] - A[k2][1]);
            float lb = fabsf(B[k][0] - B[k2][0]) + fabsf(B[k][1] - B[k2][1]);
            if(la < 1e-4f && lb < 1e-4f) continue;            /* chanfro zero */
            float pts[4][3];
            memcpy(pts[0], A[k], 12); memcpy(pts[1], A[k2], 12); memcpy(pts[2], B[k2], 12); memcpy(pts[3], B[k], 12);
            face_t *fc = &carro_faces[carro_nf];
            id[i][k] = carro_nf;
            carro_face(4, pts, cor);
            /* normal para FORA (do eixo da peça para a face) */
            float mx = (A[k][0] + A[k2][0] + B[k][0] + B[k2][0]) * 0.25f - cx;
            float my = (A[k][1] + A[k2][1] + B[k][1] + B[k2][1]) * 0.25f - cy;
            if(fc->nrm[0] * mx + fc->nrm[1] * my < 0) { fc->nrm[0] = -fc->nrm[0]; fc->nrm[1] = -fc->nrm[1]; fc->nrm[2] = -fc->nrm[2]; }
        }
    }
    /* normal de cada vértice = média das faces que se encontram nele (lados
     * vizinhos e segmentos vizinhos): dá o sombreamento suave */
    for(int i = 0; loft_suave && i + 1 < n; i++)
        for(int k = 0; k < 8; k++) {
            if(id[i][k] < 0) continue;
            face_t *fc = &carro_faces[id[i][k]];
            int k2 = (k + 1) & 7;
            const int sv[4] = { i, i, i + 1, i + 1 }, pv[4] = { k, k2, k2, k };
            for(int v = 0; v < 4; v++) {
                float sm[3] = { 0, 0, 0 };
                for(int si = sv[v] - 1; si <= sv[v]; si++) {
                    if(si < 0 || si + 1 >= n) continue;
                    for(int kk = pv[v] - 1; kk <= pv[v]; kk++) {
                        int f = id[si][kk & 7];
                        if(f < 0) continue;
                        sm[0] += carro_faces[f].nrm[0]; sm[1] += carro_faces[f].nrm[1]; sm[2] += carro_faces[f].nrm[2];
                    }
                }
                float l = sqrtf(sm[0] * sm[0] + sm[1] * sm[1] + sm[2] * sm[2]);
                if(l < 1e-6f) { memcpy(fc->vn[v], fc->nrm, 12); continue; }
                fc->vn[v][0] = sm[0] / l; fc->vn[v][1] = sm[1] / l; fc->vn[v][2] = sm[2] / l;
            }
            fc->suave = 1;
        }
    for(int t = 0; t < 2; t++) {
        if(t == 0 ? !tampa_tras : !tampa_frente) continue;
        const secao_t *q = t == 0 ? &s[0] : &s[n - 1];
        secao_pontos(q, cs, ci, A);
        face_t *fc = &carro_faces[carro_nf];
        carro_face(8, A, cor);
        fc->nrm[0] = fc->nrm[1] = 0; fc->nrm[2] = t == 0 ? -1.0f : 1.0f;
    }
}
/* tronco arredondado de uma seção só (atalho do loft) */
static void carro_tronco_r(float xr, float yr, float zr, float hxr, float hyr,
                           float xf, float yf, float zf, float hxf, float hyf, float cs, float ci, int cor) {
    secao_t s[2] = { { xr, yr, zr, hxr, hyr }, { xf, yf, zf, hxf, hyf } };
    carro_loft(s, 2, cs, ci, 1, 1, cor);
}

/* Barra fina entre dois pontos (braços de suspensão, halo, hastes). 4 faces. */
static void carro_barra(float ax, float ay, float az, float bx, float by, float bz,
                        float larg, float alt, int cor) {
    float d[3] = { bx - ax, by - ay, bz - az };
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    d[0] /= l; d[1] /= l; d[2] /= l;
    float up[3] = { 0, 1, 0 };
    if(fabsf(d[1]) > 0.9f) { up[0] = 1; up[1] = 0; }
    float sd[3] = { d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0] };
    l = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
    sd[0] /= l; sd[1] /= l; sd[2] /= l;
    float uu[3] = { sd[1] * d[2] - sd[2] * d[1], sd[2] * d[0] - sd[0] * d[2], sd[0] * d[1] - sd[1] * d[0] };
    float A[3] = { ax, ay, az }, B[3] = { bx, by, bz };
    /* cantos da seção: (s, u) */
    static const float CS[4][2] = { { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 } };
    for(int k = 0; k < 4; k++) {
        const float *c0 = CS[k], *c1 = CS[(k + 1) & 3];
        float pts[4][3];
        for(int e = 0; e < 3; e++) {
            float o0 = sd[e] * c0[0] * larg + uu[e] * c0[1] * alt;
            float o1 = sd[e] * c1[0] * larg + uu[e] * c1[1] * alt;
            pts[0][e] = A[e] + o0; pts[1][e] = A[e] + o1;
            pts[2][e] = B[e] + o1; pts[3][e] = B[e] + o0;
        }
        face_t *fc = &carro_faces[carro_nf];
        carro_face(4, pts, cor);
        /* normal exata = direção média dos dois cantos (independe da ordem) */
        float n[3];
        for(int e = 0; e < 3; e++) n[e] = sd[e] * (c0[0] + c1[0]) + uu[e] * (c0[1] + c1[1]);
        float nl = 1.0f / sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        float dt = n[0] * fc->nrm[0] + n[1] * fc->nrm[1] + n[2] * fc->nrm[2];
        if(dt < 0) {   /* ordem invertida: desvira os vértices */
            float t[3];
            memcpy(t, fc->v[1], sizeof(t)); memcpy(fc->v[1], fc->v[3], sizeof(t)); memcpy(fc->v[3], t, sizeof(t));
        }
        for(int e = 0; e < 3; e++) fc->nrm[e] = n[e] * nl;
    }
}

/* Octógono deitado no plano x = cx (lado = +1 olha p/ fora à direita). */
static void carro_disco(float cx, float cy, float cz, float r, int lado, int cor, float ofs_ang) {
    float pts[8][3];
    for(int i = 0; i < 8; i++) {
        int ii = lado > 0 ? i : 7 - i;
        float a = (ii + 0.5f) * PI_F / 4 + ofs_ang;
        pts[i][0] = cx; pts[i][1] = cy + r * cosf(a); pts[i][2] = cz + r * sinf(a);
    }
    face_t *fc = &carro_faces[carro_nf];
    carro_face(8, pts, cor);
    fc->nrm[0] = (float)lado; fc->nrm[1] = fc->nrm[2] = 0;
}

/* Roda. Detalhada = pneu "sulcado" (regra 1998-2000: sulcos ao redor da
 * banda), aro pequeno (13") com flanco alto, cubo. Os sulcos são anéis finos
 * um tico acima da banda; os gomos virados para baixo nem são criados (a
 * câmera nunca está abaixo do eixo). */
/* Rodas que GIRAM: as faces marcadas com roda = 1..4 são giradas em volta
 * do eixo daquela roda na hora de desenhar. modo 1 = só aparece "nítido"
 * (devagar: raios e marcas do flanco girando), modo 2 = só "borrado"
 * (rápido: raios e marcas viram anéis/discos de cor misturada). */
static float roda_cy[5], roda_cz[5], roda_r[5];
static int roda_lados = 8;        /* 12 no modelo completo, 8 no médio/simples */

static void marcar_faces(int i0, int roda, int modo) {
    for(int i = i0; i < carro_nf; i++) { carro_faces[i].roda = (uint8_t)roda; carro_faces[i].modo = (uint8_t)modo; }
}
static void carro_decal(const float pts[4][3], const float uv[4][2], const float nrm[3], int tex);
/* RODA GIRANDO RÁPIDO: um octógono (3 quads) com a textura da roda borrada:
 * letras do flanco viram anel, raios viram disco com reflexos (tex 6) */
static void roda_borrada(float x, float cy, float cz, float R, int fora) {
    float p[8][3], uv[8][2];
    for(int i = 0; i < 8; i++) {
        int ii = fora > 0 ? i : 7 - i;
        float a = (ii + 0.5f) * PI_F / 4;
        p[i][0] = x; p[i][1] = cy + R * cosf(a); p[i][2] = cz + R * sinf(a);
        uv[i][0] = 0.5f + 0.5f * cosf(a); uv[i][1] = 0.5f + 0.5f * sinf(a);
    }
    static const uint8_t Q[3][4] = { { 0, 1, 2, 3 }, { 0, 3, 4, 7 }, { 4, 5, 6, 7 } };
    float n[3] = { (float)fora, 0, 0 };
    for(int f = 0; f < 3; f++) {
        float pts[4][3], u[4][2];
        for(int v = 0; v < 4; v++) { memcpy(pts[v], p[Q[f][v]], 12); u[v][0] = uv[Q[f][v]][0]; u[v][1] = uv[Q[f][v]][1]; }
        carro_decal(pts, u, n, 6);
    }
}

/* quadrilátero no plano x (flanco), entre os raios r0..r1 e ângulos a0..a1 */
static void carro_setor(float x, float cy, float cz, float r0, float r1, float a0, float a1, int fora, int cor) {
    float pts[4][3] = {
        { x, cy + r0 * cosf(a0), cz + r0 * sinf(a0) },
        { x, cy + r1 * cosf(a0), cz + r1 * sinf(a0) },
        { x, cy + r1 * cosf(a1), cz + r1 * sinf(a1) },
        { x, cy + r0 * cosf(a1), cz + r0 * sinf(a1) },
    };
    face_t *fc = &carro_faces[carro_nf];
    carro_face(4, pts, cor);
    fc->nrm[0] = (float)fora; fc->nrm[1] = fc->nrm[2] = 0;
}

/* disco de 12 lados (flanco do pneu detalhado): duas faces de 7 vértices */
static void carro_disco12(float cx, float cy, float cz, float r, int lado, int cor) {
    for(int h = 0; h < 2; h++) {
        float pts[7][3];
        for(int i = 0; i < 7; i++) {
            int ii = lado > 0 ? i : 6 - i;
            float a = (h * 6 + ii + 0.5f) * PI_F / 6;
            pts[i][0] = cx; pts[i][1] = cy + r * cosf(a); pts[i][2] = cz + r * sinf(a);
        }
        face_t *fc = &carro_faces[carro_nf];
        carro_face(7, pts, cor);
        fc->nrm[0] = (float)lado; fc->nrm[1] = fc->nrm[2] = 0;
    }
}

/* disco de 16 lados (flanco do pneu dianteiro do jogador, visto do cockpit):
 * 4 fatias de 6 vértices (centro + 5 pontos da borda) */
static void carro_disco16(float cx, float cy, float cz, float r, int lado, int cor) {
    for(int q = 0; q < 4; q++) {
        float pts[6][3];
        pts[0][0] = cx; pts[0][1] = cy; pts[0][2] = cz;
        for(int i = 0; i < 5; i++) {
            int ii = lado > 0 ? i : 4 - i;
            float a = (q * 4 + ii + 0.5f) * PI_F / 8;
            pts[i + 1][0] = cx; pts[i + 1][1] = cy + r * cosf(a); pts[i + 1][2] = cz + r * sinf(a);
        }
        face_t *fc = &carro_faces[carro_nf];
        carro_face(6, pts, cor);
        fc->nrm[0] = (float)lado; fc->nrm[1] = fc->nrm[2] = 0;
    }
}

/* Roda. Detalhada = pneu sulcado (regra 1998-2000), aro de 13" com flanco
 * alto, raios, marcas genéricas no flanco (sem marca nenhuma). */
static void carro_roda(float cx, float cy, float cz, float r, float hw, int detalhe, int sulcos, int idx) {
    /* detalhada: 12 lados (bem mais redonda); de longe: 8 */
    int N = detalhe ? roda_lados : 8;
    int i_ini = carro_nf;
    float a[16], passo = 2 * PI_F / N;
    for(int i = 0; i < N; i++) a[i] = (i + 0.5f) * passo;
    int i0 = carro_nf;
    /* completo (12 lados): banda mais estreita + OMBROS que descem até o
     * flanco (pneu de perfil arredondado), tudo com luz suave */
    int ombro = N >= 12;
    float hb = ombro ? hw * 0.84f : hw, ro = r * 0.93f;
    for(int i = 0; i < N; i++) {              /* banda de rodagem */
        face_t *fc = &carro_faces[carro_nf++];
        float a0 = a[i], a1 = a[(i + 1) % N], am = (i + 1) * passo;
        fc->n = 4; fc->cor = C_PNEU;
        float pts[4][3] = {
            { cx - hb, cy + r * cosf(a0), cz + r * sinf(a0) },
            { cx + hb, cy + r * cosf(a0), cz + r * sinf(a0) },
            { cx + hb, cy + r * cosf(a1), cz + r * sinf(a1) },
            { cx - hb, cy + r * cosf(a1), cz + r * sinf(a1) },
        };
        memcpy(fc->v, pts, sizeof(pts));
        fc->nrm[0] = 0; fc->nrm[1] = cosf(am); fc->nrm[2] = sinf(am);
        if(ombro) {
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float vn[4][3] = { { 0, c0, s0 }, { 0, c0, s0 }, { 0, c1, s1 }, { 0, c1, s1 } };
            memcpy(fc->vn, vn, sizeof(vn));
            fc->suave = 1;
            for(int l = -1; l <= 1; l += 2) {     /* ombro de cada lado */
                face_t *fo = &carro_faces[carro_nf++];
                fo->n = 4; fo->cor = C_PNEU;
                float po[4][3] = {
                    { cx + l * hb, cy + r * c0,  cz + r * s0 },
                    { cx + l * hw, cy + ro * c0, cz + ro * s0 },
                    { cx + l * hw, cy + ro * c1, cz + ro * s1 },
                    { cx + l * hb, cy + r * c1,  cz + r * s1 },
                };
                memcpy(fo->v, po, sizeof(po));
                const float k1 = 0.55f, k2 = 0.835f;   /* normal inclinada para o lado */
                fo->nrm[0] = l * k1; fo->nrm[1] = k2 * cosf(am); fo->nrm[2] = k2 * sinf(am);
                float on[4][3] = { { 0, c0, s0 }, { l * 0.80f, 0.60f * c0, 0.60f * s0 },
                                   { l * 0.80f, 0.60f * c1, 0.60f * s1 }, { 0, c1, s1 } };
                memcpy(fo->vn, on, sizeof(on));
                fo->suave = 1;
            }
        }
    }
    int fora = cx > 0 ? 1 : -1;
    if(!detalhe) {
        carro_disco(cx + hw, cy, cz, r * 0.8f, 1, C_ARO, 0);
        carro_disco(cx - hw, cy, cz, r * 0.8f, -1, C_ARO, 0);
        return;
    }
    roda_cy[idx] = cy; roda_cz[idx] = cz; roda_r[idx] = r;
    /* sulcos: anéis estreitos e escuros; o 2º recebe a marca do composto.
     * Todos os gomos existem (a roda gira, o de baixo vira o de cima). */
    float rs = r * 1.012f, gw = hw * 0.045f;
    for(int g = 0; g < sulcos; g++) {
        float gx = cx - hw + (g + 1) * (2 * hw) / (sulcos + 1);
        int cor = (g == 1) ? C_FAIXA_PNEU : C_SULCO;
        for(int i = 0; i < N; i++) {
            float am = (i + 1) * passo;
            float a0 = a[i], a1 = a[(i + 1) % N];
            face_t *fc = &carro_faces[carro_nf++];
            fc->n = 4; fc->cor = cor;
            float pts[4][3] = {
                { gx - gw, cy + rs * cosf(a0), cz + rs * sinf(a0) },
                { gx + gw, cy + rs * cosf(a0), cz + rs * sinf(a0) },
                { gx + gw, cy + rs * cosf(a1), cz + rs * sinf(a1) },
                { gx - gw, cy + rs * cosf(a1), cz + rs * sinf(a1) },
            };
            memcpy(fc->v, pts, sizeof(pts));
            fc->nrm[0] = 0; fc->nrm[1] = cosf(am); fc->nrm[2] = sinf(am);
        }
    }
    marcar_faces(i0, idx, 0);                 /* banda + sulcos giram sempre */

    /* lado de dentro: só o flanco */
    float x = cx + fora * hw;
    if(N == 16) {
        carro_disco16(cx - fora * hw, cy, cz, ro * 1.005f, -fora, C_PNEU);
        carro_disco16(x, cy, cz, ro * 1.005f, fora, C_PNEU);
    } else if(N == 12) {
        carro_disco12(cx - fora * hw, cy, cz, ro * 1.005f, -fora, C_PNEU);
        carro_disco12(x, cy, cz, ro * 1.005f, fora, C_PNEU);
    } else {
        carro_disco(cx - fora * hw, cy, cz, r * 0.97f, -fora, C_PNEU, 0);
        carro_disco(x, cy, cz, r * 0.97f, fora, C_PNEU, 0);
    }

    /* marcas brancas no flanco (dois blocos opostos, como letreiros sem texto) */
    i0 = carro_nf;
    for(int k = 0; k < 2; k++) {
        float b = k * PI_F;
        for(int h = 0; h < 2; h++) {             /* marca FICTÍCIA ("RODAX") em letras brancas */
            float a0 = h ? b + 0.06f : b - 0.42f, a1 = h ? b + 0.42f : b - 0.06f;
            face_t *fl = &carro_faces[carro_nf];
            carro_setor(x + fora * 0.003f, cy, cz, r * 0.79f, r * 0.92f, a0, a1, fora, C_MARCA);
            float u0 = fora > 0 ? 0.0f : 1.0f, u1 = 1.0f - u0;
            fl->tex = 4;
            fl->uv[0][0] = u0; fl->uv[0][1] = 1; fl->uv[1][0] = u0; fl->uv[1][1] = 0;
            fl->uv[2][0] = u1; fl->uv[2][1] = 0; fl->uv[3][0] = u1; fl->uv[3][1] = 1;
        }
    }
    marcar_faces(i0, idx, 1);
    /* ...que, girando rápido, viram um anel cinza-claro */
    i0 = carro_nf;
    if(roda_lados >= 12) roda_borrada(x + fora * 0.013f, cy, cz, r * 0.92f, fora);   /* completo: textura */
    else {
        carro_disco(x + fora * 0.002f, cy, cz, r * 0.92f, fora, C_BORRAO_PNEU, 0);
        carro_disco(x + fora * 0.004f, cy, cz, r * 0.79f, fora, C_PNEU, PI_F / 8);
    }
    marcar_faces(i0, 0, 2);

    /* aro: borda, fundo escuro, raios, cubo */
    carro_disco(x + fora * 0.006f, cy, cz, r * 0.56f, fora, C_ARO, PI_F / 8);
    carro_disco(x + fora * 0.009f, cy, cz, r * 0.50f, fora, C_DISCO, 0);   /* disco de freio (acende nas freadas) */
    i0 = carro_nf;
    for(int k = 0; k < 5; k++) {
        float b = k * 2 * PI_F / 5;
        carro_setor(x + fora * 0.012f, cy, cz, r * 0.16f, r * 0.50f, b - 0.20f, b + 0.20f, fora, C_ARO);
    }
    marcar_faces(i0, idx, 1);
    if(roda_lados < 12) {
        i0 = carro_nf;
        carro_disco(x + fora * 0.012f, cy, cz, r * 0.50f, fora, C_BORRAO_ARO, 0);
        marcar_faces(i0, 0, 2);
    }
    carro_disco(x + fora * 0.016f, cy, cz, r * 0.17f, fora, C_CARBONO, 0);
    for(int i = i_ini; i < carro_nf; i++) face_lod[i] = marca_lod;   /* faces criadas direto também */
}

/* Bitola estreita (regra de 1998: carro com 1,80 m de largura). */
#define RD_X  0.74f     /* centro da roda dianteira */
#define RD_HW 0.155f
#define RT_X  0.71f     /* traseira, mais larga */
#define RT_HW 0.19f

/* Capacete arredondado: loft de 5 seções (cantos chanfrados, luz suave) e a
 * textura do piloto projetada como numa caixa: cada face pega o quadrante
 * (lado / topo / trás / frente) do eixo para onde mais aponta. */
static void carro_capacete_redondo(void) {
    const float X = 0.128f, Y0 = 0.56f, Y1 = 0.82f, Z0 = 0.25f, Z1 = 0.55f, m = 1.0f / 256.0f;
    secao_t c[5] = { { 0, 0.700f, 0.245f, 0.085f, 0.085f }, { 0, 0.690f, 0.285f, 0.118f, 0.122f },
                     { 0, 0.690f, 0.400f, 0.127f, 0.132f }, { 0, 0.690f, 0.515f, 0.122f, 0.127f },
                     { 0, 0.700f, 0.565f, 0.090f, 0.095f } };
    int i0 = carro_nf;
    carro_loft(c, 5, 0.45f, 0.30f, 1, 1, C_CAPACETE);
    for(int i = i0; i < carro_nf; i++) {
        face_t *fc = &carro_faces[i];
        if(fc->n != 4) continue;                        /* tampas (8 lados): cor lisa */
        float ax = fabsf(fc->nrm[0]), ay = fabsf(fc->nrm[1]), az = fabsf(fc->nrm[2]);
        for(int v = 0; v < 4; v++) {
            float x = fc->v[v][0], y = fc->v[v][1], z = fc->v[v][2], u, t;
            float su = limitar((Z1 - z) / (Z1 - Z0), 0, 1), sv = limitar((Y1 - y) / (Y1 - Y0), 0, 1);
            if(fc->nrm[1] > 0 && ay >= ax && ay >= az) {        /* topo */
                u = 0.5f + limitar((x / X + 1) * 0.5f, 0, 1) * 0.5f; t = su * 0.5f;
            } else if(az > ax && az > ay && fc->nrm[2] < 0) {    /* trás */
                u = limitar((x / X + 1) * 0.5f, 0, 1) * 0.5f; t = 0.5f + sv * 0.5f;
            } else if(az > ax && az > ay) {                      /* frente, acima da viseira */
                u = 0.5f + limitar((x / X + 1) * 0.5f, 0, 1) * 0.5f;
                t = 0.5f + limitar((Y1 - y) / (Y1 - 0.757f), 0, 1) * 0.5f;
            } else { u = su * 0.5f; t = sv * 0.5f; }             /* laterais */
            /* não deixa a amostragem invadir o quadrante vizinho */
            float qu = u < 0.5f ? 0 : 0.5f, qv = t < 0.5f ? 0 : 0.5f;
            if(u >= 0.5f && u <= 0.5f) qu = 0.5f;
            fc->uv[v][0] = limitar(u, qu + m, qu + 0.5f - m);
            fc->uv[v][1] = limitar(t, qv + m, qv + 0.5f - m);
        }
        fc->tex = 3;
    }
}

/* Modelo de longe (barato), já com a silhueta e os blocos de cor da pintura. */
static void montar_carro_simples(void) {
    carro_tronco(0, 0.33f, -0.55f, 0.40f, 0.22f, 0, 0.36f, 1.38f, 0.22f, 0.17f, C_PRINC); /* monocoque */
    carro_tronco(0, 0.40f,  1.38f, 0.21f, 0.13f, 0, 0.31f, 2.70f, 0.06f, 0.04f, C_NARIZ); /* bico alto */
    carro_caixa(0, 0.085f, 2.76f, 0.73f, 0.025f, 0.19f, C_ASA);                            /* asa diant. */
    carro_caixa(-0.73f, 0.14f, 2.74f, 0.015f, 0.09f, 0.24f, C_PLACA);                      /* placas da */
    carro_caixa( 0.73f, 0.14f, 2.74f, 0.015f, 0.09f, 0.24f, C_PLACA);                      /* asa diant. */
    carro_caixa(0, 0.93f, -2.30f, 0.50f, 0.02f, 0.08f, C_SEC);                             /* 2º elemento */
    carro_tronco(-0.36f, 0.24f, -1.15f, 0.12f, 0.08f, -0.58f, 0.34f, 0.38f, 0.18f, 0.16f, C_TERC);
    carro_tronco( 0.36f, 0.24f, -1.15f, 0.12f, 0.08f,  0.58f, 0.34f, 0.38f, 0.18f, 0.16f, C_TERC);
    carro_tronco(0, 0.36f, -1.90f, 0.12f, 0.10f, 0, 0.84f, 0.02f, 0.14f, 0.14f, C_MOTOR); /* motor+airbox */
    carro_caixa(0, 0.68f,  0.40f, 0.125f, 0.13f, 0.15f, C_CAPACETE);                      /* piloto */
    /* asa traseira em elementos (antes era um bloco maciço) */
    carro_caixa(0, 0.80f, -2.20f, 0.50f, 0.025f, 0.17f, C_ASA);
    carro_caixa(0, 1.01f, -2.33f, 0.50f, 0.018f, 0.07f, C_ASA);
    carro_caixa(0, 0.45f, -2.24f, 0.40f, 0.018f, 0.09f, C_CARBONO);                       /* beam wing */
    /* detalhes que dão a "cara" de F1 de longe: viseira, abertura do cockpit,
     * boca do airbox, entradas dos sidepods, placa do número, ponta do bico,
     * braços de suspensão */
    carro_caixa(0, 0.72f, 0.556f, 0.105f, 0.035f, 0.012f, C_VISOR);
    carro_caixa(0, 0.545f, 0.40f, 0.16f, 0.012f, 0.38f, C_ESCURO);
    carro_caixa(0, 0.89f, 0.035f, 0.09f, 0.07f, 0.012f, C_ESCURO);
    carro_caixa(-0.58f, 0.36f, 0.395f, 0.135f, 0.10f, 0.012f, C_ESCURO);
    carro_caixa( 0.58f, 0.36f, 0.395f, 0.135f, 0.10f, 0.012f, C_ESCURO);
    carro_tronco(0, 0.527f, 1.55f, 0.11f, 0.008f, 0, 0.495f, 1.85f, 0.095f, 0.008f, C_PAINEL);
    carro_tronco(0, 0.31f, 2.55f, 0.07f, 0.05f, 0, 0.30f, 2.73f, 0.035f, 0.03f, C_SEC);
    for(int l = -1; l <= 1; l += 2) {
        carro_barra(l * 0.20f, 0.44f, 1.50f, l * (RD_X - RD_HW), 0.46f, 1.55f, 0.04f, 0.012f, C_CARBONO);
        carro_barra(l * 0.16f, 0.40f, -1.45f, l * (RT_X - RT_HW), 0.50f, -1.55f, 0.04f, 0.012f, C_CARBONO);
    }
    carro_caixa(-0.51f, 0.74f, -2.22f, 0.02f, 0.30f, 0.26f, C_PLACA);
    carro_caixa( 0.51f, 0.74f, -2.22f, 0.02f, 0.30f, 0.26f, C_PLACA);
    carro_roda(-RD_X, 0.33f,  1.55f, 0.33f, RD_HW, 0, 0, 0);
    carro_roda( RD_X, 0.33f,  1.55f, 0.33f, RD_HW, 0, 0, 0);
    carro_roda(-RT_X, 0.35f, -1.55f, 0.35f, RT_HW, 0, 0, 0);
    carro_roda( RT_X, 0.35f, -1.55f, 0.35f, RT_HW, 0, 0, 0);
}

/* Modelo de perto, inspirado nos F1 de 1998-2000: bico alto com a asa
 * pendurada em pilones, cockpit aberto (sem halo), airbox grande com câmera
 * de TV, sidepods longos com chaminés de escape, defletores laterais, asa
 * traseira alta e estreita com 3 elementos, pneus sulcados com aro pequeno.
 * Pintura em blocos (monocoque / bico / sidepods / motor / asas / placas)
 * e painéis lisos de "patrocinador" (sem marca nenhuma). */
static int extra_ini, extra_fim;      /* faixa de faces "extras" (só nível completo) */
static void montar_carro_detalhado(void) {
    /* assoalho (prancha) e difusor */
    carro_caixa(0, 0.06f, -0.05f, 0.56f, 0.025f, 1.30f, C_CARBONO);
    carro_tronco(0, 0.19f, -2.30f, 0.45f, 0.05f, 0, 0.08f, -1.30f, 0.50f, 0.02f, C_CARBONO);

    /* versão "caixa" do corpo, só para os rivais no nível médio */
    marca_lod = 2;
    carro_tronco(0, 0.33f, -0.55f, 0.40f, 0.22f, 0, 0.36f, 1.38f, 0.22f, 0.17f, C_PRINC);
    carro_tronco(0, 0.40f,  1.38f, 0.21f, 0.13f, 0, 0.33f, 2.55f, 0.085f, 0.075f, C_NARIZ);
    carro_tronco(0, 0.33f,  2.55f, 0.085f, 0.075f, 0, 0.31f, 2.72f, 0.045f, 0.035f, C_SEC);
    carro_tronco(0, 0.70f, -0.70f, 0.10f, 0.10f, 0, 0.87f, 0.02f, 0.13f, 0.13f, C_MOTOR);
    carro_tronco(0, 0.36f, -1.90f, 0.12f, 0.10f, 0, 0.52f, -0.55f, 0.28f, 0.24f, C_MOTOR);
    for(int l = -1; l <= 1; l += 2)
        carro_tronco(l * 0.36f, 0.24f, -1.15f, 0.12f, 0.08f, l * 0.58f, 0.34f, 0.38f, 0.18f, 0.16f, C_TERC);
    marca_lod = 1;                    /* daqui: formas arredondadas (só no completo) */
    /* monocoque (laterais altas, cantos de cima arredondados) */
    {
        secao_t m[2] = { { 0, 0.33f, -0.55f, 0.40f, 0.22f }, { 0, 0.36f, 1.38f, 0.22f, 0.17f } };
        carro_loft(m, 2, CHANF_MONO, 0.30f, 1, 0, C_PRINC);
    }
    /* bico alto, bem arredondado, caindo suave até a ponta */
    {
        secao_t b[4] = { { 0, 0.40f, 1.38f, 0.21f, 0.13f }, { 0, 0.385f, 1.85f, 0.165f, 0.112f },
                         { 0, 0.36f, 2.25f, 0.12f, 0.092f }, { 0, 0.33f, 2.55f, 0.085f, 0.075f } };
        carro_loft(b, 4, 0.45f, 0.50f, 0, 0, C_NARIZ);
        secao_t p[3] = { { 0, 0.33f, 2.55f, 0.085f, 0.075f }, { 0, 0.315f, 2.67f, 0.06f, 0.05f },
                         { 0, 0.30f, 2.73f, 0.035f, 0.03f } };
        carro_loft(p, 3, 0.50f, 0.50f, 0, 1, C_SEC);                                       /* ponta */
    }
    marca_lod = 0;
    carro_tronco(0, 0.535f, 1.40f, 0.05f, 0.01f, 0, 0.41f, 2.55f, 0.035f, 0.01f, C_SEC);     /* faixa no bico */
    tampa_ini = carro_nf;
    carro_caixa(0, 0.545f, 0.38f, 0.16f, 0.012f, 0.40f, C_ESCURO);                           /* abertura */
    tampa_fim = carro_nf;
    /* placa do número no bico (painel claro) */
    carro_tronco(0, 0.527f, 1.55f, 0.11f, 0.008f, 0, 0.495f, 1.85f, 0.095f, 0.008f, C_PAINEL);

    /* piloto: capacete exposto + viseira + listra (versão caixa: nível médio) */
    marca_lod = 2;
    carro_caixa(0, 0.69f, 0.40f, 0.125f, 0.13f, 0.15f, C_CAPACETE);
    carro_caixa(0, 0.72f, 0.556f, 0.105f, 0.035f, 0.012f, C_VISOR);
    carro_caixa(0, 0.825f, 0.40f, 0.04f, 0.008f, 0.10f, C_SEC);
    /* completo: capacete ARREDONDADO, com o desenho do piloto projetado nele */
    marca_lod = 1;
    carro_capacete_redondo();
    carro_caixa(0, 0.718f, 0.571f, 0.090f, 0.032f, 0.006f, C_VISOR);
    marca_lod = 0;

    /* FILETES: linhas finas na cor do painel acompanhando sidepods e bico */
    for(int l = -1; l <= 1; l += 2) {
        carro_tronco(l * 0.486f, 0.285f, -1.10f, 0.004f, 0.011f, l * 0.766f, 0.43f, 0.36f, 0.004f, 0.013f, C_PAINEL);
        carro_tronco(l * 0.216f, 0.405f, 1.42f, 0.004f, 0.010f, l * 0.093f, 0.335f, 2.52f, 0.004f, 0.007f, C_PAINEL);
    }
    /* airbox grande atrás da cabeça + câmera de TV no topo */
    marca_lod = 1;
    carro_tronco_r(0, 0.70f, -0.70f, 0.10f, 0.10f, 0, 0.87f, 0.02f, 0.13f, 0.13f, 0.45f, 0.20f, C_MOTOR);
    marca_lod = 0;
    carro_caixa(0, 0.89f, 0.035f, 0.09f, 0.075f, 0.012f, C_ESCURO);
    carro_caixa(0, 0.975f, -0.12f, 0.055f, 0.012f, 0.03f, C_SEC);
    carro_caixa(0, 1.005f, -0.12f, 0.012f, 0.02f, 0.012f, C_CARBONO);

    /* tampa do motor descendo até o câmbio */
    {
        secao_t t[3] = { { 0, 0.36f, -1.90f, 0.12f, 0.10f }, { 0, 0.425f, -1.20f, 0.18f, 0.158f },
                         { 0, 0.52f, -0.55f, 0.28f, 0.24f } };
        marca_lod = 1;
        carro_loft(t, 3, 0.50f, 0.25f, 1, 0, C_MOTOR);
        marca_lod = 0;
    }

    for(int l = -1; l <= 1; l += 2) {
        /* sidepods longos, afinando atrás; entrada de ar; faixa no topo */
        {   /* "garrafa de Coca": afina e desce até o câmbio; topo e base arredondados */
            secao_t sp[2] = { { l * 0.36f, 0.24f, -1.15f, 0.12f, 0.08f }, { l * 0.58f, 0.34f, 0.38f, 0.18f, 0.16f } };
            marca_lod = 1;
            carro_loft(sp, 2, 0.42f, 0.38f, 1, 1, C_TERC);
            marca_lod = 0;
        }
        carro_caixa(l * 0.58f, 0.36f, 0.395f, 0.135f, 0.10f, 0.012f, C_ESCURO);
        marca_lod = 1;                /* moldura da entrada de ar (só no completo) */
        carro_tronco_r(l * 0.58f, 0.468f, 0.37f, 0.155f, 0.014f, l * 0.58f, 0.468f, 0.43f, 0.15f, 0.012f, 0.5f, 0.5f, C_TERC);
        carro_tronco_r(l * 0.72f, 0.36f, 0.37f, 0.016f, 0.11f, l * 0.72f, 0.36f, 0.43f, 0.014f, 0.105f, 0.5f, 0.5f, C_TERC);
        carro_tronco_r(l * 0.58f, 0.252f, 0.37f, 0.15f, 0.012f, l * 0.58f, 0.255f, 0.43f, 0.145f, 0.010f, 0.5f, 0.5f, C_TERC);
        marca_lod = 0;
        carro_tronco(l * 0.45f, 0.375f, -0.80f, 0.06f, 0.008f, l * 0.60f, 0.50f, 0.30f, 0.09f, 0.008f, C_SEC);
        /* painel liso de "patrocinador" na lateral do sidepod (segue a parede inclinada) */
        carro_tronco(l * 0.597f, 0.279f, -0.55f, 0.004f, 0.068f, l * 0.734f, 0.328f, 0.20f, 0.004f, 0.092f, C_PAINEL);
        /* chaminés de escape no alto do sidepod ("periscópio") */
        carro_caixa(l * 0.42f, 0.40f, -0.85f, 0.04f, 0.05f, 0.06f, C_CARBONO);
        carro_caixa(l * 0.42f, 0.452f, -0.85f, 0.028f, 0.004f, 0.045f, C_ESCURO);
        /* defletor lateral (barge board) */
        carro_caixa(l * 0.54f, 0.21f, 0.85f, 0.012f, 0.13f, 0.30f, C_CARBONO);
    }

    /* asa dianteira baixa, pendurada no bico alto por dois pilones */
    marca_lod = 2;
    carro_caixa(0, 0.085f, 2.76f, 0.72f, 0.02f, 0.18f, C_ASA);
    marca_lod = 1; loft_suave = 0;
    {   /* plano principal com perfil de asa (visto de lado) */
        secao_t w[3] = { { 0, 0.094f, 2.58f, 0.72f, 0.006f }, { 0, 0.083f, 2.84f, 0.72f, 0.022f },
                         { 0, 0.076f, 2.94f, 0.72f, 0.010f } };
        carro_loft(w, 3, 0, 0, 1, 1, C_ASA);
        /* flap: lâmina inclinada atrás do plano principal */
        secao_t fl[3] = { { 0, 0.175f, 2.545f, 0.68f, 0.004f }, { 0, 0.150f, 2.63f, 0.68f, 0.014f },
                          { 0, 0.128f, 2.70f, 0.68f, 0.007f } };
        carro_loft(fl, 3, 0, 0, 1, 1, C_SEC);
    }
    loft_suave = 1; marca_lod = 0;
    carro_caixa(-0.73f, 0.145f, 2.74f, 0.015f, 0.095f, 0.26f, C_PLACA);
    carro_caixa( 0.73f, 0.145f, 2.74f, 0.015f, 0.095f, 0.26f, C_PLACA);
    carro_barra(-0.05f, 0.27f, 2.30f, -0.07f, 0.10f, 2.58f, 0.012f, 0.03f, C_CARBONO);
    carro_barra( 0.05f, 0.27f, 2.30f,  0.07f, 0.10f, 2.58f, 0.012f, 0.03f, C_CARBONO);

    /* câmbio + luz de chuva */
    carro_caixa(0, 0.27f, -1.98f, 0.20f, 0.11f, 0.30f, C_CARBONO);
    carro_caixa(0, 0.36f, -2.30f, 0.07f, 0.035f, 0.015f, C_LUZ);

    /* asa traseira alta e estreita: 3 elementos, placas grandes com painel, beam wing */
    marca_lod = 2;
    carro_caixa(0, 0.80f, -2.20f, 0.50f, 0.025f, 0.17f, C_ASA);
    carro_tronco(0, 0.93f, -2.36f, 0.50f, 0.018f, 0, 0.88f, -2.16f, 0.50f, 0.018f, C_SEC);
    carro_tronco(0, 1.03f, -2.40f, 0.50f, 0.015f, 0, 0.99f, -2.26f, 0.50f, 0.015f, C_ASA);
    marca_lod = 1; loft_suave = 0;
    {   /* 3 elementos com perfil de lâmina, cada um mais inclinado */
        secao_t e1[3] = { { 0, 0.812f, -2.37f, 0.50f, 0.006f }, { 0, 0.800f, -2.12f, 0.50f, 0.030f },
                          { 0, 0.790f, -2.03f, 0.50f, 0.014f } };
        carro_loft(e1, 3, 0, 0, 1, 1, C_ASA);
        secao_t e2[3] = { { 0, 0.945f, -2.38f, 0.50f, 0.005f }, { 0, 0.905f, -2.22f, 0.50f, 0.020f },
                          { 0, 0.880f, -2.15f, 0.50f, 0.010f } };
        carro_loft(e2, 3, 0, 0, 1, 1, C_SEC);
        secao_t e3[3] = { { 0, 1.040f, -2.41f, 0.50f, 0.004f }, { 0, 1.010f, -2.31f, 0.50f, 0.016f },
                          { 0, 0.992f, -2.26f, 0.50f, 0.008f } };
        carro_loft(e3, 3, 0, 0, 1, 1, C_ASA);
    }
    loft_suave = 1; marca_lod = 0;
    carro_caixa(-0.51f, 0.76f, -2.22f, 0.018f, 0.31f, 0.26f, C_PLACA);
    carro_caixa( 0.51f, 0.76f, -2.22f, 0.018f, 0.31f, 0.26f, C_PLACA);
    carro_caixa(-0.531f, 0.80f, -2.22f, 0.004f, 0.13f, 0.19f, C_PAINEL);
    carro_caixa( 0.531f, 0.80f, -2.22f, 0.004f, 0.13f, 0.19f, C_PAINEL);
    carro_barra(0, 0.40f, -2.10f, 0, 0.78f, -2.20f, 0.02f, 0.05f, C_CARBONO);
    carro_caixa(0, 0.45f, -2.24f, 0.40f, 0.018f, 0.09f, C_CARBONO);

    /* pneus sulcados: 3 sulcos na frente, 4 atrás. Duas versões: 12 lados
     * (completo: jogador) e 8 lados (médio: rivais), para não pesar. */
    for(int v = 0; v < 2; v++) {
        marca_lod = v == 0 ? 1 : 2;
        roda_lados = v == 0 ? 16 : 8;          /* dianteiras do jogador: 16 lados (aparecem no cockpit) */
        carro_roda(-RD_X, 0.33f,  1.55f, 0.33f, RD_HW, 1, 3, 1);
        carro_roda( RD_X, 0.33f,  1.55f, 0.33f, RD_HW, 1, 3, 2);
        roda_lados = v == 0 ? 12 : 8;
        carro_roda(-RT_X, 0.35f, -1.55f, 0.35f, RT_HW, 1, 4, 3);
        carro_roda( RT_X, 0.35f, -1.55f, 0.35f, RT_HW, 1, 4, 4);
    }
    marca_lod = 0; roda_lados = 8;

    /* daqui em diante: peças que NÃO entram na visão de cockpit */
    carro_decalques();
    nf_cockpit = carro_nf;
    for(int l = -1; l <= 1; l += 2) {
        /* proteções laterais da cabeça (regra de 1996) */
        carro_caixa(l * 0.17f, 0.60f, 0.30f, 0.045f, 0.07f, 0.24f, C_PRINC);
        /* retrovisor em haste curta na lateral do cockpit */
        carro_barra(l * 0.26f, 0.52f, 0.78f, l * 0.40f, 0.60f, 0.80f, 0.012f, 0.012f, C_CARBONO);
        marca_lod = 2;
        carro_caixa(l * 0.45f, 0.61f, 0.81f, 0.07f, 0.035f, 0.03f, C_PRINC);
        marca_lod = 1;            /* carenagem aerodinâmica: larga atrás (espelho), afina na frente */
        {
            secao_t rv[3] = { { l * 0.45f, 0.61f, 0.782f, 0.074f, 0.038f }, { l * 0.452f, 0.612f, 0.82f, 0.068f, 0.034f },
                              { l * 0.44f, 0.612f, 0.865f, 0.040f, 0.020f } };
            carro_loft(rv, 3, 0.55f, 0.55f, 0, 1, C_PRINC);
        }
        marca_lod = 0;
        carro_caixa(l * 0.45f, 0.61f, 0.775f, 0.058f, 0.026f, 0.006f, C_ARO);
        /* suspensão: braço superior e inferior até cada roda */
        carro_barra(l * 0.20f, 0.44f, 1.50f, l * (RD_X - RD_HW), 0.46f, 1.55f, 0.04f, 0.012f, C_CARBONO);
        carro_barra(l * 0.20f, 0.22f, 1.60f, l * (RD_X - RD_HW), 0.22f, 1.55f, 0.05f, 0.012f, C_CARBONO);
        carro_barra(l * 0.16f, 0.40f, -1.45f, l * (RT_X - RT_HW), 0.50f, -1.55f, 0.04f, 0.012f, C_CARBONO);
        carro_barra(l * 0.16f, 0.20f, -1.60f, l * (RT_X - RT_HW), 0.22f, -1.55f, 0.05f, 0.012f, C_CARBONO);
    }

    /* DETALHES EXTRAS (só no nível completo: no médio dos rivais somem) */
    extra_ini = carro_nf;
    for(int l = -1; l <= 1; l += 2) {
        /* dutos de freio dentro das rodas */
        carro_caixa(l * (RD_X - RD_HW - 0.045f), 0.33f, 1.55f, 0.04f, 0.10f, 0.11f, C_CARBONO);
        carro_caixa(l * (RT_X - RT_HW - 0.045f), 0.35f, -1.55f, 0.04f, 0.11f, 0.12f, C_CARBONO);
        /* pushrods (do braço inferior ao alto do chassi) e barras de direção */
        carro_barra(l * (RD_X - RD_HW - 0.02f), 0.24f, 1.56f, l * 0.15f, 0.50f, 1.42f, 0.012f, 0.012f, C_CARBONO);
        carro_barra(l * 0.19f, 0.33f, 1.66f, l * (RD_X - RD_HW - 0.02f), 0.33f, 1.64f, 0.010f, 0.010f, C_CARBONO);
        carro_barra(l * (RT_X - RT_HW - 0.02f), 0.25f, -1.54f, l * 0.14f, 0.46f, -1.35f, 0.012f, 0.012f, C_CARBONO);
        /* semieixo do câmbio à roda traseira */
        carro_barra(l * 0.18f, 0.30f, -1.58f, l * (RT_X - RT_HW - 0.06f), 0.35f, -1.55f, 0.022f, 0.022f, C_ARO);
        /* defletores sob o bico (turning vanes) */
        carro_caixa(l * 0.13f, 0.20f, 1.95f, 0.008f, 0.075f, 0.22f, C_CARBONO);
        /* segundo defletor lateral, curvo, à frente do sidepod */
        carro_tronco(l * 0.47f, 0.27f, 0.45f, 0.008f, 0.11f, l * 0.52f, 0.25f, 0.68f, 0.008f, 0.09f, C_CARBONO);
        /* flip-ups à frente dos pneus traseiros */
        carro_tronco(l * 0.64f, 0.11f, -1.02f, 0.10f, 0.012f, l * 0.64f, 0.19f, -1.10f, 0.10f, 0.012f, C_CARBONO);
        /* aletas verticais do difusor */
        carro_caixa(l * 0.20f, 0.13f, -2.05f, 0.008f, 0.065f, 0.26f, C_CARBONO);
        /* aletas das placas da asa dianteira (pé) */
        carro_caixa(l * 0.66f, 0.072f, 2.86f, 0.06f, 0.006f, 0.09f, C_CARBONO);
        /* 2º flap da asa dianteira, na parte de fora (sobe para trás) */
        carro_tronco(l * 0.52f, 0.215f, 2.585f, 0.17f, 0.010f, l * 0.52f, 0.155f, 2.70f, 0.17f, 0.010f, C_ASA);
        /* aletas de refrigeração no topo do sidepod (escuras) */
        for(int k = 0; k < 3; k++) {
            float z = -0.52f - 0.075f * k, t = (z + 1.15f) / 0.75f;
            float yt = 0.32f + 0.09f * t, xs = 0.36f + 0.11f * t + 0.03f;
            carro_caixa(l * xs, yt + 0.004f, z, 0.055f, 0.006f, 0.014f, C_ESCURO);
        }
        /* saída do escape (boca escura atrás das chaminés) */
        carro_caixa(l * 0.42f, 0.43f, -0.93f, 0.03f, 0.022f, 0.012f, C_ESCURO);
    }
    for(int l = -1; l <= 1; l += 2) {
        /* braços em "A": a 2ª perna de cada braço de suspensão */
        carro_barra(l * 0.20f, 0.44f, 1.30f, l * (RD_X - RD_HW), 0.46f, 1.55f, 0.016f, 0.010f, C_CARBONO);
        carro_barra(l * 0.20f, 0.22f, 1.35f, l * (RD_X - RD_HW), 0.22f, 1.55f, 0.016f, 0.010f, C_CARBONO);
        carro_barra(l * 0.16f, 0.40f, -1.75f, l * (RT_X - RT_HW), 0.50f, -1.55f, 0.016f, 0.010f, C_CARBONO);
        carro_barra(l * 0.16f, 0.20f, -1.80f, l * (RT_X - RT_HW), 0.22f, -1.55f, 0.016f, 0.010f, C_CARBONO);
        /* "asinhas" no fim dos sidepods, à frente das rodas traseiras (1998-2000) */
        carro_caixa(l * 0.50f, 0.50f, -0.95f, 0.008f, 0.07f, 0.10f, C_ASA);
        carro_tronco(l * 0.43f, 0.57f, -1.02f, 0.07f, 0.006f, l * 0.43f, 0.55f, -0.88f, 0.07f, 0.006f, C_ASA);
        /* borda acolchoada da abertura do cockpit */
        carro_caixa(l * 0.165f, 0.56f, 0.62f, 0.012f, 0.014f, 0.17f, C_ESCURO);
        /* espelho do retrovisor (vidro claro) e haste dupla */
        carro_barra(l * 0.26f, 0.50f, 0.84f, l * 0.40f, 0.60f, 0.82f, 0.010f, 0.010f, C_CARBONO);
        /* tomadas de ar dos freios dianteiros (aletas para fora) */
        carro_caixa(l * (RD_X - RD_HW - 0.05f), 0.24f, 1.70f, 0.03f, 0.05f, 0.02f, C_ESCURO);
        /* placas da asa dianteira: recorte escuro e borda de baixo */
        carro_caixa(l * 0.745f, 0.145f, 2.74f, 0.002f, 0.06f, 0.16f, C_SEC);
        /* asa traseira: aleta de baixo (beam wing) com flap */
        carro_caixa(l * 0.28f, 0.47f, -2.31f, 0.12f, 0.010f, 0.035f, C_ASA);
    }
    carro_caixa(0, 0.56f, 1.05f, 0.04f, 0.015f, 0.06f, C_ESCURO);          /* tomada de ar do cockpit */
    /* gurney flap no elemento de cima da asa traseira e na asa dianteira */
    carro_caixa(0, 1.05f, -2.405f, 0.50f, 0.014f, 0.004f, C_ESCURO);
    carro_caixa(0, 0.155f, 2.60f, 0.66f, 0.010f, 0.004f, C_ESCURO);
    /* volante visível dentro do cockpit e encosto de cabeça escuro */
    carro_caixa(0, 0.56f, 0.76f, 0.12f, 0.035f, 0.015f, C_CARBONO);
    carro_caixa(0, 0.56f, 0.21f, 0.15f, 0.05f, 0.03f, C_ESCURO);
    /* antena / pitot no bico e luz de câmera de bordo atrás do airbox */
    carro_barra(0.06f, 0.50f, 1.95f, 0.06f, 0.64f, 1.90f, 0.006f, 0.006f, C_CARBONO);
    carro_caixa(0, 0.80f, -0.55f, 0.03f, 0.03f, 0.05f, C_SEC);
    /* tampa do câmbio/estrutura de impacto traseira mais desenhada */
    carro_tronco(0, 0.33f, -2.28f, 0.10f, 0.06f, 0, 0.30f, -2.40f, 0.06f, 0.04f, C_CARBONO);
    for(int l = -1; l <= 1; l += 2) {
        /* aletas (louvers) por fora das placas da asa traseira */
        for(int k = 0; k < 4; k++)
            carro_caixa(l * 0.532f, 0.60f + k * 0.055f, -2.40f, 0.006f, 0.008f, 0.07f, C_ESCURO);
        /* pé das placas da asa traseira (prende no difusor) */
        carro_caixa(l * 0.51f, 0.47f, -2.26f, 0.022f, 0.012f, 0.20f, C_CARBONO);
        /* cascatas da asa dianteira: dois defletores escalonados por fora */
        carro_tronco(l * 0.60f, 0.170f, 2.62f, 0.10f, 0.006f, l * 0.60f, 0.215f, 2.74f, 0.10f, 0.006f, C_ASA);
        carro_tronco(l * 0.60f, 0.215f, 2.66f, 0.08f, 0.005f, l * 0.60f, 0.250f, 2.76f, 0.08f, 0.005f, C_SEC);
        carro_caixa(l * 0.68f, 0.21f, 2.69f, 0.006f, 0.05f, 0.09f, C_CARBONO);   /* placa da cascata */
        /* câmeras de TV nas laterais do bico */
        carro_caixa(l * 0.115f, 0.455f, 2.10f, 0.022f, 0.022f, 0.07f, C_SEC);
        carro_caixa(l * 0.115f, 0.455f, 2.175f, 0.016f, 0.016f, 0.006f, C_ESCURO);
        /* aletas extras do difusor */
        carro_caixa(l * 0.36f, 0.12f, -2.08f, 0.008f, 0.06f, 0.22f, C_CARBONO);
        /* bocal de ar do freio traseiro (aleta para fora, na frente da roda) */
        carro_caixa(l * (RT_X - RT_HW - 0.03f), 0.26f, -1.28f, 0.03f, 0.06f, 0.025f, C_ESCURO);
    }
    /* ganchos de reboque (estrutura de impacto) e antena de rádio no airbox */
    carro_caixa(0, 0.30f, -2.43f, 0.03f, 0.03f, 0.02f, C_ARO);
    carro_barra(0.05f, 0.86f, -0.30f, 0.06f, 1.02f, -0.40f, 0.006f, 0.006f, C_CARBONO);
    extra_fim = carro_nf;
}

static int n_detalhados;
/* DETALHE (botão START): ALTO = os rivais próximos (até N_MEDIO, a menos de
 * DIST_MEDIO) usam o modelo COMPLETO; NORMAL = usam o médio (mais leve) */
#ifndef DETALHE_INI
#define DETALHE_INI 1
#endif
static int detalhe_alto = DETALHE_INI;
#ifndef N_COMPLETO_ALTO
#define N_COMPLETO_ALTO 2      /* no ALTO: quantos rivais com o modelo completo */
#endif
#ifndef AREA_MIUDA
#define AREA_MIUDA 0.014f      /* m²: abaixo disso a face some no nível médio */
#endif
#ifndef DIST_MEDIO
#define DIST_MEDIO 95.0f       /* m: rivais até aqui usam o nível médio */
#endif
#ifndef N_MEDIO
#define N_MEDIO 3              /* quantos rivais no nível médio ao mesmo tempo */
#endif
static int n_miudas;   /* carros com o modelo detalhado neste quadro (limite p/ o largada) */

/* vértice único: mesma posição (0,1 mm) e mesmo grupo de matriz */
/* OCLUSÃO AMBIENTE "assada" (como nos jogos da época, na cor dos vértices):
 * o carro detalhado vira uma grade de voxels de 6 cm; de cada vértice saem 14
 * raios no hemisfério da normal (e o chão, y = 0, também bloqueia). Cantos,
 * embaixo das asas, dentro das laterais e do cockpit ficam mais escuros.
 * Calculado UMA vez no boot; em tempo real é só uma multiplicação. */
#define AO_C 0.06f
#define AO_NX 42
#define AO_NY 24
#define AO_NZ 106
static uint8_t ao_grade[AO_NX * AO_NY * AO_NZ / 8 + 1];
static inline int ao_idx(float x, float y, float z) {
    int ix = (int)((x + 1.26f) / AO_C), iy = (int)((y + 0.10f) / AO_C), iz = (int)((z + 3.0f) / AO_C);
    if(ix < 0 || iy < 0 || iz < 0 || ix >= AO_NX || iy >= AO_NY || iz >= AO_NZ) return -1;
    return (iz * AO_NY + iy) * AO_NX + ix;
}
static void ao_marca(float x, float y, float z) { int i = ao_idx(x, y, z); if(i >= 0) ao_grade[i >> 3] |= 1 << (i & 7); }
static inline int ao_cheio(float x, float y, float z) { int i = ao_idx(x, y, z); return i >= 0 && (ao_grade[i >> 3] >> (i & 7) & 1); }
static void assar_oclusao_carro(void) {
    memset(ao_grade, 0, sizeof(ao_grade));
    for(int f = nf_simples; f < carro_nf; f++) {          /* rasteriza as faces do modelo detalhado */
        const face_t *fc = &carro_faces[f];
        if(fc->tex == 6) continue;
        for(int t = 1; t + 1 < fc->n; t++) {
            const float *a = fc->v[0], *b = fc->v[t], *c = fc->v[t + 1];
            float lab = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
            float lac = sqrtf((c[0] - a[0]) * (c[0] - a[0]) + (c[1] - a[1]) * (c[1] - a[1]) + (c[2] - a[2]) * (c[2] - a[2]));
            int nu = (int)(lab / 0.03f) + 1, nv = (int)(lac / 0.03f) + 1;
            if(nu > 120) nu = 120;
            if(nv > 120) nv = 120;
            for(int i = 0; i <= nu; i++)
                for(int j = 0; j <= nv; j++) {
                    float u = i / (float)nu, v = j / (float)nv;
                    if(u + v > 1.0f) continue;
                    ao_marca(a[0] + (b[0] - a[0]) * u + (c[0] - a[0]) * v, a[1] + (b[1] - a[1]) * u + (c[1] - a[1]) * v,
                             a[2] + (b[2] - a[2]) * u + (c[2] - a[2]) * v);
                }
        }
    }
    /* normal média de cada vértice (das faces que o usam) */
    static float nm[MAX_VU][3];
    memset(nm, 0, sizeof(nm));
    for(int f = 0; f < carro_nf; f++) {
        const face_t *fc = &carro_faces[f];
        for(int v = 0; v < fc->n; v++) {
            int id = face_vid[f][v];
            const float *n = (fc->suave && v < 4) ? fc->vn[v] : fc->nrm;
            nm[id][0] += n[0]; nm[id][1] += n[1]; nm[id][2] += n[2];
        }
    }
    static const float DIRS[14][3] = {
        { 0, 1, 0 }, { 0.7f, 0.7f, 0 }, { -0.7f, 0.7f, 0 }, { 0, 0.7f, 0.7f }, { 0, 0.7f, -0.7f },
        { 1, 0.15f, 0 }, { -1, 0.15f, 0 }, { 0, 0.15f, 1 }, { 0, 0.15f, -1 },
        { 0.6f, 0.4f, 0.6f }, { -0.6f, 0.4f, 0.6f }, { 0.6f, 0.4f, -0.6f }, { -0.6f, 0.4f, -0.6f }, { 0, -0.5f, 0 } };
    static const float PASSOS[4] = { 0.09f, 0.17f, 0.28f, 0.42f };
    for(int id = 0; id < n_vu; id++) {
        float n[3] = { nm[id][0], nm[id][1], nm[id][2] };
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if(l < 1e-4f) { ao_v[id] = 1.0f; continue; }
        n[0] /= l; n[1] /= l; n[2] /= l;
        const float *p = vl[id];
        int tot = 0, bloq = 0;
        for(int d = 0; d < 14; d++) {
            /* gira a direção para o hemisfério da normal (base simples: soma com a normal) */
            float dx = DIRS[d][0] + n[0] * 1.1f, dy = DIRS[d][1] + n[1] * 1.1f, dz = DIRS[d][2] + n[2] * 1.1f;
            float dl = sqrtf(dx * dx + dy * dy + dz * dz);
            if(dl < 1e-3f) continue;
            dx /= dl; dy /= dl; dz /= dl;
            if(dx * n[0] + dy * n[1] + dz * n[2] < 0.1f) continue;
            tot++;
            for(int k = 0; k < 4; k++) {
                float x = p[0] + n[0] * 0.05f + dx * PASSOS[k], y = p[1] + n[1] * 0.05f + dy * PASSOS[k], z = p[2] + n[2] * 0.05f + dz * PASSOS[k];
                if(y < 0.0f || ao_cheio(x, y, z)) { bloq++; break; }
            }
        }
        float aberto = tot ? 1.0f - bloq / (float)tot : 1.0f;
        ao_v[id] = 0.42f + 0.58f * aberto;
    }
    for(int id = 0; id < n_vu; id++) baixo_v[id] = 0.32f * limitar((0.46f - vl[id][1]) / 0.36f, 0, 1);
    for(int f = 0; f < carro_nf; f++) {
        float b = 0;
        for(int v = 0; v < carro_faces[f].n; v++) b += baixo_v[face_vid[f][v]];
        baixo_f[f] = carro_faces[f].n ? b / carro_faces[f].n : 0;
        if(carro_faces[f].tex) baixo_f[f] *= 0.5f;
    }
    for(int f = 0; f < carro_nf; f++) {
        const face_t *fc = &carro_faces[f];
        float a = 0;
        for(int v = 0; v < fc->n; v++) a += ao_v[face_vid[f][v]];
        ao_f[f] = fc->n ? a / fc->n : 1.0f;
        if(fc->tex) ao_f[f] = 0.5f + 0.5f * ao_f[f];      /* decalques: mais suave */
    }
}
static void compilar_malha(void) {
    #define HSZ 16384
    static int16_t tab[HSZ];
    static int32_t chx[MAX_VU], chy[MAX_VU], chz[MAX_VU]; static uint8_t chg[MAX_VU];
    memset(tab, -1, sizeof(tab));
    n_vu = 0;
    for(int f = 0; f < carro_nf; f++) {
        const face_t *fc = &carro_faces[f];
        for(int v = 0; v < fc->n; v++) {
            int32_t x = (int32_t)lroundf(fc->v[v][0] * 10000.0f), y = (int32_t)lroundf(fc->v[v][1] * 10000.0f), z = (int32_t)lroundf(fc->v[v][2] * 10000.0f);
            uint8_t g = fc->roda;
            uint32_t h = h32((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ (uint32_t)z * 83492791u ^ g * 2654435761u) & (HSZ - 1);
            int id = -1;
            while(tab[h] >= 0) {
                int c = tab[h];
                if(chx[c] == x && chy[c] == y && chz[c] == z && chg[c] == g) { id = c; break; }
                h = (h + 1) & (HSZ - 1);
            }
            if(id < 0) {
                if(n_vu >= MAX_VU) { printf("ERRO: MAX_VU\n"); arch_exit(); }
                id = n_vu++;
                chx[id] = x; chy[id] = y; chz[id] = z; chg[id] = g; tab[h] = (int16_t)id;
                vl[id][0] = fc->v[v][0]; vl[id][1] = fc->v[v][1]; vl[id][2] = fc->v[v][2]; vl[id][3] = 1.0f;
            }
            face_vid[f][v] = (uint16_t)id;
        }
    }
    memset(vu_st, 0, sizeof(vu_st));
    for(int f = 0; f < carro_nf; f++) {
        const face_t *fc = &carro_faces[f];
        float ax = fabsf(fc->nrm[0]), ay = fabsf(fc->nrm[1]), az = fabsf(fc->nrm[2]);
        for(int v = 0; v < fc->n; v++) {
            const float *pv = fc->v[v];
            if(ax >= ay && ax >= az) { face_pj[f][v][0] = pv[2]; face_pj[f][v][1] = pv[1]; }
            else if(ay >= az)        { face_pj[f][v][0] = pv[0]; face_pj[f][v][1] = pv[2]; }
            else                     { face_pj[f][v][0] = pv[0]; face_pj[f][v][1] = pv[1]; }
        }
    }
    /* listas por nível de detalhe (os mesmos filtros que eram feitos face a face) */
    const uint32_t mask_cockpit = (1u << C_CAPACETE) | (1u << C_VISOR) | (1u << C_HALO);
    for(int l = 0; l < N_LST; l++) n_lista[l] = 0;
    for(int f = 0; f < nf_simples; f++) lista_f[LST_SIMPLES][n_lista[LST_SIMPLES]++] = (uint16_t)f;
    for(int f = nf_simples; f < carro_nf; f++) {
        const face_t *fc = &carro_faces[f];
        if(!(fc->miudo & (2 | 16))) lista_f[LST_JOGADOR][n_lista[LST_JOGADOR]++] = (uint16_t)f;
        if(!(fc->miudo & (2 | 4 | 8))) lista_f[LST_RIVAL][n_lista[LST_RIVAL]++] = (uint16_t)f;
        if(!(fc->miudo & 1)) lista_f[LST_MEDIO][n_lista[LST_MEDIO]++] = (uint16_t)f;
        if(f < nf_cockpit && !(fc->miudo & (2 | 16)) && !((mask_cockpit >> fc->cor) & 1) && !fc->atras)
            lista_f[LST_COCKPIT][n_lista[LST_COCKPIT]++] = (uint16_t)f;
    }
}
static void montar_listas_v(void) {
    static uint8_t marca[MAX_VU];
    for(int l = 0; l < N_LST; l++) {
        memset(marca, 0, sizeof(marca));
        n_lista_v[l] = 0;
        for(int i = 0; i < n_lista[l]; i++) {
            int f = lista_f[l][i];
            if(carro_faces[f].roda) continue;
            for(int v = 0; v < carro_faces[f].n; v++) {
                int id = face_vid[f][v];
                if(!marca[id]) { marca[id] = 1; lista_v[l][n_lista_v[l]++] = (uint16_t)id; }
            }
        }
    }
}
static int chave_face(const face_t *f) {
    int g = f->tex ? 20 + f->tex : (f->cor == C_CARBONO && !f->suave) ? 10 : e_pintura(f->cor) ? 5 : 0;
    return g * 8 + f->roda;
}
static int cmp_face(const void *a, const void *b) {
    return chave_face((const face_t *)a) - chave_face((const face_t *)b);
}
static void montar_carro(void) {
    /* x = direita, y = cima, z = frente; origem no chão, entre os eixos */
    carro_nf = 0;
    montar_carro_simples();
    nf_simples = carro_nf;
    montar_carro_detalhado();
    for(int i = 0; i < carro_nf; i++) {
        float zm = -99;
        for(int v = 0; v < carro_faces[i].n; v++) if(carro_faces[i].v[v][2] > zm) zm = carro_faces[i].v[v][2];
        carro_faces[i].atras = zm < -0.05f;
#ifndef COCKPIT_2D
        if(i >= tampa_ini && i < tampa_fim) carro_faces[i].atras = 2;   /* "tampa" da abertura: fora da visão de cockpit */
        if(i >= nf_simples) {                       /* o chassi é maciço: o que fica DENTRO da abertura sai do cockpit */
            float cxm = 0, cym = 0, czm = 0;
            const face_t *ff = &carro_faces[i];
            for(int v = 0; v < ff->n; v++) { cxm += ff->v[v][0]; cym += ff->v[v][1]; czm += ff->v[v][2]; }
            cxm /= ff->n; cym /= ff->n; czm /= ff->n;
            if(fabsf(cxm) < 0.168f && czm > -0.10f && czm < 0.86f && cym > 0.30f && cym < 0.64f) carro_faces[i].atras = 2;
        }
#endif
        /* área da face (fórmula de Newell): as bem pequenas são "miúdas" e
         * somem no nível de detalhe médio dos rivais */
        float ax = 0, ay = 0, az = 0;
        const face_t *f = &carro_faces[i];
        for(int v = 0; v < f->n; v++) {
            const float *a = f->v[v], *b = f->v[(v + 1) % f->n];
            ax += a[1] * b[2] - a[2] * b[1]; ay += a[2] * b[0] - a[0] * b[2]; az += a[0] * b[1] - a[1] * b[0];
        }
        float area = 0.5f * sqrtf(ax * ax + ay * ay + az * az);
        uint8_t m = 0;
        if(i >= nf_simples) {
            if(face_lod[i] == 2)                                     /* só no médio... */
                m = (area < AREA_MIUDA || f->modo == 1) ? 3 : 2;     /* ...e miúda: nunca */
            else if(face_lod[i] == 1 || area < AREA_MIUDA || f->modo == 1 ||
                    (i >= extra_ini && i < extra_fim)) m = 1;        /* só no completo */
        }
        /* bit 4 = "só no carro do JOGADOR": sulcos do pneu de 12 lados e peças
         * minúsculas (num rival a alguns metros não aparecem e custavam caro) */
        if(i >= nf_simples && face_lod[i] != 2 &&
           (((f->cor == C_SULCO || f->cor == C_FAIXA_PNEU) && face_lod[i] == 1) || area < 0.0035f ||
            (i >= extra_ini && i < extra_fim))) m |= 4;     /* detalhes extras: só no jogador */
        /* PNEUS: os de 12 lados (bit 8) só no jogador; os de 8 lados (bit 16)
         * no médio E nos rivais completos (antes eram só do médio) */
        if(i >= nf_simples && f->roda >= 1 && f->roda <= 4) {
            if(face_lod[i] == 1) m |= 8;
            else if(face_lod[i] == 2) m = 16 | (m == 3 ? 1 : 0);
        }
        carro_faces[i].miudo = m;
        if(carro_faces[i].miudo == 1) n_miudas++;
    }
    for(int i = nf_simples; i < carro_nf; i++)
        if(carro_faces[i].cor == C_CAPACETE || carro_faces[i].cor == C_VISOR) carro_faces[i].roda = 5;
    /* ORDEM DE ENVIO: agrupa as faces que usam o mesmo cabeçalho de textura
     * (cor lisa / carbono / cada decalque) e a mesma matriz de roda. O
     * z-buffer do PowerVR não depende da ordem, então a imagem é a mesma,
     * mas o chip troca de textura/matriz poucas vezes por carro. Cada trecho
     * (simples, cockpit, resto) é ordenado separadamente. */
    qsort(carro_faces, nf_simples, sizeof(face_t), cmp_face);
    qsort(carro_faces + nf_simples, nf_cockpit - nf_simples, sizeof(face_t), cmp_face);
    qsort(carro_faces + nf_cockpit, carro_nf - nf_cockpit, sizeof(face_t), cmp_face);
    compilar_malha();
    montar_listas_v();
    assar_oclusao_carro();
    printf("carro: %d faces simples, %d detalhado, %d vertices unicos\n", nf_simples, carro_nf - nf_simples, n_vu);
    if(carro_nf > MAX_FACES_CARRO) { printf("ERRO: MAX_FACES_CARRO pequeno (%d)\n", carro_nf); arch_exit(); }
}

/* ======================================================================== */
/* CENÁRIO: prédios reais do OSM, árvores, morros e skyline de SP ao longe   */
/* ======================================================================== */

typedef struct { float c[3], r[3], f[3]; float hx, hy, hz; uint32_t cor, topo; uint8_t tex, dmax, fixo, osm; } predio_t;   /* fixo: não some pelo tamanho (pés de placa) */   /* dmax: distância máx. x10 m (0 = padrão) */
#define MAX_PREDIOS 2000
static predio_t predios[MAX_PREDIOS];
static int n_predios;

typedef struct { float p[3]; float alt, raio, giro; uint32_t cor;
                 float b[4][3], n[4][3]; int pronta; float dy_grosso; } arvore_t;   /* b/n: copa pré-calculada */
#define MAX_ARVORES 1400
static arvore_t arvores[MAX_ARVORES];
static int n_arvores;
static char diag_txt[64];

typedef struct { float p[3][3]; uint32_t cor; } tri_t;
#define MAX_MORROS 80
static tri_t morros[MAX_MORROS];
static int n_morros;
#define MAX_SKY 70
static predio_t skyline[MAX_SKY];
static int n_sky;

#define MAX_CIDADE 90
static predio_t cidade[MAX_CIDADE];     /* prédios residenciais ao longe (sem neblina, tingidos) */
static int n_cidade;

/* ======================================================================== */
/* TERRENO: malha low poly de relevo em volta do circuito                    */
/* ======================================================================== */
/* Grade de células de 40 m cobrindo o circuito + 300 m de folga. A altura
 * de cada vértice vem da própria pista (média ponderada pela distância das
 * fatias próximas, que têm o relevo real do SRTM), 2 m abaixo dela para a
 * pista e a grama ficarem sempre por cima; longe da pista o chão sobe um
 * pouco (morros do entorno) e ganha um "ruído" que dá as facetas low poly.
 * Onde o OSM marca água ao lado da pista, o vértice vira LAGO. */

#define TER_CEL   40.0f
#define TER_MAX   72
static float   ter_h[TER_MAX][TER_MAX];
static uint32_t ter_cor[TER_MAX][TER_MAX];      /* luz assada por vértice */
static uint8_t ter_d[TER_MAX][TER_MAX];          /* distância até a pista (m, até 255) */
static uint8_t ter_agua[TER_MAX][TER_MAX];
static int     ter_nx, ter_nz;
static float   ter_x0, ter_z0;

static float terreno_em(float x, float z);
#include "relevo.h"
/* altura do RELEVO REAL (SRTM) num ponto do jogo, bilinear */
static float relevo_em(float x, float z) {
    float u = (x - REL_X0) / REL_CEL, v = (z - REL_Z0) / REL_CEL;
    u = limitar(u, 0, REL_NX - 1.001f); v = limitar(v, 0, REL_NZ - 1.001f);
    int i = (int)u, j = (int)v;
    float fu = u - i, fv = v - j;
    return ((RELEVO[j][i] * (1 - fu) + RELEVO[j][i + 1] * fu) * (1 - fv) + (RELEVO[j + 1][i] * (1 - fu) + RELEVO[j + 1][i + 1] * fu) * fv) * 0.1f;
}
static void montar_terreno(void) {
    float mnx = 1e9f, mxx = -1e9f, mnz = 1e9f, mxz = -1e9f;
    for(int k = 0; k < NF; k++) {
        if(F[k].x < mnx) mnx = F[k].x;
        if(F[k].x > mxx) mxx = F[k].x;
        if(F[k].z < mnz) mnz = F[k].z;
        if(F[k].z > mxz) mxz = F[k].z;
    }
    /* com o relevo real, o terreno vai o mais longe que a grade permite (~2,8 km) */
    float mgx = ((TER_MAX - 2) * TER_CEL - (mxx - mnx)) * 0.5f, mgz = ((TER_MAX - 2) * TER_CEL - (mxz - mnz)) * 0.5f;
    if(mgx < 300.0f) mgx = 300.0f;
    if(mgz < 300.0f) mgz = 300.0f;
    ter_x0 = mnx - mgx; ter_z0 = mnz - mgz;
    ter_nx = (int)((mxx - mnx + 2 * mgx) / TER_CEL) + 2;
    ter_nz = (int)((mxz - mnz + 2 * mgz) / TER_CEL) + 2;
    if(ter_nx > TER_MAX) ter_nx = TER_MAX;
    if(ter_nz > TER_MAX) ter_nz = TER_MAX;
    for(int j = 0; j < ter_nz; j++)
        for(int i = 0; i < ter_nx; i++) {
            float x = ter_x0 + i * TER_CEL, z = ter_z0 + j * TER_CEL;
            float sw = 0, sy = 0, dmin = 1e18f, teto = 1e9f;
            int kmin = 0;
            for(int k = 0; k < NF; k++) {
                float dx = F[k].x - x, dz = F[k].z - z, d2 = dx * dx + dz * dz;
                float w = 1.0f / ((d2 + 400.0f) * (d2 + 400.0f));
                sw += w; sy += w * F[k].y;
                if(d2 < dmin) { dmin = d2; kmin = k; }
                /* "teto": a pista mais BAIXA num raio de 62 m. Qualquer ponto de
                 * uma célula de 40 m está a menos de 57 m dos 4 cantos; se todos
                 * os cantos ficam abaixo desse teto, a superfície interpolada
                 * nunca atravessa o asfalto (antes, na Laranjinha, um trecho de
                 * pista mais alto ali perto levantava o chão por cima da pista). */
                if(d2 < 62.0f * 62.0f && F[k].y < teto) teto = F[k].y;
            }
            float d = sqrtf(dmin);
            /* perto: acompanha a pista (sem degrau); de 60 a 180 m passa para o
             * RELEVO REAL (SRTM): morros, vales e as represas em volta */
            float h_perto = sy / sw - 0.9f, h_real = relevo_em(x, z) - 0.6f;
            float t = limitar((d - 60.0f) / 120.0f, 0, 1);
            t = t * t * (3 - 2 * t);
            float h = h_perto * (1 - t) + h_real * t;
            /* lago: água do lado certo da pista, a uma distância razoável */
            const fatia_t *f = &F[kmin];
            float lat = (x - f->x) * f->rx + (z - f->z) * f->rz;
            int agua = d > 22.0f && d < 160.0f &&
                       (f->amb & (lat > 0 ? AMB_AGUA_DIR : AMB_AGUA_ESQ));
            ter_d[j][i] = (uint8_t)(d > 255.0f ? 255.0f : d);
            if(!agua && REL_AGUA > -9000.0f && d > 250.0f && relevo_em(x, z) < REL_AGUA + 0.5f) {
                ter_agua[j][i] = 1;                    /* REPRESA real (Guarapiranga/Billings) */
                ter_h[j][i] = REL_AGUA - 0.4f;
                continue;
            }
            ter_agua[j][i] = (uint8_t)agua;
            if(agua) h = f->y - 3.5f;
            if(h > teto - 1.0f) h = teto - 1.0f;     /* margem menor: sem barranco em volta de tudo */
            ter_h[j][i] = h;
        }
    /* borda da faixa de grama: terreno real naquele ponto (um pouco acima,
     * para cobrir a emenda), nunca acima de pista - 1,2 m nem abaixo de - 14 m */
    for(int k = 0; k < NF; k++)
        for(int l = 0; l < 2; l++) {
            float lat = l ? 26.0f : -26.0f;
            float x = F[k].x + F[k].rx * lat, z = F[k].z + F[k].rz * lat;
            float t = terreno_em(x, z) + 0.15f;
            borda_y[k][l] = limitar(t, F[k].y - 3.0f, F[k].y - 0.45f);     /* até 3 m (antes 14: caídas íngremes) */
            ter_saia[k][l] = terreno_em(F[k].x + F[k].rx * (lat > 0 ? 26.8f : -26.8f), F[k].z + F[k].rz * (lat > 0 ? 26.8f : -26.8f));
            if(F[k].box && !l) borda_y[k][l] = F[k].y - 0.05f;     /* pit lane: plano, na altura da pista */
        }
}

/* Altura do terreno num ponto, EXATAMENTE como ele é desenhado: cada quad
 * (cantos 0=(i,j) 1=(i+1,j) 2=(i+1,j+1) 3=(i,j+1)) vira os triângulos
 * (0,1,3) e (1,3,2) — diagonal entre os cantos 1 e 3. passo = 1 (malha fina,
 * perto) ou 2 (malha grossa de 80 m, usada a mais de 280 m da câmera). */
static float terreno_passo(float x, float z, int passo) {
    float fx = (x - ter_x0) / TER_CEL, fz = (z - ter_z0) / TER_CEL;
    fx = limitar(fx, 0, ter_nx - 1.001f - passo); fz = limitar(fz, 0, ter_nz - 1.001f - passo);
    int i = (int)fx, j = (int)fz;
    if(passo == 2) { i &= ~1; j &= ~1; }
    float tx = (fx - i) / passo, tz = (fz - j) / passo;
    float h0 = ter_h[j][i], h1 = ter_h[j][i + passo], h2 = ter_h[j + passo][i + passo], h3 = ter_h[j + passo][i];
    if(tx + tz <= 1.0f) return h0 + (h1 - h0) * tx + (h3 - h0) * tz;
    return h2 + (h3 - h2) * (1.0f - tx) + (h1 - h2) * (1.0f - tz);
}
static float terreno_em(float x, float z) { return terreno_passo(x, z, 1); }

/* Chão sob um objeto posto a "lat" metros da pista no metro s:
 * até 26 m é a grama que desce da zebra (2 m em 17 m); depois, o terreno. */
static float chao_em(float s, float lat, const float p[3]) {
    /* O chão VISÍVEL no ponto é o mais alto entre o terreno e as faixas de
     * grama (até 26 m) de TODAS as fatias que cobrem o ponto — não só a da
     * fatia onde o objeto foi posto. Nos grampos (Bico de Pato) as faixas
     * se dobram e se sobrepõem, e o terreno ali é rebaixado para não cruzar
     * a pista: usar só a fatia de origem deixava árvores flutuando. */
    (void)s; (void)lat;
    float melhor = terreno_em(p[0], p[2]);
    for(int k = 0; k < NF; k++) {
        float dx = p[0] - F[k].x, dz = p[2] - F[k].z;
        if(dx * dx + dz * dz > 30.0f * 30.0f) continue;
        float la = dx * F[k].rx + dz * F[k].rz;                 /* lateral */
        float al = dx * -F[k].rz + dz * F[k].rx;                /* ao longo */
        if(fabsf(al) > PASSO * 0.6f || fabsf(la) > 26.0f) continue;
        float y = faixa_y(k, la);
        if(y > melhor) melhor = y;
    }
    return melhor;
}

/* O ponto cai na faixa de grama (até 28 m) de um trecho da pista DIFERENTE
 * do seu (mais de 60 m de pista de distância)? Nos grampos essas faixas só
 * aparecem quando o outro trecho está na tela; uma árvore ali "flutua". */
static int sobre_outra_perna(const float p[3], int k_origem) {
    for(int k = 0; k < NF; k++) {
        int dk = abs(k - k_origem); if(dk > NF / 2) dk = NF - dk;
        if(dk * PASSO < 60) continue;
        float dx = p[0] - F[k].x, dz = p[2] - F[k].z;
        if(dx * dx + dz * dz > 34.0f * 34.0f) continue;
        float la = dx * F[k].rx + dz * F[k].rz, al = dx * -F[k].rz + dz * F[k].rx;
        if(fabsf(al) <= PASSO * 0.6f && fabsf(la) <= 30.0f) return 1;
    }
    return 0;
}

/* Caixa alinhada com a pista no metro s (lat = lateral do centro, base = altura do pé). */
static int checar_perna;     /* 1: não cria objeto que cairia em cima de OUTRO trecho da pista */
static void estrutura_sombra(float s, float larg_lat, float prof, float h);
static predio_t *caixa_na_pista(float s, float lat, float larg, float prof, float alt, float base,
                                uint32_t cor, uint32_t topo) {
    if(n_predios >= MAX_PREDIOS) return NULL;
    if(checar_perna) {
        float q[3];
        ponto_pista(fmodf(s + COMPR, COMPR), lat, q, 0, 0);
        int k0 = (int)(fmodf(s + COMPR, COMPR) / PASSO) % NF;
        float r = 0.5f * (larg > prof ? larg : prof) + 30.0f;    /* além do muro do outro trecho (26 m) */
        for(int k = 0; k < NF; k++) {
            int dk = abs(k - k0); if(dk > NF / 2) dk = NF - dk;
            if(dk * PASSO < fabsf(lat) + 20.0f) continue;        /* o próprio trecho */
            float dx = F[k].x - q[0], dz = F[k].z - q[2];
            if(dx * dx + dz * dz < r * r) return NULL;
        }
    }
    predio_t *d = &predios[n_predios++];
    d->tex = 0; d->dmax = 0;
    float p[3], fr[3], dir[3];
    ponto_pista(s, lat, p, fr, dir);
    float y0 = p[1] + base, y1 = p[1] + base + alt;          /* pé e topo */
    int k_box = (int)(fmodf(s + COMPR, COMPR) / PASSO) % NF;
    if(F[k_box].box && lat < -8.6f && lat > -46.0f) {         /* área dos boxes: na altura do pit lane */
        float g = p[1], t = terreno_em(p[0], p[2]);
        y1 = g + base + alt;
        int fund = alt >= 8.0f && base <= 0.5f;           /* só o corpo do prédio tem fundação */
        y0 = fund ? (t < g ? t : g) - 0.5f : g + base - (base <= 0.5f ? 0.3f : 0.0f);
    }
    else if(fabsf(lat) > 8.6f) {                              /* fora do asfalto: assenta no chão REAL */
        float g = chao_em(s, lat, p);
        y1 = g + base + alt;
        y0 = base > 0.5f ? g + base : g - 1.5f;              /* pé enterrado: nunca flutua */
    }
    d->c[0] = p[0]; d->c[1] = (y0 + y1) * 0.5f; d->c[2] = p[2];
    d->r[0] = dir[0]; d->r[1] = 0; d->r[2] = dir[2];
    d->f[0] = dir[2]; d->f[1] = 0; d->f[2] = -dir[0];
    d->hx = larg * 0.5f; d->hy = (y1 - y0) * 0.5f; d->hz = prof * 0.5f;
    d->cor = cor; d->topo = topo;
    return d;
}

static inline float dist2_cam(const float p[3]);
/* ARQUIBANCADAS: uma RAMPA inclinada com a torcida pintada (dezenas de
 * fileiras numa face só), mureta na frente, laterais, parede no fundo e
 * cobertura em pilares. Muito mais degraus e gente com poucos polígonos. */
typedef struct {
    float f[3];          /* centro da borda da frente, no chão */
    float t[3], o[3];    /* ao longo da pista; para fora (horizontal) */
    float meio, prof;    /* meio comprimento; profundidade */
    float h0, h1;        /* altura da mureta da frente e do topo do fundo */
    uint32_t cor_teto;
    uint8_t teto;
    uint8_t viz;                     /* bits 1/2: bloco vizinho encostado do lado -t / +t (sem parede lateral) */
    uint8_t nb, pais[8];             /* bandeiras na torcida: quantas e de que país */
    float bu[8], bd[8], bf[8];       /* posição (ao longo -1..1, na rampa 0..1) e fase */
} arq_t;
#define MAX_ARQ 64
static arq_t arqs[MAX_ARQ];
static int n_arqs;
static void arquibancada(float s, float lat_frente, float comp, float prof, float h1, uint32_t cor_teto, int teto) {
    if(n_arqs >= MAX_ARQ) return;
    arq_t *a = &arqs[n_arqs++];
    float p0[3], p1[3], fr[3], dir[3];
    float lado = lat_frente < 0 ? -1.0f : 1.0f;
    ponto_pista(s, lat_frente, p0, fr, dir);
    ponto_pista(s, lat_frente + lado, p1, 0, 0);
    float ox = p1[0] - p0[0], oz = p1[2] - p0[2], l = sqrtf(ox * ox + oz * oz);
    a->o[0] = ox / l; a->o[1] = 0; a->o[2] = oz / l;
    a->t[0] = dir[0]; a->t[1] = 0; a->t[2] = dir[2];
    l = sqrtf(a->t[0] * a->t[0] + a->t[2] * a->t[2]); a->t[0] /= l; a->t[2] /= l;
    float g = fabsf(lat_frente) > 20.0f ? chao_em(s, lat_frente, p0) + 0.3f : p0[1];
    a->f[0] = p0[0]; a->f[1] = g; a->f[2] = p0[2];
    a->meio = comp * 0.5f; a->prof = prof; a->h0 = 1.3f; a->h1 = h1;
    a->cor_teto = cor_teto; a->teto = (uint8_t)teto;
    /* bandeiras: uma a cada ~5 m de arquibancada, países campeões da F1 */
    int nb = (int)(comp / 7.0f); if(nb > 4) nb = 4; if(nb < 2) nb = 2;
    a->nb = (uint8_t)nb;
    for(int k = 0; k < nb; k++) {
        uint32_t h = h32((uint32_t)(n_arqs * 31 + k) * 2654435761u);
        a->pais[k] = (uint8_t)(h % 10 < 3 ? 0 : h % 8);            /* Brasil aparece mais */
        a->bu[k] = -0.9f + 1.8f * (k + 0.5f) / nb + ((h >> 8) % 100 - 50) * 0.002f;
        a->bd[k] = 0.05f + ((h >> 16) % 100) * 0.0030f;     /* nas fileiras da frente e do meio */
        a->bf[k] = (h >> 24) * 0.0245f;
    }
}

static int n_arq_dup;
static void ligar_arquibancadas(void) {
    /* 1º: tira arquibancadas SOBREPOSTAS (duas no mesmo lugar = as fileiras
     * "brigam" no z-buffer e a torcida pisca mesmo com a câmera parada) */
    int m = 0;
    for(int i = 0; i < n_arqs; i++) {
        const arq_t *a = &arqs[i];
        int dup = 0;
        for(int j = 0; j < m && !dup; j++) {
            const arq_t *b = &arqs[j];
            float dx = a->f[0] - b->f[0], dz = a->f[2] - b->f[2];
            float cosang = a->o[0] * b->o[0] + a->o[2] * b->o[2];
            if(cosang > 0.5f && dx * dx + dz * dz < 4.0f * 4.0f) dup = 1;
        }
        if(dup) { n_arq_dup++; continue; }
        if(m != i) arqs[m] = arqs[i];
        m++;
    }
    n_arqs = m;
    for(int i = 0; i < n_arqs; i++) {
        arq_t *a = &arqs[i];
        for(int j = 0; j < n_arqs; j++) {
            if(i == j) continue;
            const arq_t *b = &arqs[j];
            float dx = b->f[0] - a->f[0], dz = b->f[2] - a->f[2];
            float at = dx * a->t[0] + dz * a->t[2], ao = dx * a->o[0] + dz * a->o[2];
            if(fabsf(ao) > 3.0f || fabsf(fabsf(at) - (a->meio + b->meio)) > 2.5f) continue;
            a->viz |= at < 0 ? 1 : 2;
        }
    }
}
static float foco[3];            /* onde está o jogador (para a torcida se agitar) */
/* ARQUIBANCADA 3D (perto): cada fileira é um degrau de verdade (espelho +
 * piso), a torcida senta no piso, corredores de escada separam os setores,
 * guarda-corpo na frente e paredes laterais em degraus nas pontas. Os
 * vértices são transformados uma vez por canto de degrau; o resto sai por
 * interpolação no espaço da câmera (a transformação é linear). */
static void arq_degraus(const arq_t *a, float ln) {
    int nr = (int)(ln / 1.4f); if(nr > 9) nr = 9; if(nr < 4) nr = 4;
    float dh = a->h1 - a->h0;
    vv_t N[2][11], K[2][11], M[2];               /* nariz e fundo do piso de cada degrau; topo da mureta */
    for(int sd = 0; sd < 2; sd++) {
        float sg = sd ? 1.0f : -1.0f;
        float bx = a->f[0] + a->t[0] * sg * a->meio, bz = a->f[2] + a->t[2] * sg * a->meio;
        para_vista(bx, a->f[1] + a->h0, bz, 0, &M[sd]);
        for(int r = 0; r < nr; r++) {
            float D0 = a->prof * r / nr, D1 = a->prof * (r + 1) / nr, H = a->f[1] + a->h0 + dh * (r + 1) / nr;
            para_vista(bx + a->o[0] * D0, H, bz + a->o[2] * D0, 0, &N[sd][r]);
            para_vista(bx + a->o[0] * D1, H, bz + a->o[2] * D1, 0, &K[sd][r]);
        }
    }
    vv_t z0, z1;                                  /* "para cima" no espaço da câmera */
    para_vista(a->f[0], a->f[1], a->f[2], 0, &z0); para_vista(a->f[0], a->f[1] + 1.0f, a->f[2], 0, &z1);
    float up[3] = { z1.x - z0.x, z1.y - z0.y, z1.z - z0.z };
    float nf[3] = { -a->o[0], 0, -a->o[2] }, nu[3] = { 0, 1, 0 };
    uint32_t c_piso = cor_escala(0xFFC6C8CA, sombra_luz(nu)), c_esp = cor_escala(0xFF9EA2A8, sombra_luz(nf));
    #define LERPV(o, A, B, t) do { (o).x = (A).x + ((B).x - (A).x) * (t); (o).y = (A).y + ((B).y - (A).y) * (t); (o).z = (A).z + ((B).z - (A).z) * (t); } while(0)
    /* degraus: espelho (vertical) e piso */
    for(int r = 0; r < nr; r++) {
        vv_t q[4];
        q[0] = r ? K[0][r - 1] : M[0]; q[1] = r ? K[1][r - 1] : M[1]; q[2] = N[1][r]; q[3] = N[0][r];
        for(int v = 0; v < 4; v++) q[v].c = c_esp;
        poligono(q, 4);
        q[0] = N[0][r]; q[1] = N[1][r]; q[2] = K[1][r]; q[3] = K[0][r];
        for(int v = 0; v < 4; v++) q[v].c = c_piso;
        poligono(q, 4);
    }
    /* paredes laterais em degraus (só nas pontas do conjunto) */
    for(int sd = 0; sd < 2; sd++) {
        if(a->viz & (sd ? 2 : 1)) continue;
        float nl[3] = { a->t[0] * (sd ? 1.0f : -1.0f), 0, a->t[2] * (sd ? 1.0f : -1.0f) };
        uint32_t cl = cor_escala(0xFFA8ACB2, sombra_luz(nl));
        for(int r = 0; r < nr; r++) {
            float H = a->h0 + dh * (r + 1) / nr + 1.5f;     /* altura acima do pé (-1,5 m) */
            vv_t q[4] = { N[sd][r], K[sd][r], K[sd][r], N[sd][r] };
            q[2].x = K[sd][r].x - up[0] * H; q[2].y = K[sd][r].y - up[1] * H; q[2].z = K[sd][r].z - up[2] * H;
            q[3].x = N[sd][r].x - up[0] * H; q[3].y = N[sd][r].y - up[1] * H; q[3].z = N[sd][r].z - up[2] * H;
            for(int v = 0; v < 4; v++) q[v].c = cl;
            poligono(q, 4);
        }
    }
    /* torcida sentada em cada degrau, interrompida nos corredores de escada */
    int na = (int)(a->meio * 2 / 11.0f); if(na < 1) na = 1;
    float gap = 0.65f / (2 * a->meio);
    float umax = a->meio * 2 / 8.0f;
    float lz = sombra_luz(nf);
    uint32_t cl = cor_escala(0xFFFFFFFF, limitar(lz * 1.15f + 0.12f, 0, 1.25f));
    float dxf = (a->f[0] - foco[0]), dzf = (a->f[2] - foco[2]);
    float agito = dxf * dxf + dzf * dzf < 140.0f * 140.0f ? 1.0f : 0.3f;
    (void)agito;
    pvr_prim(dist2_cam(a->f) > 18.0f * 18.0f ? &hdr_torcida_longe : &hdr_torcida, sizeof(pvr_poly_hdr_t));   /* a meia distância: suavizada */
    for(int r = 0; r < nr; r++) {
        vv_t bl, br;
        LERPV(bl, N[0][r], K[0][r], 0.45f); LERPV(br, N[1][r], K[1][r], 0.45f);
        float ola = fsin(t_clima * 3.0f - r * 0.45f + a->bf[0]);
        float pula = 1.0f + agito * 0.10f * (ola > 0.6f ? (ola - 0.6f) * 2.5f : 0.0f);
        int fil = (r + (int)(a->bf[1] * 3)) & 7;
        float v0 = (fil * 32 + 1) / 256.0f, v1 = (fil * 32 + 26) / 256.0f, du = (r & 1) * 0.37f;
        for(int sg = 0; sg <= na; sg++) {
            float s0 = sg == 0 ? 0.0f : (float)sg / (na + 1) + gap, s1 = sg == na ? 1.0f : (float)(sg + 1) / (na + 1) - gap;
            vv_t a0, a1;
            LERPV(a0, bl, br, s0); LERPV(a1, bl, br, s1);
            vvt_t f4[4];
            const vv_t *pp[2] = { &a0, &a1 };
            const float ss[2] = { s0, s1 };
            for(int e = 0; e < 2; e++) {
                vvt_t *lo = &f4[e], *hi = &f4[3 - e];
                lo->x = pp[e]->x + up[0] * 0.02f; lo->y = pp[e]->y + up[1] * 0.02f; lo->z = pp[e]->z + up[2] * 0.02f;
                hi->x = pp[e]->x + up[0] * pula; hi->y = pp[e]->y + up[1] * pula; hi->z = pp[e]->z + up[2] * pula;
                lo->u = hi->u = du + umax * ss[e]; lo->v = v1; hi->v = v0;
                lo->c = hi->c = cl; lo->o = hi->o = 0;
            }
            poligono_tex(f4, 4);
        }
    }
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
    /* guarda-corpo na frente (tubo) */
    float gc[3] = { a->f[0] - a->o[0] * 0.05f, a->f[1] + a->h0 + 0.95f, a->f[2] - a->o[2] * 0.05f };
    desenhar_caixa(gc, a->t, a->o, a->meio, 0.03f, 0.03f, 0xFFD8DCE0, 0xFFD8DCE0);
    #undef LERPV
}
static void desenhar_arquibancada(const arq_t *a) {
    /* 8 cantos: frente-baixo, frente-mureta, fundo-topo, fundo-baixo (x2 lados) */
    float w[8][3];
    for(int k = 0; k < 8; k++) {
        float sg = (k & 1) ? 1.0f : -1.0f;
        int fundo = k >= 4, alto = (k >> 1) & 1;
        float y = fundo ? (alto ? a->h1 : -1.5f) : (alto ? a->h0 : -1.5f);
        for(int e = 0; e < 3; e += 2) w[k][e] = a->f[e] + a->t[e] * sg * a->meio + (fundo ? a->o[e] * a->prof : 0);
        w[k][1] = a->f[1] + y;
    }
    vv_t q8[8];
    for(int k = 0; k < 8; k++) para_vista(w[k][0], w[k][1], w[k][2], 0, &q8[k]);
    /* normais das faces (mundo) */
    float dh = a->h1 - a->h0, ln = sqrtf(dh * dh + a->prof * a->prof);
    float nr[3] = { -a->o[0] * dh / ln, a->prof / ln, -a->o[2] * dh / ln };   /* rampa: para cima e para a pista */
    float nf[3] = { -a->o[0], 0, -a->o[2] }, nb[3] = { a->o[0], 0, a->o[2] };
    float nt0[3] = { -a->t[0], 0, -a->t[2] }, nt1[3] = { a->t[0], 0, a->t[2] };
    /* faces: índices dos cantos (frente-baixo 0/1, frente-mureta 2/3, fundo-baixo 4/5, fundo-topo 6/7) */
    static const uint8_t FACE[5][4] = { { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 5, 4, 6, 7 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    int perto3d = dist2_cam(a->f) < 38.0f * 38.0f;
    const float *NN[5] = { nr, nf, nb, nt0, nt1 };
    for(int fc = 0; fc < 5; fc++) {
        const float *n = NN[fc], *p0 = w[FACE[fc][0]];
        if(n[0] * (p0[0] - cam_p[0]) + n[1] * (p0[1] - cam_p[1]) + n[2] * (p0[2] - cam_p[2]) >= 0) continue;
        if((fc == 3 && (a->viz & 1)) || (fc == 4 && (a->viz & 2))) continue;   /* emenda com o bloco vizinho */

        float lz = sombra_luz(n);
        if(perto3d && (fc == 0 || fc == 3 || fc == 4)) continue;      /* perto: degraus 3D (depois do laço) */
        if(fc == 0) {          /* rampa com a torcida: 4 fileiras a cada 3 m de rampa */
            uint32_t cl = cor_escala(0xFFFFFFFF, lz);
            float vmax = ln / 6.0f, umax = a->meio * 2 / 4.0f;
            cl = cor_escala(0xFFFFFFFF, limitar(lz * 1.15f + 0.12f, 0, 1.25f));   /* torcida mais viva */
            float uv[4][2] = { { 0, vmax }, { umax, vmax }, { umax, 0 }, { 0, 0 } };
            /* 6 pedaços ao longo da arquibancada: a torcida "pula" (a textura
             * sobe e desce um pouco) e, com carros perto, faz OLA */
            float dxf = (a->f[0] - foco[0]), dzf = (a->f[2] - foco[2]);
            float agito = dxf * dxf + dzf * dzf < 140.0f * 140.0f ? 1.0f : 0.25f;
            pvr_prim(&hdr_torcida_longe, sizeof(pvr_poly_hdr_t));   /* longe: textura suavizada (não cintila) */
            const vv_t *A0 = &q8[FACE[fc][0]], *A1 = &q8[FACE[fc][1]], *A2 = &q8[FACE[fc][2]], *A3 = &q8[FACE[fc][3]];
            int nsg = agito > 0.5f ? 3 : 1;            /* longe: um pedaço só, sem animação */
            for(int sg = 0; sg < nsg; sg++) {
                float t0 = sg / (float)nsg, t1 = (sg + 1) / (float)nsg;
                float ola = sinf(t_clima * 3.0f - sg * 0.9f + a->bf[0]);
                float dv = nsg == 1 ? 0.0f : agito * (0.04f * (ola > 0 ? ola : 0) + 0.012f * sinf(t_clima * 9.0f + sg * 1.7f));
                vvt_t t[4];
                const vv_t *L0 = A0, *L1 = A1, *H1 = A2, *H0 = A3;
                float tt[4] = { t0, t1, t1, t0 };
                const vv_t *ba[4] = { L0, L0, H0, H0 }, *bb[4] = { L1, L1, H1, H1 };
                float vvv[4] = { uv[0][1], uv[1][1], uv[2][1], uv[3][1] };
                for(int v = 0; v < 4; v++) {
                    float k2 = tt[v];
                    t[v].x = ba[v]->x + (bb[v]->x - ba[v]->x) * k2;
                    t[v].y = ba[v]->y + (bb[v]->y - ba[v]->y) * k2;
                    t[v].z = ba[v]->z + (bb[v]->z - ba[v]->z) * k2;
                    t[v].u = umax * k2; t[v].v = vvv[v] + dv; t[v].c = cl;
                }
                poligono_tex(t, 4);
            }
            pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
            continue;
        }
        uint32_t c = cor_escala(fc == 1 ? 0xFFB8BCC2 : 0xFF9AA0A8, lz);   /* concreto */
        vvt_t t[4];
        for(int v = 0; v < 4; v++) {
            const vv_t *qq = &q8[FACE[fc][v]]; const float *ww = w[FACE[fc][v]];
            float ao = fc <= 2 ? ww[0] * a->t[0] + ww[2] * a->t[2] : ww[0] * a->o[0] + ww[2] * a->o[2];
            t[v].x = qq->x; t[v].y = qq->y; t[v].z = qq->z; t[v].u = ao * 0.25f; t[v].v = ww[1] * 0.5f;
            t[v].c = (FACE[fc][v] & 2) ? c : cor_escala(c, 0.66f);
        }
        pvr_prim(&hdr_concreto, sizeof(hdr_concreto));
        poligono_tex(t, 4);
        pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
    }
    if(perto3d) arq_degraus(a, ln);
    if(a->teto) {
        if(perto3d) {          /* pilares segurando a cobertura, na borda da frente dela */
            float yt = a->f[1] + a->h1 + 2.7f, dpil = a->prof * 0.30f;
            float yb = a->f[1] + a->h0 + (a->h1 - a->h0) * 0.30f;
            int np = (int)(a->meio * 2 / 9.5f) + 1;
            for(int k = 0; k <= np; k++) {
                float u = -0.96f + 1.92f * k / np;
                float pc[3] = { a->f[0] + a->t[0] * u * a->meio + a->o[0] * dpil, (yb + yt) * 0.5f,
                                a->f[2] + a->t[2] * u * a->meio + a->o[2] * dpil };
                desenhar_caixa(pc, a->t, a->o, 0.16f, (yt - yb) * 0.5f, 0.16f, 0xFFB8BCC2, 0xFFB8BCC2);
            }
        }
        /* COBERTURA em balanço: a parede do fundo SOBE até a laje (nada
         * solto no ar), laje grossa avançando sobre ~70% da torcida e uma
         * testeira colorida na borda da frente */
        float yt = a->f[1] + a->h1 + 2.7f;
        float pf[3] = { a->f[0] + a->o[0] * (a->prof - 0.2f), (a->f[1] + a->h1 + yt) * 0.5f, a->f[2] + a->o[2] * (a->prof - 0.2f) };
        desenhar_caixa(pf, a->t, a->o, a->meio, (yt - a->f[1] - a->h1) * 0.5f + 0.2f, 0.2f, 0xFF9AA0A8, 0xFF9AA0A8);
        float dl = a->prof * 0.72f;
        float c[3] = { a->f[0] + a->o[0] * (a->prof - dl * 0.5f), yt + 0.2f, a->f[2] + a->o[2] * (a->prof - dl * 0.5f) };
        desenhar_caixa(c, a->t, a->o, a->meio - 0.02f, 0.22f, dl * 0.5f, 0xFF7E848C, a->cor_teto);
        float bt[3] = { a->f[0] + a->o[0] * (a->prof - dl), yt + 0.15f, a->f[2] + a->o[2] * (a->prof - dl) };
        desenhar_caixa(bt, a->t, a->o, a->meio - 0.02f, 0.45f, 0.08f, a->cor_teto, a->cor_teto);   /* testeira */
    }
    /* BANDEIRAS tremulando (só de perto) */
    if(dist2_cam(a->f) < 130.0f * 130.0f) {
        float dh2 = a->h1 - a->h0;
        for(int k = 0; k < a->nb; k++) {
            float d = a->bd[k];
            float b0[3] = { a->f[0] + a->t[0] * a->bu[k] * a->meio + a->o[0] * d * a->prof,
                            a->f[1] + a->h0 + dh2 * d,
                            a->f[2] + a->t[2] * a->bu[k] * a->meio + a->o[2] * d * a->prof };
            float topo = 5.6f;                       /* mastro alto: o pano fica acima das cabeças */
            float mastro[3] = { b0[0], b0[1] + topo * 0.5f, b0[2] };
            desenhar_caixa(mastro, a->t, a->o, 0.05f, topo * 0.5f, 0.05f, 0xFF606468, 0xFF606468);
            /* pano de 1,2 x 0,8 m em 3 faixas verticais que ondulam */
            int pa = a->pais[k];
            float cu = (pa & 3) * 0.25f, cv = (pa >> 2) * 0.5f;
            vv_t q[2][4];
            for(int c = 0; c < 4; c++) {
                float x = c * 1.0f;                      /* bandeirão de 3 x 2 m (exagerado, como nos jogos da época) */
                float onda = sinf(t_clima * 6.5f + a->bf[k] - c * 1.3f) * 0.38f * (c / 3.0f);
                for(int r = 0; r < 2; r++) {
                    float y = b0[1] + topo - r * 2.0f - (c * c) * 0.04f;
                    para_vista(b0[0] + a->t[0] * x + a->o[0] * onda, y, b0[2] + a->t[2] * x + a->o[2] * onda, 0, &q[r][c]);
                }
            }
            uint32_t cl = cor_escala(0xFFFFFFFF, 0.9f * brilho + 0.1f);
            pvr_prim(&hdr_bandeiras, sizeof(hdr_bandeiras));
            for(int c = 0; c < 3; c++) {
                vvt_t t[4];
                const vv_t *p4[4] = { &q[0][c], &q[0][c + 1], &q[1][c + 1], &q[1][c] };
                const float uu[4] = { c / 3.0f, (c + 1) / 3.0f, (c + 1) / 3.0f, c / 3.0f }, vv[4] = { 0, 0, 1, 1 };
                for(int v = 0; v < 4; v++) {
                    t[v].x = p4[v]->x; t[v].y = p4[v]->y; t[v].z = p4[v]->z; t[v].c = cl;
                    t[v].u = cu + 0.008f + uu[v] * 0.234f; t[v].v = cv + 0.015f + vv[v] * 0.47f;
                }
                poligono_tex(t, 4);
            }
            pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
        }
    }
}

/* chão da faixa de grama EXATAMENTE como é desenhado (interpolado entre fatias) */
static float chao_preciso(float s, float lat) {
    s = fmodf(s + COMPR, COMPR);
    float fk = s / PASSO; int k = (int)fk; float t = fk - k;
    return faixa_y(k % NF, lat) * (1 - t) + faixa_y((k + 1) % NF, lat) * t;
}
static void placa_na_grama(float s, float lat, int marca) {
    if(n_predios + 3 > MAX_PREDIOS) return;
    float g0 = chao_preciso(s - 2.6f, lat), g1 = chao_preciso(s + 2.6f, lat), gm = chao_preciso(s, lat);
    float topo_chao = g0 > g1 ? g0 : g1; if(gm > topo_chao) topo_chao = gm;
    {   /* onde a grama da pista e o terreno real não batem (aterro), não põe placa:
         * visto do outro lado do circuito ela ficaria em "pernas de pau" */
        float pc[3];
        ponto_pista(s, lat, pc, 0, 0);
        float tt = terreno_passo(pc[0], pc[2], 1);
        if(topo_chao - tt > 1.2f) return;
    }
    float yb = topo_chao + 0.30f;                        /* borda de baixo do painel */
    float p[3], fr[3], dir[3];
    ponto_pista(s, lat, p, fr, dir);
    predio_t *d = &predios[n_predios++];
    d->c[0] = p[0]; d->c[1] = yb + 0.75f; d->c[2] = p[2];
    d->r[0] = dir[0]; d->r[1] = 0; d->r[2] = dir[2];
    d->f[0] = dir[2]; d->f[1] = 0; d->f[2] = -dir[0];
    d->hx = 3.0f; d->hy = 0.75f; d->hz = 0.06f;
    d->cor = 0xFFFFFFFF; d->topo = (uint32_t)marca; d->tex = 8; d->dmax = 20;   /* até 200 m: sempre na malha fina do terreno */
    for(int e = -1; e <= 1; e += 2) {                    /* pés */
        float gp = e < 0 ? g0 : g1, q[3];
        ponto_pista(s + e * 2.6f, lat, q, 0, 0);
        /* se a faixa de grama da pista não está sendo desenhada (trecho do outro
         * lado do circuito), o chão ali é o TERRENO: o pé desce até o mais baixo */
        float t1 = terreno_passo(q[0], q[2], 1), t2 = terreno_passo(q[0], q[2], 2);
        if(t1 < gp) gp = t1;
        if(t2 < gp) gp = t2;
        predio_t *pe = &predios[n_predios++];
        pe->c[0] = q[0]; pe->c[2] = q[2];
        pe->c[1] = (gp - 0.4f + yb + 0.1f) * 0.5f; pe->hy = (yb + 0.1f - (gp - 0.4f)) * 0.5f;
        pe->r[0] = dir[0]; pe->r[1] = 0; pe->r[2] = dir[2];
        pe->f[0] = dir[2]; pe->f[1] = 0; pe->f[2] = -dir[0];
        pe->hx = 0.06f; pe->hz = 0.06f;
        pe->cor = pe->topo = 0xFF505458; pe->tex = 0; pe->dmax = 20; pe->fixo = 1;   /* some junto com o painel */
    }
}

/* Pórtico de largada: posição fixa; as luzes são desenhadas à parte. */
#define S_PORTICO 6.0f

static float postos_s[20];
static int8_t postos_l[20];
static int n_postos;
/* uma pessoa simples (corpo + cabeça), na lateral lat do metro s */
static void pessoa(float s, float lat, uint32_t roupa, int dmax) {
    static const uint32_t PELE[4] = { 0xFFE0B08C, 0xFFC08A62, 0xFF8A5A3C, 0xFFF0C8A4 };
    predio_t *d = caixa_na_pista(s, lat, 0.42f, 0.28f, 1.25f, 0.05f, roupa, roupa);
    if(d) d->dmax = (uint8_t)dmax;
    d = caixa_na_pista(s, lat, 0.22f, 0.22f, 0.24f, 1.32f, PELE[h32((uint32_t)(s * 7 + lat)) & 3], 0xFF2A2018);
    if(d) d->dmax = (uint8_t)dmax;
}
static void montar_marcos(void) {
    /* --- pórtico de largada (pilares + viga) --- */
    caixa_na_pista(S_PORTICO, -10.5f, 0.8f, 0.8f, 8.0f, 0, 0xFF5A5E66, 0xFF5A5E66);
    caixa_na_pista(S_PORTICO,  10.5f, 0.8f, 0.8f, 8.0f, 0, 0xFF5A5E66, 0xFF5A5E66);
    caixa_na_pista(S_PORTICO, 0, 21.8f, 1.2f, 1.4f, 6.6f, 0xFF30343A, 0xFF30343A);

    /* --- prédio dos boxes: lado esquerdo da reta, em blocos de 12 m que
     * acompanham a curva da pista; fachada alterna garagem/parede --- */
    /* PRÉDIO DOS BOXES com GARAGENS de verdade (de 10 m, abertas para o
     * pit): pilares entre elas, fundo na cor da equipe, pneus empilhados e
     * carrinho de ferramentas dentro; em cima, o andar das equipes com
     * vidraça (janelas) e marquise. Corpo do prédio atrás. */
    {
        static const uint32_t EQUIPE[8] = { 0xFFB8BCC2, 0xFFD0141C, 0xFFF5D010, 0xFFD01E28, 0xFF3C8CDC,
                                            0xFF1E3CA8, 0xFF1A2450, 0xFFF2F3F4 };
        int k = 0;
        for(float s = 4045.0f; s < COMPR + 35.0f; s += 10.0f, k++) {
            uint32_t eq = EQUIPE[(k / 2) % 8];
            predio_t *d;
            /* corpo (atrás das garagens) e andar de cima com vidraça */
            d = caixa_na_pista(s, -36.0f, 10.05f, 14.0f, 9.0f, -0.5f, 0xFFC8C6BE, 0xFFB8BCC2);
            if(d) d->tex = 7;                       /* concreto aparente (o paredão visto da Laranjinha) */
            d = caixa_na_pista(s, -26.0f, 10.05f, 6.0f, 4.6f, 4.0f, 0xFF9CB8D2, 0xFFB8BCC2);   /* vidro espelhado */
            if(d) d->tex = 6;
            caixa_na_pista(s, -22.6f, 10.05f, 1.2f, 0.35f, 4.0f, 0xFFF2F2F2, 0xFFF2F2F2);   /* marquise */
            /* pilar entre garagens, fundo colorido, piso */
            caixa_na_pista(s - 5.0f, -26.0f, 0.7f, 6.0f, 4.0f, -0.3f, 0xFFC8CCD2, 0xFFC8CCD2);
            caixa_na_pista(s, -28.9f, 9.3f, 0.2f, 4.0f, -0.3f, eq, eq);
            caixa_na_pista(s, -26.0f, 9.3f, 6.0f, 0.05f, 0.0f, 0xFF6A6E74, 0xFF6A6E74);
            /* dentro: pneus empilhados (2 pilhas) e carrinho de ferramentas na cor da equipe */
            d = caixa_na_pista(s - 3.2f, -28.0f, 0.65f, 0.65f, 1.3f, 0.0f, 0xFF1C1C20, 0xFF2A2A2E);
            if(d) d->dmax = 12;
            d = caixa_na_pista(s - 2.4f, -28.0f, 0.65f, 0.65f, 0.9f, 0.0f, 0xFF1C1C20, 0xFF2A2A2E);
            if(d) d->dmax = 12;
            d = caixa_na_pista(s + 3.0f, -28.3f, 1.6f, 0.7f, 1.1f, 0.0f, cor_escala(eq, 0.8f), 0xFF505458);
            if(d) d->dmax = 12;
            /* mecânicos na garagem (macacão na cor da equipe) */
            pessoa(s - 1.0f, -27.0f, cor_escala(eq, 0.9f), 5);
            pessoa(s + 1.4f, -25.2f, cor_escala(eq, 0.9f), 5);
            /* placa da equipe em cima da garagem */
            caixa_na_pista(s, -22.7f, 6.0f, 0.12f, 0.6f, 3.35f, eq, eq);
        }
    }
    /* torre de controle: corpo, cabine de vidro no alto, laje, antena, radar */
    caixa_na_pista(COMPR + 52.0f, -34.0f, 9.0f, 9.0f, 19.0f, -0.5f, 0xFFE8E8EA, 0xFFD0D2D4);
    {
        predio_t *d = caixa_na_pista(COMPR + 52.0f, -34.0f, 10.4f, 10.4f, 3.4f, 18.5f, 0xFF9CB8D2, 0xFF9CB8D2);
        if(d) d->tex = 6;
        caixa_na_pista(COMPR + 52.0f, -34.0f, 11.2f, 11.2f, 0.5f, 21.9f, 0xFF2F6FB0, 0xFFE8E8EA);
        caixa_na_pista(COMPR + 50.0f, -32.5f, 0.15f, 0.15f, 7.0f, 22.4f, 0xFF8A8E94, 0xFF8A8E94);
        caixa_na_pista(COMPR + 54.5f, -36.0f, 1.2f, 1.2f, 1.0f, 22.4f, 0xFFF2F2F2, 0xFFF2F2F2);
    }
    checar_perna = 1;
    /* PÓDIO na frente do prédio dos boxes: 3 degraus e painel atrás */
    {
        float sp0 = COMPR + 22.0f;
        caixa_na_pista(sp0, -27.5f, 2.0f, 1.6f, 1.05f, 3.8f, 0xFFF4F4F4, 0xFFE8B020);
        caixa_na_pista(sp0 - 2.0f, -27.5f, 2.0f, 1.6f, 0.75f, 3.8f, 0xFFF4F4F4, 0xFFC0C4C8);
        caixa_na_pista(sp0 + 2.0f, -27.5f, 2.0f, 1.6f, 0.50f, 3.8f, 0xFFF4F4F4, 0xFFB07040);
        predio_t *d = caixa_na_pista(sp0, -28.6f, 7.0f, 0.15f, 2.2f, 4.5f, 0xFFFFFFFF, 8);
        if(d) { d->tex = 8; d->topo = 8; d->dmax = 25; }
    }
    /* PADDOCK atrás dos boxes: caminhões das equipes e carros estacionados */
    {
        static const uint32_t EQ[8] = { 0xFFB8BCC2, 0xFFD0141C, 0xFFF5D010, 0xFFD01E28, 0xFF3C8CDC, 0xFF1E3CA8, 0xFF1A2450, 0xFFF2F3F4 };
        int k = 0;
        for(float s = 4060.0f; s < COMPR + 20.0f; s += 16.0f, k++) {
            uint32_t eq = EQ[k % 8];
            predio_t *d = caixa_na_pista(s, -52.0f, 12.5f, 2.5f, 3.6f, 0.4f, eq, 0xFFD8DADC);
            if(d) d->dmax = 22;
            d = caixa_na_pista(s + 7.6f, -52.0f, 2.4f, 2.5f, 2.6f, 0.4f, cor_escala(eq, 0.8f), 0xFF505458);
            if(d) d->dmax = 22;
        }
        static const uint32_t CAR[6] = { 0xFFE8E8EA, 0xFF202226, 0xFF9AA0A8, 0xFFB02020, 0xFF2A4C8C, 0xFFC8C0A8 };
        for(float s = 4060.0f; s < COMPR + 20.0f; s += 3.0f)
            for(int fila = 0; fila < 2; fila++) {
                uint32_t h = h32((uint32_t)(s * 3) + fila * 977u);
                if(h % 5 == 0) continue;
                uint32_t c = CAR[h % 6];
                float lat = -62.0f - fila * 6.0f;
                predio_t *d = caixa_na_pista(s, lat, 1.8f, 4.3f, 0.75f, 0.15f, c, c);
                if(d) d->dmax = 12;
                d = caixa_na_pista(s, lat, 1.5f, 2.2f, 0.55f, 0.9f, 0xFF404A54, c);
                if(d) d->dmax = 9;
            }
    }
    /* SAFETY CAR parado na saída dos boxes (luzes no teto), AMBULÂNCIA e
     * REBOQUE nos postos de resgate (Sol e Junção) */
    {
        float sc = 610.0f;
        caixa_na_pista(sc, -15.5f, 1.85f, 4.6f, 0.62f, 0.18f, 0xFFE8E8EA, 0xFFE8E8EA);      /* carroceria */
        caixa_na_pista(sc - 0.2f, -15.5f, 1.55f, 2.2f, 0.48f, 0.80f, 0xFF30343C, 0xFFE8E8EA);  /* cabine */
        caixa_na_pista(sc - 0.2f, -15.5f, 1.30f, 0.30f, 0.14f, 1.28f, 0xFFFF9020, 0xFFFFB020); /* barra de luzes */
        static const float RES[2] = { 790.0f, 3250.0f };
        for(int k = 0; k < 2; k++) {
            float s0 = RES[k];
            int kk = (int)(s0 / PASSO) % NF;
            float lado = F[kk].curva > 0 ? -1.0f : 1.0f;
            caixa_na_pista(s0, lado * 33.0f, 2.1f, 5.4f, 2.4f, 0.3f, 0xFFF4F4F4, 0xFFF4F4F4);    /* ambulância */
            caixa_na_pista(s0, lado * 33.0f, 2.12f, 5.42f, 0.25f, 1.1f, 0xFFD02020, 0xFFF4F4F4); /* faixa vermelha */
            caixa_na_pista(s0 + 9.0f, lado * 33.0f, 2.3f, 7.0f, 1.0f, 0.4f, 0xFFF0C020, 0xFF505458);   /* reboque (prancha) */
            caixa_na_pista(s0 + 12.0f, lado * 33.0f, 2.3f, 2.2f, 2.0f, 0.4f, 0xFFF0C020, 0xFFF0C020);  /* cabine */
        }
    }
    /* TORCEDORES em pé atrás das grades nas curvas lentas */
    {
        static const float ZONA[][3] = { { 380.0f, 520.0f, 1.0f }, { 3230.0f, 3330.0f, 1.0f }, { 2700.0f, 2760.0f, -1.0f } };
        static const uint32_t ROUPA[6] = { 0xFF2E9A48, 0xFFF0C820, 0xFF2D5FB0, 0xFFEDEDED, 0xFFD02020, 0xFF202020 };
        for(unsigned z = 0; z < sizeof(ZONA) / sizeof(ZONA[0]); z++)
            for(float s = ZONA[z][0]; s < ZONA[z][1]; s += 4.5f) {
                uint32_t h = h32((uint32_t)s * 31u + z);
                if(h % 3 == 0) continue;
                int k = (int)(s / PASSO) % NF;
                float lado = F[k].curva > 0 ? -1.0f : 1.0f;
                if(ZONA[z][2] < 0) lado = -lado;
                pessoa(s, lado * (29.5f + (h % 4) * 0.6f), ROUPA[h % 6], 9);
            }
    }
    /* PNEUS EMPILHADOS na frente do muro nas áreas de escape (brita) e CONES na entrada do pit */
    for(int k = 0; k < NF; k += 5) {
        if(!F[k].brita) continue;
        for(int l = 0; l < 2; l++) {
            if(!(F[k].brita & (l ? 2 : 1))) continue;
            float lat = l ? 23.6f : -23.6f;
            for(int pilha = 0; pilha < 3; pilha++) {
                predio_t *d = caixa_na_pista(k * (float)PASSO + pilha * 0.75f, lat, 0.62f, 0.62f, 0.9f, 0.0f, 0xFF1C1C1E, 0xFF2A2A2C);
                if(d) d->dmax = 14;
                d = caixa_na_pista(k * (float)PASSO + pilha * 0.75f, lat, 0.64f, 0.64f, 0.09f, 0.45f, 0xFFE8E8E8, 0xFFE8E8E8);
                if(d) d->dmax = 9;
            }
        }
    }
    for(float s = 4030.0f; s < 4095.0f; s += 6.5f) {
        predio_t *d = caixa_na_pista(s, -8.95f, 0.16f, 0.16f, 0.42f, 0.0f, 0xFFF07018, 0xFFF0F0F0);
        if(d) d->dmax = 8;
    }
    checar_perna = 0;
    /* TELÃO na reta, depois da linha (estrutura); a imagem é desenhada à parte */
    caixa_na_pista(TELAO_S, TELAO_L, 0.5f, 0.5f, 9.5f, 0.0f, 0xFF505458, 0xFF505458);
    caixa_na_pista(TELAO_S, TELAO_L + 0.4f, 10.2f, 0.4f, 6.0f, 7.0f, 0xFF202226, 0xFF202226);
    {   /* face da tela, virada para os carros que chegam */
        float a[3], b[3];
        ponto_pista(TELAO_S - 0.25f, TELAO_L + 0.4f - 4.7f, a, 0, 0);
        ponto_pista(TELAO_S - 0.25f, TELAO_L + 0.4f + 4.7f, b, 0, 0);
        float g = chao_em(TELAO_S, TELAO_L, a);
        float y0 = g + 7.3f, y1 = g + 12.7f;
        float P[4][3] = { { a[0], y0, a[2] }, { b[0], y0, b[2] }, { b[0], y1, b[2] }, { a[0], y1, a[2] } };
        memcpy(telao_q, P, sizeof(P));
    }

    /* --- arquibancadas da reta (lado direito), em degraus, com o verde e
     * amarelo da torcida; cada bloco tem 3 níveis --- */
    static const uint32_t TORCIDA[4] = { 0xFF2E9A48, 0xFFF0C820, 0xFF2D5FB0, 0xFFEDEDED };
    for(float s = 3990.0f; s < COMPR - 10.0f; s += 28.0f) {
        int i = (int)(s / 28.0f);
        arquibancada(s, 27.0f, 28.0f, 11.0f, 7.5f, TORCIDA[i & 3], 1);
    }
    /* arquibancada no S do Senna (lado de fora da primeira curva) */
    for(float s = 380.0f; s < 470.0f; s += 15.0f)
        arquibancada(s, 32.0f, 15.0f, 9.0f, 6.0f, TORCIDA[((int)s / 15) & 3], 0);

    /* --- passarela sobre a Reta Oposta, com painel de propaganda genérico --- */
    float sp = 1250.0f;
    estrutura_sombra(sp, 13.5f, 1.3f, 7.4f);              /* passarela */
    estrutura_sombra(S_PORTICO, 10.9f, 0.6f, 7.3f);       /* pórtico de largada */
    caixa_na_pista(sp, -12.5f, 1.0f, 2.0f, 7.0f, 0, 0xFFB0B4B8, 0xFFB0B4B8);
    caixa_na_pista(sp,  12.5f, 1.0f, 2.0f, 7.0f, 0, 0xFFB0B4B8, 0xFFB0B4B8);
    caixa_na_pista(sp, 0, 27.0f, 2.6f, 1.0f, 7.0f, 0xFFC8CCD0, 0xFFC8CCD0);
    {
        predio_t *d = caixa_na_pista(sp - 1.4f, 0, 27.0f, 0.2f, 1.8f, 7.2f, 0xFFFFFFFF, 8);
        if(d) { d->tex = 8; d->topo = 8; }
        caixa_na_pista(sp, 0, 27.0f, 2.4f, 0.12f, 8.05f, 0xFF8A8E94, 0xFF8A8E94);
    }

    /* --- placas de publicidade GENÉRICAS (desenhos abstratos, sem marcas)
     * na frente dos muros de pneus: reta dos boxes, reta oposta e subida --- */
    {
        static const float TRECHOS[][2] = { { 3980.0f, 4300.0f }, { 0.0f, 240.0f }, { 1080.0f, 1500.0f }, { 3300.0f, 3900.0f } };
        int k = 0;
        for(unsigned t = 0; t < sizeof(TRECHOS) / sizeof(TRECHOS[0]); t++)
            for(float s = TRECHOS[t][0]; s < TRECHOS[t][1] && s < COMPR; s += 36.0f)
                for(int lado = -1; lado <= 1; lado += 2) {
                    if(((int)(s / 36.0f) & 1) != (lado > 0)) continue;              /* um lado por vez */
                    if(lado < 0 && F[(int)(s / PASSO) % NF].box) continue;    /* lado dos boxes: muro do pit */
                    (void)k; continue;            /* publicidade agora fica pintada nos muros */
                }
    }

    /* --- placas de publicidade NA GRAMA (como as de Interlagos): o painel
     * fica acima do ponto MAIS ALTO do chão sob ele, e cada pé desce até o
     * chão da sua ponta (encosta: nada flutua nem entra na grama) --- */
    {
        static const float TR[][2] = { { 0.0f, 300.0f }, { 950.0f, 1450.0f }, { 3350.0f, 3950.0f }, { 600.0f, 720.0f } };
        int k = 0;
        for(unsigned t = 0; t < sizeof(TR) / sizeof(TR[0]); t++)
            for(float s = TR[t][0]; s < TR[t][1]; s += 55.0f, k++) {
                int lado = (k & 1) ? 1 : -1;
                if(lado < 0 && F[(int)(s / PASSO) % NF].box) lado = 1;
                #ifdef PLACA_TESTE
                placa_na_grama(s, lado * 14.5f, 8);
#else
                placa_na_grama(s, lado * 14.5f, (k % 4) == 2 ? 8 : (int)(h32((uint32_t)k * 977u + 1) & 7));
#endif
            }
    }

    /* --- postos dos fiscais de pista: casinhas brancas com teto laranja --- */
    for(float s = 150.0f; s < COMPR - 200.0f; s += 330.0f) {
        int lado = ((int)(s / 330.0f) & 1) ? 1 : -1;
        if(F[(int)(s / PASSO) % NF].box) continue;
        if(n_postos < 20) { postos_s[n_postos] = s; postos_l[n_postos++] = (int8_t)lado; }
        caixa_na_pista(s, lado * 15.0f, 2.4f, 2.2f, 2.6f, 0, 0xFFF0F0EE, 0xFFE87A1E);
        caixa_na_pista(s, lado * 13.85f, 1.6f, 0.08f, 0.9f, 1.0f, 0xFF2A3038, 0xFF2A3038);   /* janela */
    }

    /* --- POSTES DE ILUMINAÇÃO na reta dos boxes (atrás do muro e da arquibancada) --- */
    for(float s = 3990.0f; s < COMPR + 60.0f; s += 45.0f) {
        caixa_na_pista(s, 25.6f, 0.25f, 0.25f, 14.0f, 0.0f, 0xFF8A8E94, 0xFF8A8E94);         /* poste */
        caixa_na_pista(s, 25.0f, 1.6f, 0.5f, 0.35f, 14.0f, 0xFF4A4E54, 0xFFF4F0D8);         /* luminária */
    }
    /* --- TORRES DE CÂMERA DE TV nas curvas principais: plataforma, câmera e cinegrafista --- */
    {
        static const float TV[] = { 470, 780, 1560, 2260, 2760, 3330 };
        for(unsigned c = 0; c < sizeof(TV) / sizeof(TV[0]); c++) {
            float s0 = TV[c];
            int k = (int)(s0 / PASSO) % NF;
            int lado = F[k].curva > 0 ? 1 : -1;            /* por dentro da curva */
            if(F[k].box && lado < 0) lado = 1;
            float lat = lado * 27.5f;
            for(int pe = -1; pe <= 1; pe += 2) {
                predio_t *d = caixa_na_pista(s0 + pe * 0.9f, lat, 0.12f, 0.12f, 4.0f, 0.0f, 0xFF6A6E74, 0xFF6A6E74);
                if(d) d->dmax = 20;
            }
            predio_t *d = caixa_na_pista(s0, lat, 2.4f, 2.4f, 0.18f, 4.0f, 0xFF50545A, 0xFF70747A);   /* plataforma */
            if(d) d->dmax = 20;
            d = caixa_na_pista(s0, lat - lado * 0.6f, 0.35f, 0.55f, 0.35f, 4.9f, 0xFF202226, 0xFF202226);   /* câmera */
            if(d) d->dmax = 12;
            pessoa(s0 + 0.5f, lat, 0xFF2A3A5A, 12);
            /* a pessoa nasce no chão: sobe ela para a plataforma */
            for(int q = n_predios - 2; q < n_predios; q++) predios[q].c[1] += 4.2f;
        }
    }

    /* --- fotógrafos atrás das grades, por fora das curvas fortes --- */
    {
        static const float CURVAS[] = { 437, 744, 1512, 2122, 2716, 3277, 3498 };
        for(unsigned c = 0; c < sizeof(CURVAS) / sizeof(CURVAS[0]); c++) {
            float s0 = CURVAS[c];
            int k = (int)(s0 / PASSO) % NF;
            int lado = F[k].curva > 0 ? -1 : 1;           /* por fora da curva */
            if(F[k].box && lado < 0) continue;
            for(int i = 0; i < 3; i++) {
                float s = s0 - 12.0f + i * 9.0f;
                pessoa(s, lado * 26.2f, (i & 1) ? 0xFF2A3A5A : 0xFF3A3A3E, 6);
                predio_t *d = caixa_na_pista(s, lado * 25.85f, 0.16f, 0.12f, 0.12f, 1.25f, 0xFF101012, 0xFF101012);   /* câmera */
                if(d) d->dmax = 6;
            }
        }
    }

    /* --- placas de frenagem 300/200/100 m antes das curvas fortes --- */
    const pista_osm_t *P = &PISTA_INTERLAGOS;
    for(int f = 0; f < P->n_frenagens; f++)
        for(int m = 1; m <= 3; m++)
            caixa_na_pista((float)P->frenagens[f] - m * 100.0f, 11.5f, 1.4f, 0.2f, 0.8f + m * 0.5f, 0,
                           m == 1 ? 0xFFD03030 : 0xFFF0F0F0, 0xFF808080);

    /* --- "mar de prédios" de SP: torres residenciais a 0,9-1,8 km --- */
    for(int i = 0; i < 400 && n_cidade < 28; i++) {
        float ang = aleat(i + 5000) * 2 * PI_F;
        float raio = 1200.0f + aleat(i + 5100) * 800.0f;
        float x = CENTRO_X + cosf(ang) * raio, z = CENTRO_Z + sinf(ang) * raio;
        /* não pode cair perto da pista */
        int perto = 0;
        for(int k = 0; k < NF; k += 8) {
            float dx = F[k].x - x, dz = F[k].z - z;
            if(dx * dx + dz * dz < 380.0f * 380.0f) { perto = 1; break; }
        }
        if(perto) continue;
        predio_t *d = &cidade[n_cidade++];
        d->hx = 9 + aleat(i + 5200) * 8; d->hz = 9 + aleat(i + 5300) * 8;
        d->hy = 12 + aleat(i + 5400) * 18;            /* torres residenciais de 8-15 andares */
        d->c[0] = x; d->c[1] = d->hy - 12; d->c[2] = z;
        float g = aleat(i + 5500) * PI_F;
        d->r[0] = cosf(g); d->r[1] = 0; d->r[2] = sinf(g);
        d->f[0] = -sinf(g); d->f[1] = 0; d->f[2] = cosf(g);
        static const uint32_t TONS[4] = { 0xFFE4DED2, 0xFFD2C6B4, 0xFFC2C8CE, 0xFFEAE6DC };
        float t = 0.30f + (raio - 900.0f) / 900.0f * 0.30f;
        d->cor = cor_mistura(TONS[h32(i) & 3], COR_NEBLINA, t);
        d->topo = cor_mistura(0xFF9A8E80, COR_NEBLINA, t);
    }
}

static void montar_cenario(void) {
    const pista_osm_t *P = &PISTA_INTERLAGOS;
    montar_marcos();       /* primeiro: têm prioridade no limite de desenho */

    /* prédios/arquibancadas reais, orientados pela pista */
    for(int i = 0; i < P->n_predios && n_predios < MAX_PREDIOS; i++) {
        const osm_predio_t *o = &P->predios[i];
        if(fabsf(o->lateral) - o->larg * 0.5f < 10.0f) continue;      /* não invade a pista */
        {   /* distância REAL até a pista (qualquer trecho, não só o do segmento):
             * nada pode entrar além do muro (26 m), e casas pequenas só bem longe
             * (dentro do autódromo não há bairro) */
            float bp[3];
            ponto_pista((float)o->seg, o->lateral, bp, 0, 0);
            float dmin = 1e18f;
            for(int k = 0; k < NF; k += 2) {
                float dx = F[k].x - bp[0], dz = F[k].z - bp[2], dd = dx * dx + dz * dz;
                if(dd < dmin) dmin = dd;
            }
            float borda = sqrtf(dmin) - 0.5f * sqrtf(o->larg * o->larg + o->prof * o->prof);
            if(borda < 30.0f) continue;
            int casa = o->tipo == 0 && o->alt < 10.0f && o->larg * o->prof < 450.0f;
            if(casa && borda < 210.0f) continue;
            if(o->tipo == 1) {   /* arquibancada do mapa em cima de uma que o jogo já criou: pula (piscava) */
                int sobre = 0;
                for(int q = 0; q < n_arqs && !sobre; q++) {
                    float cx = arqs[q].f[0] + arqs[q].o[0] * arqs[q].prof * 0.5f, cz = arqs[q].f[2] + arqs[q].o[2] * arqs[q].prof * 0.5f;
                    float dx = cx - bp[0], dz = cz - bp[2];
                    float r = arqs[q].meio + arqs[q].prof * 0.5f + 0.5f * (o->larg > o->prof ? o->larg : o->prof);
                    if(dx * dx + dz * dz < r * r) sobre = 1;
                }
                if(sobre) continue;
            }
        }
        if(o->seg / PASSO >= NF) continue;
        predios[n_predios].dmax = 0;
        if(o->tipo == 1) {        /* arquibancada do mapa: rampa com torcida */
            float lf = (fabsf(o->lateral) - o->larg * 0.5f) * (o->lateral < 0 ? -1.0f : 1.0f);
            arquibancada((float)o->seg, lf, o->prof, limitar(o->larg, 5.0f, 25.0f), limitar(o->alt, 5.0f, 14.0f),
                         (h32(i * 7 + 3) & 1) ? 0xFF3C6FB0 : 0xFFD0A030, o->prof < 80.0f);
            continue;
        }
        {   /* nada de casa colada no complexo dos boxes (lado do pit, perto) */
            int perto_box = 0;
            for(int j = -25; j <= 25; j += 5) if(F[((int)(o->seg / PASSO) + j + NF) % NF].box) perto_box = 1;
            if(perto_box && o->lateral < 0 && fabsf(o->lateral) < 95.0f) continue;
        }
        predio_t *d = &predios[n_predios++];
        d->osm = 1;
        float base[3], fr[3], dir[3];
        ponto_pista((float)o->seg, o->lateral, base, fr, dir);
        float alt = limitar(o->alt, 3.0f, 90.0f);
        float g = chao_em((float)o->seg, o->lateral, base);
        float y0 = g - 2.0f, y1 = g + alt;
        d->c[0] = base[0]; d->c[1] = (y0 + y1) * 0.5f; d->c[2] = base[2];
        d->r[0] = dir[0]; d->r[1] = 0; d->r[2] = dir[2];
        d->f[0] = dir[2]; d->f[1] = 0; d->f[2] = -dir[0];             /* horizontal */
        /* no gerador: larg = medida PERPENDICULAR à pista, prof = AO LONGO dela
         * (antes estavam trocadas: todos os prédios ficavam girados 90°) */
        d->hx = o->prof * 0.5f; d->hy = (y1 - y0) * 0.5f; d->hz = o->larg * 0.5f;
        uint32_t hh = h32(i * 7 + 3);
        d->tex = 0;
        if(o->tipo == 1) { d->cor = 0xFF8E98A6; d->topo = (hh & 1) ? 0xFF3C6FB0 : 0xFFD0A030; d->tex = 1; }   /* arquibancada */
        else if(o->tipo == 2) { d->cor = 0xFFB4B8BC; d->topo = 0xFF9AA0A6; }
        else {
            /* bairros em volta do autódromo: casas e sobrados de 1-2 andares,
             * paredes claras ou coloridas e TELHADO DE CERÂMICA; os poucos
             * prédios mais altos têm laje cinza. Todos com janelas. */
            static const uint32_t PAREDES[8] = { 0xFFE6E0D2, 0xFFD8CDB6, 0xFFEDE6CF, 0xFFCFC3AE, 0xFFE8D9B0,
                                                 0xFFC9D2D8, 0xFFE2C4A8, 0xFFD6DCC8 };
            static const uint32_t TELHAS[3] = { 0xFFB0583A, 0xFF9C4A30, 0xFFA8643E };
            d->cor = PAREDES[hh % 8];
            d->topo = alt < 10.0f ? TELHAS[(hh >> 4) % 3] : 0xFF8E9298;
            d->tex = 6;
        }
    }

    /* árvores: densas onde o OSM marca mata; esparsas no resto. Ipês amarelos
     * e roxos de vez em quando (a cara de SP em agosto/setembro). */
    for(int k = 0; k < NF && n_arvores < MAX_ARVORES; k++) {
        for(int lado = -1; lado <= 1; lado += 2) {
            int mata = F[k].amb & (lado < 0 ? AMB_MATA_ESQ : AMB_MATA_DIR);
            uint32_t sem = k * 2 + (lado > 0);
            if(mata ? (k % 2) : (k % 5 || aleat(sem) < 0.5f)) continue;
            if(F[k].box && lado < 0) continue;
            float lim = lado < 0 ? F[k].larg_esq : F[k].larg_dir;
            float lat = 28.0f + aleat(sem + 99) * (mata ? 30.0f : 55.0f);   /* fora do muro (área de escape livre) */
            if(lat > lim - 4) continue;
            arvore_t *a = &arvores[n_arvores++];
            ponto_pista(k * (float)PASSO, lado * lat, a->p, 0, 0);
            if(sobre_outra_perna(a->p, k)) { n_arvores--; continue; }   /* faixa de OUTRO trecho: não planta */
            a->p[1] = chao_em(k * (float)PASSO, lado * lat, a->p) - 0.3f;           /* assenta no chão */
            /* de longe o terreno usa a malha grossa: guarda quanto o chão muda */
            float tf = terreno_passo(a->p[0], a->p[2], 1);
            a->dy_grosso = (a->p[1] + 0.3f <= tf + 0.01f) ? terreno_passo(a->p[0], a->p[2], 2) - tf : 0.0f;
            a->alt = 6.0f + aleat(sem + 7) * 7.0f;
            a->raio = 2.2f + aleat(sem + 13) * 1.8f;
            a->giro = aleat(sem + 21) * PI_F;
            float r = aleat(sem + 31);
            a->cor = r < 0.10f ? 0xFFFFE23A : r < 0.15f ? 0xFFB04C9A : r < 0.18f ? 0xFFF08CC0 : r < 0.20f ? 0xFFF4F0EA :
                     r < 0.58f ? 0xFF3E7A2E : 0xFF4E8C34;
        }
    }
    /* ARBUSTOS: moitas baixas nas encostas, perto da pista (mais vida na grama) */
    for(int k = 1; k < NF && n_arvores < MAX_ARVORES; k += 3) {
        for(int lado = -1; lado <= 1; lado += 2) {
            uint32_t sem = 70000 + k * 2 + (lado > 0);
            if(aleat(sem) < 0.55f) continue;
            if(F[k].box && lado < 0) continue;
            float lim = lado < 0 ? F[k].larg_esq : F[k].larg_dir;
            float lat = 27.0f + aleat(sem + 3) * 18.0f;
            if(lat > lim - 3) continue;
            arvore_t *a = &arvores[n_arvores++];
            ponto_pista(k * (float)PASSO, lado * lat, a->p, 0, 0);
            if(sobre_outra_perna(a->p, k)) { n_arvores--; continue; }
            a->p[1] = chao_em(k * (float)PASSO, lado * lat, a->p) - 0.2f;
            float tf = terreno_passo(a->p[0], a->p[2], 1);
            a->dy_grosso = (a->p[1] + 0.2f <= tf + 0.01f) ? terreno_passo(a->p[0], a->p[2], 2) - tf : 0.0f;
            a->alt = 1.4f + aleat(sem + 7) * 1.2f;
            a->raio = 1.1f + aleat(sem + 13) * 0.9f;
            a->giro = aleat(sem + 21) * PI_F;
            a->cor = aleat(sem + 31) < 0.5f ? 0xFF35682A : 0xFF467A30;
        }
    }

#ifdef DIAG_ARV
    {
        int n = 0, pior = -1; float mx = 0;
        for(int i = 0; i < n_arvores; i++) {
            float d = arvores[i].p[1] - terreno_em(arvores[i].p[0], arvores[i].p[2]);
            if(d > 0.8f) { n++; if(d > mx) { mx = d; pior = i; } }
        }
        int ks = 0; float dm = 1e18f;
        if(pior >= 0) for(int k = 0; k < NF; k++) {
            float dx = F[k].x - arvores[pior].p[0], dz = F[k].z - arvores[pior].p[2];
            if(dx * dx + dz * dz < dm) { dm = dx * dx + dz * dz; ks = k; }
        }
        snprintf(diag_txt, sizeof(diag_txt), "ARV %d/%d MAX %.1fm s%d d%.0f", n, n_arvores, (double)mx, ks * PASSO, (double)sqrtf(dm));
    }
#endif
    /* anel de morros ao longe (a Serra da Cantareira/Mar ficam azuladas pela névoa) */
    for(int i = 0; i < 40 && n_morros < MAX_MORROS; i++) {
        float ang = i * 2 * PI_F / 40 + aleat(i) * 0.1f;
        float raio = 2600.0f + aleat(i + 500) * 600.0f;
        float larg = 0.12f + aleat(i + 900) * 0.1f;
        float alt = 90.0f + aleat(i + 1300) * 180.0f;
        tri_t *t = &morros[n_morros++];
        float a0 = ang - larg, a1 = ang + larg;
        t->p[0][0] = CENTRO_X + cosf(a0) * raio; t->p[0][1] = -60; t->p[0][2] = CENTRO_Z + sinf(a0) * raio;
        t->p[1][0] = CENTRO_X + cosf(ang) * raio; t->p[1][1] = alt; t->p[1][2] = CENTRO_Z + sinf(ang) * raio;
        t->p[2][0] = CENTRO_X + cosf(a1) * raio; t->p[2][1] = -60; t->p[2][2] = CENTRO_Z + sinf(a1) * raio;
        t->cor = cor_mistura(0xFF4F6E58, COR_NEBLINA, 0.45f + aleat(i + 77) * 0.2f);
    }

    /* skyline do centro de SP, que fica ao NORTE de Interlagos (-Z) */
    for(int i = 0; i < MAX_SKY; i++) {
        float ang = -PI_F / 2 + (aleat(i + 2000) - 0.5f) * 1.3f;       /* norte ± 37° */
        float raio = 3300.0f + aleat(i + 2100) * 1800.0f;
        predio_t *d = &skyline[n_sky++];
        d->hx = 18 + aleat(i + 2200) * 22; d->hz = d->hx;
        d->hy = 40 + aleat(i + 2300) * 85;
        d->c[0] = CENTRO_X + cosf(ang) * raio; d->c[1] = d->hy - 30; d->c[2] = CENTRO_Z + sinf(ang) * raio;
        d->r[0] = 1; d->r[1] = 0; d->r[2] = 0; d->f[0] = 0; d->f[1] = 0; d->f[2] = 1;
        d->cor = cor_mistura(0xFF8C939C, COR_NEBLINA, 0.35f + aleat(i + 2400) * 0.3f);
        d->topo = d->cor;
    }
}

static inline float dist2_cam(const float p[3]) {
    float dx = p[0] - cam_p[0], dz = p[2] - cam_p[2];
    return dx * dx + dz * dz;
}
static inline int na_frente(const float p[3], float folga) {
    return cam_f[0] * (p[0] - cam_p[0]) + cam_f[2] * (p[2] - cam_p[2]) > -folga;
}
/* Dentro do campo de visão HORIZONTAL (não só "na frente"): descarta o que
 * fica fora da tela pelos lados. raio = tamanho do objeto (folga). */
static inline int na_campo(const float p[3], float raio) {
    float dx = p[0] - cam_p[0], dz = p[2] - cam_p[2];
    float fr = cam_f[0] * dx + cam_f[2] * dz;
    if(fr < -raio) return 0;
    float lado = cam_r[0] * dx + cam_r[2] * dz;
    float tg = CX / FOCO * 1.12f;          /* meia-largura da tela / foco, com margem */
    return fabsf(lado) < fr * tg + raio * 1.5f;
}

/* a água é desenhada depois do terreno, com outra textura (animada) */
#define MAX_AGUA 400
static float agua_q[MAX_AGUA][5];      /* x0, z0, x1, z1, altura */
static int n_agua_q;
static void ter_quad(int i0, int j0, int passo) {
    int i1 = i0 + passo, j1 = j0 + passo;
    if(i1 >= ter_nx || j1 >= ter_nz) return;
    float x0 = ter_x0 + i0 * TER_CEL, x1 = ter_x0 + i1 * TER_CEL;
    float z0 = ter_z0 + j0 * TER_CEL, z1 = ter_z0 + j1 * TER_CEL;
    float h00 = ter_h[j0][i0], h10 = ter_h[j0][i1], h11 = ter_h[j1][i1], h01 = ter_h[j1][i0];
    int agua = ter_agua[j0][i0] + ter_agua[j0][i1] + ter_agua[j1][i1] + ter_agua[j1][i0];
    uint32_t cor;
    if(agua >= 2) {
        float h = 1e9f;
        if(ter_agua[j0][i0] && h00 < h) h = h00;
        if(ter_agua[j0][i1] && h10 < h) h = h10;
        if(ter_agua[j1][i1] && h11 < h) h = h11;
        if(ter_agua[j1][i0] && h01 < h) h = h01;
        if(n_agua_q < MAX_AGUA) {
            float *a = agua_q[n_agua_q++];
            a[0] = x0; a[1] = z0; a[2] = x1; a[3] = z1; a[4] = h;
        }
        return;
    } else {
        /* luz ASSADA por vértice (ter_cor): sombreamento suave, sem contas aqui */
        int n = 0;
        (void)n;
        cor = 0;
    }
    (void)cor;
    vv_t q[4];
    para_vista(x0, h00, z0, 0, &q[0]);
    para_vista(x1, h10, z0, 0, &q[1]);
    para_vista(x1, h11, z1, 0, &q[2]);
    para_vista(x0, h01, z1, 0, &q[3]);
    if(fora_tela(&q[0]) & fora_tela(&q[1]) & fora_tela(&q[2]) & fora_tela(&q[3])) return;   /* fora da tela */
    /* grama com textura (a cada 10 m, alinhada ao mundo) */
    const float e = 0.1f;
    vvt_t t[4] = { { q[0].x, q[0].y, q[0].z, x0 * e, z0 * e, ter_cor[j0][i0], 0 }, { q[1].x, q[1].y, q[1].z, x1 * e, z0 * e, ter_cor[j0][i1], 0 },
                   { q[2].x, q[2].y, q[2].z, x1 * e, z1 * e, ter_cor[j1][i1], 0 }, { q[3].x, q[3].y, q[3].z, x0 * e, z1 * e, ter_cor[j1][i0], 0 } };

    poligono_tex(t, 4);
}

/* ======================================================================== */
/* LUZ PRÉ-CALCULADA ("assada"): oclusão da pista e luz do terreno           */
/* ======================================================================== */
static float ter_ao[TER_MAX][TER_MAX], ter_nrm[TER_MAX][TER_MAX][3];
static uint32_t ter_base[TER_MAX][TER_MAX];      /* cor do gramado (seco, viçoso, gasto) — pré-calculada */
static int ter_linha_assar;
static void ter_montar_base(void) {
    static const uint32_t GRAMA[3] = { 0xFF4E8A36, 0xFF56923A, 0xFF4A8232 };
    for(int j = 0; j < ter_nz; j++)
        for(int i = 0; i < ter_nx; i++) {
            if(ter_agua[j][i]) { ter_base[j][i] = 0xFF6A7444; continue; }
            float x = ter_x0 + i * TER_CEL, z = ter_z0 + j * TER_CEL;
            /* manchas grandes: grama SECA (amarelada) e VIÇOSA (escura), como num
             * gramado de verdade no inverno paulista */
            float v = 0.5f + 0.27f * sinf(x * 0.0093f + z * 0.0051f) + 0.23f * sinf(z * 0.0147f - x * 0.0061f + 1.3f)
                    + 0.14f * (aleat(i * 31 + j * 7) - 0.5f);
            uint32_t b = GRAMA[h32(i * 31 + j * 7) % 3];
            if(v > 0.60f) b = cor_mistura(b, 0xFF94985A, limitar((v - 0.60f) / 0.25f, 0, 0.75f));
            else if(v < 0.38f) b = cor_mistura(b, 0xFF2E6828, limitar((0.38f - v) / 0.25f, 0, 0.6f));
            int dd = ter_d[j][i];
            if(dd >= 24 && dd < 44) b = cor_mistura(b, 0xFF7E8654, 0.30f);    /* gasto junto às cercas */
            ter_base[j][i] = b;
        }
}
static void ter_assar_linha(int j) {
    /* névoa nos VALES: o chão abaixo da pista fica enevoado (mais de manhã,
     * nublado ou com chuva) — separa as camadas de morros */
    float vk = limitar(0.22f + 0.30f * nuvens + 0.30f * chuva + 0.25f * manha, 0, 0.8f);
    for(int i = 0; i < ter_nx; i++) {
        const float *n = ter_nrm[j][i];
        float l = (0.35f + 0.75f * sombra_luz(n) - 0.2f) * ter_ao[j][i];
        l *= fator_nuvem(ter_x0 + i * TER_CEL, ter_z0 + j * TER_CEL);     /* sombra de nuvem (atualiza aos poucos) */
        uint32_t c = cor_escala(ter_base[j][i], l);
        float kv = limitar((-6.0f - ter_h[j][i]) / 28.0f, 0, 1) * vk;
        if(kv > 0.01f) c = cor_mistura(c, cor_neb, kv);
        ter_cor[j][i] = c;
    }
}
static void assar_luz(void) {
    /* --- terreno: normal por VÉRTICE (vizinhos) e oclusão por matas/construções --- */
    for(int j = 0; j < ter_nz; j++)
        for(int i = 0; i < ter_nx; i++) {
            int i0 = i > 0 ? i - 1 : i, i1 = i + 1 < ter_nx ? i + 1 : i;
            int j0 = j > 0 ? j - 1 : j, j1 = j + 1 < ter_nz ? j + 1 : j;
            float dx = (ter_h[j][i1] - ter_h[j][i0]) / ((i1 - i0) * TER_CEL + 1e-3f);
            float dz = (ter_h[j1][i] - ter_h[j0][i]) / ((j1 - j0) * TER_CEL + 1e-3f);
            float il = 1.0f / sqrtf(dx * dx + 1 + dz * dz);
            ter_nrm[j][i][0] = -dx * il; ter_nrm[j][i][1] = il; ter_nrm[j][i][2] = -dz * il;
            ter_ao[j][i] = 1.0f;
        }
    for(int a = 0; a < n_arvores; a++) {          /* mata densa escurece o chão em volta */
        int i = (int)((arvores[a].p[0] - ter_x0) / TER_CEL + 0.5f), j = (int)((arvores[a].p[2] - ter_z0) / TER_CEL + 0.5f);
        if(i < 0 || j < 0 || i >= ter_nx || j >= ter_nz) continue;
        ter_ao[j][i] *= arvores[a].alt < 3.0f ? 0.985f : 0.955f;
    }
    for(int d = 0; d < n_predios; d++) {
        int i = (int)((predios[d].c[0] - ter_x0) / TER_CEL + 0.5f), j = (int)((predios[d].c[2] - ter_z0) / TER_CEL + 0.5f);
        if(i < 0 || j < 0 || i >= ter_nx || j >= ter_nz) continue;
        ter_ao[j][i] *= 0.985f;
    }
    for(int j = 0; j < ter_nz; j++) for(int i = 0; i < ter_nx; i++) if(ter_ao[j][i] < 0.72f) ter_ao[j][i] = 0.72f;
    ter_montar_base();
    for(int j = 0; j < ter_nz; j++) ter_assar_linha(j);

    /* --- pista: grade de fatias (32 m) para achar os vizinhos de cada objeto --- */
    #define GC 32.0f
    #define GN 96
    static int16_t g_ini[GN * GN], g_prox[MAX_FATIAS];
    float gx0 = ter_x0, gz0 = ter_z0;
    for(int c = 0; c < GN * GN; c++) g_ini[c] = -1;
    for(int k = 0; k < NF; k++) {
        int cx = (int)((F[k].x - gx0) / GC), cz = (int)((F[k].z - gz0) / GC);
        if(cx < 0 || cz < 0 || cx >= GN || cz >= GN) { g_prox[k] = -1; continue; }
        g_prox[k] = g_ini[cz * GN + cx]; g_ini[cz * GN + cx] = (int16_t)k;
    }
    static float ao[MAX_FATIAS][NPTS];
    for(int k = 0; k < NF; k++)
        for(int i = 0; i < NPTS; i++) {
            float a = 1.0f;
            float lat = fabsf(LAT[i]);
            if(i >= 1 && i <= NPTS - 2 && lat >= 25.0f) a *= 0.80f;              /* pé do muro */
            if(F[k].box && i >= 1 && i <= 2) a *= i == 2 ? 0.74f : 0.84f;        /* junto aos boxes */
            if(i >= 3 && i <= 6) a *= 0.97f + 0.03f * (lat < 6.9f);              /* beirada do asfalto */
            ao[k][i] = a;
        }
    /* construções: sombra de contato perto e "telhado" por cima da pista */
    for(int d = 0; d < n_predios; d++) {
        const predio_t *pr = &predios[d];
        float raio = pr->hx + pr->hz + 14.0f;
        int c0x = (int)((pr->c[0] - raio - gx0) / GC), c1x = (int)((pr->c[0] + raio - gx0) / GC);
        int c0z = (int)((pr->c[2] - raio - gz0) / GC), c1z = (int)((pr->c[2] + raio - gz0) / GC);
        for(int cz = c0z; cz <= c1z; cz++) for(int cx = c0x; cx <= c1x; cx++) {
            if(cx < 0 || cz < 0 || cx >= GN || cz >= GN) continue;
            for(int k = g_ini[cz * GN + cx]; k >= 0; k = g_prox[k]) {
                for(int i = 1; i < NPTS - 1; i++) {
                    float px = F[k].x + F[k].rx * LAT[i], pz = F[k].z + F[k].rz * LAT[i];
                    float dx = px - pr->c[0], dz = pz - pr->c[2];
                    float lr = dx * pr->r[0] + dz * pr->r[2], lf = dx * pr->f[0] + dz * pr->f[2];
                    float ex = fabsf(lr) - pr->hx, ez = fabsf(lf) - pr->hz;
                    float base = pr->c[1] - pr->hy;
                    if(ex < 0 && ez < 0 && base > F[k].y + 2.0f) { ao[k][i] *= 0.60f; continue; }   /* coberto */
                    float dist = (ex > ez ? ex : ez);
                    if(dist < 0) dist = 0;
                    float alt = pr->hy * 2.0f;
                    if(alt < 0.8f || base > F[k].y + 2.0f) continue;
                    float oc = alt / (alt + dist * 2.5f + 1.0f);
                    ao[k][i] *= 1.0f - 0.30f * oc;
                }
            }
        }
    }
    for(int k = 0; k < NF; k++) {                 /* recapeamentos: o tom do asfalto muda por trecho */
        float sk = k * (float)PASSO;
        float tom = 0.955f + 0.05f * sinf(sk * 0.0113f + 1.3f) * sinf(sk * 0.0047f + 0.4f) + 0.02f * sinf(sk * 0.031f);
        ao[k][4] *= tom; ao[k][5] *= tom;
    }
    for(int k = 0; k < NF; k++)
        for(int i = 0; i < NPTS; i++) {
            float a = ao[k][i] < 0.5f ? 0.5f : ao[k][i];
            ao_p[k][i] = (uint8_t)(a * 255.0f);
        }
    /* SOMBRAS PROJETADAS das arquibancadas (e coberturas) sobre a pista, para
     * o sol do dia e o do entardecer: um raio de cada ponto da pista até o sol */
    for(int sol = 0; sol < 2; sol++) {
        float t = (float)sol;
        float hx = -0.669f + (-0.985f + 0.669f) * t, hz = -0.743f + (-0.174f + 0.743f) * t;
        float hl = sqrtf(hx * hx + hz * hz), yy = 0.75f - 0.35f * t, hh = sqrtf(1 - yy * yy);
        float L[3] = { hx / hl * hh, yy, hz / hl * hh };
        uint8_t (*dst)[NPTS] = sol ? somb_t : somb_d;
        for(int k = 0; k < NF; k++)
            for(int i = 0; i < NPTS; i++) {
                dst[k][i] = 255;
                if(i == 0 || i == NPTS - 1) continue;
                float P[3];
                ponto_pista(k * (float)PASSO, LAT[i], P, 0, 0);
                P[1] += 0.2f;
                for(int q = 0; q < n_arqs; q++) {
                    const arq_t *a = &arqs[q];
                    float topo = a->h1 + (a->teto ? 3.0f : 0.0f);
                    float C[3] = { a->f[0] + a->o[0] * a->prof * 0.5f, a->f[1] + (topo - 1.5f) * 0.5f, a->f[2] + a->o[2] * a->prof * 0.5f };
                    float dx = P[0] - C[0], dz = P[2] - C[2];
                    if(dx * dx + dz * dz > 140.0f * 140.0f) continue;
                    /* raio no espaço da arquibancada (eixos t, y, o) */
                    float o0 = dx * a->t[0] + dz * a->t[2], o1 = P[1] - C[1], o2 = dx * a->o[0] + dz * a->o[2];
                    float d0 = L[0] * a->t[0] + L[2] * a->t[2], d1 = L[1], d2 = L[0] * a->o[0] + L[2] * a->o[2];
                    float he[3] = { a->meio, (topo + 1.5f) * 0.5f, a->prof * 0.5f };
                    float oo[3] = { o0, o1, o2 }, dd[3] = { d0, d1, d2 };
                    float tmin = 0.0f, tmax = 1e9f; int ok = 1;
                    for(int e = 0; e < 3 && ok; e++) {
                        if(fabsf(dd[e]) < 1e-6f) { if(fabsf(oo[e]) > he[e]) ok = 0; continue; }
                        float t1 = (-he[e] - oo[e]) / dd[e], t2 = (he[e] - oo[e]) / dd[e];
                        if(t1 > t2) { float w = t1; t1 = t2; t2 = w; }
                        if(t1 > tmin) tmin = t1;
                        if(t2 < tmax) tmax = t2;
                        if(tmin > tmax) ok = 0;
                    }
                    if(ok) { dst[k][i] = 70; break; }
                }
            }
    }
    /* LUZ MANCHADA: a sombra das copas (dia e entardecer) cai na pista e na
     * grama perto das árvores — manchas que somem com nuvens (como as outras) */
    for(int sol = 0; sol < 2; sol++) {
        float t = (float)sol;
        float hx = -0.669f + (-0.985f + 0.669f) * t, hz = -0.743f + (-0.174f + 0.743f) * t;
        float hl = sqrtf(hx * hx + hz * hz), yy = 0.75f - 0.35f * t, hh = sqrtf(1 - yy * yy);
        float L[3] = { hx / hl * hh, yy, hz / hl * hh };
        uint8_t (*dst)[NPTS] = sol ? somb_t : somb_d;
        for(int a = 0; a < n_arvores; a++) {
            const arvore_t *av = &arvores[a];
            if(av->alt < 3.0f) continue;
            float h = av->alt * 0.75f;
            float cx = av->p[0] - L[0] / L[1] * h, cz = av->p[2] - L[2] / L[1] * h;
            float r = av->raio * 1.25f;
            int c0x = (int)((cx - r - 30 - gx0) / GC), c1x = (int)((cx + r + 30 - gx0) / GC);
            int c0z = (int)((cz - r - 30 - gz0) / GC), c1z = (int)((cz + r + 30 - gz0) / GC);
            for(int gz = c0z; gz <= c1z; gz++) for(int gx = c0x; gx <= c1x; gx++) {
                if(gx < 0 || gz < 0 || gx >= GN || gz >= GN) continue;
                for(int k = g_ini[gz * GN + gx]; k >= 0; k = g_prox[k])
                    for(int i = 1; i < NPTS - 1; i++) {
                        float px = F[k].x + F[k].rx * LAT[i], pz = F[k].z + F[k].rz * LAT[i];
                        float dx = px - cx, dz = pz - cz;
                        if(dx * dx + dz * dz > r * r) continue;
                        float mancha = 0.55f + 0.25f * aleat(a * 31 + i);    /* copa vazada: manchas */
                        int v = (int)(dst[k][i] * mancha);
                        dst[k][i] = (uint8_t)(v < 60 ? 60 : v);
                    }
            }
        }
    }
    for(int k = 0; k < NF; k++) for(int i = 0; i < NPTS; i++) somb[k][i] = 255;
}

static void desenhar_terreno_(float alcance);
static void desenhar_terreno(float alcance) {
    nuv_quadro();
    /* a luz do terreno acompanha o clima: reassa 2 linhas por quadro (volta completa ~0,6 s) */
    for(int r = 0; r < 2 && ter_nz > 0; r++) { ter_assar_linha(ter_linha_assar); ter_linha_assar = (ter_linha_assar + 1) % ter_nz; }
    n_agua_q = 0;
    pvr_prim(&hdr_grama, sizeof(hdr_grama));
    desenhar_terreno_(alcance);
    if(n_agua_q) {
        /* LAGOS: textura de ondinhas que desliza devagar; cor muda com o céu */
        pvr_prim(&hdr_agua, sizeof(hdr_agua));
        uint32_t ca = cor_escala(cor_mistura(cor_mistura(0xFF4F86AC, 0xFF56626E, nuvens * 0.5f + chuva * 0.3f),
                                             cor_mistura(cor_neb, cor_ceu, 0.5f), 0.55f), brilho);   /* reflete o céu */
        float sl = sqrtf(LUZ[0] * LUZ[0] + LUZ[2] * LUZ[2]) + 1e-4f, slx = LUZ[0] / sl, slz = LUZ[2] / sl;
        float brilho_sol = limitar(manha + tarde + 0.5f * (1.0f - nuvens * 1.5f), 0, 1) * (1.0f - chuva);
        uint32_t c_sol = cor_mistura(0xFFFFF6E0, 0xFFFFB060, tarde);
        float du = t_clima * 0.020f, dv = t_clima * 0.013f, e = 0.08f;
        for(int k = 0; k < n_agua_q; k++) {
            const float *a = agua_q[k];
            vv_t q[4];
            uint32_t cv[4];
            const float vx[4] = { a[0], a[2], a[2], a[0] }, vz[4] = { a[1], a[1], a[3], a[3] };
            for(int v = 0; v < 4; v++) {              /* reflexo do sol: na direção do sol, vista da câmera */
                float dx = vx[v] - cam_p[0], dz = vz[v] - cam_p[2];
                float cs = (dx * slx + dz * slz) * __frsqrt(dx * dx + dz * dz + 1.0f);
                float g = cs > 0.80f ? (cs - 0.80f) * 5.0f : 0.0f;
                cv[v] = g > 0 ? cor_mistura(ca, c_sol, g * g * 0.85f * brilho_sol) : ca;
            }
            para_vista(a[0], a[4], a[1], cv[0], &q[0]); para_vista(a[2], a[4], a[1], cv[1], &q[1]);
            para_vista(a[2], a[4], a[3], cv[2], &q[2]); para_vista(a[0], a[4], a[3], cv[3], &q[3]);
            if(fora_tela(&q[0]) & fora_tela(&q[1]) & fora_tela(&q[2]) & fora_tela(&q[3])) continue;
            vvt_t t[4] = { { q[0].x, q[0].y, q[0].z, a[0] * e + du, a[1] * e + dv, cv[0] }, { q[1].x, q[1].y, q[1].z, a[2] * e + du, a[1] * e + dv, cv[1] },
                           { q[2].x, q[2].y, q[2].z, a[2] * e + du, a[3] * e + dv, cv[2] }, { q[3].x, q[3].y, q[3].z, a[0] * e + du, a[3] * e + dv, cv[3] } };
            poligono_tex(t, 4);
        }
    }
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
}
static void desenhar_terreno_(float alcance) {
    for(int j = 0; j + 2 < ter_nz; j += 2)
        for(int i = 0; i + 2 < ter_nx; i += 2) {
            float c[3] = { ter_x0 + (i + 1) * TER_CEL, 0, ter_z0 + (j + 1) * TER_CEL };
            float d2 = dist2_cam(c);
            if(d2 > alcance * alcance || !na_campo(c, 60.0f)) continue;
            if(d2 < 280.0f * 280.0f) {
                ter_quad(i, j, 1); ter_quad(i + 1, j, 1);
                ter_quad(i, j + 1, 1); ter_quad(i + 1, j + 1, 1);
            }
            else ter_quad(i, j, 2);
        }
}

/* Nuvens: blocos achatados lá no alto (com a face de BAIXO, que é a que se vê). */
#define N_NUVENS 26
static float nuv[N_NUVENS][6];      /* x, y, z, meia-largura, meia-altura, meia-prof. */
static void montar_nuvens(void) {
    for(int i = 0; i < N_NUVENS; i++) {
        float ang = aleat(i + 8000) * 2 * PI_F, r = 900.0f + aleat(i + 8100) * 1700.0f;
        nuv[i][0] = CENTRO_X + cosf(ang) * r; nuv[i][2] = CENTRO_Z + sinf(ang) * r;
        nuv[i][1] = 230.0f + aleat(i + 8200) * 160.0f;
        nuv[i][3] = 120.0f + aleat(i + 8300) * 200.0f; nuv[i][4] = 12.0f + aleat(i + 8400) * 16.0f;
        nuv[i][5] = 80.0f + aleat(i + 8500) * 140.0f;
    }
}
/* NUVENS FOFAS: cada nuvem = 5 "tufos" (quadros de frente para a câmera,
 * com textura de algodão translúcida), na lista translúcida. Posições fixas
 * no mundo (não "nadam" com a câmera). */
static void desenhar_nuvens_tr(void) {
    if(nuvens < 0.12f) return;
    int n = (int)(N_NUVENS * (nuvens - 0.05f) / 0.95f);
    if(n > N_NUVENS) n = N_NUVENS;
    uint32_t cb = cor_mistura(cor_mistura(0xFFF4F6F8, 0xFF8A9299, chuva * 0.8f), cor_neb, 0.25f);
    cb = cor_escala(cb, 0.88f + 0.12f * brilho + relampago * 0.5f);
    uint32_t alfa = (uint32_t)(limitar(0.55f + nuvens * 0.45f, 0, 1) * 255) << 24;
    pvr_prim(&hdr_nuvem, sizeof(hdr_nuvem));
    const float up[3] = { 0, 1, 0 };
    for(int i = 0; i < n; i++) {
        float *c = nuv[i];
        if(!na_campo(c, c[3] + c[5])) continue;
        for(int t = 0; t < 5; t++) {
            uint32_t h = h32((uint32_t)(i * 8 + t) * 2654435761u);
            float ox = ((h & 255) / 255.0f - 0.5f) * c[3] * 1.3f, oz = (((h >> 8) & 255) / 255.0f - 0.5f) * c[5] * 1.2f;
            float tam = c[3] * (0.45f + ((h >> 16) & 255) / 255.0f * 0.35f) * (t == 0 ? 1.25f : 1.0f);
            float ce[3] = { c[0] + ox, c[1] + (t == 0 ? c[4] * 0.6f : 0.0f), c[2] + oz };
            float w = tam, hh = tam * 0.55f;
            uint32_t cor = alfa | (cor_escala(cb, t == 0 ? 1.0f : 0.94f) & 0xFFFFFF);
            vvt_t q[4];
            const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { 1, 1, -1, -1 };
            for(int v = 0; v < 4; v++) {
                vv_t pv;
                para_vista(ce[0] + cam_r[0] * w * sx[v] + up[0] * hh * sy[v], ce[1] + hh * sy[v],
                           ce[2] + cam_r[2] * w * sx[v], 0, &pv);
                q[v].x = pv.x; q[v].y = pv.y; q[v].z = pv.z; q[v].c = cor;
                q[v].u = sx[v] > 0 ? 1.0f : 0.0f; q[v].v = sy[v] > 0 ? 0.0f : 1.0f;
            }
            poligono_tex(q, 4);
        }
    }
}

/* ALAMBRADOS sobre os muros (guardados no passo opaco, desenhados na
 * lista translúcida): tela em losango com cabo no alto */
static void desenhar_cercas_tr(void) {
    if(!n_cerca) return;
    pvr_prim(&hdr_cerca, sizeof(hdr_cerca));
    for(int i = 0; i < n_cerca; i++) poligono_tex(cerca_q[i], 4);
}

/* NUVENS POLIGONAIS DETALHADAS: cada cúmulo = uma base achatada + 3 a 5
 * "tufos" empilhados, todos prismas OCTOGONAIS que afinam para cima (parecem
 * arredondados). A câmera está sempre abaixo das nuvens: só laterais e a
 * barriga são desenhadas. Laterais com degradê por vértice (claras em cima,
 * acinzentadas embaixo) e sombreadas pelo lado do sol; barriga escura. */
static const float OCT[8][2] = {
    { 0.924f, 0.383f }, { 0.383f, 0.924f }, { -0.383f, 0.924f }, { -0.924f, 0.383f },
    { -0.924f, -0.383f }, { -0.383f, -0.924f }, { 0.383f, -0.924f }, { 0.924f, -0.383f } };
static void bloco_nuvem(float cx, float cy, float cz, float rx, float rz, float hy, float afina,
                        uint32_t c_alto, uint32_t c_baixo, uint32_t c_barriga, int barriga) {
    vv_t b[8], t[8];
    float w[8][3];
    for(int k = 0; k < 8; k++) {
        w[k][0] = cx + OCT[k][0] * rx; w[k][1] = cy - hy; w[k][2] = cz + OCT[k][1] * rz;
        para_vista(w[k][0], w[k][1], w[k][2], 0, &b[k]);
        para_vista(cx + OCT[k][0] * rx * afina, cy + hy, cz + OCT[k][1] * rz * afina, 0, &t[k]);
    }
    float lh = sqrtf(LUZ[0] * LUZ[0] + LUZ[2] * LUZ[2]) + 1e-4f;
    for(int k = 0; k < 8; k++) {
        int k2 = (k + 1) & 7;
        float nx = (OCT[k][0] + OCT[k2][0]) * 0.5f, nz = (OCT[k][1] + OCT[k2][1]) * 0.5f;
        float mx = (w[k][0] + w[k2][0]) * 0.5f, mz = (w[k][2] + w[k2][2]) * 0.5f;
        if(nx * (mx - cam_p[0]) + nz * (mz - cam_p[2]) >= 0 && cy - hy > cam_p[1]) {
            /* lateral de costas: só se vê se a câmera estiver de baixo e a face inclinar para baixo — não */
            continue;
        }
        float sol = (nx * LUZ[0] + nz * LUZ[2]) / lh;            /* lado do sol mais claro */
        float k_l = 0.86f + 0.14f * (sol > 0 ? sol : 0) - 0.06f * (sol < 0 ? -sol : 0);
        uint32_t ca = cor_escala(c_alto, k_l), cbx = cor_escala(c_baixo, k_l);
        vv_t q[4] = { b[k], b[k2], t[k2], t[k] };
        q[0].c = q[1].c = cbx; q[2].c = q[3].c = ca;
        poligono(q, 4);
    }
    if(barriga && cy - hy > cam_p[1]) {                         /* barriga (vista de baixo) */
        vv_t q[8];
        for(int k = 0; k < 8; k++) { q[k] = b[7 - k]; q[k].c = c_barriga; }
        poligono(q, 8);
    }
}
static void desenhar_nuvens(void) {
    if(nuvens < 0.12f) return;
    int n = (int)(N_NUVENS * (nuvens - 0.05f) / 0.95f);
    if(n > N_NUVENS) n = N_NUVENS;
    float lz = 0.86f + 0.14f * brilho + relampago * 0.5f;
    uint32_t c_alto = cor_escala(cor_mistura(cor_mistura(0xFFFAFBFC, 0xFF9AA2AA, chuva * 0.8f), cor_neb, 0.18f), lz);
    uint32_t c_baixo = cor_escala(cor_mistura(c_alto, 0xFF9AA4AE, 0.25f + chuva * 0.25f), 1.0f);
    uint32_t c_bar = cor_escala(cor_mistura(c_alto, 0xFF7A848E, 0.40f + chuva * 0.3f), 1.0f);
    for(int i = 0; i < n; i++) {
        float *c = nuv[i];
        if(!na_campo(c, c[3] + c[5])) continue;
        /* base achatada e larga */
        bloco_nuvem(c[0], c[1], c[2], c[3] * 0.72f, c[5] * 0.72f, c[4] * 0.35f, 0.92f, c_baixo, c_bar, c_bar, 1);
        /* tufos em cima: 3 a 5, tamanhos e alturas variadas */
        uint32_t h0 = h32((uint32_t)i * 2654435761u);
        int nt = 4 + (int)(h0 % 3);
        for(int t = 0; t < nt; t++) {
            uint32_t h = h32((uint32_t)(i * 16 + t) * 40503u + 7);
            float ox = ((h & 255) / 255.0f - 0.5f) * c[3] * 1.05f;
            float oz = (((h >> 8) & 255) / 255.0f - 0.5f) * c[5] * 0.9f;
            float r = 0.36f + ((h >> 16) & 255) / 255.0f * 0.30f;
            if(t == 0) { ox *= 0.3f; oz *= 0.3f; r += 0.12f; }       /* tufo central maior */
            float alt = c[4] * (1.1f + ((h >> 24) & 255) / 255.0f * 1.3f) * (t == 0 ? 1.5f : 1.0f);
            float y0 = c[1] + c[4] * 0.35f - alt * 0.35f;           /* tufos "afundados" na base */
            bloco_nuvem(c[0] + ox, y0 + alt, c[2] + oz, c[3] * r, c[5] * r * 1.1f, alt, 0.55f,
                        c_alto, c_baixo, c_bar, 1);
        }
    }
}

/* CIRROS: faixas finas e altas (só vistas por baixo), um pouco mais claras
 * que o céu; somem com tempo fechado */
#define N_CIRROS 14
static void desenhar_cirros(void) {
    float k = (1.0f - chuva) * (0.6f + 0.4f * (1.0f - nuvens));
    if(k < 0.1f) return;
    uint32_t c = cor_mistura(cor_ceu, 0xFFFFFFFF, 0.22f * k);
    for(int i = 0; i < N_CIRROS; i++) {
        uint32_t h = h32((uint32_t)i * 7919u + 77);
        float ang = (h & 1023) / 1023.0f * 2 * PI_F, r = 1400.0f + ((h >> 10) & 1023) * 1.4f;
        float cc[3] = { CENTRO_X + cosf(ang) * r, 700.0f + ((h >> 20) & 255), CENTRO_Z + sinf(ang) * r };
        if(!na_campo(cc, 700.0f)) continue;
        float dir = 0.6f + ((h >> 4) & 63) * 0.01f, cl = 380.0f + ((h >> 12) & 255), lg = 22.0f + ((h >> 18) & 31);
        float dx = cosf(dir), dz = sinf(dir);
        float p[4][2] = { { -cl, -lg }, { cl, -lg * 0.4f }, { cl, lg * 0.4f }, { -cl, lg } };
        vv_t q[4];
        for(int v = 0; v < 4; v++)
            para_vista(cc[0] + dx * p[v][0] - dz * p[v][1], cc[1], cc[2] + dz * p[v][0] + dx * p[v][1], c, &q[v]);
        poligono(q, 4);
    }
}

#if SKYBOX
/* ANEL DE SILHUETAS a ~1,7 km do centro do autódromo (fixo no mundo): morros
 * e, ao norte, blocos da cidade, em tons de névoa. Como está mais perto que
 * o céu pintado, desliza contra ele quando o carro anda (profundidade). */
#define N_ANEL 72
static float anel_h[N_ANEL], anel_y0;
static uint8_t anel_cid[N_ANEL];
static void montar_anel(void) {
    float ymin = 1e9f;
    for(int k = 0; k < NF; k++) if(F[k].y < ymin) ymin = F[k].y;
    anel_y0 = ymin - 25.0f;
    for(int i = 0; i < N_ANEL; i++) {
        float a = (i + 0.5f) * 2 * PI_F / N_ANEL;
        float norte = expf(-powf(((a - 1.5f * PI_F)) / 0.9f, 2));          /* 270° = norte */
        anel_cid[i] = 0;          /* a cidade agora é o panorama pintado (desenhar_cidade_tr) */
        (void)norte;
        /* morros do anel: o ponto mais alto do RELEVO REAL naquela direção (1,5 a 2,6 km) */
        float hm = -1e9f;
        for(float r = 1500.0f; r <= 2600.0f; r += 100.0f)
            for(int da = -1; da <= 1; da++) {
                float aa = a + da * 0.03f;
                float hr = relevo_em(CENTRO_X + cosf(aa) * r, CENTRO_Z + sinf(aa) * r);
                if(hr > hm) hm = hr;
            }
        anel_h[i] = limitar(hm - anel_y0 - 25.0f + 8.0f, 12.0f, 140.0f);
    }
}
static void desenhar_anel(void) {
    const float R = 1700.0f;
    uint32_t c_morro = cor_escala(cor_mistura(0xFF4E6E5A, cor_neb, 0.62f + 0.25f * chuva), 0.92f + 0.08f * brilho);
    uint32_t c_cid = cor_escala(cor_mistura(0xFFB8B0A2, cor_neb, 0.55f + 0.25f * chuva), 0.92f + 0.08f * brilho);
    uint32_t c_base = cor_neb;
    for(int i = 0; i < N_ANEL; i++) {
        float a0 = i * 2 * PI_F / N_ANEL, a1 = (i + 1) * 2 * PI_F / N_ANEL;
        float am = (a0 + a1) * 0.5f;
        float cm[3] = { CENTRO_X + fcos(am) * R, anel_y0, CENTRO_Z + fsin(am) * R };
        if(!na_campo(cm, 160.0f)) continue;
        float h0 = anel_h[i], h1 = anel_h[(i + 1) % N_ANEL];
        uint32_t ct = anel_cid[i] ? c_cid : c_morro;
        if(anel_cid[i]) h1 = h0;                                         /* bloco: topo reto */
        else h1 = (h0 + h1) * 0.5f;
        vv_t q[4];
        para_vista(CENTRO_X + fcos(a0) * R, anel_y0, CENTRO_Z + fsin(a0) * R, c_base, &q[0]);
        para_vista(CENTRO_X + fcos(a1) * R, anel_y0, CENTRO_Z + fsin(a1) * R, c_base, &q[1]);
        para_vista(CENTRO_X + fcos(a1) * R, anel_y0 + 25.0f + h1, CENTRO_Z + fsin(a1) * R, ct, &q[2]);
        para_vista(CENTRO_X + fcos(a0) * R, anel_y0 + 25.0f + h0, CENTRO_Z + fsin(a0) * R, ct, &q[3]);
        poligono(q, 4);
    }
}
/* CIDADE PINTADA (como os horizontes de Le Mans/MSR): panorama de São Paulo
 * numa textura com recorte (prédios, janelas, caixas d'água, antenas) num
 * arco ao norte, na frente dos morros do anel. A névoa entra pela cor somada
 * (cor = textura x (1-h) + névoa x h). No entardecer, uma 2ª passada soma as
 * janelas acesas e as luzes vermelhas das antenas. */
#define CID_SEG 26
static void nevoa_faixa_tr(float R, float y0, float y1, float alfa, float a_cent, float largura);
static void desenhar_cidade_camada(float R, float ALT, float u_ofs, float nev_extra, float contraluz);
static void desenhar_cidade_tr(void) {
    /* 3 camadas: a do fundo (mais alta, mais enevoada, em contraluz no
     * entardecer), névoa entre elas e a da frente (a de antes) */
    desenhar_cidade_camada(2150.0f, 380.0f, 0.43f, 0.28f, 0.45f);
    nevoa_faixa_tr(1900.0f, 0.0f, 210.0f, 0.55f, 1.5f * PI_F, 1.1f);
    desenhar_cidade_camada(1640.0f, 270.0f, 0.0f, 0.0f, 0.0f);
}
static void desenhar_cidade_camada(float R, float ALT, float u_ofs, float nev_extra, float contraluz) {
    const float A0 = 1.5f * PI_F - 1.05f, A1 = 1.5f * PI_F + 1.05f;
    float h = limitar(0.22f + nev_extra + 0.20f * nuvens + 0.28f * chuva, 0, 0.9f);
    float l = (1.0f - h) * (0.80f + 0.20f * brilho) * (1.0f - 0.30f * tarde) * (1.0f - contraluz * limitar(tarde, 0, 1));
    uint32_t cg = (uint32_t)(limitar(l, 0, 1) * 255);
    uint32_t o = cor_escala(cor_neb, h) & 0x00FFFFFF;
    float tt = limitar(tarde, 0, 1); tt = tt * tt * (3 - 2 * tt);
    float kl = limitar(tt * (1.0f - 0.4f * chuva) + 0.30f * pesada + 0.15f * nuvens * chuva, 0, 1);
    for(int passe = 0; passe < 2; passe++) {
        if(passe == 1 && kl < 0.03f) break;
        pvr_prim(passe ? &hdr_cidade_luz : &hdr_cidade, sizeof(pvr_poly_hdr_t));
        for(int i = 0; i < CID_SEG; i++) {
            float t0 = i / (float)CID_SEG, t1 = (i + 1) / (float)CID_SEG;
            float a0 = A0 + (A1 - A0) * t0, a1 = A0 + (A1 - A0) * t1;
            float am = (a0 + a1) * 0.5f;
            float cm[3] = { CENTRO_X + fcos(am) * R, anel_y0 + ALT * 0.4f, CENTRO_Z + fsin(am) * R };
            if(!na_campo(cm, 220.0f)) continue;
            float tv[2] = { t0, t1 }, aa[2] = { a0, a1 };
            vvt_t q[4];
            for(int e = 0; e < 2; e++) {
                float ext = tv[e] < 0.5f ? tv[e] : 1.0f - tv[e];
                float hf = limitar(ext / 0.16f, 0.30f, 1.0f);            /* pontas: só os prédios baixos */
                uint32_t al = (uint32_t)(255 * limitar(ext / 0.05f, 0, 1));
                float u = (aa[e] - A0) * R / 1930.0f + u_ofs;             /* 1 repetição a cada ~1,2 km */
                float x = CENTRO_X + fcos(aa[e]) * R, z = CENTRO_Z + fsin(aa[e]) * R;
                vv_t b, t;
                para_vista(x, anel_y0, z, 0, &b);
                para_vista(x, anel_y0 + ALT * hf, z, 0, &t);
                uint32_t c = passe ? (0xFF000000 | (uint32_t)(kl * 255) * 0x010101u) : (al << 24 | cg * 0x010101u);
                int ib = e ? 1 : 0, it = e ? 2 : 3;
                q[ib].x = b.x; q[ib].y = b.y; q[ib].z = b.z; q[ib].u = u; q[ib].v = 1.0f; q[ib].c = c; q[ib].o = passe ? 0 : o;
                q[it].x = t.x; q[it].y = t.y; q[it].z = t.z; q[it].u = u; q[it].v = 1.0f - hf; q[it].c = c; q[it].o = passe ? 0 : o;
            }
            poligono_tex(q, 4);
        }
    }
}
/* CAMADAS DO MEIO DO CAMINHO: (1) anel de BAIRROS a ~1,1 km (telhados,
 * prédios baixos, linha de árvores), com a névoa somada; (2) faixa de
 * NÉVOA URBANA (poluição) entre a cidade e os bairros, mais densa ao norte. */
static void desenhar_bairros_tr(void) {
    const float R = 1120.0f, ALT = 40.0f;
    const int NS = 40;
    float h = limitar(0.34f + 0.20f * nuvens + 0.28f * chuva, 0, 0.9f);
    float l = (1.0f - h) * (0.80f + 0.20f * brilho) * (1.0f - 0.25f * tarde);
    uint32_t cg = (uint32_t)(limitar(l, 0, 1) * 255) * 0x010101u | 0xFF000000;
    uint32_t o = cor_escala(cor_neb, h) & 0x00FFFFFF;
    float tt = limitar(tarde, 0, 1); tt = tt * tt * (3 - 2 * tt);
    float kl = limitar(tt * (1.0f - 0.4f * chuva) + 0.30f * pesada, 0, 1);
    uint32_t ckl = 0xFF000000 | (uint32_t)(kl * 255) * 0x010101u;
    pvr_prim(&hdr_bairros, sizeof(pvr_poly_hdr_t));
    for(int i = 0; i < NS; i++) {
        float a0 = i * 2 * PI_F / NS, a1 = (i + 1) * 2 * PI_F / NS, am = (a0 + a1) * 0.5f;
        float cm[3] = { CENTRO_X + fcos(am) * R, anel_y0 + ALT * 0.5f, CENTRO_Z + fsin(am) * R };
        if(!na_campo(cm, 120.0f)) continue;
        float u0 = a0 * R / 1190.0f, u1 = a1 * R / 1190.0f;      /* 512 px a cada ~1,2 km */
        vv_t b0, b1, t0, t1;
        float x0 = CENTRO_X + fcos(a0) * R, z0 = CENTRO_Z + fsin(a0) * R, x1 = CENTRO_X + fcos(a1) * R, z1 = CENTRO_Z + fsin(a1) * R;
        static float by[41]; static int by_ok;
        if(!by_ok) {                      /* altura do relevo sob o anel: calculada uma vez */
            for(int k = 0; k <= NS; k++) { float a = k * 2 * PI_F / NS; by[k] = terreno_passo(CENTRO_X + fcos(a) * R, CENTRO_Z + fsin(a) * R, 2) - 5.0f; }
            by_ok = 1;
        }
        float y0 = by[i], y1 = by[i + 1];
        para_vista(x0, y0, z0, 0, &b0);
        para_vista(x1, y1, z1, 0, &b1);
        para_vista(x1, y1 + ALT, z1, 0, &t1);
        para_vista(x0, y0 + ALT, z0, 0, &t0);
        vvt_t q[4] = { { b0.x, b0.y, b0.z, u0, 1, cg, o }, { b1.x, b1.y, b1.z, u1, 1, cg, o },
                       { t1.x, t1.y, t1.z, u1, 0, cg, o }, { t0.x, t0.y, t0.z, u0, 0, cg, o } };
        poligono_tex(q, 4);
        if(kl > 0.03f) {               /* entardecer: janelas e postes acesos */
            pvr_prim(&hdr_bairros_luz, sizeof(pvr_poly_hdr_t));
            for(int v = 0; v < 4; v++) { q[v].c = ckl; q[v].o = 0; }
            poligono_tex(q, 4);
            /* trânsito na avenida: faróis num sentido, lanternas no outro, andando */
            pvr_prim(&hdr_transito, sizeof(pvr_poly_hdr_t));
            for(int sent = 0; sent < 2; sent++) {
                float desl = (sent ? -1.0f : 1.0f) * t_clima * 0.012f;
                float va = sent ? 0.5f : 0.0f, vb = sent ? 1.0f : 0.5f;
                vv_t c0, c1, c2, c3;
                float r2 = R - 25.0f;
                float xa = CENTRO_X + fcos(a0) * r2, za = CENTRO_Z + fsin(a0) * r2, xb = CENTRO_X + fcos(a1) * r2, zb = CENTRO_Z + fsin(a1) * r2;
                para_vista(xa, y0 + 5.2f, za, 0, &c0); para_vista(xb, y1 + 5.2f, zb, 0, &c1);
                para_vista(xb, y1 + 7.0f, zb, 0, &c2); para_vista(xa, y0 + 7.0f, za, 0, &c3);
                float ua = a0 * r2 / 420.0f + desl, ub = a1 * r2 / 420.0f + desl;
                vvt_t w[4] = { { c0.x, c0.y, c0.z, ua, vb, ckl, 0 }, { c1.x, c1.y, c1.z, ub, vb, ckl, 0 },
                               { c2.x, c2.y, c2.z, ub, va, ckl, 0 }, { c3.x, c3.y, c3.z, ua, va, ckl, 0 } };
                poligono_tex(w, 4);
            }
            pvr_prim(&hdr_bairros, sizeof(pvr_poly_hdr_t));
        }
    }
}
/* faixa de névoa num arco (centrada no ângulo a_cent), mais densa no meio */
static void nevoa_faixa_tr(float R, float y0, float y1, float alfa, float a_cent, float largura) {
    const int NS = 24;
    uint32_t cn = cor_mistura(cor_neb, 0xFFA89A86, 0.30f * (1.0f - chuva)) & 0x00FFFFFF;
    if(tarde > 0.05f) cn = cor_mistura(cn | 0xFF000000, 0xFFE8A878, 0.45f * limitar(tarde, 0, 1)) & 0x00FFFFFF;
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    for(int i = 0; i < NS; i++) {
        float a0 = a_cent - largura + 2 * largura * i / NS, a1 = a_cent - largura + 2 * largura * (i + 1) / NS;
        float am = (a0 + a1) * 0.5f;
        float cm[3] = { CENTRO_X + fcos(am) * R, anel_y0 + (y0 + y1) * 0.5f, CENTRO_Z + fsin(am) * R };
        if(!na_campo(cm, 200.0f)) continue;
        float e0 = 1.0f - fabsf(a0 - a_cent) / largura, e1 = 1.0f - fabsf(a1 - a_cent) / largura;
        uint32_t al0 = (uint32_t)(255 * alfa * e0) << 24, al1 = (uint32_t)(255 * alfa * e1) << 24;
        vv_t q[4];
        para_vista(CENTRO_X + fcos(a0) * R, anel_y0 + y0, CENTRO_Z + fsin(a0) * R, al0 | cn, &q[0]);
        para_vista(CENTRO_X + fcos(a1) * R, anel_y0 + y0, CENTRO_Z + fsin(a1) * R, al1 | cn, &q[1]);
        para_vista(CENTRO_X + fcos(a1) * R, anel_y0 + y1, CENTRO_Z + fsin(a1) * R, cn, &q[2]);
        para_vista(CENTRO_X + fcos(a0) * R, anel_y0 + y1, CENTRO_Z + fsin(a0) * R, cn, &q[3]);
        poligono(q, 4);
    }
}
static void desenhar_nevoa_urbana_tr(void) {
    const float R = 1560.0f;
    const int NS = 36;
    uint32_t cn = cor_mistura(cor_neb, 0xFFA89A86, 0.35f * (1.0f - chuva)) & 0x00FFFFFF;
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    for(int i = 0; i < NS; i++) {
        float a0 = i * 2 * PI_F / NS, a1 = (i + 1) * 2 * PI_F / NS, am = (a0 + a1) * 0.5f;
        float cm[3] = { CENTRO_X + fcos(am) * R, anel_y0 + 60.0f, CENTRO_Z + fsin(am) * R };
        if(!na_campo(cm, 160.0f)) continue;
        static float nn[37]; static int nn_ok;
        if(!nn_ok) { for(int k = 0; k <= NS; k++) { float d = k * 2 * PI_F / NS - 1.5f * PI_F; nn[k] = 0.35f + 0.65f * expf(-d * d / 0.8f); } nn_ok = 1; }   /* norte = cidade */
        float n0 = nn[i], n1 = nn[i + 1];
        uint32_t al0 = (uint32_t)(150 * n0) << 24, al1 = (uint32_t)(150 * n1) << 24;
        vv_t q[4];
        para_vista(CENTRO_X + fcos(a0) * R, anel_y0 + 10.0f, CENTRO_Z + fsin(a0) * R, al0 | cn, &q[0]);
        para_vista(CENTRO_X + fcos(a1) * R, anel_y0 + 10.0f, CENTRO_Z + fsin(a1) * R, al1 | cn, &q[1]);
        para_vista(CENTRO_X + fcos(a1) * R, anel_y0 + 150.0f, CENTRO_Z + fsin(a1) * R, cn, &q[2]);
        para_vista(CENTRO_X + fcos(a0) * R, anel_y0 + 150.0f, CENTRO_Z + fsin(a0) * R, cn, &q[3]);
        poligono(q, 4);
    }
}
/* NUVENS EM MOVIMENTO: um anel de nuvens translúcidas na frente do céu
 * pintado, andando devagar com o vento. Centrado na câmera: fica "no
 * infinito", como o céu. */
static void desenhar_nuvens_mov_tr(void) {
    float k = (0.55f + 0.45f * limitar(nuvens * 1.6f, 0, 1)) * (1.0f - 0.65f * chuva);
    if(k < 0.05f) return;
    const float R = 2600.0f, Y0 = 210.0f, Y1 = 760.0f;
    const int NS = 28;
    uint32_t cb = cor_mistura(0xFFFFFFFF, cor_neb, 0.20f + 0.25f * nuvens);
    cb = cor_escala(cb, 0.70f + 0.30f * brilho);
    if(tarde > 0.05f) cb = cor_mistura(cb, 0xFFFFC0A0, 0.55f * limitar(tarde, 0, 1));
    uint32_t c = ((uint32_t)(k * 255) << 24) | (cb & 0x00FFFFFF);
    float desl = t_clima * 0.0022f;                          /* volta inteira em ~7 min */
    pvr_prim(&hdr_nuv_mov, sizeof(hdr_nuv_mov));
    for(int camada = 0; camada < 2; camada++) {
        float r = R - camada * 300.0f, y0 = Y0 + camada * 90.0f, y1 = Y1 - camada * 120.0f;
        float vel = camada ? 1.6f : 1.0f, rp = camada ? 4.0f : 3.0f, v_ofs = 0.0f;   /* repetições INTEIRAS: com 4,3 a textura "saltava" no fecho do anel e cortava a nuvem */   /* textura inteira na altura (antes a 2ª camada cortava a nuvem no meio) */
        for(int i = 0; i < NS; i++) {
            float a0 = i * 2 * PI_F / NS, a1 = (i + 1) * 2 * PI_F / NS, am = (a0 + a1) * 0.5f;
            /* o segmento é largo (~580 m): testa o meio E as duas pontas; antes só o meio,
             * e um segmento meio na tela sumia deixando a nuvem cortada em linha reta */
            float cm[3] = { cam_p[0] + fcos(am) * r, cam_p[1] + (y0 + y1) * 0.5f, cam_p[2] + fsin(am) * r };
            float ca[3] = { cam_p[0] + fcos(a0) * r, cm[1], cam_p[2] + fsin(a0) * r };
            float cz[3] = { cam_p[0] + fcos(a1) * r, cm[1], cam_p[2] + fsin(a1) * r };
            if(!na_campo(cm, 900.0f) && !na_campo(ca, 900.0f) && !na_campo(cz, 900.0f)) continue;
            float u0 = a0 / (2 * PI_F) * rp + desl * vel + camada * 0.37f, u1 = a1 / (2 * PI_F) * rp + desl * vel + camada * 0.37f;
            vv_t q[4];
            para_vista(cam_p[0] + fcos(a0) * r, cam_p[1] + y0, cam_p[2] + fsin(a0) * r, 0, &q[0]);
            para_vista(cam_p[0] + fcos(a1) * r, cam_p[1] + y0, cam_p[2] + fsin(a1) * r, 0, &q[1]);
            para_vista(cam_p[0] + fcos(a1) * r, cam_p[1] + y1, cam_p[2] + fsin(a1) * r, 0, &q[2]);
            para_vista(cam_p[0] + fcos(a0) * r, cam_p[1] + y1, cam_p[2] + fsin(a0) * r, 0, &q[3]);
            /* entardecer: as nuvens do lado do sol ficam acesas (douradas e mais opacas) */
            uint32_t c0 = c, c1 = c;
            if(tarde > 0.05f) {
                float sa = atan2f(LUZ[2], LUZ[0]);
                float d0 = fcos(a0 - sa), d1 = fcos(a1 - sa);
                d0 = d0 > 0 ? d0 * d0 * d0 * d0 : 0; d1 = d1 > 0 ? d1 * d1 * d1 * d1 : 0;
                float k0 = tarde * d0, k1 = tarde * d1;
                c0 = ((uint32_t)limitar((c >> 24) * (1.0f + 0.5f * k0), 0, 255) << 24) | (cor_mistura(c, 0xFFFFD088, k0 * 0.85f) & 0xFFFFFF);
                c1 = ((uint32_t)limitar((c >> 24) * (1.0f + 0.5f * k1), 0, 255) << 24) | (cor_mistura(c, 0xFFFFD088, k1 * 0.85f) & 0xFFFFFF);
            }
            vvt_t t[4] = { { q[0].x, q[0].y, q[0].z, u0, 1.0f, c0, 0 }, { q[1].x, q[1].y, q[1].z, u1, 1.0f, c1, 0 },
                           { q[2].x, q[2].y, q[2].z, u1, v_ofs, c1, 0 }, { q[3].x, q[3].y, q[3].z, u0, v_ofs, c0, 0 } };
            poligono_tex(t, 4);
        }
    }
}
/* PÁSSAROS: de vez em quando um bando cruza o céu batendo as asas */
static void desenhar_passaros(void) {
    float ciclo = fmodf(t_clima, 41.0f);
    if(ciclo > 22.0f || chuva > 0.5f) return;
    float t = ciclo / 22.0f;
    uint32_t sem = (uint32_t)(t_clima / 41.0f);
    float ang = aleat(sem * 7 + 1) * 2 * PI_F;
    float dx = fcos(ang), dz = fsin(ang);
    float c0[3] = { cam_p[0] - dx * 240.0f + dz * 80.0f + dx * 480.0f * t, cam_p[1] + 45.0f + 25.0f * aleat(sem * 7 + 2),
                    cam_p[2] - dz * 240.0f - dx * 80.0f + dz * 480.0f * t };
    uint32_t cor = cor_escala(0xFF2A2C30, 0.8f + 0.2f * brilho);
    for(int b = 0; b < 6; b++) {
        float lado = (b & 1) ? 1.0f : -1.0f, fila = (float)((b + 1) / 2);
        float p[3] = { c0[0] - dx * fila * 3.0f + dz * lado * fila * 2.4f, c0[1] + fila * 0.5f + aleat(sem * 13 + b) * 1.5f,
                       c0[2] - dz * fila * 3.0f - dx * lado * fila * 2.4f };
        float bate = fsin(t_clima * 11.0f + b * 1.9f) * 0.55f;
        vv_t q[3];
        for(int asa = -1; asa <= 1; asa += 2) {
            para_vista(p[0], p[1], p[2], cor, &q[0]);
            para_vista(p[0] + dx * 0.35f, p[1], p[2] + dz * 0.35f, cor, &q[1]);
            para_vista(p[0] + dz * asa * 0.9f, p[1] + bate, p[2] - dx * asa * 0.9f, cor, &q[2]);
            if(q[0].z < -PERTO && q[1].z < -PERTO && q[2].z < -PERTO) poligono(q, 3);
        }
    }
}
/* AVIÕES chegando em Congonhas (ao norte): cruzam o céu de leste para oeste,
 * descendo, a cada ~75 s; luzes piscando no entardecer/chuva */
static void desenhar_aviao(void) {
    float ciclo = fmodf(t_clima + 20.0f, 75.0f);
    if(ciclo > 48.0f) return;
    float t = ciclo / 48.0f;
    float p[3] = { CENTRO_X + 3600.0f - 7200.0f * t, 560.0f - 220.0f * t, CENTRO_Z - 2600.0f - 300.0f * t };
    if(!na_campo(p, 60.0f)) return;
    static const float Rr[3] = { -1, 0, 0 }, Ff[3] = { 0, 0, 1 };      /* voa para -x (oeste) */
    float dir[3] = { -0.998f, -0.06f, 0 }, lado[3] = { 0, 0, 1 };
    (void)Rr; (void)Ff;
    uint32_t c = cor_escala(cor_mistura(0xFFE8ECF0, cor_neb, 0.45f), 0.85f + 0.15f * brilho);
    float fu[3] = { p[0], p[1], p[2] };
    desenhar_caixa(fu, lado, dir, 2.2f, 2.2f, 19.0f, c, c);                    /* fuselagem */
    float asa[3] = { p[0] + 2.0f, p[1] - 0.5f, p[2] };
    desenhar_caixa(asa, lado, dir, 17.0f, 0.4f, 3.5f, c, c);                   /* asas */
    float leme[3] = { p[0] + 17.0f, p[1] + 4.0f, p[2] };
    desenhar_caixa(leme, lado, dir, 0.4f, 4.0f, 2.2f, c, c);                   /* leme */
    float est[3] = { p[0] + 17.5f, p[1] + 1.0f, p[2] };
    desenhar_caixa(est, lado, dir, 6.0f, 0.3f, 1.6f, c, c);                    /* estabilizador */
    if((tarde > 0.3f || nuvens > 0.6f) && ((int)(t_clima * 1.5f) & 1)) {
        float luz[3] = { p[0], p[1] - 2.6f, p[2] };
        desenhar_caixa(luz, lado, dir, 1.5f, 1.5f, 1.5f, 0xFFFF3020, 0xFFFF3020);
    }
}
#endif

static void desenhar_longe(void) {
#if SKYBOX
    desenhar_anel();
    desenhar_aviao();
    desenhar_passaros();
#endif
#if !SKYBOX
    desenhar_cirros();
    for(int i = 0; i < n_morros; i++) {
        tri_t *t = &morros[i];
        if(!na_campo(t->p[1], 450.0f)) continue;
        vv_t q[3];
        uint32_t cm = cor_escala(cor_mistura(t->cor, cor_neb, nuvens * 0.5f + chuva * 0.3f), brilho);
        for(int v = 0; v < 3; v++) para_vista(t->p[v][0], t->p[v][1], t->p[v][2], cm, &q[v]);
        poligono(q, 3);
    }
    pvr_prim(&hdr_janelas, sizeof(hdr_janelas)); caixa_lote = 1;
#endif
    for(int i = 0; i < n_cidade; i++) {
        predio_t *d = &cidade[i];
        if(!na_campo(d->c, d->hx + d->hz)) continue;
        float tn = nuvens * 0.4f + chuva * 0.35f;
        caixa_tex = 6;
        desenhar_caixa(d->c, d->r, d->f, d->hx, d->hy, d->hz, cor_mistura(d->cor, cor_neb, tn), cor_mistura(d->topo, cor_neb, tn));
        caixa_tex = 0;
    }
    caixa_lote = 0; pvr_prim(&hdr_longe, sizeof(hdr_longe));
#if !SKYBOX
    for(int i = 0; i < n_sky; i++) {
        predio_t *d = &skyline[i];
        if(!na_campo(d->c, d->hx + d->hz)) continue;
        float tn = nuvens * 0.45f + chuva * 0.45f;
        desenhar_caixa(d->c, d->r, d->f, d->hx, d->hy, d->hz, cor_mistura(d->cor, cor_neb, tn), cor_mistura(d->topo, cor_neb, tn));
    }
    desenhar_nuvens();
#endif
}

/* ÁRVORES EM "X" (como nos jogos da época): dois planos cruzados com uma
 * textura de árvore VAZADA (copa recortada, galhos, tronco), na lista
 * translúcida. Parecem bem mais orgânicas que as pirâmides e custam menos.
 * -DARV_X=0 volta às pirâmides. */
#ifndef ARV_X
#define ARV_X 1
#endif
#define MAX_ARV_X 160
static uint16_t arv_x[MAX_ARV_X];
static inline int e_ipe(uint32_t c) { return c == 0xFFFFE23A || c == 0xFFB04C9A || c == 0xFFF08CC0 || c == 0xFFF4F0EA; }
static float arv_dy[MAX_ARV_X];
static int n_arv_x;
static pvr_poly_hdr_t hdr_arvx[3];      /* 0 folhosa, 1 pinheiro, 2 ipê */
static void desenhar_arvores_tr(void) {
    if(!n_arv_x) return;
    float sh = sqrtf(LUZ[0] * LUZ[0] + LUZ[2] * LUZ[2]) + 1e-4f;
    for(int tipo = 0; tipo < 3; tipo++) {
        int ativo = 0;
        for(int k = 0; k < n_arv_x; k++) {
            const arvore_t *a = &arvores[arv_x[k]];
            int ipe = e_ipe(a->cor);
            int pinheiro = !ipe && a->alt >= 3.0f && (h32((uint32_t)arv_x[k] * 17 + 3) % 10) < 3;
            if((ipe ? 2 : pinheiro) != tipo) continue;
            if(!ativo) { pvr_prim(&hdr_arvx[tipo], sizeof(pvr_poly_hdr_t)); ativo = 1; }
            float y0 = a->p[1] + arv_dy[k] - 0.2f, y1 = y0 + a->alt * 1.08f;
            float w = a->raio * (ipe ? 1.65f : pinheiro ? 1.05f : 1.30f);
            for(int pl = 0; pl < 2; pl++) {
                float ang = a->giro + pl * PI_F * 0.5f;
                float cx = fcos(ang) * w, cz = fsin(ang) * w;
                /* lado virado para o sol mais claro (luz pelo plano) */
                float d = fabsf(-fsin(ang) * LUZ[0] + fcos(ang) * LUZ[2]) / sh;
                float lz = (luz_base + luz_dir * (0.35f + 0.65f * d)) * brilho;
                if(ipe) lz = 0.80f + 0.25f * lz;     /* flores claras, sombra suave */
                uint32_t c = cor_escala(a->cor, lz * (ipe ? 1.0f : 1.15f)), cb = cor_escala(c, 0.72f);   /* base mais escura */
                vvt_t t[4];
                const float px[4] = { -cx, cx, cx, -cx }, pz[4] = { -cz, cz, cz, -cz }, py[4] = { y0, y0, y1, y1 };
                const float U[4] = { 0, 1, 1, 0 }, V[4] = { 1, 1, 0, 0 };
                for(int v = 0; v < 4; v++) {
                    vv_t q;
                    para_vista(a->p[0] + px[v], py[v], a->p[2] + pz[v], 0, &q);
                    t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = V[v];
                    t[v].c = v < 2 ? cb : c; t[v].o = 0;
                }
                poligono_tex(t, 4);
            }
        }
    }
}
static void criar_textura_arvores(void) {
    /* 64x128 ARGB4444: tronco embaixo, copa recortada com "tufos" e vãos.
     * A copa cabe INTEIRA na textura (borda com folga), senão sai cortada reta. */
    static const float BF[6][4] = {      /* folhosa: cx, cy, rx, ry */
        { 0, 0.66f, 0.34f, 0.24f }, { -0.24f, 0.57f, 0.20f, 0.16f }, { 0.24f, 0.59f, 0.21f, 0.16f },
        { -0.14f, 0.79f, 0.20f, 0.14f }, { 0.15f, 0.81f, 0.19f, 0.13f }, { 0, 0.47f, 0.24f, 0.13f } };
    static const float BI[7][4] = {      /* ipê: copa larga e achatada, em tufos soltos */
        { 0, 0.77f, 0.40f, 0.12f }, { -0.27f, 0.70f, 0.18f, 0.10f }, { 0.27f, 0.71f, 0.19f, 0.10f },
        { -0.10f, 0.85f, 0.22f, 0.09f }, { 0.14f, 0.86f, 0.20f, 0.08f }, { -0.36f, 0.78f, 0.10f, 0.08f }, { 0.37f, 0.79f, 0.10f, 0.07f } };
    for(int tipo = 0; tipo < 2; tipo++) {     /* o ipê (2) vem do texturas.bin */
        for(int y = 0; y < 128; y++)
            for(int x = 0; x < 64; x++) {
                float u = (x + 0.5f) / 64.0f - 0.5f, v = 1.0f - (y + 0.5f) / 128.0f;   /* v: 0 base .. 1 topo */
                int a = 0, l = 0, r = 0, g = 0, b = 0;
                /* tronco */
                float alt_tronco = tipo == 1 ? 0.25f : tipo == 2 ? 0.66f : 0.38f;
                if(fabsf(u) < 0.040f + 0.02f * (1 - v) && v < alt_tronco) { a = 15; r = 8; g = 6; b = 4; }
                /* galhos do ipê: saem do tronco em "V" até a copa */
                if(tipo == 2 && v > 0.40f && v < 0.80f) {
                    float t = (v - 0.40f) / 0.40f;
                    static const float G[4] = { -0.32f, -0.13f, 0.12f, 0.31f };
                    for(int g = 0; g < 4; g++)
                        if(fabsf(u - G[g] * t) < 0.018f + 0.012f * (1 - t)) { a = 15; r = 8; g = 6; b = 4; }
                }
                float dens = 0;
                if(tipo != 1) {
                    const float (*B)[4] = tipo == 2 ? BI : BF;
                    int nb = tipo == 2 ? 7 : 6;
                    for(int k = 0; k < nb; k++) {
                        float dx = (u - B[k][0]) / B[k][2], dy = (v - B[k][1]) / B[k][3];
                        float e = 1.0f - (dx * dx + dy * dy);
                        if(e > dens) dens = e;
                    }
                } else {          /* pinheiro: cone em andares */
                    float meia = (0.95f - v) * 0.50f;
                    float andar = fmodf(v * 5.0f, 1.0f);
                    meia *= 0.75f + 0.25f * andar;
                    dens = v > 0.15f && meia > 0 ? 1.0f - fabsf(u) / (meia + 1e-3f) : 0;
                }
                /* folga nas bordas da textura: nunca encosta (sem corte reto) */
                dens -= (v > 0.93f ? (v - 0.93f) * 20.0f : 0) + (fabsf(u) > 0.45f ? (fabsf(u) - 0.45f) * 20.0f : 0);
                uint32_t h = h32((uint32_t)(tipo * 9000 + y * 64 + x) * 2654435761u);
                uint32_t hb = h32((uint32_t)(tipo * 777 + (y >> 2) * 16 + (x >> 2)) * 40503u + 5);   /* tufos 4x4 */
                float ruido = (h & 255) / 255.0f, tufo = (hb & 255) / 255.0f;
                float corte = tipo == 2 ? 0.22f : 0.10f;          /* ipê mais vazado (flores soltas) */
                if(dens > 0.0f && dens + 0.30f * (tufo - 0.5f) + 0.15f * (ruido - 0.5f) > corte) {
                    float lum;
                    if(tipo == 2) lum = 0.80f + 0.20f * ruido - 0.20f * (tufo < 0.25f);
                    else {
                        lum = 0.70f + 0.30f * v + 0.18f * (ruido - 0.5f) - 0.25f * (dens < 0.25f);
                        if(((h >> 8) & 15) == 0) lum -= 0.25f;     /* buraquinhos escuros */
                    }
                    l = (int)(15 * limitar(lum, 0.25f, 1.0f));
                    a = 15; r = g = b = l;
                }
                tex_px[y * 64 + x] = (uint16_t)(a << 12 | r << 8 | g << 4 | b);
            }
        pvr_ptr_t t = tex_subir(64, 128);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 64, 128, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_arvx[tipo], &cxt);
    }
    {   /* IPÊ pintado no PC: galhos ramificados e cachos de flores, com mipmap */
        memcpy(tex_px, tex_bin + TEXB_IPE, 128 * 128 * 2);
        mip_4444 = 1;
        pvr_ptr_t t = tex_subir_mip(128);
        mip_4444 = 0;
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 128, 128, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        cxt.txr.mipmap = PVR_MIPMAP_ENABLE;
        cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
        pvr_poly_compile(&hdr_arvx[2], &cxt);
    }
}

/* triângulo de copa com textura de folhagem (projeção pelo mundo, 3 m) */
static int folha_ativa;
#define MAX_SOMBRA_CEN 140
static float sombra_cen[MAX_SOMBRA_CEN][4][3];
static int n_sombra_cen;
/* sombra de um objeto: retângulo no chão (centro, eixos a/b, meias-medidas) */
static void sombra_chao(float cx, float cy, float cz, float ax, float az, float ha, float hb) {
    if(n_sombra_cen >= MAX_SOMBRA_CEN) return;
    float bx = -az, bz = ax;
    const float sa[4] = { -1, 1, 1, -1 }, sb[4] = { -1, -1, 1, 1 };
    for(int v = 0; v < 4; v++) {
        sombra_cen[n_sombra_cen][v][0] = cx + ax * ha * sa[v] + bx * hb * sb[v];
        sombra_cen[n_sombra_cen][v][1] = cy;
        sombra_cen[n_sombra_cen][v][2] = cz + az * ha * sa[v] + bz * hb * sb[v];
    }
    n_sombra_cen++;
}
static void folha_tri(float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2, uint32_t c) {
    const float P[3][3] = { { x0, y0, z0 }, { x1, y1, z1 }, { x2, y2, z2 } };
    vvt_t t[3];
    for(int v = 0; v < 3; v++) {
        vv_t q;
        para_vista(P[v][0], P[v][1], P[v][2], c, &q);
        t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].c = c;
        t[v].u = (P[v][0] + P[v][2]) * 0.33f; t[v].v = P[v][1] * 0.33f;
    }
    poligono_tex(t, 3);
}
#ifndef DIST_CASAS
#define DIST_CASAS 250.0f      /* além disso as casas já somem na névoa (custavam até 2,8 ms) */
#endif
#ifdef DIAG_CEN
static uint64_t cen_ac[5], cen_t0; static int cen_n; static char cen_txt[64];
static int dg_tipo[10];
#define CEN_T(i) do { uint64_t _a = timer_us_gettime64(); if(i) cen_ac[(i) - 1] += _a - cen_t0; cen_t0 = _a; } while(0)
#else
#define CEN_T(i) do { } while(0)
#endif
/* GRADE ESPACIAL (como nos jogos da época): o cenário é dividido em células
 * de 100 m; por quadro só as células perto e no campo de visão são visitadas,
 * da mais perto para a mais longe (antes: 1500 prédios x 2 passes e 1400
 * árvores testados um a um a cada quadro). */
#define GR_CEL 100.0f
#define GR_MAX 48
typedef struct { float x0, z0; int nx, nz; uint16_t ini[GR_MAX * GR_MAX + 1]; uint16_t *lista; } grade_t;
static uint16_t gr_lista_p[MAX_PREDIOS], gr_lista_a[MAX_ARVORES];
static grade_t gr_p = { .lista = gr_lista_p }, gr_a = { .lista = gr_lista_a };
static void grade_montar(grade_t *g, int n, const float *(*pos)(int)) {
    float mnx = 1e9f, mnz = 1e9f, mxx = -1e9f, mxz = -1e9f;
    for(int i = 0; i < n; i++) { const float *p = pos(i); if(p[0] < mnx) mnx = p[0]; if(p[0] > mxx) mxx = p[0]; if(p[2] < mnz) mnz = p[2]; if(p[2] > mxz) mxz = p[2]; }
    g->x0 = mnx; g->z0 = mnz;
    g->nx = (int)((mxx - mnx) / GR_CEL) + 1; g->nz = (int)((mxz - mnz) / GR_CEL) + 1;
    if(g->nx > GR_MAX) g->nx = GR_MAX;
    if(g->nz > GR_MAX) g->nz = GR_MAX;
    static uint16_t cont[GR_MAX * GR_MAX];
    memset(cont, 0, sizeof(cont));
    static uint16_t cel[MAX_PREDIOS > MAX_ARVORES ? MAX_PREDIOS : MAX_ARVORES];
    for(int i = 0; i < n; i++) {
        const float *p = pos(i);
        int cx = (int)((p[0] - g->x0) / GR_CEL), cz = (int)((p[2] - g->z0) / GR_CEL);
        if(cx >= g->nx) cx = g->nx - 1;
        if(cz >= g->nz) cz = g->nz - 1;
        cel[i] = (uint16_t)(cz * g->nx + cx); cont[cel[i]]++;
    }
    int nc = g->nx * g->nz, acc = 0;
    for(int c = 0; c < nc; c++) { g->ini[c] = (uint16_t)acc; acc += cont[c]; }
    g->ini[nc] = (uint16_t)acc;
    static uint16_t pos_c[GR_MAX * GR_MAX];
    for(int c = 0; c < nc; c++) pos_c[c] = g->ini[c];
    for(int i = 0; i < n; i++) g->lista[pos_c[cel[i]]++] = (uint16_t)i;   /* ordem original dentro da célula */
}
/* candidatos: objetos das células visíveis, ordenadas da mais perto para a mais longe */
static int grade_candidatos(const grade_t *g, float raio, uint16_t *out, int max) {
    static uint16_t cels[GR_MAX * GR_MAX]; static float dc[GR_MAX * GR_MAX];
    int nc = 0;
    int c0x = (int)((cam_p[0] - raio - g->x0) / GR_CEL), c1x = (int)((cam_p[0] + raio - g->x0) / GR_CEL);
    int c0z = (int)((cam_p[2] - raio - g->z0) / GR_CEL), c1z = (int)((cam_p[2] + raio - g->z0) / GR_CEL);
    if(c0x < 0) c0x = 0;
    if(c0z < 0) c0z = 0;
    if(c1x >= g->nx) c1x = g->nx - 1;
    if(c1z >= g->nz) c1z = g->nz - 1;
    for(int cz = c0z; cz <= c1z; cz++)
        for(int cx = c0x; cx <= c1x; cx++) {
            int c = cz * g->nx + cx;
            if(g->ini[c] == g->ini[c + 1]) continue;
            float m[3] = { g->x0 + (cx + 0.5f) * GR_CEL, cam_p[1], g->z0 + (cz + 0.5f) * GR_CEL };
            float d2 = dist2_cam(m);
            if(d2 > (raio + 75.0f) * (raio + 75.0f) || !na_campo(m, 110.0f)) continue;   /* 110 m: meia diagonal + objetos grandes */
            int k = nc++;
            while(k > 0 && dc[k - 1] > d2) { cels[k] = cels[k - 1]; dc[k] = dc[k - 1]; k--; }
            cels[k] = (uint16_t)c; dc[k] = d2;
        }
    int n = 0;
    for(int k = 0; k < nc && n < max; k++)
        for(int j = g->ini[cels[k]]; j < g->ini[cels[k] + 1] && n < max; j++) out[n++] = g->lista[j];
    return n;
}
static const float *pos_predio(int i) { return predios[i].c; }
static const float *pos_arvore(int i) { return arvores[i].p; }
static uint16_t cand_p[MAX_PREDIOS], cand_a[MAX_ARVORES];
static int n_cand_p, n_cand_a;
#define MAX_SILH 200
static uint16_t silh[MAX_SILH];
static int n_silh;
/* CASAS E PRÉDIOS: 6 tipos de fachada (sorteados pelo tamanho), telhado de
 * duas águas nas casas baixas e janelas acesas no entardecer (só perto) */
static uint8_t tipo_pred[MAX_PREDIOS], perto_pista[MAX_PREDIOS];
static void montar_tipos_predio(void) {
    for(int i = 0; i < n_predios; i++) {
        const predio_t *d = &predios[i];
        float dmin = 1e18f; int kmin = 0;
        for(int k = 0; k < NF; k += 2) {
            float dx = F[k].x - d->c[0], dz = F[k].z - d->c[2], d2 = dx * dx + dz * dz;
            if(d2 < dmin) { dmin = d2; kmin = k; }
        }
        float lim = F[kmin].box ? 75.0f : 25.0f;   /* complexo dos boxes: 75 m; resto da pista: 25 m */
        if(d->osm) lim = 130.0f;                    /* dentro do autódromo não há casas */
        perto_pista[i] = dmin < lim * lim;         /* boxes, torre, garagens: sem telhado de casa */
        /* caixa grande e lisa longe da pista (prédios do OSM sem tipo): ganha fachada */
        if(d->osm && d->tex == 0 && !perto_pista[i] && d->hy * 2 >= 6.0f && d->hx >= 4.0f && d->hz >= 4.0f && !d->dmax) predios[i].tex = 6;
        uint32_t h = h32((uint32_t)i * 2654435761u + 77);
        float alt = d->hy * 2, area = d->hx * d->hz * 4;
        int t;
        if(alt < 7.5f) t = area > 500.0f ? 4 : (h % 5 == 0 ? 3 : h % 5 == 1 ? 2 : 5);     /* baixos: casas e galpões */
        else if(alt > 30.0f) t = (h % 3 == 0) ? 1 : (h % 3 == 1 ? 0 : 2);                 /* altos: vidro, residencial */
        else t = (int[]){ 0, 2, 3, 0, 5 }[h % 5];
        if(!d->osm) t = 1;          /* construções do autódromo (andar das equipes nos boxes): vidraça */
        else if(perto_pista[i]) {   /* prédio do mapa DENTRO do autódromo: galpão / bloco de serviço */
            t = (d->hy * 2 < 8.0f) ? 4 : 1;
            predios[i].cor = (h & 1) ? 0xFFD8DADC : 0xFFC4C8CC;
            predios[i].topo = 0xFF9AA0A6;
        }
        tipo_pred[i] = (uint8_t)t;
    }
}
static uint32_t cor_tipo(uint32_t c, int t) {
#if FACH_FOTO
    if(t != 4) return cor_mistura(c, 0xFFFFFFFF, 0.70f);     /* fachada de FOTO já tem cor: só um leve tom */
#endif
    if(t == 1) return cor_mistura(c, 0xFF9AB2C6, 0.75f);      /* vidro azulado */
    if(t == 3) return cor_mistura(c, 0xFFC0705A, 0.70f);      /* tijolo */
    if(t == 4) return cor_mistura(c, 0xFFBCC0C4, 0.60f);      /* metal do galpão */
    return c;
}
#define MAX_JL 40
static uint16_t jl_i[MAX_JL];
static uint8_t jl_k[MAX_JL];
static int n_jl;
static void desenhar_janelas_tr(void) {
    if(!n_jl) return;
    pvr_prim(&hdr_jan_luz, sizeof(pvr_poly_hdr_t));
    caixa_tex = 6; caixa_lote = 1;
    for(int k = 0; k < n_jl; k++) {
        const predio_t *d = &predios[jl_i[k]];
        uint32_t g = jl_k[k];
        desenhar_caixa(d->c, d->r, d->f, d->hx + 0.05f, d->hy, d->hz + 0.05f, 0xFF000000 | g * 0x010101u, 0xFF000000);
    }
    caixa_tex = 0; caixa_lote = 0;
}
#define MAX_TIPO_LOTE 120
static uint16_t lote_i[6][MAX_TIPO_LOTE];
static int n_lote[6];
#define MAX_TELHADO 110
static uint16_t telh[MAX_TELHADO];
static int n_telh;
static void desenhar_telhado(const predio_t *d, uint32_t h) {
    int ao_longo = d->hx >= d->hz;
    const float *a = ao_longo ? d->r : d->f, *b = ao_longo ? d->f : d->r;
    float L = (ao_longo ? d->hx : d->hz) + 0.35f, W = (ao_longo ? d->hz : d->hx) + 0.45f, H = W * 0.52f;
    float y0 = d->c[1] + d->hy - 0.12f, y1 = d->c[1] + d->hy + H;
    static const uint32_t TEL[4] = { 0xFFB4603E, 0xFF9E4E32, 0xFFAA6A48, 0xFF8E908C };
    uint32_t base = TEL[h % 4];
    float sh = 1.0f / sqrtf(H * H + W * W);
    for(int lado = -1; lado <= 1; lado += 2) {            /* as duas águas */
        float n[3] = { b[0] * lado * H * sh, W * sh, b[2] * lado * H * sh };
        float nl = n[0] * LUZ[0] + n[1] * LUZ[1] + n[2] * LUZ[2];
        uint32_t c = tinta_tarde(cor_escala(base, (luz_base + luz_dir * (nl > 0 ? nl : 0)) * brilho), nl);
        uint32_t ce = cor_escala(c, 0.80f);                  /* beiral mais escuro */
        vv_t q[4];
        para_vista(d->c[0] + a[0] * L + b[0] * W * lado, y0, d->c[2] + a[2] * L + b[2] * W * lado, ce, &q[0]);
        para_vista(d->c[0] - a[0] * L + b[0] * W * lado, y0, d->c[2] - a[2] * L + b[2] * W * lado, ce, &q[1]);
        para_vista(d->c[0] - a[0] * L, y1, d->c[2] - a[2] * L, c, &q[2]);
        para_vista(d->c[0] + a[0] * L, y1, d->c[2] + a[2] * L, c, &q[3]);
        poligono(q, 4);
    }
    /* oitões (triângulos de parede nas pontas) */
    float Lw = L - 0.35f, Ww = W - 0.45f;
    for(int ponta = -1; ponta <= 1; ponta += 2) {
        float nl = ponta * (a[0] * LUZ[0] + a[2] * LUZ[2]);
        uint32_t c = cor_escala(cor_tipo(d->cor, tipo_pred[d - predios]), (luz_base + luz_dir * (nl > 0 ? nl : 0)) * brilho);
        vv_t q[3];
        para_vista(d->c[0] + a[0] * Lw * ponta + b[0] * Ww, y0 + 0.1f, d->c[2] + a[2] * Lw * ponta + b[2] * Ww, c, &q[0]);
        para_vista(d->c[0] + a[0] * Lw * ponta - b[0] * Ww, y0 + 0.1f, d->c[2] + a[2] * Lw * ponta - b[2] * Ww, c, &q[1]);
        para_vista(d->c[0] + a[0] * Lw * ponta, y1 - 0.05f, d->c[2] + a[2] * Lw * ponta, c, &q[2]);
        poligono(q, 3);
    }
}
static void desenhar_cenario(void) {
    folha_ativa = 0;
    n_arv_x = 0;
    n_sombra_cen = 0;
    /* direção e comprimento das sombras (sol baixo = sombra longa) */
    float sh = sqrtf(LUZ[0] * LUZ[0] + LUZ[2] * LUZ[2]) + 1e-4f;
    float sdx = -LUZ[0] / sh, sdz = -LUZ[2] / sh, sk = limitar(sh / (LUZ[1] + 0.05f), 0.4f, 3.0f);
    int n = 0;
    CEN_T(0);
    for(int i = 0; i < n_arqs; i++) {
        arq_t *a = &arqs[i];
        float c[3] = { a->f[0] + a->o[0] * a->prof * 0.5f, a->f[1], a->f[2] + a->o[2] * a->prof * 0.5f };
        if(dist2_cam(c) > (dist_visao + 80) * (dist_visao + 80) || !na_campo(c, a->meio + a->prof)) continue;
        desenhar_arquibancada(a);
    }
    CEN_T(1);
    for(int lote = 0; lote < 2; lote++) {     /* 0: o resto; 1: casas com janelas, sob UM cabeçalho */
        if(lote) { CEN_T(2); caixa_lote = 1; }
        if(!lote) n_cand_p = grade_candidatos(&gr_p, dist_visao + 80.0f > 460.0f ? dist_visao + 80.0f : 460.0f, cand_p, MAX_PREDIOS);
        for(int ci = 0; ci < n_cand_p && n < 230; ci++) {
            int i = cand_p[ci];
            predio_t *d = &predios[i];
            if((d->tex == 6) != lote) continue;
            float d2 = dist2_cam(d->c), lim = d->dmax ? d->dmax * 10.0f : lote ? DIST_CASAS : dist_visao + 80;   /* casas: até DIST_CASAS */
            if(d2 > lim * lim) {
                /* LOD: casa longe vira silhueta (1 polígono) até 460 m */
                if(lote && d2 < 460.0f * 460.0f && n_silh < MAX_SILH && na_campo(d->c, d->hx + d->hz + 4.0f)) silh[n_silh++] = (uint16_t)i;
                continue;
            }
            if(!lote && !d->fixo) {   /* objeto pequeno some antes (pessoa a 150 m = 2 pixels) */
                float tam = d->hx > d->hy ? d->hx : d->hy; if(d->hz > tam) tam = d->hz;
                float lt = 25.0f + tam * 42.0f;
                if(d2 > lt * lt) continue;
            }
            if(!na_campo(d->c, d->hx + d->hz + 4.0f)) continue;
            caixa_tex = d->tex;
            if(d2 < 170.0f * 170.0f && d->hy > 1.0f && d->hy < 20.0f && d->tex != 8) {   /* sombra no chão */
                float L = d->hy * 2 * sk * 0.6f;
                float ex = fabsf(sdx * d->r[0] + sdz * d->r[2]) * L * 0.5f, ez = fabsf(sdx * d->f[0] + sdz * d->f[2]) * L * 0.5f;
                if(n_sombra_cen < MAX_SOMBRA_CEN) {
                    float cx = d->c[0] + sdx * L * 0.5f, cz = d->c[2] + sdz * L * 0.5f, cy = d->c[1] - d->hy + 0.12f;
                    const float sa[4] = { -1, 1, 1, -1 }, sb[4] = { -1, -1, 1, 1 };
                    for(int v = 0; v < 4; v++) {
                        sombra_cen[n_sombra_cen][v][0] = cx + d->r[0] * (d->hx + ex) * sa[v] + d->f[0] * (d->hz + ez) * sb[v];
                        sombra_cen[n_sombra_cen][v][1] = cy;
                        sombra_cen[n_sombra_cen][v][2] = cz + d->r[2] * (d->hx + ex) * sa[v] + d->f[2] * (d->hz + ez) * sb[v];
                    }
                    n_sombra_cen++;
                }
            }
            if(lote) {
                int t = tipo_pred[i];
                if(n_lote[t] < MAX_TIPO_LOTE) lote_i[t][n_lote[t]++] = (uint16_t)i;
                if(d->osm && d->hy * 2 < 7.5f && t != 4 && !perto_pista[i] && d->hx < 9.0f && d->hz < 9.0f && d2 < 260.0f * 260.0f && n_telh < MAX_TELHADO)
                    telh[n_telh++] = (uint16_t)i;
                caixa_tex = 0;
                n++;
                continue;
            }
            desenhar_caixa(d->c, d->r, d->f, d->hx, d->hy, d->hz, d->cor, d->topo);
#ifdef DIAG_CEN
            dg_tipo[d->tex < 9 ? d->tex : 9]++;
#endif
            caixa_tex = 0;
            n++;
        }
        if(lote) {
            /* fachadas: um cabeçalho por tipo */
            caixa_tex = 6;
            for(int t = 0; t < 6; t++) {
                if(!n_lote[t]) continue;
                pvr_prim(&hdr_fach[t], sizeof(pvr_poly_hdr_t));
                caixa_vidro = t == 1;
                for(int k = 0; k < n_lote[t]; k++) {
                    const predio_t *d = &predios[lote_i[t][k]];
                    desenhar_caixa(d->c, d->r, d->f, d->hx, d->hy, d->hz, cor_tipo(d->cor, t), d->topo);
                }
            }
            caixa_vidro = 0;
            /* entardecer: janelas acesas somadas (só nos prédios perto) */
            float tt = limitar(tarde, 0, 1); tt = tt * tt * (3 - 2 * tt);
            float kl = limitar(tt + 0.4f * pesada, 0, 1);
            n_jl = 0;
            if(kl > 0.05f) {                   /* a passada é desenhada na lista translúcida */
                for(int t = 0; t < 6 && n_jl < MAX_JL; t++) {
                    if(t == 4) continue;
                    for(int k = 0; k < n_lote[t] && n_jl < MAX_JL; k++) {
                        float d2 = dist2_cam(predios[lote_i[t][k]].c);
                        if(d2 > 220.0f * 220.0f) continue;
                        jl_i[n_jl] = lote_i[t][k];
                        jl_k[n_jl++] = (uint8_t)(limitar(kl * (1.35f - sqrtf(d2) / 260.0f), 0, 1) * 255);
                    }
                }
            }
            caixa_tex = 0;
            for(int t = 0; t < 6; t++) n_lote[t] = 0;
            pvr_prim(&hdr_mundo, sizeof(hdr_mundo)); caixa_lote = 0;
            for(int k = 0; k < n_telh; k++) desenhar_telhado(&predios[telh[k]], h32((uint32_t)telh[k] * 31u));
            n_telh = 0;
        }
    }
    {   /* silhuetas das casas distantes: um quadrilátero de frente para a câmera */
        float lz = (luz_base + luz_dir * 0.55f) * brilho;
        for(int tp = 0; tp < 6; tp++) {             /* com a fachada do tipo (antes: cor lisa) */
        int ativo = 0;
        for(int k = 0; k < n_silh; k++) {
            if(tipo_pred[silh[k]] != tp) continue;
            if(!ativo) { pvr_prim(&hdr_fach[tp], sizeof(pvr_poly_hdr_t)); ativo = 1; }
            const predio_t *d = &predios[silh[k]];
            float dx = d->c[0] - cam_p[0], dz = d->c[2] - cam_p[2];
            float il = 1.0f / sqrtf(dx * dx + dz * dz + 1e-3f), w = (d->hx + d->hz) * 0.62f;
            float px = -dz * il * w, pz = dx * il * w;
            float y0 = d->c[1] - d->hy, y1 = d->c[1] + d->hy;
            uint32_t c = cor_escala(d->cor, lz), cb = cor_escala(d->cor, lz * 0.8f);
            c = cor_escala(cor_tipo(d->cor, tp), lz); cb = cor_escala(c, 0.8f);
            vv_t q[4];
            para_vista(d->c[0] - px, y0, d->c[2] - pz, cb, &q[0]);
            para_vista(d->c[0] + px, y0, d->c[2] + pz, cb, &q[1]);
            para_vista(d->c[0] + px, y1, d->c[2] + pz, c, &q[2]);
            para_vista(d->c[0] - px, y1, d->c[2] - pz, c, &q[3]);
            float uw = 2 * w / 12.0f, vh = (y1 - y0) / 12.0f;
            const float U[4] = { 0, uw, uw, 0 }, V[4] = { vh, vh, 0, 0 };
            vvt_t t[4];
            for(int v = 0; v < 4; v++) { t[v].x = q[v].x; t[v].y = q[v].y; t[v].z = q[v].z; t[v].u = U[v]; t[v].v = V[v]; t[v].c = q[v].c; t[v].o = 0; }
            poligono_tex(t, 4);
        }
        }
        if(n_silh) pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
        n_silh = 0;
    }
    n = 0;
    CEN_T(3);
    n_cand_a = grade_candidatos(&gr_a, dist_visao < 420.0f ? dist_visao : 420.0f, cand_a, MAX_ARVORES);
    for(int ci = 0; ci < n_cand_a && n < 135; ci++) {
        int i = cand_a[ci];
        arvore_t *a = &arvores[i];
        float d2 = dist2_cam(a->p);
        float lim_arv = dist_visao < 420.0f ? dist_visao : 420.0f;
        if(d2 > lim_arv * lim_arv || !na_campo(a->p, a->raio + 3.0f)) continue;
        n++;
        /* copa: pirâmide de 4 lados sobre um tronco fino. De longe (sem o
         * tronco) a copa desce até perto do chão, senão parece flutuar; e se o
         * terreno ali está na malha grossa, a árvore acompanha esse chão. */
        float dy = 0;
        if(a->dy_grosso != 0) {
            float cx = ter_x0 + ((((int)((a->p[0] - ter_x0) / TER_CEL)) & ~1) + 1) * TER_CEL;
            float cz = ter_z0 + ((((int)((a->p[2] - ter_z0) / TER_CEL)) & ~1) + 1) * TER_CEL;
            float bc[3] = { cx, 0, cz };
            if(dist2_cam(bc) >= 280.0f * 280.0f) dy = a->dy_grosso;
        }
        float desce = d2 < 150.0f * 150.0f ? 0.0f : a->alt * 0.24f;   /* sem tronco: copa até o chão */
        float topo[3] = { a->p[0], a->p[1] + a->alt + dy, a->p[2] };
        float base_y = a->p[1] + a->alt * 0.3f;
        /* a geometria da copa é fixa: calcula uma vez só (sinf/cosf/sqrtf
         * por árvore a cada quadro custavam caro no SH4) */
        if(!a->pronta) {
            for(int v = 0; v < 4; v++) {
                float ang = a->giro + v * PI_F / 2;
                a->b[v][0] = a->p[0] + cosf(ang) * a->raio; a->b[v][1] = base_y; a->b[v][2] = a->p[2] + sinf(ang) * a->raio;
            }
            for(int v = 0; v < 4; v++) {
                float *p0 = a->b[v], *p1 = a->b[(v + 1) & 3];
                float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
                float e2[3] = { topo[0] - p0[0], topo[1] - p0[1], topo[2] - p0[2] };
                float nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                float mx = (p0[0] + p1[0]) * 0.5f - a->p[0], mz = (p0[2] + p1[2]) * 0.5f - a->p[2];
                if(nn[0] * mx + nn[2] * mz < 0) { nn[0] = -nn[0]; nn[1] = -nn[1]; nn[2] = -nn[2]; }
                float l = 1.0f / sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                a->n[v][0] = nn[0] * l; a->n[v][1] = nn[1] * l; a->n[v][2] = nn[2] * l;
            }
            a->pronta = 1;
        }
        if(!folha_ativa) { pvr_prim(&hdr_folhas, sizeof(hdr_folhas)); folha_ativa = 1; }
        if(d2 < 110.0f * 110.0f) {       /* sombra da copa no chão */
            float L = a->alt * 0.7f * sk;
            sombra_chao(a->p[0] + sdx * L * 0.5f, a->p[1] + 0.35f, a->p[2] + sdz * L * 0.5f, sdx, sdz,
                        a->raio * 0.9f + L * 0.35f, a->raio * 0.95f);
        }
        if(a->alt < 3.0f && d2 > 130.0f * 130.0f) continue;      /* arbusto: só perto */
#if ARV_X
        if(n_arv_x < MAX_ARV_X) { arv_x[n_arv_x] = (uint16_t)i; arv_dy[n_arv_x++] = dy; }
        continue;
#endif
        if(d2 < 75.0f * 75.0f && a->alt >= 3.0f) {
            /* DE PERTO: copa arredondada (duas pirâmides de 6 lados unidas pela
             * "cintura") ou pinheiro em dois andares */
            int conifera = (h32((uint32_t)i * 17 + 3) % 10) < 3 && !e_ipe(a->cor);
            float cx = a->p[0], cz = a->p[2], y0 = a->p[1] + dy;
            int nl = conifera ? 4 : 6, andares = conifera ? 2 : 1;
            for(int an = 0; an < andares; an++) {
                float r = conifera ? a->raio * (an ? 0.68f : 1.05f) : a->raio * 1.15f;
                float ycint = conifera ? y0 + a->alt * (an ? 0.58f : 0.30f) : y0 + a->alt * 0.55f;
                float ytop = conifera ? y0 + a->alt * (an ? 1.0f : 0.78f) : y0 + a->alt;
                float ybai = y0 + a->alt * 0.26f;
                float ring[6][3];
                for(int v = 0; v < nl; v++) {
                    float ang = a->giro + v * 2 * PI_F / nl;
                    ring[v][0] = cx + fcos(ang) * r; ring[v][1] = ycint; ring[v][2] = cz + fsin(ang) * r;   /* seno/cosseno do SH4 (fsca) */
                }
                for(int part = 0; part < (conifera ? 1 : 2); part++) {
                    float ap[3] = { cx, part ? ybai : ytop, cz };
                    for(int v = 0; v < nl; v++) {
                        const float *p0 = ring[v], *p1 = ring[(v + 1) % nl];
                        float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
                        float e2[3] = { ap[0] - p0[0], ap[1] - p0[1], ap[2] - p0[2] };
                        float nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                        float mx = (p0[0] + p1[0]) * 0.5f - cx, mz = (p0[2] + p1[2]) * 0.5f - cz;
                        if(nn[0] * mx + nn[2] * mz < 0) { nn[0] = -nn[0]; nn[1] = -nn[1]; nn[2] = -nn[2]; }
                        if(nn[0] * (p0[0] - cam_p[0]) + nn[1] * (p0[1] - cam_p[1]) + nn[2] * (p0[2] - cam_p[2]) >= 0) continue;
                        float il = __frsqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
                        nn[0] *= il; nn[1] *= il; nn[2] *= il;
                        float luz = sombra_luz(nn) * (part ? 0.78f : 1.0f) * (0.93f + 0.07f * (float)((v * 5 + i) % 3));
                        if(a->cor == 0xFFFFE23A) luz = 0.80f + 0.25f * luz;
                        uint32_t c = cor_escala(a->cor, luz);
                        folha_tri(p0[0], p0[1], p0[2], p1[0], p1[1], p1[2], ap[0], ap[1], ap[2], c);
                    }
                }
            }
        }
        else for(int v = 0; v < 4; v++) {
            float *p0 = a->b[v], *p1 = a->b[(v + 1) & 3];
            const float *nn = a->n[v];
            float by0 = p0[1] + dy - desce, by1 = p1[1] + dy - desce;
            if(nn[0] * (p0[0] - cam_p[0]) + nn[1] * (by0 - cam_p[1]) + nn[2] * (p0[2] - cam_p[2]) >= 0) continue;
            float luz = sombra_luz(nn);
            if(a->cor == 0xFFFFE23A) luz = 0.80f + 0.25f * luz;   /* ipê: flores claras, sombra suave */
            uint32_t c = cor_escala(a->cor, luz);
            folha_tri(p0[0], by0, p0[2], p1[0], by1, p1[2], topo[0], topo[1], topo[2], c);
        }
        /* base da copa (vista por baixo quando a árvore fica no alto do barranco) */
        if(d2 < 150.0f * 150.0f && a->alt >= 3.0f) {
            if(folha_ativa) { pvr_prim(&hdr_mundo, sizeof(hdr_mundo)); folha_ativa = 0; }
            float R[3] = { 1, 0, 0 }, Fr[3] = { 0, 0, 1 };
            float c[3] = { a->p[0], a->p[1] + a->alt * 0.15f, a->p[2] };
            desenhar_caixa(c, R, Fr, 0.22f, a->alt * 0.15f + 0.3f, 0.22f, 0xFF5A4030, 0xFF5A4030);
        }
    }
    if(folha_ativa) { pvr_prim(&hdr_mundo, sizeof(hdr_mundo)); folha_ativa = 0; }
    CEN_T(4);
}

/* As 5 luzes vermelhas do pórtico: acendem uma a uma e apagam na largada. */
static int luzes_acesas;
static void desenhar_luzes(void) {
    float p[3], fr[3], dir[3];
    ponto_pista(S_PORTICO - 0.75f, 0, p, fr, dir);
    float R[3] = { dir[0], 0, dir[2] }, Fr[3] = { dir[2], 0, -dir[0] };
    if(dist2_cam(p) > 400.0f * 400.0f) return;
    for(int i = 0; i < 5; i++) {
        float c[3] = { p[0] + R[0] * (i - 2) * 1.6f, p[1] + 7.3f, p[2] + R[2] * (i - 2) * 1.6f };
        uint32_t cor = i < luzes_acesas ? 0xFFFF2A1A : 0xFF3A1010;
        desenhar_caixa(c, R, Fr, 0.45f, 0.45f, 0.15f, cor, 0xFF202020);
    }
}

/* ======================================================================== */
/* CARROS E FÍSICA 2.5D                                                      */
/* ======================================================================== */

#define N_CARROS 8
typedef struct {
    float s, x, v, psi;     /* metro na pista, lateral, velocidade (m/s), ângulo c/ a pista */
    float volante;          /* -1..1 (visual e controle) */
    float sujeira;          /* 0..1: poeira/borracha acumulada (parte de baixo escurece) */
    float alvo_x, habil;    /* IA: faixa preferida, % da velocidade-limite */
    int   marcha;
    int   volta;
    float t_volta, melhor;
    int   derrapando, fora;
    float deriva, lado_deriva;  /* 0..1 = quanto está escorregando; lado da curva */
    float acel_in;              /* acelerador pedido neste quadro (para o som) */
    float om_real;              /* rotação que o carro realmente faz (com inércia) */
    float vacuo;                /* 0..1: no vácuo de outro carro */
    float desliza;              /* m/s de lado, escorregando no barranco */
    float calor;                /* 0..1 disco de freio aquecido (brilho laranja) */
    float freio_in, cab_l;      /* freio deste quadro; inclinação lateral da cabeça suavizada */
    uint32_t cores[N_CORES];
} carro_t;

static carro_t carros[N_CARROS];
static float batida_jogador;      /* > 0: o jogador bateu neste quadro (força) */
static float baque;               /* tranco leve na câmera após um toque (0..1, decai) */

/* Pinturas: o jogador tem uma pintura original; os rivais usam as CORES das
 * equipes de 1999 (sem logos/marcas de patrocinadores). Estilo do fim dos anos 90:
 * grandes blocos de cor (bico, sidepods, motor e asas contrastando com o
 * monocoque), painéis lisos onde iriam os patrocinadores e a placa do número. */
typedef struct { uint32_t princ, sec, terc, nariz, motor, asa, placa, painel, capacete; } pintura_t;
static const pintura_t PINTURAS[N_CARROS] = {
    /* jogador: azul-petróleo com amarelo */
    { 0xFF0E5C8C, 0xFFF2C21B, 0xFF0E5C8C, 0xFFF2C21B, 0xFF0E5C8C, 0xFFF2C21B, 0xFF0E5C8C, 0xFFF0F0EC, 0xFFF2C21B },
    /* RIVAIS com as cores das equipes de 1999 (só as cores, sem logos):
     *   princ    sec        terc(sidepod) nariz     motor      asa        placa(asa) painel     capacete */
    /* McLaren MP4/14 (#1): prata e preto */
    { 0xFFB8BCC2, 0xFF1C1D21, 0xFF26282C, 0xFFB8BCC2, 0xFFB8BCC2, 0xFF1C1D21, 0xFF1C1D21, 0xFFC8C8CC, 0xFFF0F0F0 },
    /* Ferrari F399 (#3): vermelho com detalhes brancos */
    { 0xFFD0141C, 0xFFF2F2F2, 0xFFD0141C, 0xFFD0141C, 0xFFD0141C, 0xFFD0141C, 0xFFD0141C, 0xFFF2F2F2, 0xFFD82020 },
    /* Jordan 199 (#8): amarelo com preto e dourado */
    { 0xFFF5D010, 0xFF18181A, 0xFFF5D010, 0xFFF5D010, 0xFFF5D010, 0xFF18181A, 0xFFF5D010, 0xFFC8A030, 0xFFF0F0F0 },
    /* Williams FW21 (#6): vermelho e branco */
    { 0xFFD01E28, 0xFFF2F2F2, 0xFFD01E28, 0xFFD01E28, 0xFFF2F2F2, 0xFFD01E28, 0xFFF2F2F2, 0xFFF2F2F2, 0xFF2050C0 },
    /* Benetton B199 (#9): azul-claro e branco */
    { 0xFF3C8CDC, 0xFFF2F2F2, 0xFF3C8CDC, 0xFF3C8CDC, 0xFFF2F2F2, 0xFF2A62B4, 0xFF3C8CDC, 0xFFE8DC30, 0xFFF0D020 },
    /* Prost AP02 (#18): azul e branco */
    { 0xFF1E3CA8, 0xFFF2F2F2, 0xFF1E3CA8, 0xFF1E3CA8, 0xFF1E3CA8, 0xFF1E3CA8, 0xFFF2F2F2, 0xFFF2F2F2, 0xFF2060D0 },
    /* Sauber C18 (#11): azul-escuro com vermelho e prata */
    { 0xFF1A2450, 0xFFD02030, 0xFF1A2450, 0xFFD02030, 0xFF1A2450, 0xFF1A2450, 0xFF1A2450, 0xFFB8BCC4, 0xFFD02030 },
};

/* números dos carros (o jogador é o 7; no build do SF3, o 16 do Barrichello) */
static const uint8_t NUMERO_CARRO[N_CARROS] = { 7, 1, 3, 8, 6, 9, 18, 11 };   /* números reais de 1999 */
/* CAPACETES dos pilotos de 1999 (desenho simplificado, só as cores e a forma
 * geral). Os de Häkkinen, Frentzen, Fisichella e Alesi seguem descrições do
 * desenho; os de M. Schumacher, R. Schumacher, Panis e Barrichello são
 * aproximados. */
static const capacete_t CAPACETES[N_CARROS] = {
    /* jogador (original): amarelo com coroa e faixa azul-petróleo */
    { 0xFFF2C21B, 0xFF0E5C8C, { 0xFF0E5C8C, 0, 0 }, 0, 0.52f, 0.08f, CAP_RETAS, 1 },
    /* Häkkinen: branco, coroa azul, 3 faixas em tons de azul com filete preto */
    { 0xFFF2F2F2, 0xFF1E50B4, { 0xFF0A2A78, 0xFF1E5AC8, 0xFF62A8EC }, 0xFF101010, 0.58f, 0.055f, CAP_RETAS, 3 },
    /* M. Schumacher (aprox.): branco, coroa vermelha, faixa nas cores da Alemanha */
    { 0xFFF2F2F2, 0xFFD8181C, { 0xFF141414, 0xFFD8181C, 0xFFF0C820 }, 0, 0.50f, 0.04f, CAP_CURVA, 3 },
    /* Frentzen: azul-marinho, faixa preto-vermelho-amarelo que sobe até a viseira */
    { 0xFF14204A, 0xFF14204A, { 0xFF101010, 0xFFD8181C, 0xFFF0C820 }, 0, 0.62f, 0.05f, CAP_CURVA, 3 },
    /* R. Schumacher (aprox.): branco, coroa azul, faixas azul e vermelha */
    { 0xFFF2F2F2, 0xFF1E3C9C, { 0xFF1E3C9C, 0xFFD8181C, 0 }, 0, 0.56f, 0.06f, CAP_CURVA, 2 },
    /* Fisichella: branco, X amarelo e verde com losango azul, gota azul no topo */
    { 0xFFF2F2F2, 0xFF1E50B4, { 0xFFF0D020, 0xFF20A040, 0xFF1E50B4 }, 0, 0, 0.065f, CAP_X, 0 },
    /* Panis (aprox.): azul, coroa branca, faixas branca e vermelha */
    { 0xFF1C3C9C, 0xFFF2F2F2, { 0xFFF2F2F2, 0xFFD8181C, 0 }, 0, 0.55f, 0.06f, CAP_RETAS, 2 },
    /* Alesi: branco, linhas vermelha e preta nas laterais, coroa azul em gota */
    { 0xFFF2F2F2, 0xFF1E48B0, { 0xFFD8181C, 0xFF141414, 0 }, 0xFFF2F2F2, 0.55f, 0.05f, CAP_CURVA, 2 },
};
#ifdef PINTURA_STEWART
/* Barrichello (aprox.): branco, coroa azul, faixas verde e amarela */
static const capacete_t CAPACETE_BARRICHELLO =
    { 0xFFF2F2F2, 0xFF1E48B0, { 0xFF1E9A40, 0xFFF0D020, 0 }, 0, 0.55f, 0.06f, CAP_RETAS, 2 };
#endif
static const capacete_t *capacete_de(int i) {
#ifdef PINTURA_STEWART
    if(i == 0) return &CAPACETE_BARRICHELLO;
#endif
    return &CAPACETES[i];
}

#ifdef CARRO_TEX
static uint16_t dec_faixa[N_CARROS][64 * 64], dec_num[N_CARROS][128 * 64], dec_cap[N_CARROS][128 * 128];
#endif
static void criar_texturas_carro(void) {
    for(int i = 0; i < N_CARROS; i++) {
        const pintura_t *pt = &PINTURAS[i];
        int numero = NUMERO_CARRO[i];
        uint32_t cor_asa = pt->placa;                   /* cor das placas laterais da asa */
        pvr_ptr_t tf, tn;
#ifdef PINTURA_STEWART
        if(i == 0) { gerar_tartan(); numero = 16; cor_asa = 0x1A2550; }
        else
#endif
        {
            uint32_t fundo = pt->princ, faixa = pt->sec;
            uint32_t filete = (pt->painel != fundo && pt->painel != faixa) ? pt->painel : pt->terc;
            if(filete == fundo || filete == faixa) filete = 0xFFF0F0F0;
            gerar_faixa(fundo, faixa, filete);
        }
        tf = tex_subir(64, 64);
#ifdef CARRO_TEX
        memcpy(dec_faixa[i], tex_px, 64 * 64 * 2);
#endif
        gerar_numeros(numero, cor_asa);
        tn = tex_subir(128, 64);
#ifdef CARRO_TEX
        memcpy(dec_num[i], tex_px, 128 * 64 * 2);
#endif
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 64, 64,
                         tf, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        pvr_poly_compile(&hdr_faixa[i], &cxt);                    /* faixa: repete ao longo */
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 128, 64,
                         tn, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_num[i], &cxt);
        gerar_capacete(capacete_de(i));
        pvr_ptr_t tc = tex_subir(128, 128);
#ifdef CARRO_TEX
        memcpy(dec_cap[i], tex_px, 128 * 128 * 2);
#endif
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 128, 128,
                         tc, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_capacete[i], &cxt);
    }
}

/* Aceleração do motor (m/s²): limitada pela tração em baixa, pela potência em alta. */
/* ~800 cv para ~600 kg: potência/massa ~1000 W/kg -> a = 1000/v (limitada
 * pela tração em baixa). Com o arrasto abaixo, a final fica em ~315 km/h. */
static inline float motor(float v) { float a = 1000.0f / (v + 1.0f); return a > 11.0f ? 11.0f : a; }
/* arrasto: dá ~320 km/h de final */
static inline float arrasto(float v) { return 0.15f + 0.00142f * v * v; }

static void iniciar_carros(void) {
    for(int i = 0; i < N_CARROS; i++) {
        carro_t *c = &carros[i];
        memset(c, 0, sizeof(*c));
        int fila = i == 0 ? 2 : (i < 5 ? i - 1 : i);         /* jogador larga em 5º */
        int pos = i == 0 ? 4 : (i <= 4 ? i - 1 : i);
        (void)fila;
        c->s = COMPR - 12.0f - pos * 8.0f;
        c->x = (pos & 1) ? 3.0f : -3.0f;
        c->alvo_x = c->x;
        c->habil = 0.90f + 0.012f * (N_CARROS - i);
        c->volta = -1;          /* larga ATRÁS da linha: a 1ª passagem não conta */
        c->melhor = 0;
        const pintura_t *pt = &PINTURAS[i];
        c->cores[C_PRINC] = pt->princ;   c->cores[C_SEC] = pt->sec;     c->cores[C_TERC] = pt->terc;
        c->cores[C_NARIZ] = pt->nariz;   c->cores[C_MOTOR] = pt->motor; c->cores[C_ASA] = pt->asa;
        c->cores[C_PLACA] = pt->placa;   c->cores[C_PAINEL] = pt->painel;
        c->cores[C_MARCA] = 0xFFC8C8C8;
        c->cores[C_PNEU] = 0xFF2A2A2E;
        c->cores[C_SULCO] = 0xFF0C0C0E;
        c->cores[C_CAPACETE] = pt->capacete;
        c->cores[C_CARBONO] = 0xFF2A2B30;
        c->cores[C_ARO] = 0xFF8C9096;
        c->cores[C_VISOR] = 0xFF14161A;
        c->cores[C_HALO] = 0xFF24262B;
        c->cores[C_LUZ] = 0xFF401212;
        c->cores[C_ESCURO] = 0xFF101114;
        /* como no fim dos anos 90: o composto mais macio tem um sulco pintado de branco */
        c->cores[C_FAIXA_PNEU] = (i & 1) ? 0xFFE8E8E8 : 0xFF0C0C0E;
        if(S_INI >= 0) { c->s = fmodf(S_INI + 20.0f * (i ? i : -0.0f) - 30.0f + COMPR, COMPR); c->v = F[(int)(c->s / PASSO) % NF].vlim * 0.9f; }
    }
#ifdef PINTURA_STEWART
    /* SÓ POR CURIOSIDADE (build separado, fora do formula.cdi): cores inspiradas
     * no Stewart SF3 de 1999: branco, faixa TARTAN (textura) dos dois lados do
     * cockpit e o número 16 no bico e nas placas da asa. Sem marcas de patrocinadores. */
    {
        carro_t *c = &carros[0];
        const uint32_t BRANCO = 0xFFF2F3F4, TARTAN = 0xFFA8202A, MARINHO = 0xFF1A2550;
        c->cores[C_PRINC] = BRANCO;  c->cores[C_NARIZ] = BRANCO;  c->cores[C_MOTOR] = BRANCO;
        c->cores[C_TERC]  = BRANCO;  c->cores[C_PLACA] = BRANCO;
        (void)TARTAN;
        c->cores[C_SEC]   = MARINHO;                    /* detalhes: ponta do bico, flaps, airbox */
        c->cores[C_ASA]   = MARINHO;  c->cores[C_PAINEL] = MARINHO;
        c->cores[C_CAPACETE] = 0xFF2256C8;
    }
#endif
    for(int i = 0; i < N_CARROS; i++) carros[i].cores[C_CAPACETE] = capacete_de(i)->base;
#ifdef DEMO_AUTO
    carros[0].habil = 1.08f;    /* no demo, o "piloto" passa do limite para mostrar a derrapagem */
#endif
    if(S_INI >= 0) { carros[0].s = S_INI; carros[0].v = F[(int)(S_INI / PASSO) % NF].vlim * 0.9f; }
#ifdef PARADO
    if(S_INI >= 0) { carros[0].s = S_INI; carros[0].v = 0; carros[0].x = 0; }      /* teste: carro parado no ponto */
    for(int i = 1; i < N_CARROS; i++) carros[i].s = fmodf(S_INI + 300.0f + i * 15.0f, COMPR);   /* rivais longe (sem tapar a vista) */
#endif
}

/* Um passo da física. acel/freio em 0..1; omega_cmd = quanto o piloto QUER girar (rad/s). */
static void fisica(carro_t *c, float acel, float freio, float omega_cmd, float dt) {
    c->freio_in = freio;
    {   /* disco de freio: esquenta freando forte em alta, esfria devagar */
        float alvo = limitar(freio * c->v / 60.0f, 0, 1);
        c->calor += (alvo - c->calor) * limitar((alvo > c->calor ? 3.0f : 0.5f) * dt, 0, 1);
    }
    const fatia_t *f = fatia_em(c->s);
    float v = c->v;
    c->fora = fabsf(c->x) > 8.6f;                          /* passou da zebra = grama */
    int na_zebra = !c->fora && f->zebra && fabsf(c->x) > 7.2f;
    /* entra na grama aos poucos (1,5 m): sem o "tranco" de perder metade da aderência de uma vez */
    float gr = limitar((fabsf(c->x) - 8.6f) / 1.5f, 0, 1);
    float grip = aderencia(v) * (1.0f - 0.55f * gr) * (na_zebra ? 0.92f : 1.0f);
    grip *= 1.0f - 0.07f * c->vacuo;                      /* ar turbulento atrás de outro carro */

    /* --- longitudinal --- */
    float a_motor = acel * motor(v);
    float a_freio = freio * desaceleracao(v) * (c->fora ? 0.55f : 1.0f);
    float rampa = f->rampa;
    if(c->fora) rampa = (chao_preciso(c->s + 2.0f, c->x) - chao_preciso(c->s - 2.0f, c->x)) / 4.0f;   /* subida/descida da GRAMA */
    float a = a_motor - arrasto(v) * (1.0f - 0.30f * c->vacuo) - 9.8f * rampa;   /* vácuo: menos arrasto */
    a -= a_freio;
    if(c->fora) a -= (2.0f + 0.010f * v * v) * gr;        /* grama segura o carro (entra aos poucos) */
    if(c->fora && (f->brita & (c->x < 0 ? 1 : 2))) a -= 5.0f + 0.020f * v * v;   /* brita: afunda e freia */
    v += a * dt;

    /* --- "círculo de aderência" (versão arcade): o pneu divide o que tem
     * entre frear e virar. Frear forte E virar forte = sai de frente; por isso
     * se freia em linha reta, antes da curva. Acelerador cheio em curva
     * LENTA (marcha baixa, motor forte) = a traseira perde aderência. */
    float uso_freio = 0, potencia = 0;
    if(c == &carros[0]) {                 /* a IA não sabe dosar pé nem contra-esterçar */
        uso_freio = limitar(a_freio / (desaceleracao(v) + 1.0f), 0, 1);
        potencia = acel * limitar((32.0f - v) / 20.0f, 0, 1);      /* forte abaixo de ~115 km/h */
    }
    grip *= 1.0f - 0.18f * uso_freio * uso_freio;
    grip *= 1.0f - 0.12f * potencia;

    /* --- lateral: o volante pede uma rotação; a aderência limita ---
     * Até "omax" o pneu segura. Entre omax e 1,35·omax o carro ESCORREGA:
     * ainda gira (dá para contornar a curva sem frear tanto), mas o pneu
     * arrasta (perde um pouco de velocidade), o carro desliza um pouco para
     * fora e sai fumaça. Além disso, o volante não adianta mais nada. */
    float omax = grip / (v > 2.0f ? v : 2.0f);
    float lim = omax * FAIXA_DERRAPAGEM;
    /* peso: o carro não vira instantaneamente (~0,04 s) */
    c->om_real += (omega_cmd - c->om_real) * limitar(24.0f * dt, 0, 1);
    float om = limitar(c->om_real, -lim, lim);
    float exc = fabsf(om) - omax;                           /* > 0 = escorregando */
    if(exc > 0 && v > 12.0f) {
        float k = exc / (lim - omax);                       /* 0..1 */
        c->deriva += (k - c->deriva) * 8.0f * dt;
        v -= exc * v * 0.20f * dt;                          /* pneu arrastando */
        float sinal = om > 0 ? 1.0f : -1.0f;
        if(potencia > 0.3f) {
            /* SOBRE-ESTERÇO: a traseira escapa e o carro GIRA para dentro da
             * curva (é preciso contra-esterçar ou aliviar o acelerador) */
            c->psi += sinal * exc * (0.9f * potencia) * dt * 4.0f;
            c->x -= sinal * exc * v * 0.08f * dt;
        } else {
            /* SUBESTERÇO: a frente não obedece e o carro desliza para fora */
            c->x -= sinal * exc * v * 0.13f * k * dt;      /* progressivo: quanto mais além, mais desliza */
        }
    }
    else c->deriva -= c->deriva * 6.0f * dt;
    if(fabsf(omega_cmd) > lim * 1.05f && v > 12.0f) v -= (fabsf(omega_cmd) - lim) * v * 0.10f * dt;
    c->derrapando = c->deriva > 0.15f;
    c->lado_deriva = om > 0 ? 1.0f : -1.0f;
    if(v < 0) v = 0;

    /* a pista "gira embaixo" do carro: em curva à direita, sem esterçar, o
     * carro aponta cada vez mais para a ESQUERDA da pista -> sai por fora */
    c->psi += (om - f->curva * v) * dt;
    c->psi = limitar(c->psi, -0.8f, 0.8f);

    if(c->fora) {
        /* BARRANCO: a gravidade puxa o carro para o lado mais baixo e,
         * subindo o barranco, ele perde velocidade (descendo, ganha) */
        float incl = (chao_preciso(c->s, c->x + 0.5f) - chao_preciso(c->s, c->x - 0.5f));
        /* escorrega de leve (até ~1 m/s) e sobe/desce perdendo/ganhando pouco */
        c->desliza += (limitar(-incl * 9.8f * 0.15f, -1.0f, 1.0f) - c->desliza) * limitar(1.5f * dt, 0, 1);
        v -= incl * sinf(c->psi) * 9.8f * 0.5f * dt;
        if(v < 0) v = 0;
    } else c->desliza *= 1.0f - limitar(6.0f * dt, 0, 1);
    c->x += (v * sinf(c->psi) + c->desliza) * dt;
    c->s += v * cosf(c->psi) * dt;

    /* muro de pneus no fim da grama */
    if(fabsf(c->x) > 24.0f) {
        /* só é BATIDA se chegou com velocidade contra o muro; encostado,
         * o carro desliza junto dele e pode sair (antes perdia 60% da
         * velocidade A CADA QUADRO e ficava "travado") */
        float sg = c->x > 0 ? 1.0f : -1.0f;
        float contra = (v * sinf(c->psi) + c->desliza) * sg;
        c->x = sg * 24.0f;
        if(c->desliza * sg > 0) c->desliza = 0;
        if(contra > 3.0f) {
            if(c == &carros[0] && v > 8.0f) batida_jogador = limitar(contra / 15.0f, 0.3f, 1.0f);
            v *= 0.65f;
            c->psi *= -0.3f;
        } else {
            v *= 1.0f - limitar(0.6f * dt, 0, 1);              /* raspando no muro */
            if(c->psi * sg > 0) c->psi *= 1.0f - limitar(4.0f * dt, 0, 1);
        }
    }
    if(c->s >= COMPR) {
        c->s -= COMPR;
        if(c->volta >= 0 && (c->melhor <= 0 || c->t_volta < c->melhor)) c->melhor = c->t_volta;
        c->volta++;
        c->t_volta = 0;
    }
    if(c->s < 0) c->s += COMPR;
    c->v = v;

    /* marcha automática (só para o painel/som futuro) */
    float kmh = v * 3.6f;
    c->marcha = kmh < 90 ? 1 : kmh < 130 ? 2 : kmh < 165 ? 3 : kmh < 200 ? 4 : kmh < 235 ? 5 : kmh < 270 ? 6 : 7;
}

/* Rotação 0..1 para o som: sobe dentro de cada marcha e cai na troca. */
static float rotacao(const carro_t *c) {
    static const float FAIXA[8] = { 0, 90, 130, 165, 200, 235, 270, 340 };
    float kmh = c->v * 3.6f;
    int g = c->marcha < 1 ? 1 : c->marcha > 7 ? 7 : c->marcha;
    float t = (kmh - FAIXA[g - 1]) / (FAIXA[g] - FAIXA[g - 1]);
    t = limitar(t, 0, 1);
    return g == 1 ? 0.22f + 0.78f * t : 0.55f + 0.45f * t;
}

/* Piloto automático (rivais e modo demo): segue a velocidade-limite da pista. */
static void ia_pilotar(carro_t *c, float *acel, float *freio, float *omega) {
    const fatia_t *f = fatia_em(c->s);
    const fatia_t *f2 = fatia_em(c->s + 20.0f);
    float vt = (f->vlim < f2->vlim ? f->vlim : f2->vlim) * c->habil;
    if(fator_grip < 0.999f) vt *= sqrtf(fator_grip) * 0.97f;   /* v de curva ~ raiz da aderência */
    *acel = c->v < vt ? 1.0f : 0.0f;
    *freio = c->v > vt + 1.5f ? 0.8f : 0.0f;

    /* desvia de quem estiver logo à frente na mesma faixa */
    for(int i = 0; i < N_CARROS; i++) {
        carro_t *o = &carros[i];
        if(o == c) continue;
        float ds = fmodf(o->s - c->s + COMPR, COMPR);
        if(ds < 25.0f && fabsf(o->x - c->x) < 2.4f && o->v < c->v + 1.0f)
            c->alvo_x = o->x > 0 ? o->x - 3.2f : o->x + 3.2f;
    }
    c->alvo_x = limitar(c->alvo_x, -5.2f, 5.2f);
    float psi_d = limitar(0.06f * (c->alvo_x - c->x), -0.12f, 0.12f);
    *omega = f->curva * c->v + 4.0f * (psi_d - c->psi);
    c->volante = limitar(*omega / 0.6f, -1, 1);
}

/* Contato carro a carro: empurra para os lados e o de trás perde velocidade. */
static void colisoes(void) {
    for(int i = 0; i < N_CARROS; i++)
        for(int j = i + 1; j < N_CARROS; j++) {
            carro_t *a = &carros[i], *b = &carros[j];
            float ds = fmodf(b->s - a->s + COMPR * 1.5f, COMPR) - COMPR * 0.5f;
            float dx = b->x - a->x;
            if(fabsf(ds) < 5.2f && fabsf(dx) < 1.9f) {
                /* CONTATO SUAVE: em vez de "teleportar" o carro para trás (dava
                 * solavancos), os carros se afastam aos poucos e trocam um
                 * pouco de velocidade: quem bate perde um tanto, quem é tocado
                 * ganha um empurrãozinho, e os dois perdem um pouco no baque. */
                float emp = (1.9f - fabsf(dx)) * 0.12f * (dx >= 0 ? 1 : -1);
                a->x -= emp; b->x += emp;
                carro_t *tras = ds > 0 ? a : b, *frente = ds > 0 ? b : a;
                if(fabsf(ds) < 4.8f) {
                    float fecha = tras->v - frente->v;           /* velocidade de aproximação */
                    if(fecha > 0) {
                        if((i == 0 || j == 0) && fecha > 1.5f) {
                            float f = limitar(fecha / 12.0f, 0.15f, 0.8f);
                            if(f > batida_jogador) batida_jogador = f;
                            if(f > baque) baque = f;
                        }
                        tras->v   -= fecha * 0.55f + 0.15f;
                        frente->v += fecha * 0.30f;
                        tras->v *= 0.995f; frente->v *= 0.997f;
                        if(tras->v < 0) tras->v = 0;
                    }
                    /* desfaz a sobreposição devagar (máx. 6 cm por quadro) */
                    float corr = limitar((4.8f - fabsf(ds)) * 0.2f, 0, 0.06f);
                    tras->s -= corr; if(tras->s < 0) tras->s += COMPR;
                    frente->s += corr * 0.5f; if(frente->s >= COMPR) frente->s -= COMPR;
                }
            }
        }
}

/* Base 3D do carro no mundo, a partir de (s, x, psi). */
static void base_carro(const carro_t *c, float P[3], float R[3], float U[3], float Fr[3]) {
    float fp[3], dp[3];
    ponto_pista(c->s, c->x, P, fp, dp);
    float incl = 0;                              /* inclinação lateral do chão (dy/dx) */
    if(fabsf(c->x) > 8.6f) {                     /* fora do asfalto: altura e inclinação da grama */
        /* interpolada entre as fatias (antes: degraus a cada 4 m = carro "pulando") */
        P[1] = chao_preciso(c->s, c->x);
        incl = (chao_preciso(c->s, c->x + 0.6f) - chao_preciso(c->s, c->x - 0.6f)) / 1.2f;
        incl = limitar(incl, -0.45f, 0.45f);
        /* e também para a frente/trás: o carro acompanha a subida/descida da grama */
        float g = (chao_preciso(c->s + 1.5f, c->x) - chao_preciso(c->s - 1.5f, c->x)) / 3.0f;
        float h = sqrtf(fp[0] * fp[0] + fp[2] * fp[2]) + 1e-6f, m = 1.0f / sqrtf(1 + g * g);
        fp[0] = fp[0] / h * m; fp[2] = fp[2] / h * m; fp[1] = g * m;
    }
    /* escorregando, a traseira "abre": o carro aparece virado para dentro */
    float ang = c->psi + c->lado_deriva * c->deriva * 0.16f;
    float cs = cosf(ang), sn = sinf(ang);
    Fr[0] = fp[0] * cs + dp[0] * sn; Fr[1] = fp[1] * cs; Fr[2] = fp[2] * cs + dp[2] * sn;
    R[0] = -Fr[2]; R[1] = 0; R[2] = Fr[0];
    float n = 1.0f / sqrtf(R[0] * R[0] + R[2] * R[2]);
    R[0] *= n; R[2] *= n;
    if(incl != 0) {                              /* rola o carro com o barranco */
        R[1] = incl; float m = 1.0f / sqrtf(1 + incl * incl);
        R[0] *= m; R[1] *= m; R[2] *= m;
    }
    U[0] = R[1] * Fr[2] - R[2] * Fr[1]; U[1] = R[2] * Fr[0] - R[0] * Fr[2]; U[2] = R[0] * Fr[1] - R[1] * Fr[0];
}

/* os N_MEDIO rivais VISÍVEIS mais próximos da câmera ganham o modelo
 * detalhado (antes valia a ordem da lista, e um carro colado podia sair simples) */
static uint8_t escolhido[N_CARROS];
static void escolher_detalhados(void) {
    float d[N_CARROS];
    for(int i = 0; i < N_CARROS; i++) {
        float P[3], R[3], U[3], Fr[3];
        base_carro(&carros[i], P, R, U, Fr);
        d[i] = (i > 0 && na_campo(P, 4.0f)) ? dist2_cam(P) : 1e12f;
        escolhido[i] = 0;
    }
    /* 1 = completo (os mais próximos, só no ALTO); 2 = médio */
    for(int k = 0; k < N_MEDIO; k++) {
        int m = -1;
        for(int i = 1; i < N_CARROS; i++) if(!escolhido[i] && d[i] < 1e11f && (m < 0 || d[i] < d[m])) m = i;
        if(m < 0) break;
        /* completo só BEM perto (a 30 m+ a diferença não aparece na tela) */
        escolhido[m] = (detalhe_alto && k < N_COMPLETO_ALTO && d[m] < 45.0f * 45.0f) ? 1 : 2;
    }
}

static int gravar_sombra, n_sombras;
static float sombra_w[N_CARROS][4][3];
static float refl_p[N_CARROS][3]; static uint32_t refl_cor[N_CARROS]; static uint8_t refl_luz[N_CARROS];
/* asfalto molhado: cada carro "reflete" numa faixa borrada que vem em direção
 * à câmera (como reflexo em chão molhado), + o brilho vermelho da luz traseira */
static void desenhar_reflexos_tr(void) {
    if(molhado < 0.25f || !n_sombras) return;
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    for(int i = 0; i < n_sombras; i++) {
        float *P = refl_p[i];
        float dx = cam_p[0] - P[0], dz = cam_p[2] - P[2], d2 = dx * dx + dz * dz;
        if(d2 > 90.0f * 90.0f || d2 < 1.0f) continue;
        float il = __frsqrt(d2); dx *= il; dz *= il;
        float lx = -dz, lz = dx;
        float L = 3.5f;
        uint32_t a0 = (uint32_t)(molhado * 95) << 24, cr = refl_cor[i] & 0xFFFFFF;
        vv_t q[4];
        para_vista(P[0] + lx * 0.85f, P[1] + 0.03f, P[2] + lz * 0.85f, a0 | cr, &q[0]);
        para_vista(P[0] - lx * 0.85f, P[1] + 0.03f, P[2] - lz * 0.85f, a0 | cr, &q[1]);
        para_vista(P[0] - lx * 0.6f + dx * L, P[1] + 0.03f, P[2] - lz * 0.6f + dz * L, cr, &q[2]);
        para_vista(P[0] + lx * 0.6f + dx * L, P[1] + 0.03f, P[2] + lz * 0.6f + dz * L, cr, &q[3]);
        poligono(q, 4);
        if(refl_luz[i]) {
            uint32_t ar = (uint32_t)(molhado * 150) << 24;
            para_vista(P[0] + lx * 0.25f, P[1] + 0.035f, P[2] + lz * 0.25f, ar | 0xFF3020, &q[0]);
            para_vista(P[0] - lx * 0.25f, P[1] + 0.035f, P[2] - lz * 0.25f, ar | 0xFF3020, &q[1]);
            para_vista(P[0] - lx * 0.15f + dx * 5.0f, P[1] + 0.035f, P[2] - lz * 0.15f + dz * 5.0f, 0xFF3020, &q[2]);
            para_vista(P[0] + lx * 0.15f + dx * 5.0f, P[1] + 0.035f, P[2] + lz * 0.15f + dz * 5.0f, 0xFF3020, &q[3]);
            poligono(q, 4);
        }
    }
}
/* SOMBRA DOS CARROS COM VOLUME MODIFICADOR (recurso do PowerVR): para cada
 * parte do carro (monocoque, laterais, eixos, asas) um prisma que vai do carro
 * até abaixo do chão, inclinado na direção da luz. Onde o asfalto/zebra/grama
 * fica DENTRO do prisma, o chip escurece o pixel ("cheap shadow"). A sombra
 * acompanha o relevo e a zebra sozinha, sem custo de processador. */
static pvr_mod_hdr_t hdr_mv, hdr_mv_ult;
static int mv_pronto;
static float mv_P[N_CARROS][3], mv_R[N_CARROS][3], mv_F[N_CARROS][3];
static int mv_n;
static void mv_registrar(const float P[3], const float R[3], const float Fr[3]) {
    if(mv_n >= N_CARROS) return;
    memcpy(mv_P[mv_n], P, 12); memcpy(mv_R[mv_n], R, 12); memcpy(mv_F[mv_n], Fr, 12);
    mv_n++;
}
static inline void mv_tri(const vv_t *a, const vv_t *b, const vv_t *c, int ultimo) {
    if(ultimo) pvr_prim(&hdr_mv_ult, sizeof(hdr_mv_ult));
    pvr_modifier_vol_t m;
    const vv_t *v3[3] = { a, b, c };
    float *o = &m.ax;
    for(int k = 0; k < 3; k++) {
        float iz = __frsqrt(v3[k]->z * v3[k]->z);
        o[k * 3 + 0] = (CX + ESPELHAR * FOCO * v3[k]->x * iz) * XS;
        o[k * 3 + 1] = CY - FOCO * v3[k]->y * iz;
        o[k * 3 + 2] = iz;
    }
    m.flags = PVR_CMD_VERTEX_EOL;
    m.d1 = m.d2 = m.d3 = m.d4 = m.d5 = m.d6 = 0;
    pvr_prim(&m, sizeof(m));
}
/* prisma convexo: contorno (x lateral, z ao longo) na altura h acima de P, e
 * o mesmo contorno levado na direção contrária à luz até abaixo do chão */
static void mv_prisma(const float P[3], const float R[3], const float Fr[3], const float (*pts)[2], int n, float h, float ly) {
    vv_t top[12], bot[12];
    float t = (h + 1.4f) / ly;
    for(int i = 0; i < n; i++) {
        float x = P[0] + R[0] * pts[i][0] + Fr[0] * pts[i][1], z = P[2] + R[2] * pts[i][0] + Fr[2] * pts[i][1];
        float y = P[1] + h;
        para_vista(x, y, z, 0, &top[i]);
        para_vista(x - LUZ[0] * t, y - LUZ[1] * t, z - LUZ[2] * t, 0, &bot[i]);
        if(top[i].z > -PERTO || bot[i].z > -PERTO) return;
    }
    int ntri = 2 * (n - 2) + 2 * n, k = 0;
    for(int i = 1; i + 1 < n; i++) { k++; mv_tri(&top[0], &top[i], &top[i + 1], k == ntri); if(k == ntri) pvr_prim(&hdr_mv, sizeof(hdr_mv)); }
    for(int i = 1; i + 1 < n; i++) { k++; mv_tri(&bot[0], &bot[i + 1], &bot[i], k == ntri); if(k == ntri) pvr_prim(&hdr_mv, sizeof(hdr_mv)); }
    for(int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        k++; mv_tri(&top[i], &bot[i], &bot[j], k == ntri); if(k == ntri) pvr_prim(&hdr_mv, sizeof(hdr_mv));
        k++; mv_tri(&top[i], &bot[j], &top[j], k == ntri); if(k == ntri) pvr_prim(&hdr_mv, sizeof(hdr_mv));
    }
}
/* estruturas que fazem sombra dinâmica sobre a pista e os carros */
#define MAX_ESTR 4
static float estr_P[MAX_ESTR][3], estr_R[MAX_ESTR][3], estr_F[MAX_ESTR][3], estr_d[MAX_ESTR][3];
static int n_estr;
static void estrutura_sombra(float s, float larg_lat, float prof, float h) {
    if(n_estr >= MAX_ESTR) return;
    float p[3], fr[3], dir[3];
    ponto_pista(fmodf(s + COMPR, COMPR), 0, p, fr, dir);
    memcpy(estr_P[n_estr], p, 12);
    estr_R[n_estr][0] = dir[0]; estr_R[n_estr][1] = 0; estr_R[n_estr][2] = dir[2];
    estr_F[n_estr][0] = dir[2]; estr_F[n_estr][1] = 0; estr_F[n_estr][2] = -dir[0];
    estr_d[n_estr][0] = larg_lat; estr_d[n_estr][1] = prof; estr_d[n_estr][2] = h;
    n_estr++;
}
/* carro DENTRO da sombra de uma estrutura: escurece o carro inteiro */
static float sombra_estrutura_no_carro(const float P[3]) {
    float sol = limitar(luz_dir / 0.5f, 0, 1) * (1.0f - 0.9f * nuvens) * (1.0f - chuva);
    if(sol < 0.08f || LUZ[1] < 0.15f) return 1.0f;
    for(int k = 0; k < n_estr; k++) {
        float t = (estr_d[k][2] - 0.5f) / LUZ[1];             /* onde a sombra da estrutura cai na altura do carro */
        float cx = estr_P[k][0] - LUZ[0] * t, cz = estr_P[k][2] - LUZ[2] * t;
        float dx = P[0] - cx, dz = P[2] - cz;
        float a = dx * estr_R[k][0] + dz * estr_R[k][2], b = dx * estr_F[k][0] + dz * estr_F[k][2];
        if(fabsf(a) < estr_d[k][0] && fabsf(b) < estr_d[k][1] + 0.4f) return 1.0f - 0.38f * limitar(sol, 0, 1);
    }
    return 1.0f;
}
static void desenhar_sombras_mv(void) {
    float sol = limitar(luz_dir / 0.5f, 0, 1.3f) * (1.0f - 0.9f * nuvens) * (1.0f - chuva);
    if(!mv_pronto) {
        pvr_mod_compile(&hdr_mv, PVR_LIST_OP_MOD, PVR_MODIFIER_OTHER_POLY, PVR_CULLING_NONE);
        pvr_mod_compile(&hdr_mv_ult, PVR_LIST_OP_MOD, PVR_MODIFIER_INCLUDE_LAST_POLY, PVR_CULLING_NONE);
        mv_pronto = 1;
    }
    if(sol < 0.08f || LUZ[1] < 0.15f || (!mv_n && !n_estr)) { mv_n = 0; return; }
    pvr_set_shadow_scale(1, limitar(1.0f - 0.42f * sol, 0.5f, 1.0f));
    float ly = LUZ[1] > 0.2f ? LUZ[1] : 0.2f;
    pvr_prim(&hdr_mv, sizeof(hdr_mv));
    /* CARRO: contorno de cima (convexo, sem degraus) + as 4 rodas separadas */
    static const float CONT[10][2] = { { -0.86f, 2.95f }, { 0.86f, 2.95f }, { 0.86f, 2.55f }, { 0.76f, 0.55f }, { 0.76f, -1.25f },
                                       { 0.52f, -2.50f }, { -0.52f, -2.50f }, { -0.76f, -1.25f }, { -0.76f, 0.55f }, { -0.86f, 2.55f } };
    static const float RODA[4][3] = { { -0.82f, 1.62f, 0.66f }, { 0.82f, 1.62f, 0.66f }, { -0.80f, -1.55f, 0.70f }, { 0.80f, -1.55f, 0.70f } };
    for(int c = 0; c < mv_n; c++) {
        const float *P = mv_P[c], *R = mv_R[c], *Fr = mv_F[c];
        if(dist2_cam(P) > 70.0f * 70.0f) continue;
        if(dist2_cam(P) < 3.0f * 3.0f) continue;        /* câmera DENTRO do carro (cockpit): o volume encosta na câmera
                                                            e deixa um risco pontilhado na tela; a sombra nem aparece */
        float pts[10][2];
        for(int i = 0; i < 10; i++) { pts[i][0] = CONT[i][0]; pts[i][1] = CONT[i][1]; }
        mv_prisma(P, R, Fr, pts, 10, 0.62f, ly);
        for(int w = 0; w < 4; w++) {
            float q[4][2] = { { RODA[w][0] - 0.19f, RODA[w][1] - 0.30f }, { RODA[w][0] + 0.19f, RODA[w][1] - 0.30f },
                              { RODA[w][0] + 0.19f, RODA[w][1] + 0.30f }, { RODA[w][0] - 0.19f, RODA[w][1] + 0.30f } };
            mv_prisma(P, R, Fr, q, 4, RODA[w][2], ly);
        }
    }
    /* ESTRUTURAS sobre a pista (passarela, pórtico): a sombra anda com o sol */
    for(int k = 0; k < n_estr; k++) {
        if(dist2_cam(estr_P[k]) > 220.0f * 220.0f) continue;
        float q[4][2] = { { -estr_d[k][0], -estr_d[k][1] }, { estr_d[k][0], -estr_d[k][1] }, { estr_d[k][0], estr_d[k][1] }, { -estr_d[k][0], estr_d[k][1] } };
        mv_prisma(estr_P[k], estr_R[k], estr_F[k], q, 4, estr_d[k][2], ly);
    }
    mv_n = 0;
}
static void desenhar_sombras_tr(void) {
    /* sombras do cenário (árvores e construções) */
    float sol = limitar(luz_dir / 0.5f, 0, 1.3f) * (1.0f - 0.85f * nuvens) * (1.0f - chuva);
#ifdef SEM_SOMBRA_CEN
    n_sombra_cen = 0;
#endif
    if(n_sombra_cen && sol > 0.05f) {
        pvr_prim(&hdr_sombra, sizeof(hdr_sombra));
        uint32_t a = (uint32_t)(limitar(140 * sol, 0, 170)) << 24;
        static const float U[4] = { 0, 1, 1, 0 }, V[4] = { 0, 0, 1, 1 };
        for(int i = 0; i < n_sombra_cen; i++) {
            vvt_t t[4];
            for(int v = 0; v < 4; v++) {
                vv_t q;
                para_vista(sombra_cen[i][v][0], sombra_cen[i][v][1], sombra_cen[i][v][2], 0, &q);
                t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = V[v]; t[v].c = a | 0x1C2026;
            }
            poligono_tex(t, 4);
        }
    }
    if(!n_sombras) return;
    pvr_prim(&hdr_sombra_carro, sizeof(hdr_sombra_carro));     /* formato do carro (rodas, asas, laterais) */
    float sol_mv = limitar(luz_dir / 0.5f, 0, 1) * (1.0f - 0.9f * nuvens) * (1.0f - chuva);
    uint32_t a = (uint32_t)(210 * (0.55f + 0.45f * (1.0f - nuvens * 0.6f)) * (1.0f - 0.45f * sol_mv)) << 24;
    static const float U[4] = { 0, 1, 1, 0 }, V[4] = { 0, 0, 1, 1 };
    for(int i = 0; i < n_sombras; i++) {
        vvt_t t[4];
        for(int v = 0; v < 4; v++) {
            vv_t q;
            para_vista(sombra_w[i][v][0], sombra_w[i][v][1], sombra_w[i][v][2], 0, &q);
            t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = V[v]; t[v].c = a | 0x202428; t[v].o = 0;
        }
        poligono_tex(t, 4);
    }
}
/* ======================================================================== */
/* CARRO TEXTURIZADO (protótipo): modelo SIMPLES + textura "assada" do       */
/* modelo DETALHADO. No boot, o chip de vídeo fotografa o carro detalhado    */
/* (sem rodas, sem luz direcional, com a oclusão) de cima, dos lados, de    */
/* frente e de trás numa textura 256x256 por carro; na corrida, cada face do */
/* modelo simples usa a vista da direção da sua normal.                     */
/* ======================================================================== */
static float neb_ini_ant;           /* (definida mais abaixo) */
#define CTEX_S 46.0f                   /* pixels por metro na textura (layout justo ao carro) */
static pvr_ptr_t ctex_tex[N_CARROS];
static pvr_poly_hdr_t ctex_hdr[N_CARROS];
static int ctex_ok[N_CARROS];
static inline int cor_de_roda(int c) {
    return c == C_PNEU || c == C_ARO || c == C_BORRAO_PNEU || c == C_BORRAO_ARO || c == C_DISCO ||
           c == C_SULCO || c == C_FAIXA_PNEU || c == C_MARCA;
}
/* (u,v) em pixels da vista escolhida para um ponto do carro */
static inline void ctex_uv(int vista, const float *p, float *u, float *v) {
    switch(vista) {
        case 0: *u = (p[2] + 2.55f) * CTEX_S; *v = (p[0] + 1.0f) * CTEX_S; break;                 /* cima: linhas 0..92 */
        case 1: *u = (p[2] + 2.55f) * CTEX_S; *v = 94.0f + (1.06f - p[1]) * CTEX_S; break;        /* lado esq.: 94..143 */
        case 2: *u = (p[2] + 2.55f) * CTEX_S; *v = 146.0f + (1.06f - p[1]) * CTEX_S; break;       /* lado dir.: 146..195 */
        case 3: *u = (p[0] + 1.0f) * CTEX_S; *v = 198.0f + (1.06f - p[1]) * CTEX_S; break;        /* frente */
        default: *u = 100.0f + (p[0] + 1.0f) * CTEX_S; *v = 198.0f + (1.06f - p[1]) * CTEX_S; break;  /* trás */
    }
}
/* "ASSAR" o carro por SOFTWARE (no boot, no processador): as 5 vistas do
 * modelo detalhado são rasterizadas com z-buffer numa imagem 256x256 na RAM
 * (cor da pintura x oclusão x um leve sombreado de forma; decalques de faixa,
 * número e capacete amostrados das cópias na RAM). Funciona igual no
 * emulador e no console, sem depender do render-to-texture. */
#ifdef CARRO_TEX
#define CT_N 512                       /* rasteriza em 512 e reduz para 256 (bordas e peças finas limpas) */
static uint16_t ct_cor[CT_N * CT_N];
static float ct_z[CT_N * CT_N];
static uint8_t ct_ok[CT_N * CT_N];
static const uint16_t *ct_dec; static int ct_dw, ct_dh, ct_wrap;
static inline uint16_t ct_amostra(float u, float v) {
    if(ct_wrap) { u -= floorf(u); v -= floorf(v); }
    else { u = limitar(u, 0, 0.999f); v = limitar(v, 0, 0.999f); }
    return ct_dec[(int)(v * ct_dh) * ct_dw + (int)(u * ct_dw)];
}
static inline uint16_t ct_565(float r, float g, float b) {
    int R = (int)limitar(r, 0, 255), G = (int)limitar(g, 0, 255), B = (int)limitar(b, 0, 255);
    return (uint16_t)((R >> 3) << 11 | (G >> 2) << 5 | (B >> 3));
}
/* triângulo: posições (x,y em pixels, z = profundidade: maior = mais perto) e 3 atributos por vértice */
static void ct_tri(const float *A, const float *B, const float *C, int decal) {
    float x0 = A[0], y0 = A[1], x1 = B[0], y1 = B[1], x2 = C[0], y2 = C[1];
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if(fabsf(area) < 1e-6f) return;
    int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))), bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int by0 = (int)floorf(fminf(y0, fminf(y1, y2))), by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if(bx0 < 0) bx0 = 0;
    if(by0 < 0) by0 = 0;
    if(bx1 > CT_N - 1) bx1 = CT_N - 1;
    if(by1 > CT_N - 1) by1 = CT_N - 1;
    float ia = 1.0f / area;
    for(int y = by0; y <= by1; y++)
        for(int x = bx0; x <= bx1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * ia;
            float w1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * ia;
            float w2 = 1.0f - w0 - w1;
            if(w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
            float z = A[2] * w0 + B[2] * w1 + C[2] * w2;
            int i = y * CT_N + x;
            if(ct_ok[i] && z <= ct_z[i]) continue;
            float a0 = A[3] * w0 + B[3] * w1 + C[3] * w2, a1 = A[4] * w0 + B[4] * w1 + C[4] * w2, a2 = A[5] * w0 + B[5] * w1 + C[5] * w2;
            uint16_t c;
            if(decal) {
                uint16_t t = ct_amostra(a0, a1);
                c = ct_565(((t >> 11) << 3) * a2, (((t >> 5) & 63) << 2) * a2, ((t & 31) << 3) * a2);
            } else c = ct_565(a0, a1, a2);
            ct_cor[i] = c; ct_z[i] = z; ct_ok[i] = 1;
        }
}
#endif
static uint16_t hib_lista[MAX_FACES_CARRO];
static int n_hib;
/* peça do modelo detalhado (nível médio) que fica em GEOMETRIA no híbrido:
 * rodas, asas e placas, braços de suspensão */
static int hib_peca(const face_t *fc) {
    if(fc->roda >= 1 && fc->roda <= 4) return 1;
    if(fc->cor == C_ASA || fc->cor == C_PLACA) return 1;
    float cx = 0, cy = 0, cz = 0;
    for(int v = 0; v < fc->n; v++) { cx += fc->v[v][0]; cy += fc->v[v][1]; cz += fc->v[v][2]; }
    cx /= fc->n; cy /= fc->n; cz /= fc->n;
    if(fc->cor == C_CARBONO && fabsf(cx) > 0.30f && cy > 0.25f && cy < 0.60f && (fabsf(cz - 1.55f) < 0.35f || fabsf(cz + 1.55f) < 0.35f)) return 1;
    if(fc->cor == C_SEC && cy > 0.70f && cz < -1.8f) return 1;     /* elementos da asa traseira */
    return 0;
}
/* peça do modelo SIMPLES que o híbrido não desenha (vem da geometria acima) */
static int hib_pula_simples(const face_t *fc) {
    if(cor_de_roda(fc->cor) || fc->cor == C_ASA || fc->cor == C_PLACA) return 1;
    float cy = 0, cz = 0, cx = 0;
    for(int v = 0; v < fc->n; v++) { cx += fc->v[v][0]; cy += fc->v[v][1]; cz += fc->v[v][2]; }
    cx /= fc->n; cy /= fc->n; cz /= fc->n;
    if(fc->cor == C_SEC && cy > 0.70f && cz < -1.8f) return 1;
    if(fc->cor == C_CARBONO && fabsf(cx) > 0.30f && cy > 0.30f && cy < 0.60f) return 1;   /* braços de suspensão */
    return 0;
}
static void montar_hibrido(void) {
    static uint8_t marca[MAX_VU];
    memset(marca, 0, sizeof(marca));
    n_hib = 0; n_lista_v[5] = 0;
    for(int i = 0; i < n_lista[LST_MEDIO]; i++) {
        int f = lista_f[LST_MEDIO][i];
        const face_t *fc = &carro_faces[f];
        if(!hib_peca(fc)) continue;
        hib_lista[n_hib++] = (uint16_t)f;
        if(fc->roda) continue;
        for(int v = 0; v < fc->n; v++) {
            int id = face_vid[f][v];
            if(!marca[id]) { marca[id] = 1; lista_v[5][n_lista_v[5]++] = (uint16_t)id; }
        }
    }
}
static void assar_carros_tex(void) {
#ifdef CARRO_TEX
    montar_hibrido();
    static const float CP[5][3] = { { 0, 1, 0 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
#ifdef JOGADOR_HIB
    for(int ci = 0; ci < N_CARROS; ci++) {
#else
    for(int ci = 1; ci < N_CARROS; ci++) {
#endif
        if(pvr_mem_available() < 700 * 1024) break;          /* sem memória: o resto fica no modelo antigo */
        memset(ct_ok, 0, sizeof(ct_ok));
        const uint32_t *cores = carros[ci].cores;
        for(int vw = 0; vw < 5; vw++) {
            for(int li = 0; li < n_lista[LST_RIVAL]; li++) {
                int f = lista_f[LST_RIVAL][li];
                const face_t *fc = &carro_faces[f];
                if(cor_de_roda(fc->cor) || (fc->roda >= 1 && fc->roda <= 4) || fc->tex == 4 || fc->tex == 6) continue;
                const float *n = fc->nrm;
                if(n[0] * CP[vw][0] + n[1] * CP[vw][1] + n[2] * CP[vw][2] <= 0.02f) continue;   /* de costas para esta vista */
                int decal = fc->tex == 1 || fc->tex == 2 || fc->tex == 3;
                if(decal) {
                    ct_dec = fc->tex == 1 ? dec_faixa[ci] : fc->tex == 2 ? dec_num[ci] : dec_cap[ci];
                    ct_dw = fc->tex == 1 ? 64 : 128; ct_dh = fc->tex == 3 ? 128 : 64; ct_wrap = fc->tex == 1;
                }
                uint32_t base = cores[fc->cor];
                float V[8][6];
                int nv = fc->n > 8 ? 8 : fc->n;
                for(int v = 0; v < nv; v++) {
                    const float *pv = fc->v[v];
                    float u, vv;
                    ctex_uv(vw, pv, &u, &vv);
                    V[v][0] = u * (CT_N / 256.0f); V[v][1] = vv * (CT_N / 256.0f);
                    V[v][2] = pv[0] * CP[vw][0] + pv[1] * CP[vw][1] + pv[2] * CP[vw][2];
                    const float *nv3 = (fc->suave && v < 4) ? fc->vn[v] : n;
                    float sh = (0.80f + 0.20f * nv3[1]) * ao_v[face_vid[f][v]];      /* forma + oclusão */
                    if(decal && v < 4) { V[v][3] = fc->uv[v][0]; V[v][4] = fc->uv[v][1]; V[v][5] = sh * 1.05f; }
                    else { V[v][3] = ((base >> 16) & 255) * sh; V[v][4] = ((base >> 8) & 255) * sh; V[v][5] = (base & 255) * sh; }
                }
                for(int t = 1; t + 1 < nv; t++) ct_tri(V[0], V[t], V[t + 1], decal && nv == 4);
            }
        }
        /* borda: espalha a cor para os pixels vazios em volta (sem "halo" escuro no filtro e no mipmap) */
        for(int it = 0; it < 6; it++) {
            for(int y = 0; y < CT_N; y++)
                for(int x = 0; x < CT_N; x++) {
                    int i = y * CT_N + x;
                    if(ct_ok[i]) continue;
                    int j = -1;
                    if(x > 0 && ct_ok[i - 1] == 1) j = i - 1;
                    else if(x < CT_N - 1 && ct_ok[i + 1] == 1) j = i + 1;
                    else if(y > 0 && ct_ok[i - CT_N] == 1) j = i - CT_N;
                    else if(y < CT_N - 1 && ct_ok[i + CT_N] == 1) j = i + CT_N;
                    if(j >= 0) { ct_cor[i] = ct_cor[j]; ct_ok[i] = 2; }
                }
            for(int i = 0; i < CT_N * CT_N; i++) if(ct_ok[i] == 2) ct_ok[i] = 1;
        }
        for(int y = 0; y < 256; y++)                     /* média 2x2 -> 256x256 */
            for(int x = 0; x < 256; x++) {
                int r = 0, g = 0, b = 0, n = 0;
                for(int k = 0; k < 4; k++) {
                    int i = (2 * y + (k >> 1)) * CT_N + 2 * x + (k & 1);
                    if(!ct_ok[i]) continue;
                    uint16_t c = ct_cor[i];
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31; n++;
                }
                tex_px[y * 256 + x] = n ? (uint16_t)(((r + n / 2) / n) << 11 | ((g + n / 2) / n) << 5 | ((b + n / 2) / n)) : 0x18E3;
            }
        ctex_tex[ci] = tex_subir_mip(256);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256, ctex_tex[ci], PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
        cxt.gen.specular = PVR_SPECULAR_ENABLE;              /* reflexo/brilho somado por vértice */
        pvr_poly_compile(&ctex_hdr[ci], &cxt);
        ctex_ok[ci] = 1;
    }
#endif
}
static void desenhar_carro_tex(const carro_t *c, const float P[3], const float R[3], const float U[3], const float Fr[3]) {
    int ci = (int)(c - carros);
    uint32_t cores[N_CORES];
    memcpy(cores, c->cores, sizeof(cores));
    if(molhado > 0.15f && ((int)(t_clima * 4.0f + ci * 0.37f) & 1)) cores[C_LUZ] = 0xFFFF3020;
    {
        float d = limitar(((c->volta < 0 ? 0 : c->volta) * COMPR + c->s) / (6.0f * COMPR), 0, 1);
        cores[C_PNEU] = cor_mistura(cores[C_PNEU], 0xFF1C1B1A, 0.55f * d);
    }
    int ativo = -1;                                        /* 0 = cor lisa, 1 = textura */
    int glint_feito = 0;
    for(int f = 0; f < nf_simples; f++) {
        const face_t *fc = &carro_faces[f];
        const float *ln = fc->nrm;
        float nw[3] = { R[0] * ln[0] + U[0] * ln[1] + Fr[0] * ln[2], R[1] * ln[0] + U[1] * ln[1] + Fr[1] * ln[2],
                        R[2] * ln[0] + U[2] * ln[1] + Fr[2] * ln[2] };
        const float *v0 = fc->v[0];
        float w0[3] = { P[0] + R[0] * v0[0] + U[0] * v0[1] + Fr[0] * v0[2], P[1] + R[1] * v0[0] + U[1] * v0[1] + Fr[1] * v0[2],
                        P[2] + R[2] * v0[0] + U[2] * v0[1] + Fr[2] * v0[2] };
        if(nw[0] * (w0[0] - cam_p[0]) + nw[1] * (w0[1] - cam_p[1]) + nw[2] * (w0[2] - cam_p[2]) >= 0) continue;
        float d = nw[0] * LUZ[0] + nw[1] * LUZ[1] + nw[2] * LUZ[2];
        float luz = (luz_base + luz_dir * (d > 0 ? d : 0)) * brilho;
        float ax = fabsf(ln[0]), ay = fabsf(ln[1]), az = fabsf(ln[2]);
        int vista = (ay >= ax && ay >= az) ? (ln[1] > 0 ? 0 : -1) : (ax >= az ? (ln[0] < 0 ? 1 : 2) : (ln[2] > 0 ? 3 : 4));
#ifndef SEM_HIBRIDO
        if(hib_pula_simples(fc)) continue;
#endif
        int lisa = cor_de_roda(fc->cor) || vista < 0 || fc->tex || fc->cor == C_VISOR;
        if(fc->cor == C_VISOR) cores[C_VISOR] = cor_escala(cor_mistura(c->cores[C_VISOR], cor_mistura(cor_ceu, 0xFFFFFFFF, 0.25f), 0.55f), 1.0f);
        if(lisa) {
            if(ativo != 0) { pvr_prim(&hdr_mundo, sizeof(hdr_mundo)); ativo = 0; }
            uint32_t cl = cor_escala(vista < 0 ? 0xFF202226 : cores[fc->cor], luz);
            vv_t q[8];
            for(int v = 0; v < fc->n; v++) {
                const float *pv = fc->v[v];
                para_vista(P[0] + R[0] * pv[0] + U[0] * pv[1] + Fr[0] * pv[2], P[1] + R[1] * pv[0] + U[1] * pv[1] + Fr[1] * pv[2],
                           P[2] + R[2] * pv[0] + U[2] * pv[1] + Fr[2] * pv[2], cl, &q[v]);
            }
            poligono(q, fc->n);
            continue;
        }
        if(ativo != 1) { pvr_prim(&ctex_hdr[ci], sizeof(pvr_poly_hdr_t)); ativo = 1; }
        uint32_t cl = cor_escala(0xFFFFFFFF, luz * 1.08f);
        vvt_t t[8];
        for(int v = 0; v < fc->n; v++) {
            const float *pv = fc->v[v];
            vv_t q;
            para_vista(P[0] + R[0] * pv[0] + U[0] * pv[1] + Fr[0] * pv[2], P[1] + R[1] * pv[0] + U[1] * pv[1] + Fr[1] * pv[2],
                       P[2] + R[2] * pv[0] + U[2] * pv[1] + Fr[2] * pv[2], 0, &q);
            float u, vv;
            ctex_uv(vista, pv, &u, &vv);
            t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = u * (1.0f / 256.0f); t[v].v = vv * (1.0f / 256.0f);
            t[v].c = cl; t[v].o = 0;
#ifndef SEM_REFLEXO_RIVAL
            {   /* REFLEXO barato: céu refletido (mais forte de lado, Fresnel) + brilho do sol */
                float vx = cam_p[0] - (P[0] + R[0] * pv[0] + U[0] * pv[1] + Fr[0] * pv[2]);
                float vy = cam_p[1] - (P[1] + R[1] * pv[0] + U[1] * pv[1] + Fr[1] * pv[2]);
                float vz = cam_p[2] - (P[2] + R[2] * pv[0] + U[2] * pv[1] + Fr[2] * pv[2]);
                float il = __frsqrt(vx * vx + vy * vy + vz * vz + 1e-4f);
                vx *= il; vy *= il; vz *= il;
                float nv = nw[0] * vx + nw[1] * vy + nw[2] * vz;
                float fr = 1.0f - (nv > 0 ? nv : 0); fr *= fr;
                float hx = LUZ[0] + vx, hy = LUZ[1] + vy, hz = LUZ[2] + vz;
                float ih = __frsqrt(hx * hx + hy * hy + hz * hz + 1e-4f);
                float sp = (nw[0] * hx + nw[1] * hy + nw[2] * hz) * ih;
                sp = sp > 0 ? sp : 0; sp *= sp; sp *= sp; sp *= sp; sp *= sp;       /* ^16 */
                float sol = luz_dir * 2.0f * (1.0f - 0.8f * nuvens) * (1.0f - 0.5f * chuva);
                uint32_t ceu = cor_mistura(cor_ceu, cor_neb, 0.4f);
                float kc = 0.28f * fr * brilho, ks = 0.75f * sp * sol;
                int r = (int)(((ceu >> 16) & 255) * kc + 255 * ks), g = (int)(((ceu >> 8) & 255) * kc + 245 * ks), b = (int)((ceu & 255) * kc + 225 * ks);
                t[v].o = (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
                if(ks > 0.45f && !glint_feito) {                 /* "bloom": estrela do sol na pintura */
                    float W[3] = { P[0] + R[0] * pv[0] + U[0] * pv[1] + Fr[0] * pv[2], P[1] + R[1] * pv[0] + U[1] * pv[1] + Fr[1] * pv[2],
                                   P[2] + R[2] * pv[0] + U[2] * pv[1] + Fr[2] * pv[2] };
                    if(dist2_cam(W) < 80.0f * 80.0f) { brilho_add(W, 0.35f + 0.4f * ks, ((uint32_t)limitar(ks * 200, 0, 220) << 24) | 0xFFF6E0); glint_feito = 1; }
                }
            }
#endif
        }
        poligono_tex(t, fc->n);
    }
    if(ativo == 1) pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
}
/* peças em geometria do híbrido (rodas que giram, asas, placas, suspensão) */
static void desenhar_hibrido(const float P[3], const float R[3], const float U[3], const float Fr[3], const uint32_t *cores) {
#ifdef SEM_HIBRIDO
    return;
#endif
    if(!n_hib) return;
    int sv = lista_atual;
    lista_atual = 5;
    desenhar_faces(hib_lista, n_hib, P, R, U, Fr, cores, 0);
    lista_atual = sv;
}
/* BRILHOS ("bloom" da época): halos somados em pontos muito claros —
 * reflexo do sol na pintura dos rivais, luz traseira na chuva, luzes do
 * safety car. Coletados durante o desenho e desenhados na lista TR. */
#define MAX_BRILHO 48
static float br_p[MAX_BRILHO][3], br_r[MAX_BRILHO];
static uint32_t br_c[MAX_BRILHO];
static int n_brilho;
static inline void brilho_add(const float p[3], float r, uint32_t c) {
    if(n_brilho >= MAX_BRILHO) return;
    memcpy(br_p[n_brilho], p, 12); br_r[n_brilho] = r; br_c[n_brilho++] = c;
}
static void desenhar_brilhos_tr(void) {
    if(!n_brilho) return;
    pvr_prim(&hdr_brilho_luz, sizeof(hdr_brilho_luz));
    for(int i = 0; i < n_brilho; i++) {
        vv_t m;
        para_vista(br_p[i][0], br_p[i][1], br_p[i][2], 0, &m);
        if(m.z > -PERTO - 0.5f) continue;
        float r = br_r[i];
        uint32_t c = br_c[i];
        vvt_t q[4] = { { m.x - r, m.y - r, m.z, 0, 1, c, 0 }, { m.x + r, m.y - r, m.z, 1, 1, c, 0 },
                       { m.x + r, m.y + r, m.z, 1, 0, c, 0 }, { m.x - r, m.y + r, m.z, 0, 0, c, 0 } };
        poligono_tex(q, 4);
    }
    n_brilho = 0;
}
/* CALOR DO ESCAPAMENTO: ar quente tremulando atrás dos carros em velocidade */
static void desenhar_calor_tr(void) {
    int primeiro = 1;
    for(int i = 0; i < N_CARROS; i++) {
        const carro_t *c = &carros[i];
        if(c->v < 30.0f) continue;
        float P[3], R[3], U[3], Fr[3];
        base_carro(c, P, R, U, Fr);
        if(dist2_cam(P) > 45.0f * 45.0f) continue;
        if(primeiro) { pvr_prim(&hdr_nuvem, sizeof(hdr_nuvem)); primeiro = 0; }
        float k = limitar((c->v - 30.0f) / 40.0f, 0, 1) * (1.0f - 0.7f * chuva);
        for(int l = 0; l < 2; l++) {
            float tr = t_clima * 9.0f + i * 1.7f + l * 2.1f;
            float ox = fsin(tr) * 0.06f, oy = fcos(tr * 1.3f) * 0.05f;
            float C[3];
            for(int e = 0; e < 3; e++) C[e] = P[e] - Fr[e] * (2.75f + l * 0.55f) + U[e] * (0.52f + oy) + R[e] * ox;
            vv_t m;
            para_vista(C[0], C[1], C[2], 0, &m);
            if(m.z > -PERTO - 1.0f) continue;
            float r = 0.38f + l * 0.12f;
            uint32_t a = ((uint32_t)(18 * k) << 24) | 0xE8E8E8;
            vvt_t q[4] = { { m.x - r, m.y - r * 0.6f, m.z, 0, 1, a, 0 }, { m.x + r, m.y - r * 0.6f, m.z, 1, 1, a, 0 },
                           { m.x + r, m.y + r * 0.6f, m.z, 1, 0, a, 0 }, { m.x - r, m.y + r * 0.6f, m.z, 0, 0, a, 0 } };
            poligono_tex(q, 4);
        }
    }
}
/* ======================================================================== */
/* CARRO .OBJ (modelo externo, 433 triângulos, 11 pinturas fictícias)       */
/* ======================================================================== */
/* conv_obj.py converte modelo_obj/Car.obj para carro_obj.h (já na escala e
 * nos eixos do jogo); as pinturas vão em VQ com mipmap no texturas.bin.
 * Rivais usam este modelo; o jogador escolhe na tela de título (original ou
 * uma das 11 equipes). Rodas giram (e as da frente esterçam); luz por vértice
 * no espaço do carro; faces de costas descartadas antes de transformar. */
#if !defined(SEM_CARRO_OBJ) && !__has_include("carro_obj.h")
#define SEM_CARRO_OBJ                     /* o modelo .obj (de terceiros) não está na pasta: carro original */
#endif
#ifndef SEM_CARRO_OBJ
#include "carro_obj.h"
static pvr_poly_hdr_t hdr_obj[OBJ_NPINT];
static int obj_ok;
static int obj_pint[N_CARROS];               /* -1 = modelo original do jogo */
#ifndef JOGADOR_OBJ
#define JOGADOR_OBJ 0
#endif
static int jogador_obj = JOGADOR_OBJ;        /* 0 = carro original; 1..11 = equipe do .obj */
static uint8_t obj_ck_pula[OBJ_NTRI];         /* 1 = some na visão de cockpit (em volta da câmera) */
static void criar_carro_obj(void) {
    int bytes = TEXB_OBJ_TEX1 - TEXB_OBJ_TEX0;
    for(int i = 0; i < OBJ_NPINT; i++) {
        pvr_ptr_t t = pvr_mem_malloc(bytes);
        if(!t) return;
        pvr_txr_load(tex_bin + TEXB_OBJ_TEX0 + i * bytes, t, bytes);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                         256, 256, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
        pvr_poly_compile(&hdr_obj[i], &cxt);
    }
    for(int t = 0; t < OBJ_NTRI; t++) {
        if(t >= OBJ_PARTE_INI[5]) { obj_ck_pula[t] = 1; continue; }          /* capacete */
        if(t >= OBJ_PARTE_INI[1]) continue;                                   /* rodas */
        float cx = 0, cy = 0, cz = 0;
        for(int v = 0; v < 3; v++) { const float *a = OBJ_V[OBJ_T[t][v]]; cx += a[0]; cy += a[1]; cz += a[2]; }
        cx /= 3; cy /= 3; cz /= 3;
        obj_ck_pula[t] = cz < 0.95f && cz > -1.2f && fabsf(cx) < 0.42f && cy > 0.28f;
    }
    obj_ok = 1;
}
/* equipes dos carros: o jogador (se escolheu uma) e os rivais com as outras */
static void obj_atribuir(void) {
    int prox = 0;
    for(int i = 0; i < N_CARROS; i++) {
        if(i == 0) { obj_pint[0] = jogador_obj - 1; }
        else {
            if(prox == jogador_obj - 1) prox++;
            obj_pint[i] = prox % OBJ_NPINT;
            prox++;
        }
        if(obj_pint[i] >= 0) {
            carro_t *c = &carros[i];
            c->cores[C_PRINC] = OBJ_COR[obj_pint[i]][0];
            c->cores[C_SEC] = OBJ_COR[obj_pint[i]][1];
            c->cores[C_NARIZ] = c->cores[C_TERC] = c->cores[C_MOTOR] = c->cores[C_PRINC];
            c->cores[C_ASA] = c->cores[C_SEC];
        }
    }
}
static void desenhar_carro_obj(const carro_t *c, const float P[3], const float R[3], const float U[3], const float Fr[3], int cockpit) {
    int pint = obj_pint[c - carros];
    float M[3][3], T[3], dP[3] = { P[0] - cam_p[0], P[1] - cam_p[1], P[2] - cam_p[2] };
    const float *eixo[3] = { cam_r, cam_u, cam_f };
    for(int l = 0; l < 3; l++) {
        const float *e = eixo[l];
        float sg = l == 2 ? -1.0f : 1.0f;
        M[l][0] = sg * (e[0] * R[0] + e[1] * R[1] + e[2] * R[2]);
        M[l][1] = sg * (e[0] * U[0] + e[1] * U[1] + e[2] * U[2]);
        M[l][2] = sg * (e[0] * Fr[0] + e[1] * Fr[1] + e[2] * Fr[2]);
        T[l]    = sg * (e[0] * dP[0] + e[1] * dP[1] + e[2] * dP[2]);
    }
    static matrix_t mt __attribute__((aligned(32)));
    for(int l = 0; l < 3; l++) { mt[0][l] = M[l][0]; mt[1][l] = M[l][1]; mt[2][l] = M[l][2]; mt[3][l] = T[l]; }
    mt[0][3] = mt[1][3] = mt[2][3] = 0; mt[3][3] = 1;
    mat_load(&mt);
    float Ll[3] = { R[0] * LUZ[0] + R[1] * LUZ[1] + R[2] * LUZ[2], U[0] * LUZ[0] + U[1] * LUZ[1] + U[2] * LUZ[2],
                    Fr[0] * LUZ[0] + Fr[1] * LUZ[1] + Fr[2] * LUZ[2] };
    float kb = brilho * luz_base * 1.05f + 0.05f, kd = brilho * luz_dir * 1.05f, kc = 0.10f * brilho;
    float suj = c->sujeira * 0.35f;
    int borrado = c->v > 14.0f;
    /* 1) cada vértice ÚNICO transformado e iluminado uma vez */
    static vv_t tv[OBJ_NV];
    int pmax = cockpit ? 5 : 6;                                  /* cockpit: sem o capacete */
    for(int pt = 0; pt < pmax; pt++) {
        float gc = 1, gs = 0, ec = 1, es = 0;
        const float *C0 = OBJ_RODA[pt >= 1 && pt <= 4 ? pt : 0];
        int roda = pt >= 1 && pt <= 4;
        if(roda) {
            float r = OBJ_RODA[pt][1];
            float ang = borrado ? fmodf(c->s / r * 0.035f + pt * 0.4f, 2 * PI_F) : fmodf(c->s / r, 2 * PI_F);
            gc = fcos(-ang); gs = fsin(-ang);
            if(pt <= 2) { float st = -c->volante * 0.32f; ec = fcos(st); es = fsin(st); }
        }
        for(int i = OBJ_VPARTE_INI[pt]; i < OBJ_VPARTE_INI[pt + 1]; i++) {
            const float *a = OBJ_V[i];
            float x = a[0], y = a[1], z = a[2], nx = a[3], ny = a[4], nz = a[5];
            if(roda) {
                float dx = x - C0[0], dy = y - C0[1], dz = z - C0[2];
                float y1 = dy * gc - dz * gs, z1 = dy * gs + dz * gc;
                x = C0[0] + dx * ec + z1 * es; y = C0[1] + y1; z = C0[2] - dx * es + z1 * ec;
                float n1 = ny * gc - nz * gs, n2 = ny * gs + nz * gc;
                float nx2 = nx * ec + n2 * es; nz = -nx * es + n2 * ec; nx = nx2; ny = n1;
            }
            float d = nx * Ll[0] + ny * Ll[1] + nz * Ll[2];
            float k = kb + kd * (d > 0 ? d : 0) + kc * (ny > 0 ? ny : 0);
            if(y < 0.35f) k *= 1.0f - suj * (1.0f - y * (1.0f / 0.35f));     /* sujeira embaixo */
            int g = (int)(limitar(k, 0, 1.0f) * 255);
            mat_trans_single3_nodiv(x, y, z);
            tv[i].x = x; tv[i].y = y; tv[i].z = z; tv[i].c = 0xFF000000 | (uint32_t)g * 0x010101u;
        }
    }
    /* 2) triângulos: descarte de costas no espaço da câmera (a ordem dos cantos já vem certa) */
    pvr_prim(&hdr_obj[pint], sizeof(pvr_poly_hdr_t));
    int tmax = cockpit ? OBJ_PARTE_INI[5] : OBJ_NTRI;
    for(int t = 0; t < tmax; t++) {
        if(cockpit && obj_ck_pula[t]) continue;
        const vv_t *A = &tv[OBJ_T[t][0]], *B = &tv[OBJ_T[t][1]], *Cc = &tv[OBJ_T[t][2]];
        float ux = B->x - A->x, uy = B->y - A->y, uz = B->z - A->z, wx = Cc->x - A->x, wy = Cc->y - A->y, wz = Cc->z - A->z;
        float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
        if(nx * A->x + ny * A->y + nz * A->z <= 0) continue;   /* de costas (carro -> câmera inverte a orientação: z p/ frente vs -z) */
        vvt_t q[3];
        const vv_t *src[3] = { A, B, Cc };
        for(int v = 0; v < 3; v++) {
            q[v].x = src[v]->x; q[v].y = src[v]->y; q[v].z = src[v]->z; q[v].c = src[v]->c; q[v].o = 0;
            q[v].u = OBJ_UV[t][v][0]; q[v].v = OBJ_UV[t][v][1];
        }
        poligono_tex(q, 3);
    }
    mat_load(&mat_cam);
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
}
#endif
static void desenhar_carro(const carro_t *c, int cockpit) {
    float P[3], R[3], U[3], Fr[3];
    suj_atual = c->sujeira;
    base_carro(c, P, R, U, Fr);
    if(!cockpit) {
        if(dist2_cam(P) > 600.0f * 600.0f || !na_campo(P, 4.0f)) return;
        /* sombra: no quadro principal vai para a lista translúcida (borda
         * esfumaçada); no retrovisor, o quad escuro simples */
        float cs[4][2] = { { -1.15f, -2.8f }, { 1.15f, -2.8f }, { 1.15f, 3.2f }, { -1.15f, 3.2f } };
        if(gravar_sombra) mv_registrar(P, R, Fr);
        if(gravar_sombra && n_sombras < N_CARROS) {
            for(int i = 0; i < 4; i++) {
                sombra_w[n_sombras][i][0] = P[0] + R[0] * cs[i][0] + Fr[0] * cs[i][1];
                sombra_w[n_sombras][i][1] = P[1] + 0.05f + Fr[1] * cs[i][1];
                sombra_w[n_sombras][i][2] = P[2] + R[2] * cs[i][0] + Fr[2] * cs[i][1];
            }
            refl_p[n_sombras][0] = P[0]; refl_p[n_sombras][1] = P[1]; refl_p[n_sombras][2] = P[2];
            refl_cor[n_sombras] = c->cores[C_PRINC];
            refl_luz[n_sombras] = molhado > 0.15f && ((int)(t_clima * 4.0f + (c - carros) * 0.37f) & 1);
            if(refl_luz[n_sombras] && dist2_cam(P) < 140.0f * 140.0f) {     /* HALO da luz traseira na chuva */
                float L[3];
                for(int e = 0; e < 3; e++) L[e] = P[e] - Fr[e] * 2.47f + U[e] * 0.43f;
                brilho_add(L, 0.55f + 0.35f * molhado, ((uint32_t)(150 + 80 * chuva) << 24) | 0xFF3020);
            }
            n_sombras++;
        } else {
            vv_t q[4];
            for(int i = 0; i < 4; i++)
                para_vista(P[0] + R[0] * cs[i][0] * 0.8f + Fr[0] * cs[i][1] * 0.9f, P[1] + 0.04f + Fr[1] * cs[i][1],
                           P[2] + R[2] * cs[i][0] * 0.8f + Fr[2] * cs[i][1] * 0.9f, 0xFF2E3034, &q[i]);
            poligono(q, 4);
        }
    }
    /* perto (ou o próprio jogador): modelo detalhado; longe: o simples */
    /* NÍVEIS DE DETALHE: jogador = detalhado completo; até 3 rivais a menos
     * de DIST_MEDIO = detalhado sem as peças miúdas (médio); o resto = simples */
    int perto = cockpit || c == carros || (n_detalhados < 1 + N_MEDIO && escolhido[c - carros] &&
                                           dist2_cam(P) < DIST_MEDIO * DIST_MEDIO);
    int medio = perto && !cockpit && c != carros && escolhido[c - carros] != 1;
    if(perto) n_detalhados++;
    uint32_t cores[N_CORES];
    memcpy(cores, c->cores, sizeof(cores));
    cores[C_DISCO] = cor_mistura(0xFF16171A, 0xFFFF5A10, c->calor * 0.9f);
    cores[C_VISOR] = cor_escala(cor_mistura(cores[C_VISOR], cor_mistura(cor_ceu, 0xFFFFFFFF, 0.25f), 0.55f), 0.75f + 0.25f * brilho);   /* viseira reflete o céu */
    {   /* DESGASTE: o pneu escurece e perde o tom com as voltas */
        float d = limitar(((c->volta < 0 ? 0 : c->volta) * COMPR + c->s) / (6.0f * COMPR), 0, 1);
        cores[C_PNEU] = cor_mistura(cores[C_PNEU], 0xFF1C1B1A, 0.55f * d);
    }
    {   /* cabeça: vai para fora nas curvas (força lateral) e para a frente nas freadas */
        const fatia_t *ff = fatia_em(c->s);
        float lat = limitar(c->v * c->v * ff->curva / 35.0f, -1, 1);
        ((carro_t *)c)->cab_l += (lat - c->cab_l) * 0.15f;
        cab_dx = -c->cab_l * 0.035f;
        cab_dz = c->freio_in * limitar(c->v / 40.0f, 0, 1) * 0.03f;
        giro_c[5] = 1; giro_s[5] = 0; giro_cy[5] = 0; giro_cz[5] = 0;
    }
#ifdef PINT_RIVAIS
    pint_on = c == carros || (perto && !medio);   /* teste: tinta também nos rivais completos (~0,8 ms) */
#else
#ifdef SEM_PINT
    pint_on = 0;
#else
    pint_on = c == carros;                       /* tinta/metálica só no jogador (custo) */
#endif
#endif
    spec_on = (c == carros && !cockpit) ? 1 : 0;          /* brilho só no jogador, nas superfícies curvas (custo) */
    spec_forca = 0.85f * (1.0f - 0.7f * nuvens) * (1.0f - 0.4f * chuva) + 0.15f * tarde;
    /* luz de chuva traseira piscando com a pista molhada */
    if(molhado > 0.15f && ((int)(t_clima * 4.0f + (c - carros) * 0.37f) & 1)) cores[C_LUZ] = 0xFFFF3020;
    /* rodas: ângulo = distância rodada / raio. Devagar, raios e marcas giram
     * nítidos; rápido (> ~50 km/h), viram borrão — e a banda continua girando
     * num ritmo "estroboscópico" mais lento, como nas imagens de TV. */
    int borrado = c->v > 14.0f;
    modo_oculto = (borrado || medio) ? 1 : 2;      /* médio: só a roda "borrada" (sem raios) */
    for(int k = 1; k <= 4; k++) {
        float r = roda_r[k] > 0 ? roda_r[k] : 0.34f;
        float ang = fmodf(c->s / r, 2 * PI_F);
        if(borrado) ang = fmodf(c->s / r * 0.035f + k * 0.4f, 2 * PI_F);
        giro_c[k] = cosf(ang); giro_s[k] = sinf(ang);
        giro_cy[k] = roda_cy[k]; giro_cz[k] = roda_cz[k];
    }
    if(borrado || medio) {
        float tremor = 0.94f + 0.06f * aleat((uint32_t)(t_clima * 60.0f) + (uint32_t)(c - carros) * 7);
        cores[C_BORRAO_PNEU] = cor_escala(cor_mistura(cores[C_PNEU], cores[C_MARCA], 0.22f), tremor);
        cores[C_BORRAO_ARO]  = cor_escala(cor_mistura(cores[C_ESCURO], cores[C_ARO], 0.48f), tremor);
    }
    /* no cockpit, capacete/viseira/halo do próprio piloto ficariam em volta da câmera */
    uint32_t pular = cockpit ? (1u << C_CAPACETE) | (1u << C_VISOR) | (1u << C_HALO) : 0;
    pular_atras = (uint8_t)cockpit;
    pular_miudo = medio ? 1 : 2;      /* médio: sem miúdas/arredondadas; completo: sem as "caixas" */
    if(!medio && c != carros) pular_miudo |= 4 | 8;   /* rival completo: sem os detalhes do jogador, pneu de 8 lados */
    if(!medio && c == carros) pular_miudo |= 16;      /* jogador: pneu de 12 lados */
    tex_carro_ativo = (int)(c - carros) + 1;          /* faixa e número deste carro */
#ifdef TESTE_SEM_ENVIO
    sem_envio = 1;
#endif
#ifndef SEM_CARRO_OBJ
    if(obj_ok && obj_pint[c - carros] >= 0) {
        desenhar_carro_obj(c, P, R, U, Fr, cockpit);
        sem_envio = 0; tex_carro_ativo = 0; pular_atras = 0; modo_oculto = 2; pular_miudo = 2; spec_on = 0; pint_on = 0;
        return;
    }
#endif
#ifdef CARRO_TEX
#ifdef JOGADOR_HIB
    if(!cockpit && ctex_ok[c - carros]) {
#else
    if(!cockpit && c != carros && ctex_ok[c - carros]) {
#endif
        desenhar_carro_tex(c, P, R, U, Fr);
        if(dist2_cam(P) < 110.0f * 110.0f) {              /* perto: peças finas em geometria; longe basta a textura */
            modo_oculto = borrado ? 1 : 2;
            tex_carro_ativo = (int)(c - carros) + 1;
            pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
            desenhar_hibrido(P, R, U, Fr, cores);
        }
        tex_carro_ativo = 0; pular_atras = 0; modo_oculto = 2; pular_miudo = 2; spec_on = 0; pint_on = 0;
        return;
    }
#endif
    int lst = !perto ? LST_SIMPLES : cockpit ? LST_COCKPIT : medio ? LST_MEDIO : c == carros ? LST_JOGADOR : LST_RIVAL;
    lista_atual = lst;
#ifdef DIAG_CARRO
    uint64_t _t0 = timer_us_gettime64();
#endif
    desenhar_faces(lista_f[lst], n_lista[lst], P, R, U, Fr, cores, pular);
#ifdef DIAG_CARRO
    car_ac[lst] += timer_us_gettime64() - _t0; car_n[lst]++;
#endif
    sem_envio = 0;
    pular_miudo = 2;
    spec_on = 0; pint_on = 0;
    tex_carro_ativo = 0;
    pular_atras = 0;
    modo_oculto = 2;
}

/* ======================================================================== */
/* POÇAS D'ÁGUA (só com a pista encharcada)                                  */
/* ======================================================================== */
/* A água junta onde a pista é mais BAIXA: cada ponto de mínimo local do
 * relevo real ganha um grupo de poças, mais algumas espalhadas perto das
 * bordas (o asfalto tem caimento para os lados). Cada poça é um polígono
 * irregular de 7 lados deitado sobre o asfalto, na cor do céu (o "espelho"),
 * com um reflexo mais claro por dentro que tremula com a chuva. */

#define MAX_POCAS 110
typedef struct { float s, x; float ds[7], dx[7]; } poca_t;
static poca_t pocas[MAX_POCAS];
static int n_pocas;
static int8_t poca_da_fatia[MAX_FATIAS];       /* índice de uma poça na fatia, -1 = nenhuma */

static void criar_poca(float s, float x, uint32_t sem) {
    if(n_pocas >= MAX_POCAS) return;
    poca_t *p = &pocas[n_pocas];
    p->s = s; p->x = limitar(x, -6.0f, 6.0f);
    float comp = 1.6f + aleat(sem) * 2.6f, larg = 0.9f + aleat(sem + 1) * 1.3f;
    for(int i = 0; i < 7; i++) {
        float a = i * 2 * PI_F / 7 + aleat(sem + 2) * 0.5f;
        float r = 0.7f + 0.3f * aleat(sem + 10 + i);
        p->ds[i] = cosf(a) * comp * r;
        p->dx[i] = sinf(a) * larg * r;
    }
    int k = ((int)(s / PASSO)) % NF;
    poca_da_fatia[k] = (int8_t)n_pocas;
    if(k + 1 < NF) poca_da_fatia[k + 1] = (int8_t)n_pocas;
    n_pocas++;
}

static void montar_pocas(void) {
    memset(poca_da_fatia, -1, sizeof(poca_da_fatia));
    for(int k = 0; k < NF && n_pocas < MAX_POCAS; k++) {
        /* mínimo local do relevo num trecho de ±60 m? */
        int minimo = 1;
        for(int d = -15; d <= 15 && minimo; d++)
            if(d && F[(k + d + NF) % NF].y < F[k].y) minimo = 0;
        uint32_t sem = k * 37 + 5;
        if(minimo) {
            for(int j = 0; j < 4; j++)
                criar_poca(k * (float)PASSO + (j - 1.5f) * 9.0f, (aleat(sem + j) - 0.5f) * 11.0f, sem + j * 101);
        }
        else if(k % 13 == 0 && aleat(sem + 50) < 0.7f) {
            float lado = aleat(sem + 51) < 0.5f ? -1.0f : 1.0f;   /* perto da borda */
            criar_poca(k * (float)PASSO, lado * (3.8f + aleat(sem + 52) * 2.2f), sem + 53);
        }
    }
}

/* quanto as poças aparecem: só com a pista bem molhada */
static inline float forca_pocas(void) { return limitar((molhado - 0.45f) / 0.4f, 0, 1); }

/* o carro está passando por cima de uma poça? */
static int sobre_poca(const carro_t *c) {
    int k = ((int)(c->s / PASSO)) % NF;
    int i = poca_da_fatia[k];
    if(i < 0) return 0;
    float ds = fmodf(c->s - pocas[i].s + COMPR * 1.5f, COMPR) - COMPR * 0.5f;
    return fabsf(ds) < 4.0f && fabsf(c->x - pocas[i].x) < 2.2f;
}

/* ======================================================================== */
/* FUMAÇA DOS PNEUS (partículas translúcidas que ficam paradas no mundo)     */
/* ======================================================================== */

#define MAX_FUMACA 520
typedef struct { float p[3]; float idade, vida, tam, sobe, forca, cresce; uint32_t rgb; } fumaca_t;
static fumaca_t fumaca[MAX_FUMACA];
static int fumaca_prox;

static fumaca_t *nova_particula(void) {
    fumaca_t *f = &fumaca[fumaca_prox];
    fumaca_prox = (fumaca_prox + 1) % MAX_FUMACA;
    return f;
}

/* SPRAY: na pista molhada, o pneu traseiro levanta uma névoa d'água que
 * fica para trás e esconde quem vem atrás (a imagem clássica da F1 na chuva). */
static void soltar_spray(const carro_t *c, int quadro) {
    if(molhado < 0.2f || c->v < 18.0f) return;
    int poca = forca_pocas() > 0.3f && sobre_poca(c);
    float P[3], R[3], U[3], Fr[3];
    base_carro(c, P, R, U, Fr);
    if(dist2_cam(P) > 160.0f * 160.0f) return;
    int n = poca ? 3 : 1 + (molhado > 0.6f);   /* sprites: spray mais denso; na poça, jato bem maior */
    for(int k = 0; k < n; k++) {
    uint32_t sem = quadro * 13 + (uint32_t)(c - carros) * 977 + k * 31;
    fumaca_t *f = nova_particula();
    float lado = (aleat(sem) - 0.5f) * 1.6f;
    for(int e = 0; e < 3; e++) f->p[e] = P[e] + R[e] * lado - Fr[e] * 2.3f;
    f->p[1] += 0.35f;
    f->idade = 0;
    f->vida = 0.5f + aleat(sem + 1) * 0.4f;
    f->tam = 0.6f;
    f->cresce = 2.6f;
    f->sobe = 0.8f + aleat(sem + 2) * 0.6f;
    f->forca = molhado * limitar(c->v / 55.0f, 0, 1) * (poca ? 1.3f : 1.1f);
    if(poca) { f->sobe += 0.8f; f->cresce = 2.4f; f->vida += 0.1f; }
    /* temporal: a "parede" de spray é mais alta, larga e demora a baixar */
    f->cresce += 1.8f * pesada; f->vida += 0.45f * pesada; f->forca *= 1.0f + 0.35f * pesada;
    f->rgb = 0x00C8D0D8;
    }
}

/* MARCAS DE PNEU fora da pista: cada saída deixa dois sulcos na grama/brita
 * que ficam e vão clareando com o tempo */
#define MAX_MARCA_G 260
static float mg_p[MAX_MARCA_G][4][3], mg_t[MAX_MARCA_G];
static int mg_prox;
static float mg_ult[N_CARROS][2];
static uint8_t mg_tem[N_CARROS];
static void marca_grama(const carro_t *c) {
    int ic = (int)(c - carros);
    if(!c->fora || c->v < 6.0f || fabsf(c->x) > 23.5f) { mg_tem[ic] = 0; return; }
    if(mg_tem[ic]) {
        float ds = fmodf(c->s - mg_ult[ic][0] + COMPR, COMPR);
        if(ds < 1.2f) return;
        if(ds < 6.0f) {
            for(int l = -1; l <= 1; l += 2) {
                float (*q)[3] = mg_p[mg_prox];
                float x0 = mg_ult[ic][1] + l * 0.8f, x1 = c->x + l * 0.8f;
                float s0 = mg_ult[ic][0], s1 = c->s;
                ponto_pista(s0, x0 - 0.17f, q[0], 0, 0); ponto_pista(s0, x0 + 0.17f, q[1], 0, 0);
                ponto_pista(s1, x1 + 0.17f, q[2], 0, 0); ponto_pista(s1, x1 - 0.17f, q[3], 0, 0);
                q[0][1] = q[1][1] = chao_preciso(s0, x0) + 0.04f;
                q[2][1] = q[3][1] = chao_preciso(s1, x1) + 0.04f;
                mg_t[mg_prox] = t_clima;
                mg_prox = (mg_prox + 1) % MAX_MARCA_G;
            }
        }
    }
    mg_ult[ic][0] = c->s; mg_ult[ic][1] = c->x; mg_tem[ic] = 1;
}
static void desenhar_marcas_grama_tr(void) {
    int primeiro = 1;
    for(int i = 0; i < MAX_MARCA_G; i++) {
        if(mg_t[i] <= 0) continue;
        float idade = t_clima - mg_t[i];
        if(idade < 0 || idade > 150.0f) continue;
        if(dist2_cam(mg_p[i][0]) > 120.0f * 120.0f) continue;
        if(primeiro) { pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca)); primeiro = 0; }
        uint32_t a = (uint32_t)(150.0f * (1.0f - idade / 150.0f));
        uint32_t c = (a << 24) | 0x2A2416;
        vv_t q[4]; int fora = 0;
        for(int v = 0; v < 4; v++) { para_vista(mg_p[i][v][0], mg_p[i][v][1], mg_p[i][v][2], c, &q[v]); fora |= q[v].z > -PERTO; }
        if(!fora) poligono(q, 4);
    }
}
/* VAPOR subindo do asfalto quando o sol volta depois da chuva */
static void soltar_vapor(int quadro) {
    float sol = limitar(luz_dir / 0.5f, 0, 1) * (1.0f - nuvens);
    if(molhado < 0.12f || chuva > 0.05f || sol < 0.25f || (quadro & 1)) return;
    int nj = nvis < 30 ? nvis : 30;
    if(nj < 4) return;
    uint32_t sem = quadro * 7u + 11;
    int j = 3 + (int)(aleat(sem) * (nj - 3));
    const fatia_t *f = &F[vis[j].k];
    float lat = (aleat(sem + 1) - 0.5f) * 13.0f;
    fumaca_t *p = nova_particula();
    p->p[0] = f->x + f->rx * lat; p->p[1] = f->y + 0.15f; p->p[2] = f->z + f->rz * lat;
    p->idade = 0; p->vida = 2.5f + aleat(sem + 2) * 1.5f;
    p->tam = 1.2f; p->cresce = 0.9f; p->sobe = 0.35f + aleat(sem + 3) * 0.3f;
    p->forca = limitar(molhado * sol * 2.0f, 0.15f, 0.55f);
    p->rgb = 0x00F0F2F4;
}
/* FUMAÇA COLORIDA de sinalizadores (verde e amarela) e PAPEL PICADO na
 * frente das arquibancadas perto dos carros */
static void torcida_efeitos(int quadro) {
    if(chuva > 0.6f || n_arqs == 0) return;
    if(quadro & 1) return;
    uint32_t sem = quadro * 2654435761u;
    int q = (int)(aleat(sem) * n_arqs) % n_arqs;
    const arq_t *a = &arqs[q];
    float dxf = a->f[0] - foco[0], dzf = a->f[2] - foco[2];
    if(dxf * dxf + dzf * dzf > 230.0f * 230.0f) return;
    float u = (aleat(sem + 1) - 0.5f) * 1.8f;
    float d = 0.05f + aleat(sem + 2) * 0.35f;            /* fileiras da frente (visíveis da pista) */
    float P[3] = { a->f[0] + a->t[0] * u * a->meio + a->o[0] * d * a->prof, a->f[1] + a->h0 + (a->h1 - a->h0) * d + 1.4f,
                   a->f[2] + a->t[2] * u * a->meio + a->o[2] * d * a->prof };
    if(aleat(sem + 3) < 0.35f) {                       /* sinalizador: nuvem colorida que sobe */
        for(int k = 0; k < 2; k++) {
            fumaca_t *f = nova_particula();
            memcpy(f->p, P, 12); f->p[1] += k * 0.6f;
            f->idade = 0; f->vida = 6.0f + aleat(sem + 4 + k) * 3.0f;
            f->tam = 1.4f; f->cresce = 1.9f; f->sobe = 1.6f + aleat(sem + 6) * 0.8f;
            f->forca = 3.6f;
            f->rgb = (aleat(sem + 7) < 0.5f) ? 0x0020C048 : 0x00F2D020;
        }
    } else {                                           /* papel picado caindo */
        static const uint32_t COR[5] = { 0x00F0F0F0, 0x0020B048, 0x00F2D020, 0x003060E0, 0x00E03030 };
        for(int k = 0; k < 26; k++) {
            fumaca_t *f = nova_particula();
            f->p[0] = P[0] + (aleat(sem + 10 + k) - 0.5f) * 5.0f; f->p[1] = P[1] + 2.5f + aleat(sem + 20 + k) * 2.0f;
            f->p[2] = P[2] + (aleat(sem + 30 + k) - 0.5f) * 3.0f;
            f->idade = 0; f->vida = 3.5f + aleat(sem + 40 + k) * 1.5f;
            f->tam = 0.38f; f->cresce = 0.0f; f->sobe = -0.6f - aleat(sem + 50 + k) * 0.5f;
            f->forca = 7.0f; f->rgb = COR[k % 5];
        }
    }
}
static void soltar_fumaca(const carro_t *c, int quadro) {
    marca_grama(c);
    if(c == carros) torcida_efeitos(quadro);
    if(c == carros) soltar_vapor(quadro);
    soltar_spray(c, quadro);
    {   /* sujeira: aos poucos com a corrida, rápido fora da pista e na chuva */
        carro_t *cc = (carro_t *)c;
        float k = 0.00004f + (c->fora && c->v > 5.0f ? 0.0025f : 0.0f) + (molhado * 0.00012f) * (c->v > 15.0f);
        cc->sujeira = limitar(cc->sujeira + k, 0, 1);
    }
    /* POEIRA / GRAMA levantada quando o carro sai da pista */
    if(c->fora && c->v > 6.0f && molhado < 0.5f && !(quadro & 1)) {
        float P[3], R[3], U[3], Fr[3];
        base_carro(c, P, R, U, Fr);
        int k = (int)(c->s / PASSO) % NF;
        int brita = F[k].brita != 0;
        for(int lado = -1; lado <= 1; lado += 2) {
            fumaca_t *f = nova_particula();
            uint32_t sem = quadro * 7u + (lado > 0) + (uint32_t)(c - carros) * 331u;
            for(int e = 0; e < 3; e++) f->p[e] = P[e] + R[e] * lado * 0.8f - Fr[e] * 1.7f;
            f->p[1] += 0.2f;
            f->idade = 0;
            f->vida = 0.8f + aleat(sem) * 0.6f;
            f->tam = 0.35f;
            f->cresce = 1.6f + 0.9f * aleat(sem + 1);
            f->sobe = 0.3f + 0.5f * aleat(sem + 2);
            f->forca = limitar(c->v / 40.0f, 0.3f, 1.3f) * (brita ? 1.5f : 0.9f);
            f->rgb = brita ? 0x00C0B090 : 0x00A08E5E;
        }
    }
    if(!c->derrapando || c->fora || c->v < 12.0f) return;
    if(molhado > 0.6f) return;                      /* no molhado o pneu não "fuma" */
    /* menos partículas quando escorrega pouco */
    if((quadro % 3) && c->deriva < 0.6f) return;   /* poucas partículas */
    if((quadro & 1) && c->deriva >= 0.6f) return;
    float P[3], R[3], U[3], Fr[3];
    base_carro(c, P, R, U, Fr);
    for(int lado = -1; lado <= 1; lado += 2) {
        fumaca_t *f = nova_particula();
        f->cresce = 0.7f;
        f->rgb = 0x00ECECEC;
        uint32_t sem = quadro * 2 + (lado > 0) + (uint32_t)(c - carros) * 977;
        for(int e = 0; e < 3; e++) f->p[e] = P[e] + R[e] * lado * 0.84f - Fr[e] * 1.75f;
        f->p[0] += (aleat(sem) - 0.5f) * 0.4f;
        f->p[2] += (aleat(sem + 1) - 0.5f) * 0.4f;
        f->p[1] += 0.25f;
        f->idade = 0;
        f->vida = 0.45f + aleat(sem + 2) * 0.35f;
        f->tam = 0.18f + 0.2f * c->deriva;
        f->forca = 0.5f + 0.5f * c->deriva;
        f->sobe = 0.6f + aleat(sem + 3) * 0.8f;
    }
}

static void atualizar_fumaca(float dt) {
    for(int i = 0; i < MAX_FUMACA; i++) {
        fumaca_t *f = &fumaca[i];
        if(f->idade >= f->vida) continue;
        f->idade += dt;
        f->p[1] += f->sobe * dt;
        f->tam += f->cresce * dt;               /* a nuvem espalha */
    }
}


static pvr_ptr_t tex_nuvem_ptr;
static void desenhar_fumaca(void) {
#ifndef SEM_SPRITE
    /* SPRITES do PowerVR: cada partícula = um cabeçalho com a cor + 1 vértice
     * empacotado (antes: 4 vértices completos) -> cabem mais partículas */
    static pvr_sprite_hdr_t base; static int ok;
    if(!ok && tex_nuvem_ptr) {
        pvr_sprite_cxt_t cx;
        pvr_sprite_cxt_txr(&cx, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 64, 64, tex_nuvem_ptr, PVR_FILTER_BILINEAR);
        cx.gen.culling = PVR_CULLING_NONE; cx.gen.fog_type = PVR_FOG_TABLE;
        cx.txr.env = PVR_TXRENV_MODULATEALPHA;
        pvr_sprite_compile(&base, &cx);
        ok = 1;
    }
    if(ok) {
        static const float ROT[4][2] = { { 1, 0 }, { 0.707f, 0.707f }, { 0, 1 }, { -0.707f, 0.707f } };
        for(int i = 0; i < MAX_FUMACA; i++) {
            fumaca_t *f = &fumaca[i];
            if(f->idade >= f->vida) continue;
            vv_t c;
            para_vista(f->p[0], f->p[1], f->p[2], 0, &c);
            if(c.z > -PERTO - f->tam) continue;
            float t = f->idade / f->vida;
            int a = (int)((1.0f - t) * (1.0f - t) * 80.0f * f->forca);
            if(-c.z < 7.0f) a = (int)(a * (-c.z - PERTO) / 7.0f);
            if(a <= 0) continue;
            if(a > 255) a = 255;
            float iz = __frsqrt(c.z * c.z);
            float sx = CX + FOCO * c.x * iz, sy = CY - FOCO * c.y * iz, r = FOCO * f->tam * 1.35f * iz;
            if(sx < -r || sx > 640 + r || sy < -r || sy > 480 + r) continue;
            base.argb = ((uint32_t)a << 24) | (cor_escala(f->rgb, brilho) & 0x00FFFFFF);
            pvr_prim(&base, sizeof(base));
            const float *rt = ROT[i & 3];
            float ex = rt[0] * r, ey = rt[1] * r;            /* meio-eixo girado */
            int papel = f->cresce == 0.0f && f->forca >= 6.9f;
            if(papel) {                                       /* papel picado: retângulo girando no ar */
                float gira = fsin(t_clima * 9.0f + i * 1.3f);
                float an = t_clima * 3.0f + i;
                float ca = fcos(an), sa = fsin(an);
                ex = ca * r * 0.9f; ey = sa * r * 0.9f;
                float wx = -sa * r * 0.45f * gira, wy = ca * r * 0.45f * gira;
                pvr_sprite_txr_t sp;
                sp.flags = PVR_CMD_VERTEX_EOL;
                sp.ax = (sx - ex - wx) * XS; sp.ay = sy - ey - wy; sp.az = iz;
                sp.bx = (sx + ex - wx) * XS; sp.by = sy + ey - wy; sp.bz = iz;
                sp.cx = (sx + ex + wx) * XS; sp.cy = sy + ey + wy; sp.cz = iz;
                sp.dx = (sx - ex + wx) * XS; sp.dy = sy - ey + wy;
                sp.dummy = 0;
                sp.auv = PVR_PACK_16BIT_UV(0.48f, 0.48f); sp.buv = PVR_PACK_16BIT_UV(0.52f, 0.48f); sp.cuv = PVR_PACK_16BIT_UV(0.52f, 0.52f);
                base.argb = 0xFF000000 | (cor_escala(f->rgb | 0xFF000000, brilho * (0.75f + 0.25f * fabsf(gira))) & 0xFFFFFF);
                pvr_prim(&base, sizeof(base));
                pvr_prim(&sp, sizeof(sp));
                continue;
            }
            pvr_sprite_txr_t sp;
            sp.flags = PVR_CMD_VERTEX_EOL;
            sp.ax = (sx - ex + ey) * XS; sp.ay = sy - ey - ex; sp.az = iz;
            sp.bx = (sx + ex + ey) * XS; sp.by = sy + ey - ex; sp.bz = iz;
            sp.cx = (sx + ex - ey) * XS; sp.cy = sy + ey + ex; sp.cz = iz;
            sp.dx = (sx - ex - ey) * XS; sp.dy = sy - ey + ex;
            sp.dummy = 0;
            sp.auv = PVR_PACK_16BIT_UV(0.0f, 0.0f); sp.buv = PVR_PACK_16BIT_UV(1.0f, 0.0f); sp.cuv = PVR_PACK_16BIT_UV(1.0f, 1.0f);
            pvr_prim(&sp, sizeof(sp));
            vert_quadro++;
        }
        return;
    }
#endif
    pvr_prim(&hdr_nuvem, sizeof(hdr_nuvem));       /* bolo de fumaça com borda irregular */
    for(int i = 0; i < MAX_FUMACA; i++) {
        fumaca_t *f = &fumaca[i];
        if(f->idade >= f->vida) continue;
        vv_t c;
        para_vista(f->p[0], f->p[1], f->p[2], 0, &c);
        if(c.z > -PERTO - f->tam) continue;
        float t = f->idade / f->vida;
        int a = (int)((1.0f - t) * (1.0f - t) * 80.0f * f->forca);
        if(-c.z < 7.0f) a = (int)(a * (-c.z - PERTO) / 7.0f);   /* some ao encostar na câmera */
        if(a <= 0) continue;
        if(a > 255) a = 255;
        uint32_t cor = ((uint32_t)a << 24) | (cor_escala(f->rgb, brilho) & 0x00FFFFFF);
        /* quad virado para a câmera (billboard), meio "torto" para não parecer grade */
        float s = f->tam * 1.35f, g = 0.35f * s;
        int gi = i & 3;                                /* cada partícula girada de um jeito */
        static const float UU[4][4] = { { 0, 1, 1, 0 }, { 1, 1, 0, 0 }, { 1, 0, 0, 1 }, { 0, 0, 1, 1 } };
        vvt_t q[4] = {
            { c.x - s, c.y - g, c.z, UU[gi][0], UU[gi][1], cor, 0 }, { c.x + g, c.y - s, c.z, UU[gi][1], UU[gi][2], cor, 0 },
            { c.x + s, c.y + g, c.z, UU[gi][2], UU[gi][3], cor, 0 }, { c.x - g, c.y + s, c.z, UU[gi][3], UU[gi][0], cor, 0 },
        };
        poligono_tex(q, 4);
    }
}

/* ======================================================================== */
/* RASTRO DE AR NAS PONTAS DA ASA TRASEIRA (vórtices)                        */
/* ======================================================================== */
/* Em alta velocidade, a pressão cai no centro do vórtice que sai de cada
 * ponta da asa e a umidade do ar condensa: dois fios brancos saindo das
 * placas laterais. Quase invisível no seco e ensolarado, bem marcado no
 * tempo úmido e na chuva (como em Interlagos). Cada carro guarda as últimas
 * posições das duas pontas (uma por quadro): o fio fica parado no ar
 * enquanto o carro se afasta — por isso dá tanta noção de velocidade. */
#define RASTRO_N 13
typedef struct { float p[2][RASTRO_N][3]; int cab, n; } rastro_t;
static rastro_t rastros[N_CARROS];

static void atualizar_rastros(const carro_t *eu) {
    for(int i = 0; i < N_CARROS; i++) {
        rastro_t *r = &rastros[i];
        const carro_t *c = &carros[i];
        float ds = fmodf(c->s - eu->s + COMPR * 1.5f, COMPR) - COMPR * 0.5f;
        if(fabsf(ds) > 130.0f || c->v < 30.0f) { r->n = 0; continue; }
        float P[3], R[3], U[3], Fr[3];
        base_carro(c, P, R, U, Fr);
        r->cab = (r->cab + 1) % RASTRO_N;
        for(int l = 0; l < 2; l++) {
            float lado = l ? 0.51f : -0.51f;
            for(int e = 0; e < 3; e++)
                r->p[l][r->cab][e] = P[e] + R[e] * lado + U[e] * 1.045f - Fr[e] * 2.40f;
        }
        if(r->n < RASTRO_N) r->n++;
    }
}

/* senos prontos (sinf da libm é caro no SH4) */
static const float ONDA[16] = { 0.00f, 0.38f, 0.71f, 0.92f, 1.00f, 0.92f, 0.71f, 0.38f,
                                0.00f, -0.38f, -0.71f, -0.92f, -1.00f, -0.92f, -0.71f, -0.38f };

static void desenhar_rastros(void) {
    float umid = limitar(0.18f + 0.40f * nuvens + 0.55f * molhado + 0.35f * chuva, 0, 1);
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    uint32_t rgb = cor_escala(0xFFE8ECF0, brilho) & 0x00FFFFFF;
    for(int i = 0; i < N_CARROS; i++) {
        rastro_t *r = &rastros[i];
        if(r->n < 3) continue;
        float k = limitar((carros[i].v - 28.0f) / 32.0f, 0, 1) * umid;
        if(k < 0.03f) continue;
        for(int l = 0; l < 2; l++) {
            vv_t pv[RASTRO_N];
            for(int j = 0; j < r->n; j++) {
                const float *w = r->p[l][(r->cab - j + RASTRO_N) % RASTRO_N];
                /* o fio sobe um pouco e "enrola" conforme envelhece */
                float t = (float)j / (RASTRO_N - 1);
                float en = ONDA[(j * 3 + i * 5 + l * 11) & 15] * 0.03f * t;
                para_vista(w[0] + en, w[1] + 0.10f * t + en, w[2], 0, &pv[j]);
            }
            for(int j = 0; j + 1 < r->n; j++) {
                vv_t *A = &pv[j], *B = &pv[j + 1];
                if(A->z > -PERTO - 0.2f || B->z > -PERTO - 0.2f) continue;
                float t0 = (float)j / (RASTRO_N - 1), t1 = (float)(j + 1) / (RASTRO_N - 1);
                float a0 = (1 - t0) * (1 - t0) * 85.0f * k, a1 = (1 - t1) * (1 - t1) * 85.0f * k;
                if(j == 0) a0 *= 0.4f;                         /* nasce fininho na placa */
                float perto = -A->z < 4.0f ? (-A->z - PERTO) / 4.0f : 1.0f;   /* some ao encostar na câmera */
                a0 *= perto; a1 *= perto;
                if(a0 < 1 && a1 < 1) continue;
                float dx = B->x - A->x, dy = B->y - A->y;
                float dl = sqrtf(dx * dx + dy * dy);
                if(dl < 1e-4f) continue;
                float w0 = 0.018f + 0.045f * t0, w1 = 0.018f + 0.045f * t1;  /* espalha com a idade */
                float nx = -dy / dl, ny = dx / dl;
                uint32_t c0 = ((uint32_t)a0 << 24) | rgb, c1 = ((uint32_t)a1 << 24) | rgb;
                vv_t q[4] = {
                    { A->x + nx * w0, A->y + ny * w0, A->z, c0 },
                    { B->x + nx * w1, B->y + ny * w1, B->z, c1 },
                    { B->x - nx * w1, B->y - ny * w1, B->z, c1 },
                    { A->x - nx * w0, A->y - ny * w0, A->z, c0 },
                };
                poligono(q, 4);
            }
        }
    }
}

static void desenhar_pocas(void) {
    float k = forca_pocas();
    if(k <= 0.01f) return;
    /* espelho: reflete o céu (mais claro perto do horizonte) */
    uint32_t cor_ref = cor_mistura(cor_ceu, cor_neb, 0.55f);
    uint32_t a1 = (uint32_t)(175 * k), base = (a1 << 24) | (cor_escala(cor_ref, 0.95f) & 0x00FFFFFF);
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    for(int i = 0; i < n_pocas; i++) {
        poca_t *p = &pocas[i];
        float ds = fmodf(p->s - cam_s_ref + COMPR * 1.5f, COMPR) - COMPR * 0.5f;
        if(ds < -10.0f || ds > 170.0f) continue;
        vv_t q[7], r[7], cq, cr;
        float cs[3];
        ponto_pista(p->s, p->x, cs, 0, 0);
        /* reflexo interno tremulando com os pingos */
        float tr = 0.75f + 0.25f * sinf(t_clima * 5.0f + i * 1.7f) * chuva;
        uint32_t a2 = (uint32_t)(140 * k * tr);
        uint32_t brilho_ref = (a2 << 24) | (cor_escala(cor_mistura(cor_neb, 0xFFFFFFFF, 0.3f), brilho) & 0x00FFFFFF);
        for(int v = 0; v < 7; v++) {
            float w[3];
            ponto_pista(p->s + p->ds[v], p->x + p->dx[v], w, 0, 0);
            para_vista(w[0], w[1] + 0.03f, w[2], base, &q[v]);
            ponto_pista(p->s + p->ds[v] * 0.5f - 0.3f, p->x + p->dx[v] * 0.45f, w, 0, 0);
            para_vista(w[0], w[1] + 0.05f, w[2], brilho_ref, &r[v]);
        }
        /* leque de triângulos: centro com alfa, borda transparente = borda
         * macia (o PowerVR interpola o alfa entre os vértices) */
        para_vista(cs[0], cs[1] + 0.03f, cs[2], base, &cq);
        para_vista(cs[0], cs[1] + 0.05f, cs[2], brilho_ref, &cr);
        for(int v = 0; v < 7; v++) {
            vv_t t1[3] = { cq, q[v], q[(v + 1) % 7] };
            t1[1].c &= 0x00FFFFFF; t1[2].c &= 0x00FFFFFF;
            t1[1].c |= (a1 / 5) << 24; t1[2].c |= (a1 / 5) << 24;
            poligono(t1, 3);
            vv_t t2[3] = { cr, r[v], r[(v + 1) % 7] };
            t2[1].c &= 0x00FFFFFF; t2[2].c &= 0x00FFFFFF;
            poligono(t2, 3);
        }
    }
}

/* GOTAS DE CHUVA: riscos finos num "caixote" de 30 m em volta da câmera.
 * O risco aponta na velocidade da gota RELATIVA à câmera: parada, a chuva
 * cai quase reta; a 300 km/h, ela vem "voando" na sua direção. */
#define MAX_GOTAS 320
static float gotas[MAX_GOTAS][3];
static float cam_ant[3];
static int gotas_ok;

/* GOTAS NA LENTE / VISEIRA: na câmera de dentro, gotas pousam na tela, ficam
 * um pouco e escorrem; com velocidade, o vento as arrasta para os lados */
#define MAX_GOTA_TELA 36
static float gt_x[MAX_GOTA_TELA], gt_y[MAX_GOTA_TELA], gt_r[MAX_GOTA_TELA], gt_vida[MAX_GOTA_TELA];
static void desenhar_gotas_tela(float dt, int dentro, float vel) {
    static float acum;
    if(!dentro || chuva < 0.12f) { for(int i = 0; i < MAX_GOTA_TELA; i++) gt_vida[i] = 0; return; }
    acum += dt * (6.0f + 18.0f * chuva + 14.0f * pesada);
    while(acum > 1.0f) {
        acum -= 1.0f;
        for(int i = 0; i < MAX_GOTA_TELA; i++)
            if(gt_vida[i] <= 0) {
                uint32_t h = h32((uint32_t)(t_clima * 1000.0f) + i * 977u);
                gt_x[i] = 20 + (h % 600); gt_y[i] = 70 + ((h >> 10) % 390);
                gt_r[i] = 3.0f + ((h >> 20) % 9); gt_vida[i] = 1.6f + ((h >> 4) % 100) * 0.02f;
                break;
            }
    }
    pvr_prim(&hdr_brilho_luz, sizeof(hdr_brilho_luz));
    float sx = vel / 85.0f;
    for(int i = 0; i < MAX_GOTA_TELA; i++) {
        if(gt_vida[i] <= 0) continue;
        gt_vida[i] -= dt;
        gt_y[i] += (14.0f + gt_r[i] * 3.0f) * dt * (1.0f - sx);           /* escorre */
        gt_x[i] += (gt_x[i] - 320.0f) * sx * 1.6f * dt;                    /* vento empurra para fora */
        float a = limitar(gt_vida[i], 0, 1) * (0.35f + 0.25f * chuva);
        uint32_t c = ((uint32_t)(a * 255) << 24) | cor_escala(cor_mistura(cor_ceu, 0xFFFFFFFF, 0.6f), 0.8f + 0.2f * brilho) & 0xFFFFFF;
        float r = gt_r[i], ry = r * (1.0f + 1.2f * sx);
        pvr_vertex_t v;
        const float X[4] = { gt_x[i] - r, gt_x[i] - r, gt_x[i] + r, gt_x[i] + r }, Y[4] = { gt_y[i] + ry, gt_y[i] - ry, gt_y[i] + ry, gt_y[i] - ry };
        const float U[4] = { 0, 0, 1, 1 }, V[4] = { 1, 0, 1, 0 };
        for(int k = 0; k < 4; k++) {
            v.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v.x = X[k] * XS; v.y = Y[k]; v.z = 38.0f; v.u = U[k]; v.v = V[k]; v.argb = c; v.oargb = 0;
            pvr_prim(&v, sizeof(v));
        }
    }
}
static void desenhar_chuva(float dt) {
    float cv[3] = { (cam_p[0] - cam_ant[0]) / dt, (cam_p[1] - cam_ant[1]) / dt, (cam_p[2] - cam_ant[2]) / dt };
    if(cv[0] * cv[0] + cv[2] * cv[2] > 150.0f * 150.0f) cv[0] = cv[1] = cv[2] = 0;   /* troca de câmera */
    memcpy(cam_ant, cam_p, 12);
    if(!gotas_ok) {
        for(int i = 0; i < MAX_GOTAS; i++) {
            gotas[i][0] = cam_p[0] + (aleat(i * 3) - 0.5f) * 30;
            gotas[i][1] = cam_p[1] + (aleat(i * 3 + 1) - 0.4f) * 16;
            gotas[i][2] = cam_p[2] + (aleat(i * 3 + 2) - 0.5f) * 30;
        }
        gotas_ok = 1;
    }
    int n = (int)(200 * chuva * (0.45f + 0.55f * chuva) + 80 * pesada);   /* garoa leve, chuva densa */
    if(n > MAX_GOTAS) n = MAX_GOTAS;
    if(n <= 0) return;
    /* queda + vento (no temporal o vento é forte e a chuva cai inclinada) */
    const float vq[3] = { 1.5f + 4.5f * pesada, -12.0f - 4.0f * pesada, 0.8f + 2.0f * pesada };
    float vr[3] = { vq[0] - cv[0], vq[1] - cv[1], vq[2] - cv[2] };
    uint32_t cor = ((uint32_t)(85 + 70 * chuva + 30 * pesada) << 24) | (cor_escala(0xFFD4DAE0, brilho + relampago) & 0x00FFFFFF);
#ifndef SEM_SPRITE
    /* SPRITES do PowerVR: a cor vai no cabeçalho e cada gota é UM vértice
     * empacotado (metade dos dados) -> cabem ~60% mais gotas pelo mesmo custo */
    n = (int)(n * 1.6f); if(n > MAX_GOTAS) n = MAX_GOTAS;
    static pvr_sprite_hdr_t sh; static pvr_ptr_t st; static int sp_ok;
    if(!sp_ok) {
        for(int y = 0; y < 32; y++)                       /* risco da gota: forte embaixo, some em cima */
            for(int x = 0; x < 8; x++) {
                float a = (1.0f - fabsf(x - 3.5f) / 4.0f) * (y / 31.0f);
                tex_px[y * 8 + x] = (uint16_t)((int)(15 * limitar(a * 1.3f, 0, 1)) << 12 | 0x0FFF);
            }
        st = tex_subir(8, 32); sp_ok = 1;
    }
    {
        pvr_sprite_cxt_t cx;
        pvr_sprite_cxt_txr(&cx, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 8, 32, st, PVR_FILTER_BILINEAR);
        cx.gen.culling = PVR_CULLING_NONE; cx.gen.fog_type = PVR_FOG_TABLE;
        pvr_sprite_compile(&sh, &cx);
        sh.argb = cor; sh.oargb = 0;
        pvr_prim(&sh, sizeof(sh));
    }
    for(int i = 0; i < n; i++) {
        float *g = gotas[i];
        for(int e = 0; e < 3; e++) g[e] += vq[e] * dt;
        float r[3] = { g[0] - cam_p[0], g[1] - cam_p[1], g[2] - cam_p[2] };
        if(r[0] > 15) g[0] -= 30; else if(r[0] < -15) g[0] += 30;
        if(r[2] > 15) g[2] -= 30; else if(r[2] < -15) g[2] += 30;
        if(r[1] < -6) g[1] += 16; else if(r[1] > 10) g[1] -= 16;
        vv_t a, b;
        para_vista(g[0], g[1], g[2], cor, &a);
        para_vista(g[0] - vr[0] * 0.03f, g[1] - vr[1] * 0.03f, g[2] - vr[2] * 0.03f, cor, &b);
        if(a.z > -PERTO - 0.3f || b.z > -PERTO - 0.3f) continue;
        float w = 0.006f * -a.z * 0.25f + 0.008f;
        float ia = __frsqrt(a.z * a.z), ib = __frsqrt(b.z * b.z);
        float ax = (CX + FOCO * a.x * ia) * XS, ay = CY - FOCO * a.y * ia, bx = (CX + FOCO * b.x * ib) * XS, by = CY - FOCO * b.y * ib;
        float ww = FOCO * w * ia * XS;
        pvr_sprite_txr_t sp;
        sp.flags = PVR_CMD_VERTEX_EOL;
        sp.ax = bx - ww; sp.ay = by; sp.az = ib;
        sp.bx = bx + ww; sp.by = by; sp.bz = ib;
        sp.cx = ax + ww; sp.cy = ay; sp.cz = ia;
        sp.dx = ax - ww; sp.dy = ay;
        sp.dummy = 0;
        sp.auv = PVR_PACK_16BIT_UV(0.0f, 0.0f); sp.buv = PVR_PACK_16BIT_UV(1.0f, 0.0f); sp.cuv = PVR_PACK_16BIT_UV(1.0f, 1.0f);
        pvr_prim(&sp, sizeof(sp));
        vert_quadro++;
    }
    return;
#endif
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    for(int i = 0; i < n; i++) {
        float *g = gotas[i];
        for(int e = 0; e < 3; e++) g[e] += vq[e] * dt;
        /* mantém a gota dentro do caixote em volta da câmera */
        float r[3] = { g[0] - cam_p[0], g[1] - cam_p[1], g[2] - cam_p[2] };
        if(r[0] > 15) g[0] -= 30; else if(r[0] < -15) g[0] += 30;
        if(r[2] > 15) g[2] -= 30; else if(r[2] < -15) g[2] += 30;
        if(r[1] < -6) g[1] += 16; else if(r[1] > 10) g[1] -= 16;
        vv_t a, b;
        para_vista(g[0], g[1], g[2], cor, &a);
        para_vista(g[0] - vr[0] * 0.03f, g[1] - vr[1] * 0.03f, g[2] - vr[2] * 0.03f, cor, &b);
        if(a.z > -PERTO - 0.3f || b.z > -PERTO - 0.3f) continue;
        float w = 0.006f * -a.z * 0.25f + 0.008f;
        vv_t q[4] = { { a.x - w, a.y, a.z, cor }, { a.x + w, a.y, a.z, cor },
                      { b.x + w, b.y, b.z, cor & 0x30FFFFFF }, { b.x - w, b.y, b.z, cor & 0x30FFFFFF } };
        poligono(q, 4);
    }
}

/* Atualiza o clima (alvo pelo modo; tudo muda devagar) e aplica na cena. */
static float trovao_em = -1;
static float neb_ini_ant = -1, neb_fim_ant = -1;
static uint32_t neb_cor_ant;

static void clima_atualizar(float dt) {
    float an = 0.7f, ac = 0.0f, ap = 0.0f, am = 0.0f, at = 0.0f;
    t_clima += dt;
    t_clima_pub = t_clima;
    switch(modo_clima) {
        case CLIMA_SOL:     an = 0.05f; ac = 0.0f; break;
        case CLIMA_NUBLADO: an = 0.85f; ac = 0.0f; break;
        case CLIMA_CHUVA:   an = 1.0f;  ac = 1.0f; break;
        case CLIMA_PESADA:  an = 1.0f;  ac = 1.0f; ap = 1.0f; break;
        case CLIMA_ENSOLARADO: an = 0.0f; ac = 0.0f; am = 1.0f; break;
        case CLIMA_ENTARDECER: an = 0.10f; ac = 0.0f; at = 1.0f; break;
        default: {
            /* ciclo de 6 min com RAMPAS (interpolação entre pontos-chave):
             * sol -> nublando -> garoa -> chuva -> temporal -> chuva -> abrindo -> sol */
            static const float K[][4] = {     /* tempo, nuvens, chuva, pesada */
                {   0, 0.05f, 0.0f, 0 }, {  45, 0.10f, 0.0f, 0 }, {  95, 0.80f, 0.0f, 0 },
                { 130, 0.90f, 0.35f, 0 }, { 165, 1.0f, 1.0f, 0 }, { 195, 1.0f, 1.0f, 1 },
                { 225, 1.0f, 1.0f, 0 }, { 260, 0.95f, 0.30f, 0 }, { 300, 0.70f, 0.0f, 0 },
                { 340, 0.15f, 0.0f, 0 }, { 360, 0.05f, 0.0f, 0 } };
            float f = fmodf(t_clima, 360.0f);
            int i = 0;
            while(i < 9 && f > K[i + 1][0]) i++;
            float t = (f - K[i][0]) / (K[i + 1][0] - K[i][0]);
            t = t * t * (3 - 2 * t);
            an = K[i][1] + (K[i + 1][1] - K[i][1]) * t;
            ac = K[i][2] + (K[i + 1][2] - K[i][2]) * t;
            ap = K[i][3] + (K[i + 1][3] - K[i][3]) * t;
        }
    }
    nuvens += (an - nuvens) * limitar(0.35f * dt, 0, 1);
    chuva  += (ac - chuva) * limitar(0.25f * dt, 0, 1);
    pesada += (ap - pesada) * limitar(0.20f * dt, 0, 1);
    manha  += (am - manha) * limitar(0.30f * dt, 0, 1);
    tarde  += (at - tarde) * limitar(0.30f * dt, 0, 1);
    {   /* direção da luz: manhã (leste) -> entardecer (oeste, mais baixa) */
        float t = tarde * tarde * (3 - 2 * tarde);
        float hx = -0.669f + (-0.985f + 0.669f) * t, hz = -0.743f + (-0.174f + 0.743f) * t;   /* entardecer: oeste */
        float hl = sqrtf(hx * hx + hz * hz) + 1e-4f, yy = 0.75f - 0.35f * t, hh = sqrtf(1 - yy * yy);
        LUZ[0] = hx / hl * hh; LUZ[1] = yy; LUZ[2] = hz / hl * hh;
    }
    if(chuva > molhado) molhado += (chuva - molhado) * limitar(0.10f * dt, 0, 1);   /* molha em ~10 s */
    else molhado -= 0.008f * dt;                                                       /* seca em ~2 min */
    molhado = limitar(molhado, 0, 1);
    /* lâmina d'água na chuva pesada: aquaplanagem leve */
    fator_grip  = 1.0f - 0.32f * molhado - 0.10f * pesada;
    fator_freio = 1.0f - 0.28f * molhado - 0.08f * pesada;

    /* relâmpago + trovão (com atraso, como de verdade) */
    if(chuva > 0.7f && relampago < 0.01f && (h32((uint32_t)(t_clima * 60)) % (720 - (int)(500 * pesada))) == 0) {
        relampago = 1.0f;
        trovao_em = 0.6f + aleat((uint32_t)(t_clima * 7)) * 1.8f;
    }
    relampago -= relampago * limitar(7.0f * dt, 0, 1);
    if(trovao_em > 0) { trovao_em -= dt; if(trovao_em <= 0) som_trovao(); }

    brilho = 1.0f - 0.20f * nuvens - 0.14f * chuva - 0.16f * pesada + relampago * 0.6f;
    luz_dir = 0.5f * (1.0f - 0.75f * nuvens) + 0.16f * manha;      /* manhã: sol forte, mais contraste */
    luz_base = 0.55f + 0.16f * nuvens - 0.03f * manha;
    brilho += 0.10f * manha;
    brilho -= 0.12f * tarde;
    luz_dir += 0.10f * tarde;
    cor_ceu = cor_mistura(cor_mistura(COR_CEU_TOPO, 0xFF76808A, nuvens), 0xFF555D66, chuva * 0.6f);
    cor_neb = cor_mistura(cor_mistura(COR_NEBLINA, 0xFFA8AFB6, nuvens), 0xFF7E868E, chuva * 0.7f);
    cor_ceu = cor_mistura(cor_ceu, 0xFF3C434B, pesada * 0.75f);       /* temporal: quase noite */
    cor_ceu = cor_mistura(cor_ceu, 0xFF5C9CDE, manha * 0.8f);         /* manhã: azul limpo e claro */
    cor_neb = cor_mistura(cor_neb, 0xFFF2E4C6, manha * 0.7f);         /* bruma dourada no horizonte */
    cor_neb = cor_mistura(cor_neb, 0xFF5E666E, pesada * 0.7f);
    cor_neb = cor_mistura(cor_neb, 0xFFD8A08E, tarde * 0.85f);         /* entardecer: névoa rosada */
    cor_ceu = cor_mistura(cor_ceu, 0xFF3A3C7A, tarde * 0.85f);
    if(relampago > 0.01f) {
        cor_ceu = cor_mistura(cor_ceu, 0xFFF0F4FF, relampago * 0.7f);
        cor_neb = cor_mistura(cor_neb, 0xFFE8ECF4, relampago * 0.6f);
    }
    /* neblina: na chuva forte a visão cai para ~330 m (e desenhamos menos) */
    float ini = NEBLINA_INI - 50.0f * chuva - 45.0f * pesada,
          fim = NEBLINA_FIM + 70.0f * (1.0f - limitar(nuvens * 3.0f, 0, 1)) * (1.0f - chuva) - 80.0f * nuvens - 230.0f * chuva - 150.0f * pesada;
    dist_visao = limitar(fim + 10.0f, 170.0f, DIST_VISAO);
    if(fabsf(ini - neb_ini_ant) > 1.0f || fabsf(fim - neb_fim_ant) > 1.0f || cor_neb != neb_cor_ant) {
        pvr_fog_table_color(1.0f, ((cor_neb >> 16) & 0xFF) / 255.0f, ((cor_neb >> 8) & 0xFF) / 255.0f,
                            (cor_neb & 0xFF) / 255.0f);
        pvr_fog_table_linear(ini, fim);
        pvr_set_bg_color(((cor_neb >> 16) & 0xFF) / 255.0f, ((cor_neb >> 8) & 0xFF) / 255.0f, (cor_neb & 0xFF) / 255.0f);
        neb_ini_ant = ini; neb_fim_ant = fim; neb_cor_ant = cor_neb;
    }
}

/* -DPERFIL: cronometra as etapas do quadro e mostra na tela (ms x10) */
#ifdef PERFIL
static uint64_t pf_t, pf_ac[14];
static char pf_txt[64] = "";
#define PF(i) do { uint64_t _a = timer_us_gettime64(); pf_ac[i] += _a - pf_t; pf_t = _a; } while(0)
#else
#define PF(i) do { } while(0)
#endif

static const char *NOME_MODO[N_MODOS_CLIMA] = { "DINAMICO", "SOL", "NUBLADO", "CHUVA", "CHUVA PESADA", "ENSOLARADO", "ENTARDECER" };
static const char *texto_tempo(void) {
    if(pesada > 0.5f) return "CHUVA PESADA";
    if(manha > 0.5f) return "ENSOLARADO";
    if(tarde > 0.5f) return "ENTARDECER";
    if(chuva > 0.65f) return "CHUVA FORTE";
    if(chuva > 0.15f) return "GAROA";
    if(molhado > 0.3f) return "PISTA MOLHADA";
    if(nuvens > 0.5f) return "NUBLADO";
    return "SOL";
}

/* ======================================================================== */
/* CÉU, VOLANTE E TEXTOS                                                     */
/* ======================================================================== */

/* (cabeçalhos hdr_mundo, hdr_longe, hdr_2d, hdr_tr2d declarados junto das texturas dos carros) */

/* ======================================================================== */
/* SKYBOX PINTADA (estilo dos céus de corrida do Dreamcast): um panorama de */
/* 360° num cilindro bem longe, com degradê saturado, nuvens pintadas com    */
/* borda iluminada, a serra em duas camadas e a silhueta da cidade. Gerada   */
/* por código (2 versões: limpo e fechado). -DSKYBOX=0 volta ao céu antigo.  */
/* ======================================================================== */
/* O céu é PINTADO NO PC (gerar_ceu.py -> ceu.bin) e embutido no jogo:
 * 2 variantes (limpo / fechado) x 2 metades de 1024x256 RGB565 = panorama
 * de 2048 px, repetido 2x na volta. Nenhum custo de carregamento. */
#define SKY_W 1024
#define SKY_H 256
#define SKY_HOR 232                      /* linha do horizonte na textura */
__asm__(".section .rodata\n.balign 32\n.global _ceu_bin\n_ceu_bin:\n.incbin \"ceu.bin\"\n.text\n");
extern const uint8_t ceu_bin[];
/* variantes: 0 limpo (3 texturas, volta inteira, sol pintado), 1 fechado
 * (2 texturas repetidas 2x), 2 entardecer (3 texturas, sol baixo) */
#define SKY_NV 3
static const uint8_t SKY_NTEX[SKY_NV] = { 4, 4, 3 }, SKY_REP[SKY_NV] = { 1, 1, 1 }, SKY_T0[SKY_NV] = { 0, 4, 8 };
#define SKY_VQ_BYTES (2048 + (SKY_W / 2) * (SKY_H / 2))   /* codebook + índices: 1/7,75 do tamanho */
static pvr_poly_hdr_t hdr_sky[SKY_NV][4], hdr_sky_tr[SKY_NV][4];
static uint32_t sky_zen[SKY_NV] = { 0xFF1C4FB8, 0xFF58626E, 0xFF22306C };
static uint32_t vram_livre;
static void carregar_ceu(void) {
    for(int v = 0; v < SKY_NV; v++)
        for(int h = 0; h < SKY_NTEX[v]; h++) {
            /* compressão VQ do PowerVR (gerada no PC já no formato final) */
            pvr_ptr_t t = pvr_mem_malloc(SKY_VQ_BYTES);
            pvr_txr_load(ceu_bin + (SKY_T0[v] + h) * SKY_VQ_BYTES, t, SKY_VQ_BYTES);
            const int FMT = PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, FMT, SKY_W, SKY_H, t, PVR_FILTER_BILINEAR);
            cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
            cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
            pvr_poly_compile(&hdr_sky[v][h], &cxt);
            pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, FMT, SKY_W, SKY_H, t, PVR_FILTER_BILINEAR);
            cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
            cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
            pvr_poly_compile(&hdr_sky_tr[v][h], &cxt);
        }
    vram_livre = pvr_mem_available();
}

static uint32_t cor_mod(uint32_t a, uint32_t b) {      /* multiplica cor por cor */
    int r = ((a >> 16) & 255) * ((b >> 16) & 255) / 255, g = ((a >> 8) & 255) * ((b >> 8) & 255) / 255, bb = (a & 255) * (b & 255) / 255;
    return 0xFF000000 | r << 16 | g << 8 | bb;
}
/* quanto do céu FECHADO aparece (0..1): contínuo, para a transição suave */
static float sky_mistura(void) {
#ifdef DBG_MIX
    return DBG_MIX;                       /* teste: céu no meio da transição */
#endif
    float a = limitar((nuvens - 0.30f) / 0.50f, 0, 1), b = limitar(chuva / 0.4f, 0, 1);
    float m = a > b ? a : b;
    return m * m * (3 - 2 * m);
}
static float sky_tarde(void);
/* um panorama completo (variante v); alfa < 255 = camada translúcida por cima */
static void skybox_passe(int v, int alfa, float R) {
    float k = limitar(0.62f + 0.42f * brilho - 0.25f * chuva - 0.3f * pesada + relampago * 0.6f, 0.30f, 1.0f);
    if(v == 2) k = limitar(k + 0.25f, 0, 1);                           /* entardecer: a pintura já é escura */
    uint32_t tinta = cor_escala(0xFFFFFFFF, k);
    tinta = cor_mistura(tinta, 0xFFFFE2C0, manha * 0.35f);            /* manhã: luz dourada */
    uint32_t neb = cor_neb;                                            /* abaixo do horizonte = névoa EXATA */
    const float ytop = 0.55f * R;
    const float ybot = ytop - ytop * SKY_H / (float)SKY_HOR;
    const float ymeio = 0.02f * R;
    float vh = (float)SKY_HOR / SKY_H;
    const float ys[4] = { ybot, 0.0f, ymeio, ytop };
    const float vs[4] = { 1.0f, vh, vh * (ytop - ymeio) / ytop, 0.0f };
    const uint32_t cs[4] = { neb, neb, tinta, tinta };
    const uint32_t A = (uint32_t)alfa << 24;
    const int NS = 48;
    int ntex = SKY_NTEX[v], spt = NS / (ntex * SKY_REP[v]);          /* segmentos por textura */
    float giro = v == 1 ? t_clima * 0.0035f : 0.0f;                   /* fechado: nuvens andando devagar */
    float fh = sqrtf(cam_f[0] * cam_f[0] + cam_f[2] * cam_f[2]) + 1e-4f;
    float lh = sqrtf(LUZ[0] * LUZ[0] + LUZ[2] * LUZ[2]) + 1e-4f;
    float cla = (1.0f - nuvens * 0.75f - chuva * 0.2f) * (1.0f - tarde);   /* entardecer: sol já pintado */
    for(int tx = 0; tx < ntex; tx++) {
        pvr_prim(alfa < 255 ? &hdr_sky_tr[v][tx] : &hdr_sky[v][tx], sizeof(pvr_poly_hdr_t));
        for(int i = 0; i < NS; i++) {
            if((i / spt) % ntex != tx) continue;
            float a0 = i * 2 * PI_F / NS + giro, a1 = (i + 1) * 2 * PI_F / NS + giro;
            float c0 = fcos(a0), s0 = fsin(a0), c1 = fcos(a1), s1 = fsin(a1);
            float am = (a0 + a1) * 0.5f;
            if((fcos(am) * cam_f[0] + fsin(am) * cam_f[2]) / fh < -0.45f) continue;   /* atrás da câmera */
            float u0 = (i % spt) / (float)spt, u1 = ((i % spt) + 1) / (float)spt;
            for(int b = 0; b < 3; b++) {
                vvt_t q[4];
                float yy[4] = { ys[b], ys[b], ys[b + 1], ys[b + 1] };
                float cc[4] = { c0, c1, c1, c0 }, sn[4] = { s0, s1, s1, s0 }, uu[4] = { u0, u1, u1, u0 };
                float vv[4] = { vs[b], vs[b], vs[b + 1], vs[b + 1] };
                uint32_t co[4] = { cs[b], cs[b], cs[b + 1], cs[b + 1] };
                for(int k2 = 0; k2 < 4; k2++) {
                    if(b > 0 && cla > 0.01f) {                 /* lado do sol mais quente (não na névoa) */
                        float f = (cc[k2] * LUZ[0] + sn[k2] * LUZ[2]) / lh;
                        if(f > 0) co[k2] = cor_mistura(co[k2], cor_mod(co[k2], 0xFFFFDDB0), f * f * 0.55f * cla);
                        else co[k2] = cor_mistura(co[k2], cor_mod(co[k2], 0xFFD6E0F4), -f * 0.35f * cla);
                    }
                    vv_t pv;
                    para_vista(cam_p[0] + cc[k2] * R, cam_p[1] + yy[k2], cam_p[2] + sn[k2] * R, 0, &pv);
                    q[k2].x = pv.x; q[k2].y = pv.y; q[k2].z = pv.z;
                    q[k2].u = uu[k2]; q[k2].v = vv[k2]; q[k2].c = (co[k2] & 0xFFFFFF) | A;
                }
                poligono_tex(q, 4);
            }
        }
    }
    /* tampa do zênite */
    pvr_prim(alfa < 255 ? &hdr_tr2d_3d : &hdr_longe, sizeof(pvr_poly_hdr_t));
    /* tampa do zênite na cor MISTURADA pelo tempo do momento (limpo/fechado/entardecer):
     * com o céu em transição, a tampa azul do céu limpo aparecia no alto do céu cinza */
    uint32_t zmix;
    {
        float mz = sky_mistura(), wz = sky_tarde();
        zmix = cor_mistura(cor_mistura(sky_zen[0], sky_zen[2], wz), sky_zen[1], mz);
    }
    uint32_t cz = (cor_mod(zmix, tinta) & 0xFFFFFF) | A;
    vv_t topo;
    para_vista(cam_p[0], cam_p[1] + R * 1.4f, cam_p[2], cz, &topo);
    /* mesmos vértices da borda de cima do cilindro (com o giro): antes a tampa
     * tinha metade dos segmentos e as cordas deixavam uma fresta = linha tracejada no céu */
    for(int i = 0; i < NS; i++) {
        float a0 = i * 2 * PI_F / NS + giro, a1 = (i + 1) * 2 * PI_F / NS + giro;
        vv_t q[3];
        q[0] = topo;
        para_vista(cam_p[0] + fcos(a0) * R, cam_p[1] + ytop, cam_p[2] + fsin(a0) * R, cz, &q[1]);
        para_vista(cam_p[0] + fcos(a1) * R, cam_p[1] + ytop, cam_p[2] + fsin(a1) * R, cz, &q[2]);
        poligono(q, 3);
    }
}
#ifndef SKY_R
#define SKY_R 4000.0f
#endif
/* passo opaco: a variante que domina (limpo enquanto a mistura < 1) */
static float sky_tarde(void) { float t = limitar(tarde, 0, 1); return t * t * (3 - 2 * t); }
static void desenhar_skybox(void) {
    float m = sky_mistura(), wt = sky_tarde();
    skybox_passe(m >= 0.999f ? 1 : (wt > 0.5f ? 2 : 0), 255, SKY_R);
}
/* TOPO DO CÉU: a pintura das nuvens termina numa borda reta no alto do
 * cilindro; esta faixa (cor do zênite opaca em cima -> transparente embaixo)
 * dissolve as nuvens no céu de cima, sem corte */
static void ceu_topo_tr(void) {
    float m = sky_mistura(), wt = sky_tarde();
    float k = limitar(0.62f + 0.42f * brilho - 0.25f * chuva - 0.3f * pesada + relampago * 0.6f, 0.30f, 1.0f);
    uint32_t tinta = cor_mistura(cor_escala(0xFFFFFFFF, k), 0xFFFFE2C0, manha * 0.35f);
    uint32_t z0 = cor_mistura(sky_zen[0], sky_zen[2], wt);
    uint32_t zc = cor_mod(cor_mistura(z0, sky_zen[1], m), tinta) & 0xFFFFFF;
    const float R = SKY_R * 0.95f, ytop = 0.55f * SKY_R * 0.95f / 0.95f, ybaixo = ytop * 0.70f;
    const int NS = 48;
    pvr_prim(&hdr_tr2d_3d, sizeof(pvr_poly_hdr_t));
    float fh = sqrtf(cam_f[0] * cam_f[0] + cam_f[2] * cam_f[2]) + 1e-4f;
    for(int i = 0; i < NS; i++) {
        float a0 = i * 2 * PI_F / NS, a1 = (i + 1) * 2 * PI_F / NS, am = (a0 + a1) * 0.5f;
        if((fcos(am) * cam_f[0] + fsin(am) * cam_f[2]) / fh < -0.45f) continue;   /* atrás da câmera */
        float c0 = fcos(a0), s0 = fsin(a0), c1 = fcos(a1), s1 = fsin(a1);
        vv_t q[4];
        para_vista(cam_p[0] + c0 * R, cam_p[1] + ybaixo, cam_p[2] + s0 * R, zc, &q[0]);
        para_vista(cam_p[0] + c1 * R, cam_p[1] + ybaixo, cam_p[2] + s1 * R, zc, &q[1]);
        para_vista(cam_p[0] + c1 * R, cam_p[1] + ytop * 1.01f, cam_p[2] + s1 * R, zc | 0xFF000000, &q[2]);
        para_vista(cam_p[0] + c0 * R, cam_p[1] + ytop * 1.01f, cam_p[2] + s0 * R, zc | 0xFF000000, &q[3]);
        poligono(q, 4);
    }
    /* TETO inteiro na cor do zênite (misturada como o tempo do momento): com o
     * céu mudando (limpo por baixo + fechado translúcido por cima) a tampa azul
     * do céu limpo aparecia no alto do céu cinza */
    vv_t topo;
    para_vista(cam_p[0], cam_p[1] + R * 1.4f, cam_p[2], zc | 0xFF000000, &topo);
    for(int i = 0; i < NS; i++) {
        float a0 = i * 2 * PI_F / NS, a1 = (i + 1) * 2 * PI_F / NS;
        vv_t q[3];
        q[0] = topo;
        para_vista(cam_p[0] + fcos(a0) * R, cam_p[1] + ytop * 1.01f, cam_p[2] + fsin(a0) * R, zc | 0xFF000000, &q[1]);
        para_vista(cam_p[0] + fcos(a1) * R, cam_p[1] + ytop * 1.01f, cam_p[2] + fsin(a1) * R, zc | 0xFF000000, &q[2]);
        poligono(q, 3);
    }
}
/* NUVENS SOBRE A CABEÇA: o céu pintado (cilindro) só vai até ~29 graus; acima
 * disso as nuvens "acabavam" (bem visível no cockpit, que mostra até ~35 graus).
 * Um plano alto (1.500 m) com nuvens repetidas continua o céu até o zênite,
 * aparecendo onde a pintura termina (alfa sobe de 23 a 33 graus) e andando
 * com o vento. A cor e a cobertura seguem o tempo (limpo/fechado/entardecer). */
static pvr_poly_hdr_t hdr_teto;
static int teto_ok;
static void teto_tr(void) {
#ifdef SEM_TETO
    return;
#endif
    if(!teto_ok) {
        mip_4444 = 1;
        memcpy(tex_px, tex_bin + TEXB_TETO_NUVENS, 256 * 256 * 2);
        pvr_ptr_t t = tex_subir_mip(256);
        mip_4444 = 0;
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 256, 256, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
        pvr_poly_compile(&hdr_teto, &cxt);
        teto_ok = 1;
    }
    float m = sky_mistura(), wt = sky_tarde();
    float k = limitar(0.62f + 0.42f * brilho - 0.25f * chuva - 0.3f * pesada + relampago * 0.6f, 0.30f, 1.0f);
    uint32_t cor = cor_escala(0xFFFFFFFF, 0.78f + 0.22f * k);
    cor = cor_mistura(cor, 0xFF8C939C, m * 0.75f);                  /* céu fechado: nuvens cinza */
    cor = cor_mistura(cor, 0xFFFFB888, wt * 0.6f);                  /* entardecer: douradas */
    float cob = 0.75f + 0.25f * m;                                   /* fechado: mais cobertas */
    const float H = 1500.0f;
    static const float ELEV[8] = { 90, 70, 55, 45, 38, 33, 28, 23 };   /* anéis (graus acima do horizonte) */
    float rr[8], aa[8];
    for(int i = 0; i < 8; i++) {
        float e = ELEV[i] * (PI_F / 180.0f);
        rr[i] = i == 0 ? 0.0f : H * fcos(e) / fsin(e);
        aa[i] = limitar((ELEV[i] - 23.0f) / 10.0f, 0, 1) * cob;
    }
    const int NS = 24;
    float vento = t_clima * 4.0f;                                    /* m/s: as nuvens andam */
    float fh = sqrtf(cam_f[0] * cam_f[0] + cam_f[2] * cam_f[2]) + 1e-4f;
    pvr_prim(&hdr_teto, sizeof(hdr_teto));
    for(int j = 0; j < NS; j++) {
        float a0 = j * 2 * PI_F / NS, a1 = (j + 1) * 2 * PI_F / NS, am = (a0 + a1) * 0.5f;
        float vis = (fcos(am) * cam_f[0] + fsin(am) * cam_f[2]) / fh;
        if(vis < -0.55f && cam_f[1] < 0.6f) continue;               /* atrás da câmera (olhando para a frente) */
        float c0 = fcos(a0), s0 = fsin(a0), c1 = fcos(a1), s1 = fsin(a1);
        for(int i = 0; i < 7; i++) {
            float R0 = rr[i], R1 = rr[i + 1];
            float px[4] = { c0 * R0, c1 * R0, c1 * R1, c0 * R1 }, pz[4] = { s0 * R0, s1 * R0, s1 * R1, s0 * R1 };
            float al[4] = { aa[i], aa[i], aa[i + 1], aa[i + 1] };
            vvt_t q[4];
            for(int v = 0; v < 4; v++) {
                vv_t pv;
                para_vista(cam_p[0] + px[v], cam_p[1] + H, cam_p[2] + pz[v], 0, &pv);
                q[v].x = pv.x; q[v].y = pv.y; q[v].z = pv.z; q[v].o = 0;
                /* coordenadas do MUNDO: a camada fica parada no céu enquanto o carro anda */
                q[v].u = (cam_p[0] + px[v] + vento) * (1.0f / 900.0f);
                q[v].v = (cam_p[2] + pz[v] + vento * 0.4f) * (1.0f / 900.0f);
                q[v].c = ((uint32_t)(al[v] * 255) << 24) | (cor & 0xFFFFFF);
            }
            if(i == 0) { q[1] = q[0]; }                              /* centro: leque */
            poligono_tex(q, 4);
        }
    }
}
/* passo translúcido: o fechado aparecendo por cima, um pouco mais perto */
static void desenhar_skybox_tr(void) {
    ceu_topo_tr();
    teto_tr();
    float m = sky_mistura(), wt = sky_tarde();
    if(m < 0.999f) {                     /* limpo <-> entardecer */
        float a = wt > 0.5f ? 1.0f - wt : wt;
        if(a > 0.004f) skybox_passe(wt > 0.5f ? 0 : 2, (int)(a * 255), SKY_R * 0.985f);
    }
    if(m <= 0.001f || m >= 0.999f) return;
    skybox_passe(1, (int)(m * 255), SKY_R * 0.97f);
}

static void desenhar_ceu(void);
static void desenhar_fundo_ceu(void) {
#if SKYBOX
    desenhar_skybox();
#else
    desenhar_ceu();
#endif
}

static void desenhar_ceu(void) {
    /* altura do horizonte na tela: projeta um ponto bem longe, na horizontal */
    float h[3] = { cam_p[0] + cam_f[0] * 10000, cam_p[1], cam_p[2] + cam_f[2] * 10000 };
    vv_t p;
    para_vista(h[0], h[1], h[2], 0, &p);
    float yh = CY - FOCO * p.y / -p.z;
    yh = limitar(yh, -100, 560);
    const float z = 0.00012f;    /* atrás de tudo (morros a 3 km têm ~0.0003) */
    pvr_prim(&hdr_2d, sizeof(hdr_2d));
    /* zênite mais fundo, céu, faixa clara perto do horizonte e a névoa */
    uint32_t zen = cor_mistura(cor_ceu, 0xFF1E4A8C, 0.30f * (1.0f - nuvens * 0.8f) * (1.0f - chuva));
    uint32_t perto_h = cor_mistura(cor_ceu, cor_neb, 0.55f);
    enviar_2d(0, yh - 110, z, perto_h, 0);
    enviar_2d(640, yh - 110, z, perto_h, 0);
    enviar_2d(0, yh, z, cor_neb, 0);
    enviar_2d(640, yh, z, cor_neb, 1);
    enviar_2d(0, yh - 300, z, cor_ceu, 0);
    enviar_2d(640, yh - 300, z, cor_ceu, 0);
    enviar_2d(0, yh - 110, z, perto_h, 0);
    enviar_2d(640, yh - 110, z, perto_h, 1);
    if(yh - 300 > 0) {
        enviar_2d(0, yh - 560, z, zen, 0);
        enviar_2d(640, yh - 560, z, zen, 0);
        enviar_2d(0, yh - 300, z, cor_ceu, 0);
        enviar_2d(640, yh - 300, z, cor_ceu, 1);
    }
    enviar_2d(0, yh, z, cor_neb, 0);
    enviar_2d(640, yh, z, cor_neb, 0);
    enviar_2d(0, 480, z, cor_neb, 0);
    enviar_2d(640, 480, z, cor_neb, 1);
}

/* SOL + RAIOS DE LUZ (clima ENSOLARADO; um pouco também no SOL).
 * O sol fica baixo (manhã), na mesma direção horizontal da luz da cena.
 * Brilho e raios são desenhados na profundidade do céu, na lista
 * translúcida: o PowerVR ainda testa a profundidade contra o que é opaco,
 * então morros, prédios e árvores na frente "cortam" o brilho — parecem
 * raios passando por trás deles. Um véu de luz bem fraco e um reflexo de
 * lente ficam por cima da cena. */
static void tr_vert(float x, float y, float z, uint32_t c, int fim) {
    pvr_vertex_t v;
    v.flags = fim ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v.x = x * XS; v.y = y; v.z = z; v.u = v.v = 0; v.argb = c; v.oargb = 0;
    pvr_prim(&v, sizeof(v));
}
static const float CIRC12[12][2] = {
    { 1.000f, 0.000f }, { 0.866f, 0.500f }, { 0.500f, 0.866f }, { 0.000f, 1.000f },
    { -0.500f, 0.866f }, { -0.866f, 0.500f }, { -1.000f, 0.000f }, { -0.866f, -0.500f },
    { -0.500f, -0.866f }, { 0.000f, -1.000f }, { 0.500f, -0.866f }, { 0.866f, -0.500f } };
static void tr_brilho(float x, float y, float r, float z, uint32_t centro, uint32_t borda) {
    for(int i = 0; i < 12; i++) {
        int j = (i + 1) % 12;
        tr_vert(x, y, z, centro, 0);
        tr_vert(x + CIRC12[i][0] * r, y + CIRC12[i][1] * r, z, borda, 0);
        tr_vert(x + CIRC12[j][0] * r, y + CIRC12[j][1] * r, z, borda, 1);
    }
}
/* raios fixos (senos prontos: sinf é caro no SH4) */
static const float RAIO_ANG[11][2] = {
    { 1.000f, 0.000f }, { 0.743f, 0.669f }, { 0.208f, 0.978f }, { -0.407f, 0.914f },
    { -0.866f, 0.500f }, { -0.995f, -0.105f }, { -0.743f, -0.669f }, { -0.208f, -0.978f },
    { 0.407f, -0.914f }, { 0.866f, -0.500f }, { 0.978f, 0.208f } };
static void desenhar_sol(void) {
    float forca = limitar(manha + tarde * 0.8f + 0.35f * (1.0f - nuvens * 1.6f), 0, 1) * (1.0f - chuva);
    if(forca < 0.03f) return;
    /* direção do sol: horizontal = da luz, uns 16 graus acima do horizonte */
    float hx = LUZ[0], hz = LUZ[2], hl = sqrtf(hx * hx + hz * hz);
    float hs = 0.96f + 0.02f * tarde, vs = 0.28f - 0.084f * tarde;    /* bate com o sol pintado no céu */
    float d[3] = { hx / hl * hs, vs, hz / hl * hs };
    vv_t p;
    para_vista(cam_p[0] + d[0] * 5000, cam_p[1] + d[1] * 5000, cam_p[2] + d[2] * 5000, 0, &p);
    if(p.z > -50.0f) return;                                     /* atrás da câmera */
    float sx = CX + FOCO * p.x / -p.z, sy = CY - FOCO * p.y / -p.z;
    if(sx < -500 || sx > 1140 || sy < -400 || sy > 700) return;
    const float zc = SKYBOX ? 0.00045f : 0.00013f;              /* logo à frente do céu (skybox a 4 km) */
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    uint32_t a1 = (uint32_t)(150 * forca * (1.0f - 0.6f * tarde)), a2 = (uint32_t)(70 * forca);
    tr_brilho(sx, sy, 170, zc, (a2 << 24) | 0xFFF4D8, 0x00FFE8B0);  /* halo grande */
    tr_brilho(sx, sy, 48, zc, (a1 << 24) | 0xFFFCF0, 0x00FFF0C8);   /* halo forte */
    if(tarde < 0.95f)   /* no entardecer o disco está PINTADO no céu, atrás das faixas de nuvem */
        tr_brilho(sx, sy, 15, zc, ((uint32_t)(255 * (1.0f - tarde)) << 24) | 0xFFFFF8,
                  ((uint32_t)(230 * forca * (1.0f - tarde)) << 24) | 0xFFFFF0);   /* disco */
    /* raios: cunhas que saem do sol e "respiram" devagar */
    float t = t_clima * 0.4f;
    for(int i = 0; i < 11; i++) {
        float ca = RAIO_ANG[i][0], sa = RAIO_ANG[i][1];
        float larg = 0.05f + 0.035f * (i % 3);
        float comp = 480.0f + 60.0f * ((i * 7) % 5);
        float fase = t + i * 1.7f; fase -= (int)(fase / 6.2832f) * 6.2832f;
        float pulso = 0.6f + 0.4f * (fase < 3.1416f ? fase / 3.1416f : 2.0f - fase / 3.1416f);   /* onda triangular */
        uint32_t a = (uint32_t)(34 * forca * pulso * (1.0f + 0.5f * tarde));      /* entardecer: raios mais fortes */
        tr_vert(sx, sy, zc, (a << 24) | 0xFFF0C8, 0);
        tr_vert(sx + (ca + sa * larg) * comp, sy + (sa - ca * larg) * comp, zc, 0x00FFF0C8, 0);   /* ordem = mesmo giro do halo */
        tr_vert(sx + (ca - sa * larg) * comp, sy + (sa + ca * larg) * comp, zc, 0x00FFF0C8, 1);
    }
    /* véu de luz por cima da cena + reflexo de lente (só com o sol na tela) */
    float na_tela = 1.0f - limitar((fabsf(sx - 320) - 300) / 200.0f, 0, 1);
    na_tela *= 1.0f - limitar((fabsf(sy - 200) - 220) / 200.0f, 0, 1);
    float k = forca * na_tela;
    if(k < 0.05f) return;
    tr_brilho(sx, sy, 260, 40.0f, ((uint32_t)(26 * k) << 24) | 0xFFF2D0, 0x00FFF2D0);
    /* feixes de luz descendo sobre a pista (luz atravessando a bruma da manhã) */
    static const float FX_ANG[6][2] = { { -0.643f, 0.766f }, { -0.342f, 0.940f }, { -0.087f, 0.996f },
                                        { 0.174f, 0.985f }, { 0.423f, 0.906f }, { 0.707f, 0.707f } };
    for(int i = 0; i < 6; i++) {
        float ca = FX_ANG[i][0], sa = FX_ANG[i][1];
        float larg = 0.06f + 0.04f * ((i * 5) % 3), comp = 640.0f;
        float fase = t * 0.7f + i * 2.3f; fase -= (int)(fase / 6.2832f) * 6.2832f;
        float pulso = 0.5f + 0.5f * (fase < 3.1416f ? fase / 3.1416f : 2.0f - fase / 3.1416f);
        uint32_t a = (uint32_t)((9 + 12 * tarde) * k * pulso);              /* feixes atravessando as nuvens */
        tr_vert(sx, sy, 40.0f, (a << 24) | 0xFFF4D8, 0);
        tr_vert(sx + (ca + sa * larg) * comp, sy + (sa - ca * larg) * comp, 40.0f, 0x00FFF4D8, 0);   /* ordem = mesmo giro do halo */
        tr_vert(sx + (ca - sa * larg) * comp, sy + (sa + ca * larg) * comp, 40.0f, 0x00FFF4D8, 1);
    }
    static const float FL_T[4] = { 0.45f, 0.75f, 1.25f, 1.6f };
    static const float FL_R[4] = { 10, 22, 14, 34 };
    static const uint32_t FL_C[4] = { 0xA0FFE0, 0xFFC890, 0x90D0FF, 0xC8B0FF };
    for(int i = 0; i < 4; i++) {
        float fx = sx + (320 - sx) * FL_T[i], fy = sy + (240 - sy) * FL_T[i];
        tr_brilho(fx, fy, FL_R[i], 40.0f, ((uint32_t)(22 * k) << 24) | FL_C[i], ((uint32_t)(6 * k) << 24) | FL_C[i]);
    }
}

/* Volante 2D na visão de cockpit, no estilo do fim dos anos 90: corpo de
 * carbono em "borboleta", manoplas emborrachadas com sulcos para os dedos,
 * luvas do piloto, borboletas de câmbio atrás, fileira de luzes de troca no
 * topo, visor de cristal líquido (marcha grande + velocidade, em dígitos de
 * 7 segmentos que giram junto), botões coloridos e dois seletores giratórios.
 * Tudo em polígonos — gira inteiro com o esterço. */
static float vol_cs, vol_sn;
static const float VOL_CX = 320, VOL_CY = 408;   /* alto o bastante para mostrar luvas e antebraços */

static void vol_pt(float px, float py, float z, uint32_t cor, int fim) {
    enviar_2d(VOL_CX + px * vol_cs - py * vol_sn, VOL_CY + px * vol_sn + py * vol_cs, z, cor, fim);
}
/* polígono convexo (ordem em volta) -> tira em zigue-zague */
static void vol_poli(const float (*p)[2], int n, float z, uint32_t cor) {
    int i = 0, j = n - 1, k = 0;
    while(i <= j) {
        int idx = (k & 1) ? j-- : i++;
        vol_pt(p[idx][0], p[idx][1], z, cor, i > j);
        k++;
    }
}
static void vol_ret(float x0, float y0, float x1, float y1, float z, uint32_t cor) {
    float p[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    vol_poli(p, 4, z, cor);
}
/* octógono; usa uma tabela fixa (sinf/cosf da libm custam caro no SH4) */
static const float CIRC8[8][2] = {
    {  0.9239f,  0.3827f }, {  0.3827f,  0.9239f }, { -0.3827f,  0.9239f }, { -0.9239f,  0.3827f },
    { -0.9239f, -0.3827f }, { -0.3827f, -0.9239f }, {  0.3827f, -0.9239f }, {  0.9239f, -0.3827f } };
static void vol_circ(float x, float y, float r, float z, uint32_t cor) {
    float p[8][2];
    for(int i = 0; i < 8; i++) { p[i][0] = x + r * CIRC8[i][0]; p[i][1] = y + r * CIRC8[i][1]; }
    vol_poli(p, 8, z, cor);
}
/* dígito de 7 segmentos na caixa (x, y, larg, alt); apagados = "fantasma" do LCD */
static void vol_digito(int d, float x, float y, float w, float h, float t, float z, uint32_t aceso, uint32_t apagado) {
    static const uint8_t SEG[11] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x54 /* n */ };
    uint8_t m = d < 0 ? 0 : SEG[d > 10 ? 10 : d];
    float hm = h * 0.5f;
    float R[7][4] = {
        { x + t, y,          x + w - t, y + t },          /* a */
        { x + w - t, y + t,  x + w, y + hm },             /* b */
        { x + w - t, y + hm, x + w, y + h - t },          /* c */
        { x + t, y + h - t,  x + w - t, y + h },          /* d */
        { x, y + hm,         x + t, y + h - t },          /* e */
        { x, y + t,          x + t, y + hm },             /* f */
        { x + t, y + hm - t * 0.5f, x + w - t, y + hm + t * 0.5f }, /* g */
    };
    for(int s = 0; s < 7; s++)
        vol_ret(R[s][0], R[s][1], R[s][2], R[s][3], z, (m >> s) & 1 ? aceso : apagado);
}

#ifndef VOLANTE_ANTIGO
/* VOLANTE PINTADO (arte do cockpit feita no PC, volante.py): carbono, borracha,
 * botões, seletores, visor e as luvas numa textura VQ 512x512 com alfa de 1 bit
 * (lista de recorte). Aqui (lista opaca) só vai o que muda: LEDs acesos,
 * dígitos do visor, punhos na cor da equipe e a faixa do macacão. */
/* duas texturas VQ 512x512 (cada uma com seu próprio "livro de códigos":
 * mais fidelidade): volante+luvas, e tecido/retrovisor/lateral do cockpit */
static pvr_poly_hdr_t hdr_volante, hdr_cockpit;
static int volante_tex_ok;
static int vq_pt(pvr_poly_hdr_t *h, int ofs) {
    int bytes = 2048 + 256 * 256;
    pvr_ptr_t t = pvr_mem_malloc(bytes);
    if(!t) return 0;
    pvr_txr_load(tex_bin + ofs, t, bytes);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                     512, 512, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    cxt.txr.env = PVR_TXRENV_MODULATE;
    pvr_poly_compile(h, &cxt);
    return 1;
}
static void criar_volante_tex(void) {
#ifdef PINTURA_STEWART
    volante_tex_ok = vq_pt(&hdr_volante, TEXB_VOLANTE_SF3) && vq_pt(&hdr_cockpit, TEXB_COCKPIT_SF3);
#else
    volante_tex_ok = vq_pt(&hdr_volante, TEXB_VOLANTE) && vq_pt(&hdr_cockpit, TEXB_COCKPIT);
#endif
}
static inline void vt_vert(float x, float y, float z, float u, float v, uint32_t c, int fim) {
    pvr_vertex_t vx;
    vx.flags = fim ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vx.x = x * XS; vx.y = y; vx.z = z; vx.u = u; vx.v = v; vx.argb = c; vx.oargb = 0;
    pvr_prim(&vx, sizeof(vx));
}
/* quad da tela com uma região da textura (x0 > x1 espelha) */
static void vt_quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z, uint32_t c) {
    vt_vert(x0, y1, z, u0, v1, c, 0);
    vt_vert(x0, y0, z, u0, v0, c, 0);
    vt_vert(x1, y1, z, u1, v1, c, 0);
    vt_vert(x1, y0, z, u1, v0, c, 1);
}
static inline void vol_tex(float px, float py, float z, float u, float v, uint32_t c, int fim) {
    vt_vert(VOL_CX + px * vol_cs - py * vol_sn, VOL_CY + px * vol_sn + py * vol_cs, z, u, v, c, fim);
}
/* antebraço: do pulso (atrás do punho da luva, gira com o volante) até o
 * cotovelo, que acompanha o pulso (o braço inteiro se move) */
static void vol_braco(int l, float *px, float *py, float *ex, float *ey, float *nx, float *ny) {
    float wx = l * 110.0f, wy = 24.0f;
    *px = VOL_CX + wx * vol_cs - wy * vol_sn; *py = VOL_CY + wx * vol_sn + wy * vol_cs;
    float dxp = *px - (VOL_CX + wx), dyp = *py - (VOL_CY + wy);
    *ex = VOL_CX + l * 168.0f + dxp * 0.60f; *ey = VOL_CY + 160.0f + dyp * 0.45f;
    float dx = *ex - *px, dy = *ey - *py, dl = sqrtf(dx * dx + dy * dy);
    *nx = -dy / dl; *ny = dx / dl;
    if(*nx * l < 0) { *nx = -*nx; *ny = -*ny; }         /* n aponta para o lado de FORA */
}
/* a arte 2D acompanha a luz da cena: mais escura na sombra/nublado, dourada no entardecer */
static uint32_t luz_cockpit(void) {
    uint32_t c = cor_mistura(0xFFFFFFFF, 0xFFFFC890, limitar(tarde, 0, 1) * 0.55f);
    return cor_escala(c, limitar(0.60f + 0.42f * brilho, 0.45f, 1.0f));
}
#define TX(p) ((p) / 512.0f)
/* COCKPIT (lista de recorte): vão, laterais da carroceria, retrovisores,
 * braços e o volante com as luvas (que gira) */
static void desenhar_volante_pt(uint32_t cor_punho) {
    (void)cor_punho;
    if(!volante_tex_ok) return;
    uint32_t lc = luz_cockpit();
    pvr_prim(&hdr_cockpit, sizeof(hdr_cockpit));
    /* vão do cockpit + borda da frente */
    vt_quad(150, 320, 490, 480, TX(0.5f), TX(256.5f), TX(511.5f), TX(511.5f), 44.0f, lc);
    /* laterais da carroceria (a direita é a esquerda espelhada) */
    vt_quad(0, 330, 235, 480, TX(0.5f), TX(0.5f), TX(255.5f), TX(255.5f), 46.0f, lc);
    vt_quad(640, 330, 405, 480, TX(0.5f), TX(0.5f), TX(255.5f), TX(255.5f), 46.0f, lc);
    /* braços: manga com a faixa do lado de fora */
    for(int l = -1; l <= 1; l += 2) {
        float px, py, ex, ey, nx, ny;
        vol_braco(l, &px, &py, &ex, &ey, &nx, &ny);
        const float w0 = 25.0f, w1 = 46.0f;
        const float U0 = TX(256.5f), U1 = TX(511.5f), VF = TX(128.5f), VD = TX(255.5f);
        vt_vert(px + nx * w0, py + ny * w0, 48.0f, U0, VF, lc, 0);
        vt_vert(ex + nx * w1, ey + ny * w1, 48.0f, U1, VF, lc, 0);
        vt_vert(px - nx * w0, py - ny * w0, 48.0f, U0, VD, lc, 0);
        vt_vert(ex - nx * w1, ey - ny * w1, 48.0f, U1, VD, lc, 1);
    }
    /* volante + luvas: um quad girando (arte de x -150..150, y -80..80) */
    pvr_prim(&hdr_volante, sizeof(hdr_volante));
    vol_tex(-150, 80, 50.0f, 0, 1, lc, 0);
    vol_tex(-150, -80, 50.0f, 0, 0, lc, 0);
    vol_tex(150, 80, 50.0f, 1, 1, lc, 0);
    vol_tex(150, -80, 50.0f, 1, 0, lc, 1);
}
/* carenagem dos retrovisores (miolo transparente: o vidro aparece por trás) */
static void desenhar_espelhos_pt(void) {
    if(!volante_tex_ok) return;
    uint32_t lc = luz_cockpit();
    pvr_prim(&hdr_cockpit, sizeof(hdr_cockpit));
    vt_quad(10, 280, 150, 360, TX(256.5f), TX(0.5f), TX(511.5f), TX(127.5f), 46.5f, lc);
    vt_quad(630, 280, 490, 360, TX(256.5f), TX(0.5f), TX(511.5f), TX(127.5f), 46.5f, lc);
}
/* sombra dos braços sobre a carroceria e o vão (lista translúcida, entre os dois) */
static void desenhar_sombra_bracos(void) {
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    for(int l = -1; l <= 1; l += 2) {
        float px, py, ex, ey, nx, ny;
        vol_braco(l, &px, &py, &ex, &ey, &nx, &ny);
        float ox = 6.0f * l, oy = 7.0f;
        const float w0 = 30.0f, w1 = 56.0f;
        enviar_2d(px + nx * w0 + ox, py + ny * w0 + oy, 47.0f, 0x00000000, 0);
        enviar_2d(ex + nx * w1 + ox, ey + ny * w1 + oy, 47.0f, 0x00000000, 0);
        enviar_2d(px + ox, py + oy, 47.0f, 0x70000000, 0);
        enviar_2d(ex + ox, ey + oy, 47.0f, 0x70000000, 0);
        enviar_2d(px - nx * w0 + ox, py - ny * w0 + oy, 47.0f, 0x00000000, 0);
        enviar_2d(ex - nx * w1 + ox, ey - ny * w1 + oy, 47.0f, 0x00000000, 1);
    }
}
/* brilho do vidro dos retrovisores (lista translúcida, por cima da imagem) */
static void desenhar_brilho_espelhos(void) {
    static const float M[2][4] = { { 30, 296, 106, 321 }, { 534, 296, 610, 321 } };
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    for(int i = 0; i < 2; i++) {
        float x0 = M[i][0], y0 = M[i][1], x1 = M[i][2], y1 = M[i][3];
        uint32_t a = (uint32_t)(26 + 20 * brilho) << 24 | 0xF0F6FF;
        /* reflexo diagonal no vidro + borda de cima mais clara */
        enviar_2d(x0, y1, 43.0f, 0x00F0F6FF, 0); enviar_2d(x0, y0, 43.0f, a, 0);
        enviar_2d(x0 + (x1 - x0) * 0.45f, y1, 43.0f, 0x00F0F6FF, 0); enviar_2d(x0 + (x1 - x0) * 0.62f, y0, 43.0f, 0x00F0F6FF, 1);
    }
}
static void desenhar_volante(float giro, int marcha, float kmh, float rpm, uint32_t cor_luva, uint32_t cor_punho,
                             uint32_t cor_faixa) {
    (void)cor_luva; (void)cor_punho; (void)cor_faixa;
    vol_cs = cosf(giro); vol_sn = sinf(giro);
    const float z = 50.0f;
    pvr_prim(&hdr_2d, sizeof(hdr_2d));
    /* luzes de troca ACESAS (as apagadas estão pintadas): verde, vermelho, azul */
    int acesas = (int)((rpm - 0.55f) / 0.43f * 10.0f + 0.5f);
    int pisca = rpm > 0.97f && ((int)(t_clima * 12.0f) & 1);
    for(int i = 0; i < 10 && !pisca; i++) {
        if(i >= acesas) break;
        uint32_t c = i < 3 ? 0xFF40FF60 : i < 7 ? 0xFFFF3A28 : 0xFF4A80FF;
        float lx = -50 + i * 11.1f, ly = -51;
        vol_circ(lx, ly, 3.9f, z + 0.6f, cor_escala(c, 0.8f));
        vol_circ(lx, ly, 2.6f, z + 0.65f, c);
        vol_circ(lx - 0.9f, ly - 0.9f, 1.0f, z + 0.7f, cor_mistura(c, 0xFFFFFFFF, 0.7f));
    }
    /* visor: marcha grande + velocidade */
    const uint32_t SEG_ON = 0xFF16221A, SEG_OFF = 0xFF6E8862;
    vol_digito(marcha <= 0 ? 10 : marcha, -28, -32, 14, 22, 3, z + 0.7f, SEG_ON, SEG_OFF);
    int v = (int)(kmh + 0.5f); if(v > 999) v = 999;
    for(int k = 0; k < 3; k++) {
        int dg = k == 0 ? v / 100 : k == 1 ? (v / 10) % 10 : v % 10;
        if(k == 0 && v < 100) dg = -1;
        if(k == 1 && v < 10) dg = -1;
        vol_digito(dg, 2 + k * 9.5f, -31, 7, 12, 1.7f, z + 0.7f, SEG_ON, SEG_OFF);
    }
    for(int k = 0; k < 6; k++)        /* barrinha de rotação embaixo da velocidade */
        vol_ret(2 + k * 4.8f, -14, 5.5f + k * 4.8f, -11, z + 0.7f, rpm > 0.55f + k * 0.075f ? SEG_ON : SEG_OFF);
}
#else
static void desenhar_volante_pt(uint32_t c) { (void)c; }
static void desenhar_espelhos_pt(void) {}
static void criar_volante_tex(void) {}
static void desenhar_volante(float giro, int marcha, float kmh, float rpm, uint32_t cor_luva, uint32_t cor_punho,
                             uint32_t cor_faixa) {
    vol_cs = cosf(giro); vol_sn = sinf(giro);
    const float z = 50.0f;
    pvr_prim(&hdr_2d, sizeof(hdr_2d));

    /* ANTEBRAÇOS do piloto (macacão na cor da equipe): saem dos cantos de
     * baixo da tela (cotovelos abertos, fora da tela) e chegam ao punho da
     * luva, que gira com o volante. Ficam ATRÁS do volante. */
    for(int l = -1; l <= 1; l += 2) {
        float wx = l * 176.0f, wy = 48.0f;               /* punho, no lado de fora da luva */
        float px = VOL_CX + wx * vol_cs - wy * vol_sn, py = VOL_CY + wx * vol_sn + wy * vol_cs;
        float ex = VOL_CX + l * 410.0f, ey = VOL_CY + 150.0f;     /* cotovelo aberto, fora da tela */
        float dx = ex - px, dy = ey - py, dl = sqrtf(dx * dx + dy * dy);
        float nx = dy / dl * l, ny = -dx / dl * l;                /* aponta para cima */
        float w0 = 27.0f, w1 = 58.0f;
        uint32_t c_luz = cor_escala(cor_punho, 1.05f), c_sombra = cor_escala(cor_punho, 0.62f);
        uint32_t c_meio = cor_escala(cor_punho, 0.85f);
        /* lado de cima (iluminado) e de baixo (sombra) do braço */
        enviar_2d(px + nx * w0, py + ny * w0, z - 2.0f, c_luz, 0);
        enviar_2d(ex + nx * w1, ey + ny * w1, z - 2.0f, c_luz, 0);
        enviar_2d(px, py, z - 2.0f, c_meio, 0);
        enviar_2d(ex, ey, z - 2.0f, c_meio, 0);
        enviar_2d(px - nx * w0, py - ny * w0, z - 2.0f, c_sombra, 0);
        enviar_2d(ex - nx * w1, ey - ny * w1, z - 2.0f, c_sombra, 1);
        /* faixa lateral do macacão (cor secundária) */
        float f0 = 0.35f, f1 = 0.60f;
        enviar_2d(px + nx * w0 * f1, py + ny * w0 * f1, z - 1.9f, cor_faixa, 0);
        enviar_2d(ex + nx * w1 * f1, ey + ny * w1 * f1, z - 1.9f, cor_faixa, 0);
        enviar_2d(px + nx * w0 * f0, py + ny * w0 * f0, z - 1.9f, cor_faixa, 0);
        enviar_2d(ex + nx * w1 * f0, ey + ny * w1 * f0, z - 1.9f, cor_faixa, 1);
    }

    /* borboletas de câmbio (atrás do corpo, aparecem acima das manoplas) */
    for(int l = -1; l <= 1; l += 2) {
        float p[4][2] = { { l * 92.0f, -96 }, { l * 150.0f, -84 }, { l * 146.0f, -40 }, { l * 96.0f, -60 } };
        vol_poli(p, 4, z - 0.5f, 0xFF4A4C52);
    }

    /* corpo de carbono */
    static const float CORPO[10][2] = { {-150,-24},{-126,-70},{-62,-84},{62,-84},{126,-70},{150,-24},
                                        {150,58},{90,70},{-90,70},{-150,58} };
    vol_poli(CORPO, 10, z, 0xFF1C1D21);
    /* filete mais claro no alto (reflexo do verniz do carbono) */
    vol_ret(-60, -84, 60, -79, z + 0.2f, 0xFF3A3C44);
    /* "trama" do carbono: faixas diagonais bem sutis */
    for(int i = 0; i < 5; i++) {
        float x0 = -110 + i * 48;
        float p[4][2] = { { x0, 60 }, { x0 + 10, 60 }, { x0 + 40, -20 }, { x0 + 30, -20 } };
        vol_poli(p, 4, z + 0.1f, 0xFF212227);
    }

    /* manoplas emborrachadas + sulcos dos dedos */
    for(int l = -1; l <= 1; l += 2) {
        float x0 = l > 0 ? 146 : -188, x1 = l > 0 ? 188 : -146;
        vol_ret(x0, -34, x1, 64, z + 0.3f, 0xFF35302E);
        vol_ret(x0, -34, x1, -28, z + 0.4f, 0xFF4A4440);
        for(int k = 0; k < 4; k++)
            vol_ret(x0, 4 + k * 14, x1, 8 + k * 14, z + 0.4f, 0xFF221E1D);
    }

    /* luzes de troca de marcha: 3 verdes, 3 vermelhas, 3 azuis (piscam no limite) */
    vol_ret(-78, -80, 78, -64, z + 0.5f, 0xFF0C0C0E);
    int acesas = (int)((rpm - 0.55f) / 0.43f * 9.0f + 0.5f);
    int pisca = rpm > 0.97f && ((int)(t_clima * 12.0f) & 1);
    for(int i = 0; i < 9; i++) {
        uint32_t c = i < 3 ? 0xFF30E050 : i < 6 ? 0xFFFF3020 : 0xFF3A70FF;
        if(i >= acesas || pisca) c = cor_escala(c, 0.22f);
        vol_circ(-64 + i * 16, -72, 5.0f, z + 0.6f, c);
    }

    /* visor LCD: moldura, fundo esverdeado, marcha grande e velocidade */
    vol_ret(-58, -58, 58, -14, z + 0.5f, 0xFF0A0A0C);
    vol_ret(-54, -55, 54, -17, z + 0.6f, 0xFF7C9470);
    const uint32_t SEG_ON = 0xFF16221A, SEG_OFF = 0xFF71886A;
    vol_digito(marcha <= 0 ? 10 : marcha, -12, -52, 22, 32, 4, z + 0.7f, SEG_ON, SEG_OFF);
    int v = (int)(kmh + 0.5f); if(v > 999) v = 999;
    for(int k = 0; k < 3; k++) {
        int dg = k == 0 ? v / 100 : k == 1 ? (v / 10) % 10 : v % 10;
        if(k == 0 && v < 100) dg = -1;
        if(k == 1 && v < 10) dg = -1;
        vol_digito(dg, 18 + k * 11, -44, 8, 14, 2, z + 0.7f, SEG_ON, SEG_OFF);
    }
    /* barrinha de rotação no LCD (lado esquerdo) */
    for(int k = 0; k < 6; k++)
        vol_ret(-48 + k * 5.5f, -24 - k * 2.5f, -44 + k * 5.5f, -21, z + 0.7f, rpm > 0.55f + k * 0.075f ? SEG_ON : SEG_OFF);

    /* botões coloridos (neutro, limitador, rádio, bebida, confirma) */
    static const uint32_t BOT[5] = { 0xFFD82020, 0xFFF0C010, 0xFF2050E0, 0xFF20A040, 0xFFE8E8E8 };
    static const float BX[5] = { -100, -70, 70, 100, -40 };
    static const float BY[5] = { -38, -50, -50, -38, 2 };
    for(int i = 0; i < 5; i++) {
        vol_circ(BX[i], BY[i], 9.0f, z + 0.5f, 0xFF0C0C0E);
        vol_circ(BX[i], BY[i], 6.5f, z + 0.6f, BOT[i]);
        vol_circ(BX[i] - 1.5f, BY[i] - 2.0f, 2.5f, z + 0.7f, cor_mistura(BOT[i], 0xFFFFFFFF, 0.5f));
    }
    /* dois seletores giratórios (com o traço indicador) */
    for(int l = -1; l <= 1; l += 2) {
        float x = l * 62.0f, y = 22.0f;
        vol_circ(x, y, 15.0f, z + 0.5f, 0xFF0C0C0E);
        vol_circ(x, y, 12.0f, z + 0.6f, 0xFF4C4E56);
        vol_ret(x - 1.5f, y - 11, x + 1.5f, y - 3, z + 0.7f, 0xFFF0F0F0);
        static const float MK[6][2] = { { -0.86f, -0.51f }, { -0.61f, -0.79f }, { -0.22f, -0.97f },
                                        {  0.22f, -0.97f }, {  0.61f, -0.79f }, {  0.86f, -0.51f } };
        for(int k = 0; k < 6; k++)        /* marcações em volta */
            vol_ret(x + MK[k][0] * 19 - 1.2f, y + MK[k][1] * 19 - 1.2f, x + MK[k][0] * 19 + 1.2f, y + MK[k][1] * 19 + 1.2f,
                    z + 0.5f, 0xFFB0B0B0);
    }
    /* botões pequenos embaixo + "cubo" de engate rápido */
    vol_circ(40, 2, 6.0f, z + 0.5f, 0xFF0C0C0E); vol_circ(40, 2, 4.0f, z + 0.6f, 0xFF808890);
    vol_circ(0, 46, 16.0f, z + 0.4f, 0xFF2C2E34);
    vol_circ(0, 46, 9.0f, z + 0.5f, 0xFF8C9096);

    /* luvas do piloto segurando as manoplas */
    for(int l = -1; l <= 1; l += 2) {
        float x = l * 167.0f;
        float palma[6][2] = { { x - 26, -10 }, { x + 26, -10 }, { x + 30, 20 }, { x + 22, 52 }, { x - 22, 52 }, { x - 30, 20 } };
        vol_poli(palma, 6, z + 1.0f, cor_luva);
        /* dedos dobrados sobre a manopla (frente) */
        for(int k = 0; k < 3; k++)
            vol_ret(x - 24, -6 + k * 13, x + 24, 3 + k * 13, z + 1.1f, cor_escala(cor_luva, 0.82f));
        /* polegar apoiado na frente do volante, apontando para o centro */
        float px = x - l * 26;
        float pol[4][2] = { { px, -8 }, { px - l * 20, -14 }, { px - l * 22, -2 }, { px, 6 } };
        vol_poli(pol, 4, z + 1.2f, cor_escala(cor_luva, 0.93f));
        /* punho (cor da equipe) */
        vol_ret(x - 27, 50, x + 27, 64, z + 1.0f, cor_punho);
    }
}

#endif
/* ---------------- RETROVISORES ----------------
 * A cada quadro (só no cockpit), uma cena extra é renderizada pelo PowerVR
 * DIRETO NUMA TEXTURA de 256x64 (render-to-texture): câmera no capacete
 * olhando para trás, imagem invertida como num espelho. Depois a cena
 * normal usa essa textura nos dois retrovisores (metade esquerda/direita). */
#define ESP_W 256
#define ESP_H 64
static pvr_ptr_t tex_espelho;
/* TELÃO (imagem ao vivo de uma câmera de TV) e REFLEXO na pista molhada:
 * cenas desenhadas numa TEXTURA (render-to-texture, como o retrovisor) */

static pvr_poly_hdr_t hdr_espelho;

static void desenhar_espelhos(void) {
    /* mais baixos, mais para as bordas e menores (como nos sidepods dos carros do fim dos anos 90) */
#ifdef VOLANTE_ANTIGO
    static const float M[2][4] = { { 10, 326, 122, 366 }, { 518, 326, 630, 366 } };   /* x0,y0,x1,y1 */
#else
    static const float M[2][4] = { { 30, 296, 106, 321 }, { 534, 296, 610, 321 } };   /* vidro dentro da carenagem nova */
#endif
    /* moldura (carenagem do retrovisor) */
    pvr_prim(&hdr_2d, sizeof(hdr_2d));
    for(int i = 0; i < 2; i++) {
#ifndef VOLANTE_ANTIGO
        break;                       /* a carenagem pintada vai na lista PT */
#endif
        float x0 = M[i][0] - 7, y0 = M[i][1] - 6, x1 = M[i][2] + 7, y1 = M[i][3] + 8;
        enviar_2d(x0, y1, 40, 0xFF15161A, 0); enviar_2d(x0, y0, 40, 0xFF2A2C32, 0);
        enviar_2d(x1, y1, 40, 0xFF15161A, 0); enviar_2d(x1, y0, 40, 0xFF2A2C32, 1);
    }
    pvr_prim(&hdr_espelho, sizeof(hdr_espelho));
    for(int i = 0; i < 2; i++) {
        float u0 = i * 0.5f, u1 = u0 + 0.5f;
        float xs[4] = { M[i][0], M[i][0], M[i][2], M[i][2] }, ys[4] = { M[i][3], M[i][1], M[i][3], M[i][1] };
        float us[4] = { u0, u0, u1, u1 }, vs[4] = { 1, 0, 1, 0 };
        for(int k = 0; k < 4; k++) {
            pvr_vertex_t *v = pvr_dr_target();
            v->flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            v->x = xs[k] * XS; v->y = ys[k]; v->z = 41; v->u = us[k]; v->v = vs[k];
            v->argb = 0xFFE8ECF0; v->oargb = 0;
            pvr_dr_commit(v);
        }
    }
}

/* ======================================================================== */
/* COCKPIT 3D                                                                */
/* ======================================================================== */
/* A visão de dentro é desenhada como o carro de fora: GEOMETRIA 3D na
 * posição física certa (olho do piloto dentro do capacete), iluminada pela
 * mesma luz da cena. O volante tem espessura, manoplas cilíndricas e a face
 * com botões numa textura nítida (256x128 sem compressão); as luvas envolvem
 * as manoplas; os antebraços vêm do cotovelo (dentro do cockpit) até as
 * luvas, NA FRENTE do volante; os retrovisores são caixas retangulares com o
 * vidro mostrando a imagem do retrovisor (render-to-texture).
 * Texturas pequenas repetidas (tecido, camurça, luva) em alta densidade,
 * com mipmap, como os jogos da época faziam. */
#ifndef COCKPIT_2D
static pvr_poly_hdr_t hdr_ck_face, hdr_ck_tecido, hdr_ck_camurca, hdr_ck_luva, hdr_ck_vidro, hdr_ck_carbono, hdr_ck_pintura;
static float ck_ka = 1.0f, ck_kb = 1.0f;          /* sombra de contato nas pontas A e B do próximo cilindro */
static int ck_leds;
static float ck_trep;                               /* trepidação da cena (zebra/grama): o volante treme mais */                                 /* LEDs acesos (para o brilho na lista translúcida) */
static int ck_ok;
static float ck_P[3], ck_R[3], ck_U[3], ck_F[3];      /* base do carro */
static float ck_C[3], ck_X[3], ck_Y[3], ck_N[3];      /* volante: centro e eixos (já girados) */
#define KV 0.00105f                                   /* metros por pixel da arte do volante */
#ifdef PINTURA_STEWART
static const uint32_t CK_LUVA = 0xFF2256C8, CK_LUVA_DET = 0xFFF2F2F2, CK_PUNHO = 0xFF1A2550;
#else
static const uint32_t CK_LUVA = 0xFF1E4F86, CK_LUVA_DET = 0xFFE6E8EC, CK_PUNHO = 0xFFF2C21B;
#endif

static inline uint16_t ck_565(int v) {
    if(v < 0) v = 0;
    if(v > 255) v = 255;
    return (uint16_t)((v >> 3) << 11 | (v >> 2) << 5 | (v >> 3));
}
/* texturas repetidas 64x64 (cinza: a cor vem do vértice) */
static void ck_tile(int tipo) {
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            uint32_t h = h32((uint32_t)(y * 64 + x) * 2654435761u + 77 * tipo);
            int r = (int)(h & 255) - 128;
            float fx = x / 64.0f, fy = y / 64.0f;
            int v;
            if(tipo == 0) {          /* TECIDO do macacão: trama + DOBRAS (claro/escuro) + costura */
                int tr = (((x >> 1) + (y >> 1)) & 1) ? 8 : -8;
                float dob = fsin(fy * 2 * PI_F * 2 + fsin(fx * 2 * PI_F) * 0.9f);      /* rugas em volta */
                float dob2 = fsin(fy * 2 * PI_F * 5 + fx * 3.0f) * 0.35f;
                v = 200 + tr + (int)(26 * dob + 10 * dob2) + r / 16;
                if(x == 0 || x == 1) v -= 46;                                          /* costura do painel */
                if((x == 4) && ((y & 3) < 2)) v -= 22;                                 /* pesponto */
            } else if(tipo == 1) {   /* CAMURÇA: grão fino e manchas suaves */
                uint32_t h2 = h32((uint32_t)((y >> 2) * 16 + (x >> 2)) * 977u + 5);
                v = 168 + r / 5 + (int)(h2 & 31) - 16;
            } else if(tipo == 2) {   /* LUVA: painéis com costura, furinhos de ventilação, vincos */
                v = 224 + r / 14;
                int py = y & 31;
                if(py == 0 || py == 1) v -= 55;                                        /* costura entre painéis */
                if(py == 3 && (x & 3) < 2) v -= 26;                                    /* pesponto */
                if(y > 34 && y < 60 && ((x % 6) == 3) && ((y % 6) == 3)) v -= 70;      /* furos de ventilação */
                v += (int)(14 * fsin(fx * 2 * PI_F * 3 + fy * 4));                     /* vincos suaves */
            } else {                 /* FIBRA DE CARBONO: sarja 2x2 com brilho do verniz */
                int cx = x >> 2, cy = y >> 2;
                int hor = ((cx + cy) & 1);
                float f = hor ? fsin(((y & 3) + 0.5f) * PI_F / 4) : fsin(((x & 3) + 0.5f) * PI_F / 4);
                v = 34 + (int)(30 * f * (hor ? 1.0f : 0.75f)) + r / 20;
            }
            tex_px[y * 64 + x] = ck_565(v);
        }
}
static void ck_hdr_tile(pvr_poly_hdr_t *h, int tipo) {
#ifdef CK_TILE_64
    const int N = 64;
    ck_tile(tipo);
#else
    /* textura 128x128 pintada no PC (cockpit_arte.tiles_cockpit): relevo por mapa de alturas */
    const int N = 128;
    (void)ck_tile;
    memcpy(tex_px, tex_bin + TEXB_CK_TILES + tipo * 128 * 128 * 2, 128 * 128 * 2);
#endif
    mip_4444 = 0;
    pvr_ptr_t t = tex_subir_mip(N);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, N, N, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
    cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
    pvr_poly_compile(h, &cxt);
}
static void criar_cockpit3d(void) {
    /* face do volante: 256x128 ARGB1555 (lista de recorte: os vãos do volante são transparentes) */
    memcpy(tex_px, tex_bin + TEXB_VOLANTE_FACE, 256 * 128 * 2);
    pvr_ptr_t t = tex_subir(256, 128);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED, 256, 128, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    pvr_poly_compile(&hdr_ck_face, &cxt);
    ck_hdr_tile(&hdr_ck_tecido, 0);
    ck_hdr_tile(&hdr_ck_camurca, 1);
    ck_hdr_tile(&hdr_ck_luva, 2);
    ck_hdr_tile(&hdr_ck_carbono, 3);
#ifndef CK_TILE_64
    ck_hdr_tile(&hdr_ck_pintura, 4);
#else
    hdr_ck_pintura = hdr_mundo;
#endif
    /* vidro do retrovisor: a mesma textura do render-to-texture, sem descarte de faces */
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, ESP_W, ESP_H, tex_espelho, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
    pvr_poly_compile(&hdr_ck_vidro, &cxt);
    ck_ok = 1;
}

/* ---------------- coordenadas ---------------- */
static inline void ck_w(float x, float y, float z, float w[3]) {        /* local do carro -> mundo */
    for(int e = 0; e < 3; e++) w[e] = ck_P[e] + ck_R[e] * x + ck_U[e] * y + ck_F[e] * z;
}
static inline void ck_vw(float xp, float yp, float d, float w[3]) {     /* arte do volante (px, y p/ cima) -> mundo */
    for(int e = 0; e < 3; e++) w[e] = ck_C[e] + ck_X[e] * (xp * KV) + ck_Y[e] * (yp * KV) + ck_N[e] * d;
}
static inline void ck_norm(float v[3]) {
    float l = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + 1e-12f);
    v[0] *= l; v[1] *= l; v[2] *= l;
}
static inline void ck_cross(const float a[3], const float b[3], float o[3]) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
/* luz da cena + brilho especular (verniz/couro) */
static uint32_t ck_luz(const float n[3], uint32_t cor, float spec) {
    float d = n[0] * LUZ[0] + n[1] * LUZ[1] + n[2] * LUZ[2];
    float k = brilho * (luz_base * 0.95f + luz_dir * (d > 0 ? d : 0) * 1.05f) + 0.08f;
    /* luz que vem de cima (céu) para as faces viradas para cima */
    k += 0.12f * brilho * (n[0] * ck_U[0] + n[1] * ck_U[1] + n[2] * ck_U[2] > 0 ? (n[0] * ck_U[0] + n[1] * ck_U[1] + n[2] * ck_U[2]) : 0);
    uint32_t c = cor_escala(cor, k);
    if(spec > 0 && d > 0) {
        float s = d * d; s *= s; s *= s;                  /* d^8 */
        c = cor_mistura(c, 0xFFFFFFFF, limitar(spec * s * brilho, 0, 0.6f));
    }
    if(tarde > 0.05f) c = cor_mistura(c, 0xFFFFB070, limitar(tarde, 0, 1) * 0.12f);
    return c;
}
/* pintura envernizada: mistura a cor iluminada com o que a superfície REFLETE
 * (céu em cima, horizonte claro, asfalto escuro embaixo), mais forte de lado
 * (Fresnel), e o brilho do sol refletido */
static uint32_t ck_verniz(const float p[3], const float n[3], uint32_t lit) {
    float v[3] = { p[0] - cam_p[0], p[1] - cam_p[1], p[2] - cam_p[2] };
    ck_norm(v);
    float dn = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
    float r[3] = { v[0] - 2 * dn * n[0], v[1] - 2 * dn * n[1], v[2] - 2 * dn * n[2] };
    uint32_t refl;
    if(r[1] > 0.02f) refl = cor_mistura(cor_neb, cor_ceu, limitar(r[1] * 2.2f, 0, 1));
    else refl = cor_mistura(cor_neb, cor_escala(0xFF4A4C50, 0.5f + 0.5f * brilho), limitar(-r[1] * 5.0f, 0, 1));
    float fr = 1.0f - fabsf(dn); fr = 0.18f + 0.62f * fr * fr;
    uint32_t c = cor_mistura(lit, refl, fr * 0.65f);
    float sl = r[0] * LUZ[0] + r[1] * LUZ[1] + r[2] * LUZ[2];
    if(sl > 0.80f) { float k = (sl - 0.80f) * 5.0f; k *= k; c = cor_mistura(c, 0xFFFFFFF4, limitar(k * brilho, 0, 0.85f)); }
    return c;
}
static float ck_brilho = 1.0f;                       /* sombra do lugar onde o carro está (estruturas, árvores) */
static void ck_poli(const float (*w)[3], int n, const uint32_t *c) {
    vv_t v[8];
    for(int i = 0; i < n; i++) para_vista(w[i][0], w[i][1], w[i][2], c[i], &v[i]);
    poligono(v, n);
}
static void ck_poli_t(const float (*w)[3], const float (*uv)[2], int n, const uint32_t *c) {
    vvt_t t[8];
    for(int i = 0; i < n; i++) {
        vv_t q;
        para_vista(w[i][0], w[i][1], w[i][2], 0, &q);
        t[i].x = q.x; t[i].y = q.y; t[i].z = q.z; t[i].u = uv[i][0]; t[i].v = uv[i][1]; t[i].c = c[i]; t[i].o = 0;
    }
    poligono_tex(t, n);
}
/* cilindro (cone) de A a B, raios ra->rb, setor de ângulos a0..a1 em volta do eixo
 * (ângulo 0 = direção "ref"), com normais suaves; tex = repetição da textura (0 = sem) */
static void ck_cilindro(const float A[3], const float B[3], float ra, float rb, int lados, float a0, float a1,
                        const float ref[3], uint32_t cor, float spec, float tu, float tv) {
    float d[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
    ck_norm(d);
    float k = ref[0] * d[0] + ref[1] * d[1] + ref[2] * d[2];
    float e1[3] = { ref[0] - d[0] * k, ref[1] - d[1] * k, ref[2] - d[2] * k };
    ck_norm(e1);
    float e2[3];
    ck_cross(d, e1, e2);
    /* cada "linha" do cilindro é transformada e iluminada UMA vez (antes: 4x por quad) */
    vvt_t pa[17], pb[17];
    for(int i = 0; i <= lados; i++) {
        float a = a0 + (a1 - a0) * i / lados, ca = fcos(a), sa = fsin(a);
        float n[3] = { e1[0] * ca + e2[0] * sa, e1[1] * ca + e2[1] * sa, e1[2] * ca + e2[2] * sa };
        uint32_t c = ck_luz(n, cor, spec);
        vv_t q;
        para_vista(A[0] + n[0] * ra, A[1] + n[1] * ra, A[2] + n[2] * ra, 0, &q);
        pa[i].x = q.x; pa[i].y = q.y; pa[i].z = q.z; pa[i].c = ck_ka == 1.0f ? c : cor_escala(c, ck_ka); pa[i].o = 0;
        para_vista(B[0] + n[0] * rb, B[1] + n[1] * rb, B[2] + n[2] * rb, 0, &q);
        pb[i].x = q.x; pb[i].y = q.y; pb[i].z = q.z; pb[i].c = ck_kb == 1.0f ? c : cor_escala(c, ck_kb); pb[i].o = 0;
        pa[i].u = pb[i].u = tu * i / lados; pa[i].v = 0; pb[i].v = tv;
    }
    /* caso comum (tudo na frente da câmera): UMA tira de triângulos (metade dos vértices) */
    int frente = 1;
    for(int i = 0; i <= lados && frente; i++) frente = pa[i].z <= -PERTO && pb[i].z <= -PERTO;
    if(frente && !sem_envio) {
        for(int i = 0; i <= lados; i++) {
            if(tu > 0) { enviar_tex(&pa[i], 0); enviar_tex(&pb[i], i == lados); }
            else {
                vv_t q0 = { pa[i].x, pa[i].y, pa[i].z, pa[i].c }, q1 = { pb[i].x, pb[i].y, pb[i].z, pb[i].c };
                enviar(&q0, 0); enviar(&q1, i == lados);
            }
        }
        return;
    }
    for(int i = 0; i < lados; i++) {
        if(tu > 0) {
            vvt_t t[4] = { pa[i], pb[i], pb[i + 1], pa[i + 1] };
            poligono_tex(t, 4);
        } else {
            vv_t t[4];
            const vvt_t *src[4] = { &pa[i], &pb[i], &pb[i + 1], &pa[i + 1] };
            for(int v = 0; v < 4; v++) { t[v].x = src[v]->x; t[v].y = src[v]->y; t[v].z = src[v]->z; t[v].c = src[v]->c; }
            poligono(t, 4);
        }
    }
}
/* TUBO de 3 anéis (A -> M -> B) com o anel do meio COMPARTILHADO: dois
 * cilindros separados geravam anéis diferentes na junção (fresta no braço).
 * Os anéis usam o mesmo "ref" projetado no plano de cada um; o do meio usa a
 * direção média dos dois trechos. k = sombra de contato em cada anel. */
static void ck_tubo3(const float A[3], const float M[3], const float B[3], float ra, float rm, float rb, int lados,
                     float a0, float a1, const float ref[3], uint32_t cor, float spec, float tu, float tv,
                     float ka, float km, float kb2) {
    const float *P3[3] = { A, M, B };
    float rr[3] = { ra, rm, rb }, kk[3] = { ka, km, kb2 };
    float d0[3] = { M[0] - A[0], M[1] - A[1], M[2] - A[2] }, d1[3] = { B[0] - M[0], B[1] - M[1], B[2] - M[2] };
    ck_norm(d0); ck_norm(d1);
    float dm[3] = { d0[0] + d1[0], d0[1] + d1[1], d0[2] + d1[2] };
    ck_norm(dm);
    const float *D[3] = { d0, dm, d1 };
    static vvt_t anel[3][17];
    for(int r = 0; r < 3; r++) {
        const float *d = D[r];
        float k = ref[0] * d[0] + ref[1] * d[1] + ref[2] * d[2];
        float e1[3] = { ref[0] - d[0] * k, ref[1] - d[1] * k, ref[2] - d[2] * k };
        ck_norm(e1);
        float e2[3];
        ck_cross(d, e1, e2);
        for(int i = 0; i <= lados; i++) {
            float a = a0 + (a1 - a0) * i / lados, ca = fcos(a), sa = fsin(a);
            float n[3] = { e1[0] * ca + e2[0] * sa, e1[1] * ca + e2[1] * sa, e1[2] * ca + e2[2] * sa };
            uint32_t c = ck_luz(n, cor, spec);
            if(kk[r] != 1.0f) c = cor_escala(c, kk[r]);
            vv_t q;
            para_vista(P3[r][0] + n[0] * rr[r], P3[r][1] + n[1] * rr[r], P3[r][2] + n[2] * rr[r], 0, &q);
            vvt_t *v = &anel[r][i];
            v->x = q.x; v->y = q.y; v->z = q.z; v->c = c; v->o = 0;
            v->u = tu * i / lados; v->v = tv * r * 0.5f;
        }
    }
    for(int r = 0; r < 2; r++)
        for(int i = 0; i < lados; i++) {
            vvt_t t[4] = { anel[r][i], anel[r + 1][i], anel[r + 1][i + 1], anel[r][i + 1] };
            poligono_tex(t, 4);
        }
}
/* tampa (leque) na ponta de um cilindro */
static void ck_tampa(const float A[3], const float B[3], float r, int lados, const float ref[3], uint32_t cor) {
    float d[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
    ck_norm(d);
    float k = ref[0] * d[0] + ref[1] * d[1] + ref[2] * d[2];
    float e1[3] = { ref[0] - d[0] * k, ref[1] - d[1] * k, ref[2] - d[2] * k };
    ck_norm(e1);
    float e2[3];
    ck_cross(d, e1, e2);
    float w[8][3]; uint32_t c[8];
    int n = lados > 8 ? 8 : lados;
    for(int i = 0; i < n; i++) {
        float a = 2 * PI_F * i / n;
        for(int e = 0; e < 3; e++) w[i][e] = B[e] + (e1[e] * fcos(a) + e2[e] * fsin(a)) * r * 0.92f + d[e] * r * 0.25f;
        c[i] = ck_luz(d, cor, 0.2f);
    }
    ck_poli(w, n, c);
}
/* caixa no espaço do carro (centro, meias-medidas), com uma face opcional de vidro */
static void ck_caixa(float cx, float cy, float cz, float hx, float hy, float hz, uint32_t cor, float spec) {
    static const int F[6][4] = { { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 } };
    static const float NL[6][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
    float v[8][3];
    for(int i = 0; i < 8; i++)
        ck_w(cx + ((i & 4) ? hx : -hx), cy + ((i & 2) ? hy : -hy), cz + ((i & 1) ? hz : -hz), v[i]);
    for(int f = 0; f < 6; f++) {
        float n[3];
        for(int e = 0; e < 3; e++) n[e] = ck_R[e] * NL[f][0] + ck_U[e] * NL[f][1] + ck_F[e] * NL[f][2];
        uint32_t c = ck_luz(n, cor, spec);
        float w[4][3]; uint32_t cc[4] = { c, c, c, c };
        for(int k = 0; k < 4; k++) memcpy(w[k], v[F[f][k]], 12);
        ck_poli(w, 4, cc);
    }
}
/* retângulo na face do volante (coordenadas da arte: px, y PARA BAIXO, como no desenho) */
static void ck_ret(float x0, float y0, float x1, float y1, float d, uint32_t cor) {
    float w[4][3];
    ck_vw(x0, -y0, d, w[0]); ck_vw(x1, -y0, d, w[1]); ck_vw(x1, -y1, d, w[2]); ck_vw(x0, -y1, d, w[3]);
    uint32_t c[4] = { cor, cor, cor, cor };
    ck_poli(w, 4, c);
}
static void ck_circ(float x, float y, float r, float d, uint32_t cor) {
    float w[8][3]; uint32_t c[8];
    for(int i = 0; i < 8; i++) { ck_vw(x + r * CIRC8[i][0], -(y + r * CIRC8[i][1]), d, w[i]); c[i] = cor; }
    ck_poli(w, 8, c);
}
static void ck_digito(int dg, float x, float y, float w, float h, float t, float d, uint32_t on, uint32_t off) {
    static const uint8_t SEG[11] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F, 0x54 };
    uint8_t m = dg < 0 ? 0 : SEG[dg > 10 ? 10 : dg];
    float hm = h * 0.5f;
    const float R[7][4] = {
        { x + t, y, x + w - t, y + t }, { x + w - t, y + t, x + w, y + hm }, { x + w - t, y + hm, x + w, y + h - t },
        { x + t, y + h - t, x + w - t, y + h }, { x, y + hm, x + t, y + h - t }, { x, y + t, x + t, y + hm },
        { x + t, y + hm - t * 0.5f, x + w - t, y + hm + t * 0.5f } };
    (void)off;
    for(int s = 0; s < 7; s++) if((m >> s) & 1) ck_ret(R[s][0], R[s][1], R[s][2], R[s][3], d, on);
}

/* ---------------- montagem por quadro ---------------- */
static void ck_preparar(const carro_t *eu, float giro) {
    base_carro(eu, ck_P, ck_R, ck_U, ck_F);
    /* volante: 0,59 m de altura, 0,74 m à frente; topo inclinado para a frente */
    ck_w(0, 0.612f, 0.745f, ck_C);
    {   /* volante e mãos vibram mais que o resto (coluna de direção recebe o tranco das rodas) */
        float tq = t_clima;
        float vx = ck_trep * 0.35f * fsin(tq * 131.0f), vy = ck_trep * 0.45f * fsin(tq * 97.0f + 1.3f);
        for(int e = 0; e < 3; e++) ck_C[e] += ck_R[e] * vx + ck_U[e] * vy;
    }
    const float ct = 0.955f, st = 0.296f;               /* inclinação ~17 graus */
    float Yw[3], Nw[3];
    for(int e = 0; e < 3; e++) { Yw[e] = ck_U[e] * ct + ck_F[e] * st; Nw[e] = -ck_F[e] * ct + ck_U[e] * st; }
    float cs = fcos(giro), sn = fsin(giro);
    for(int e = 0; e < 3; e++) {
        ck_X[e] = ck_R[e] * cs - Yw[e] * sn;            /* giro positivo = horário visto pelo piloto */
        ck_Y[e] = ck_R[e] * sn + Yw[e] * cs;
        ck_N[e] = Nw[e];
    }
}
/* direção em volta do eixo da manopla: 0 = para o piloto, 90 = para fora */
static inline void ck_dir(int l, float ang, float o[3]) {
    float c = fcos(ang), s = fsin(ang);
    for(int e = 0; e < 3; e++) o[e] = ck_N[e] * c + ck_X[e] * (l * s);
}
static void ck_braco_pulso(int l, float W[3]) {
    float a[3], dd[3];
    ck_vw(l * 112.0f, -24.0f, 0.004f, a);
    ck_dir(l, 0.9f, dd);
    for(int e = 0; e < 3; e++) W[e] = a[e] + dd[e] * 0.012f;
}

static void desenhar_cockpit3d(const carro_t *eu, float giro, int marcha, float kmh, float rpm) {
    if(!ck_ok) return;
    mat_load(&mat_cam);
    ck_preparar(eu, giro);
    {   /* o cockpit escurece ao passar por sombras (as mesmas que escurecem o carro) */
        float alvo_b = sombra_estrutura_no_carro(ck_P);
        int kf = (int)(fmodf(eu->s + COMPR, COMPR) / PASSO) % NF, ib = 4;
        float dmin = 1e9f;
        for(int q = 1; q < NPTS - 1; q++) { float d = fabsf(LAT[q] - eu->x); if(d < dmin) { dmin = d; ib = q; } }
        alvo_b *= 0.50f + 0.50f * (somb[kf][ib] / 255.0f);
        ck_brilho += (alvo_b - ck_brilho) * 0.25f;
    }
    float brilho_sal = brilho;
    brilho = brilho_sal * ck_brilho;
    const float D = 0.004f;                          /* eixo das manoplas um pouco para o lado do piloto */
    float A[3], B[3];
    /* ---- LATERAIS DO COCKPIT (altas, na cor do carro, com verniz) ----
     * seção de dentro para fora; a borda desce um pouco perto da frente */
    {
        static const float SX[6] = { 0.172f, 0.172f, 0.200f, 0.262f, 0.360f, 0.470f };
        static const float SY[6] = { 0.440f, 0.622f, 0.648f, 0.652f, 0.618f, 0.530f };
        static const float ZS[7] = { -0.05f, 0.12f, 0.30f, 0.46f, 0.62f, 0.78f, 0.93f };
        uint32_t pint = eu->cores[C_PRINC], acol = 0xFF2C2D31;
        for(int l = -1; l <= 1; l += 2) {
            for(int sgm = 0; sgm < 5; sgm++) {
                int dentro = sgm == 0;                       /* parede interna acolchoada */
                if(dentro) pvr_prim(&hdr_ck_camurca, sizeof(hdr_ck_camurca));
                else if(sgm == 1) pvr_prim(&hdr_ck_pintura, sizeof(hdr_ck_pintura));
                float nx = -(SY[sgm + 1] - SY[sgm]), ny = SX[sgm + 1] - SX[sgm];
                float nl = 1.0f / sqrtf(nx * nx + ny * ny); nx *= nl; ny *= nl;
                if(dentro) { nx = -1; ny = 0; }
                float n[3];
                for(int e = 0; e < 3; e++) n[e] = ck_R[e] * (nx * l) + ck_U[e] * ny;
                uint32_t c = dentro ? ck_luz(n, acol, 0.1f) : ck_luz(n, pint, 0.95f);
                if(!dentro && ny > 0.6f) c = cor_mistura(c, 0xFFCFE2F0, 0.10f + 0.10f * brilho);   /* reflexo do céu no verniz */
                for(int zi = 0; zi < 6; zi++) {
                    float w[4][3];
                    float z0 = ZS[zi], z1 = ZS[zi + 1];
                    float d0 = z0 > 0.55f ? -0.050f * (z0 - 0.55f) / 0.38f : 0, d1 = z1 > 0.55f ? -0.050f * (z1 - 0.55f) / 0.38f : 0;
                    ck_w(l * SX[sgm], SY[sgm] + d0, z0, w[0]);         ck_w(l * SX[sgm + 1], SY[sgm + 1] + d0, z0, w[1]);
                    ck_w(l * SX[sgm + 1], SY[sgm + 1] + d1, z1, w[2]); ck_w(l * SX[sgm], SY[sgm] + d1, z1, w[3]);
                    uint32_t ca = c, cb = c;
                    if(dentro) { ca = cor_escala(c, 0.45f); }      /* fundo da parede no escuro */
                    uint32_t cc[4] = { ca, c, c, ca };
                    (void)cb;
                    if(!dentro) for(int k = 0; k < 4; k++) cc[k] = ck_verniz(w[k], n, c);
                    if(dentro) {
                        const float uv[4][2] = { { 0, 2 }, { 0, 0 }, { 4, 0 }, { 4, 2 } };
                        ck_poli_t(w, uv, 4, cc);
                    } else {
                        /* SUJEIRA de pista: na parte de baixo/de fora e na frente (spray dos pneus),
                         * cresce com a sujeira acumulada do carro */
                        float suj = limitar(0.25f + eu->sujeira, 0, 1) * (sgm >= 3 ? (sgm == 4 ? 0.45f : 0.22f) : 0.0f);
                        if(suj > 0) {
                            uint32_t pd = cor_escala(0xFF6A5E52, brilho);
                            float fz0 = 0.6f + 0.4f * limitar((z0 - 0.3f) / 0.6f, 0, 1), fz1 = 0.6f + 0.4f * limitar((z1 - 0.3f) / 0.6f, 0, 1);
                            cc[0] = cor_mistura(cc[0], pd, suj * fz0); cc[3] = cor_mistura(cc[3], pd, suj * fz1);
                            if(sgm == 4) { cc[1] = cor_mistura(cc[1], pd, suj * 1.4f * fz0); cc[2] = cor_mistura(cc[2], pd, suj * 1.4f * fz1); }
                        }
                        /* textura da pintura em escala real (~25 cm por repetição) */
                        float u0 = z0 * 4.0f, u1 = z1 * 4.0f, v0 = SX[sgm] * 4.0f + SY[sgm] * 4.0f, v1 = SX[sgm + 1] * 4.0f + SY[sgm + 1] * 4.0f;
                        const float uv[4][2] = { { u0, v0 }, { u0, v1 }, { u1, v1 }, { u1, v0 } };
                        ck_poli_t(w, uv, 4, cc);
                    }
                }
            }
            /* filete na cor secundária ao longo da borda de cima */
            pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
            for(int zi = 0; zi < 6; zi++) {
                float w[4][3], z0 = ZS[zi], z1 = ZS[zi + 1];
                float d0 = z0 > 0.55f ? -0.050f * (z0 - 0.55f) / 0.38f : 0, d1 = z1 > 0.55f ? -0.050f * (z1 - 0.55f) / 0.38f : 0;
                ck_w(l * 0.300f, 0.6422f + d0, z0, w[0]); ck_w(l * 0.322f, 0.6362f + d0, z0, w[1]);
                ck_w(l * 0.322f, 0.6362f + d1, z1, w[2]); ck_w(l * 0.300f, 0.6422f + d1, z1, w[3]);
                uint32_t c = ck_luz(ck_U, eu->cores[C_SEC], 0.6f);
                uint32_t cc[4] = { c, c, c, c };
                ck_poli(w, 4, cc);
            }
            /* borda acolchoada (camurça) por cima da quina de dentro */
            pvr_prim(&hdr_ck_camurca, sizeof(hdr_ck_camurca));
            ck_w(l * 0.186f, 0.632f, -0.05f, A); ck_w(l * 0.186f, 0.594f, 0.93f, B);
            ck_cilindro(A, B, 0.020f, 0.020f, 4, -PI_F * 0.5f, PI_F * 0.5f, ck_U, acol, 0.2f, 2.0f, 10.0f);
        }
        /* ARCO da frente do cockpit (escuro), logo acima do volante: o bico aparece por cima */
        pvr_prim(&hdr_ck_carbono, sizeof(hdr_ck_carbono));
        {
            float w[4][3];
            ck_w(-0.186f, 0.594f, 0.93f, w[0]); ck_w(0.186f, 0.594f, 0.93f, w[1]);
            ck_w(0.172f, 0.520f, 0.86f, w[2]); ck_w(-0.172f, 0.520f, 0.86f, w[3]);
            uint32_t c = ck_luz(ck_U, 0xFF8A8E96, 0.3f), ce = cor_escala(c, 0.35f);
            const uint32_t cc[4] = { c, c, ce, ce };
            const float uv[4][2] = { { 0, 0 }, { 4, 0 }, { 4, 1 }, { 0, 1 } };
            ck_poli_t(w, uv, 4, cc);
        }
        pvr_prim(&hdr_ck_camurca, sizeof(hdr_ck_camurca));
        ck_w(-0.19f, 0.597f, 0.93f, A); ck_w(0.19f, 0.597f, 0.93f, B);
        ck_cilindro(A, B, 0.020f, 0.020f, 4, -PI_F * 0.5f, PI_F * 0.5f, ck_U, acol, 0.2f, 2.0f, 6.0f);
    }
    /* ---- INTERIOR: paredes e painel da frente em carbono (escurecem para baixo) ---- */
    pvr_prim(&hdr_ck_carbono, sizeof(hdr_ck_carbono));
    {
        uint32_t cl = ck_luz(ck_U, 0xFFB8BCC4, 0.0f), ce = cor_escala(cl, 0.28f);
        for(int l = -1; l <= 1; l += 2) {           /* paredes internas (vistas por dentro) */
            float w[4][3];
            ck_w(l * 0.158f, 0.560f, 0.05f, w[0]); ck_w(l * 0.158f, 0.560f, 0.80f, w[1]);
            ck_w(l * 0.158f, 0.300f, 0.80f, w[2]); ck_w(l * 0.158f, 0.300f, 0.05f, w[3]);
            const float uv[4][2] = { { 0, 0 }, { 6, 0 }, { 6, 2.2f }, { 0, 2.2f } };
            const uint32_t cc[4] = { cl, cl, ce, ce };
            ck_poli_t(w, uv, 4, cc);
        }
        float w[4][3];                              /* painel da frente (atrás do volante) */
        ck_w(-0.158f, 0.560f, 0.80f, w[0]); ck_w(0.158f, 0.560f, 0.80f, w[1]);
        ck_w(0.158f, 0.300f, 0.80f, w[2]); ck_w(-0.158f, 0.300f, 0.80f, w[3]);
        const float uv[4][2] = { { 0, 0 }, { 3, 0 }, { 3, 2.2f }, { 0, 2.2f } };
        uint32_t cp = cor_escala(cl, 0.75f), cpe = cor_escala(cl, 0.22f);
        const uint32_t cc[4] = { cp, cp, cpe, cpe };
        ck_poli_t(w, uv, 4, cc);
        ck_w(-0.158f, 0.300f, 0.05f, w[0]); ck_w(0.158f, 0.300f, 0.05f, w[1]);   /* assoalho */
        ck_w(0.158f, 0.300f, 0.80f, w[2]); ck_w(-0.158f, 0.300f, 0.80f, w[3]);
        const uint32_t cf[4] = { ce, ce, ce, ce };
        ck_poli_t(w, uv, 4, cf);
    }
    /* ---- PERNAS do piloto: coxas subindo até os joelhos, logo abaixo do volante ---- */
    pvr_prim(&hdr_ck_tecido, sizeof(hdr_ck_tecido));
    {
        uint32_t cs = cor_escala(cor_mistura(eu->cores[C_PRINC], 0xFF303034, 0.35f), 0.55f);   /* na sombra */
        for(int l = -1; l <= 1; l += 2) {
            float Q[3], J[3], Pe[3];
            ck_w(l * 0.082f, 0.36f, 0.30f, Q);              /* quadril */
            ck_w(l * 0.078f, 0.495f, 0.660f, J);            /* joelho (abaixo do volante, no escuro do cockpit) */
            ck_w(l * 0.066f, 0.36f, 0.86f, Pe);             /* canela descendo para os pedais */
            ck_tubo3(Q, J, Pe, 0.070f, 0.057f, 0.046f, 7, 0, 2 * PI_F, ck_U, cs, 0.08f, 2.0f, 4.0f, 0.30f, 0.80f, 0.30f);
            float J2[3];                                    /* joelheira (volume do joelho) */
            for(int e = 0; e < 3; e++) J2[e] = J[e] + ck_U[e] * 0.020f + ck_F[e] * 0.012f;
            ck_cilindro(J, J2, 0.058f, 0.045f, 7, 0, 2 * PI_F, ck_F, cs, 0.1f, 1.0f, 0.5f);
            ck_tampa(J, J2, 0.045f, 7, ck_F, cs);
        }
    }
    /* ---- coluna de direção ---- */
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
    ck_vw(0, 0, -0.02f, A); ck_w(0, 0.552f, 0.88f, B);
    ck_cilindro(A, B, 0.016f, 0.020f, 4, 0, 2 * PI_F, ck_U, 0xFF2A2C30, 0.4f, 0, 0);
    /* ---- espessura do volante (contorno em carbono) ---- */
    {
        static float CT[16][2];
        static int ok;
        if(!ok) {
            for(int i = 0; i < 16; i++) {
                float a = i * 2 * PI_F / 16;
                float cx = cosf(a), sy = sinf(a);
                float x = (cx > 0 ? 1 : -1) * powf(fabsf(cx), 0.35f) * 124.0f;
                float y = (sy > 0 ? 1 : -1) * powf(fabsf(sy), 0.45f) * (sy > 0 ? 72.0f : 78.0f) - 2.0f;
                CT[i][0] = x; CT[i][1] = y;
            }
            ok = 1;
        }
        for(int i = 0; i < 16; i++) {
            int j = (i + 1) % 16;
            float w[4][3];
            ck_vw(CT[i][0], CT[i][1], 0.0f, w[0]); ck_vw(CT[j][0], CT[j][1], 0.0f, w[1]);
            ck_vw(CT[j][0], CT[j][1], -0.024f, w[2]); ck_vw(CT[i][0], CT[i][1], -0.024f, w[3]);
            float n[3];
            float mx = (CT[i][0] + CT[j][0]) * 0.5f, my = (CT[i][1] + CT[j][1]) * 0.5f;
            for(int e = 0; e < 3; e++) n[e] = ck_X[e] * mx / 124.0f + ck_Y[e] * my / 75.0f;
            ck_norm(n);
            uint32_t c = ck_luz(n, 0xFF2A2C31, 0.5f);
            uint32_t cc[4] = { c, c, c, c };
            ck_poli(w, 4, cc);
        }
    }
    /* chanfro iluminado na borda da frente (tira o aspecto de "placa recortada") */
    {
        static float CT2[16][2];
        static int ok2;
        if(!ok2) {
            for(int i = 0; i < 16; i++) {
                float a = i * 2 * PI_F / 16, cx = cosf(a), sy = sinf(a);
                CT2[i][0] = (cx > 0 ? 1 : -1) * powf(fabsf(cx), 0.35f) * 124.0f;
                CT2[i][1] = (sy > 0 ? 1 : -1) * powf(fabsf(sy), 0.45f) * (sy > 0 ? 72.0f : 78.0f) - 2.0f;
            }
            ok2 = 1;
        }
        for(int i = 0; i < 16; i++) {
            int j = (i + 1) % 16;
            float w[4][3], n[3];
            ck_vw(CT2[i][0], CT2[i][1], 0.0f, w[0]); ck_vw(CT2[j][0], CT2[j][1], 0.0f, w[1]);
            ck_vw(CT2[j][0] * 0.975f, CT2[j][1] * 0.965f, 0.004f, w[2]); ck_vw(CT2[i][0] * 0.975f, CT2[i][1] * 0.965f, 0.004f, w[3]);
            float mx = (CT2[i][0] + CT2[j][0]) * 0.5f, my = (CT2[i][1] + CT2[j][1]) * 0.5f;
            for(int e = 0; e < 3; e++) n[e] = ck_X[e] * mx / 124.0f * 0.7f + ck_Y[e] * my / 75.0f * 0.7f + ck_N[e] * 0.7f;
            ck_norm(n);
            uint32_t c = ck_luz(n, 0xFF3A3D44, 0.9f);
            uint32_t cc[4] = { c, c, c, c };
            ck_poli(w, 4, cc);
        }
    }
    /* seletores giratórios e botões SALTADOS da placa (3D) */
    {
        static const float SEL[3][3] = { { -34, -38, 0 }, { 0, -48, 1 }, { 34, -38, 2 } };
        static const uint32_t SC[3] = { 0xFF2BB0A8, 0xFFE8C020, 0xFFE03020 };
        for(int k = 0; k < 3; k++) {
            float c0[3], c1[3];
            ck_vw(SEL[k][0], SEL[k][1], 0.0f, c0); ck_vw(SEL[k][0], SEL[k][1], 0.0105f, c1);
            ck_cilindro(c0, c1, 9.5f * KV, 8.5f * KV, 8, 0, 2 * PI_F, ck_Y, SC[k], 0.5f, 0, 0);
            ck_tampa(c0, c1, 8.5f * KV, 8, ck_Y, 0xFF1C1D21);
            float p0[3], p1[3];                            /* alavanca do seletor */
            ck_vw(SEL[k][0], SEL[k][1], 0.012f, p0); ck_vw(SEL[k][0] + 3.0f, SEL[k][1] + 8.0f, 0.013f, p1);
            ck_cilindro(p0, p1, 2.2f * KV, 1.8f * KV, 4, 0, 2 * PI_F, ck_N, 0xFFE8E8E8, 0.6f, 0, 0);
        }
        static const float BT[7][2] = { { -62, 32 }, { -62, 6 }, { 62, 32 }, { 62, 6 }, { -46, -14 }, { 46, -14 }, { 0, -8 } };
        static const uint32_t BC[7] = { 0xFF2BB0A8, 0xFFE03020, 0xFFF0A020, 0xFFE8E8E8, 0xFF2AA040, 0xFF3A70FF, 0xFFD8D8D8 };
        for(int k = 0; k < 7; k++) {
            float c0[3], c1[3];
            float r = k == 6 ? 4.0f : 5.4f;
            ck_vw(BT[k][0], BT[k][1], 0.0f, c0); ck_vw(BT[k][0], BT[k][1], 0.0045f, c1);
            ck_cilindro(c0, c1, r * KV, r * 0.9f * KV, 6, 0, 2 * PI_F, ck_Y, BC[k], 0.8f, 0, 0);
            ck_tampa(c0, c1, r * 0.9f * KV, 6, ck_Y, cor_mistura(BC[k], 0xFFFFFFFF, 0.15f));
        }
    }
    /* ---- manoplas cilíndricas de camurça ---- */
    pvr_prim(&hdr_ck_camurca, sizeof(hdr_ck_camurca));
    for(int l = -1; l <= 1; l += 2) {
        ck_vw(l * 109.0f, -74.0f, D, A); ck_vw(l * 109.0f, 66.0f, D, B);
        ck_cilindro(A, B, 0.0195f, 0.0195f, 8, 0, 2 * PI_F, ck_N, 0xFF3E3A38, 0.12f, 1.0f, 3.0f);
        ck_tampa(B, A, 0.0195f, 8, ck_N, 0xFF3E3A38);
        ck_tampa(A, B, 0.0195f, 8, ck_N, 0xFF3E3A38);
    }
    /* ---- MÃOS / LUVAS ----
     * Como se segura a manopla: o dorso fica do lado de FORA; cada dedo nasce no
     * nó (lado de fora), dá a volta por TRÁS da manopla e reaparece do lado de
     * DENTRO com a falange da ponta curvada para a palma; o polegar, com duas
     * falanges, fica deitado na frente. Só as falanges visíveis são desenhadas. */
    for(int l = -1; l <= 1; l += 2) {
        float gx = l * 109.0f;
        float ref[3];
        ck_dir(l, 0, ref);
        /* ponto em volta do eixo da manopla: altura (px), ângulo (0 = piloto, 90° = fora, 180° = trás) e raio */
        #define PG(y, ang, r, o) do { float a_[3], d_[3]; ck_vw(gx, (y), D, a_); ck_dir(l, (ang), d_); \
                                      for(int e_ = 0; e_ < 3; e_++) (o)[e_] = a_[e_] + d_[e_] * (r); } while(0)
        static const float DY[4] = { 47.0f, 33.0f, 19.5f, 6.5f };            /* indicador, médio, anelar, mínimo */
        static const float DR[4] = { 0.0098f, 0.0104f, 0.0098f, 0.0086f };   /* espessura (raio) */
        static const float DA[4] = { 0.00f, 0.06f, 0.03f, -0.05f };          /* médio um pouco mais comprido */
        /* --- dedos: falange de cima (sai do nó, do lado de fora) --- */
        pvr_prim(&hdr_ck_luva, sizeof(hdr_ck_luva));
        for(int k = 0; k < 4; k++) {
            float A_[3], B_[3];
            PG(DY[k], 1.62f, 0.0370f, A_);
            PG(DY[k] - 1.0f, 2.55f, 0.0345f, B_);
            ck_ka = 0.95f; ck_kb = 0.70f;                   /* vai para a sombra atrás da manopla */
            ck_cilindro(A_, B_, DR[k], DR[k] * 0.95f, 7, 0, 2 * PI_F, ck_Y, CK_LUVA, 0.3f, 0.5f, 0.6f);
            ck_ka = ck_kb = 1.0f;
            ck_tampa(B_, A_, DR[k], 7, ck_Y, CK_LUVA);      /* nó arredondado */
        }
        /* --- dedos: falange da PONTA, voltando pelo lado de dentro (aparece entre a manopla e a placa) --- */
        for(int k = 0; k < 4; k++) {
            float A_[3], M_[3], B_[3];
            float ap = 4.30f + DA[k];
            PG(DY[k] - 2.0f, 3.85f, 0.0345f, A_);          /* sai de trás da manopla */
            PG(DY[k] - 2.5f, ap, 0.0340f, M_);             /* última junta (bem visível, por dentro) */
            PG(DY[k] - 2.8f, ap + 0.70f, 0.0285f, B_);     /* ponta, curvada para a palma */
            pvr_prim(&hdr_ck_luva, sizeof(hdr_ck_luva));
            uint32_t cdedo = cor_mistura(CK_LUVA, 0xFFFFFFFF, 0.18f);   /* mais claro: lê como dedo */
            ck_ka = 0.85f; ck_kb = 1.0f;
            ck_cilindro(A_, M_, DR[k] * 1.08f, DR[k] * 1.04f, 7, 0, 2 * PI_F, ck_Y, cdedo, 0.35f, 0.5f, 0.5f);
            ck_ka = ck_kb = 1.0f;
            float T_[3];
            for(int e = 0; e < 3; e++) T_[e] = M_[e] + (B_[e] - M_[e]) * 0.88f;
            ck_tampa(A_, M_, DR[k] * 1.04f, 7, ck_Y, cdedo);                                     /* junta arredondada */
            ck_cilindro(M_, T_, DR[k] * 1.04f, DR[k] * 0.96f, 7, 0, 2 * PI_F, ck_Y, cdedo, 0.35f, 0.4f, 0.4f);
            pvr_prim(&hdr_ck_camurca, sizeof(hdr_ck_camurca));   /* só a pontinha de camurça (aderência) */
            uint32_t cponta = cor_mistura(cdedo, 0xFF9A9CA2, 0.55f);
            ck_cilindro(T_, B_, DR[k] * 0.96f, DR[k] * 0.88f, 7, 0, 2 * PI_F, ck_Y, cponta, 0.25f, 0.3f, 0.3f);
            ck_tampa(T_, B_, DR[k] * 0.88f, 7, ck_Y, cponta);
        }
        /* --- dorso da mão: casca abaulada do lado de fora, fechando no alinhamento dos nós --- */
        pvr_prim(&hdr_ck_luva, sizeof(hdr_ck_luva));
        {
            float a0[3], am[3], a1[3];
            ck_vw(gx, -22.0f, D, a0); ck_vw(gx, 20.0f, D, am); ck_vw(gx, 54.0f, D, a1);
            ck_ka = 0.80f;
            ck_cilindro(a0, am, 0.031f, 0.0405f, 9, -0.20f * l, 1.66f * l, ref, CK_LUVA, 0.30f, 1.5f, 0.8f);
            ck_ka = 1.0f;
            ck_cilindro(am, a1, 0.0405f, 0.0385f, 9, -0.20f * l, 1.66f * l, ref, CK_LUVA, 0.30f, 1.5f, 0.7f);
            float t0[3], t1[3];                             /* fecha em cima, até o indicador */
            ck_vw(gx, 54.0f, D, t0); ck_vw(gx, 60.0f, D, t1);
            ck_cilindro(t0, t1, 0.0385f, 0.026f, 7, -0.20f * l, 1.66f * l, ref, CK_LUVA, 0.30f, 1.0f, 0.3f);
        }
        /* painéis acolchoados claros no dorso (como nas luvas de corrida) */
        for(int k = 0; k < 2; k++) {
            float y0 = 2.0f + k * 22.0f, y1 = y0 + 17.0f, b0[3], b1[3];
            ck_vw(gx, y0, D, b0); ck_vw(gx, y1, D, b1);
            ck_cilindro(b0, b1, 0.0415f + 0.001f * k, 0.0405f, 3, 0.55f * l, 1.25f * l, ref,
                        cor_mistura(CK_LUVA, CK_LUVA_DET, 0.55f), 0.35f, 1.0f, 0.6f);
        }
        /* protetor de borracha sobre os nós (alinhado aos dedos) */
        pvr_prim(&hdr_ck_camurca, sizeof(hdr_ck_camurca));
        for(int k = 0; k < 4; k++) {
            float c0[3], c1[3], dd[3];
            PG(DY[k] - 4.5f, 1.45f, 0.0405f, c0);
            PG(DY[k] + 4.5f, 1.45f, 0.0405f, c1);
            ck_dir(l, 1.45f, dd);
            ck_cilindro(c0, c1, 0.0072f, 0.0072f, 4, -PI_F * 0.5f, PI_F * 0.5f, dd, 0xFF5E6168, 0.35f, 0.5f, 0.5f);
        }
        /* --- polegar: duas falanges, deitado na frente da manopla, apontando para o centro --- */
        {
            float base[3], junta[3], ponta[3], p2[3];
            PG(52.0f, 0.45f, 0.034f, base);
            ck_vw(gx - l * 6.0f, 60.0f, D + 0.024f, junta);
            ck_vw(gx - l * 22.0f, 58.0f, D + 0.020f, ponta);
            pvr_prim(&hdr_ck_luva, sizeof(hdr_ck_luva));
            ck_cilindro(base, junta, 0.0128f, 0.0112f, 6, 0, 2 * PI_F, ck_Y, CK_LUVA, 0.3f, 1.0f, 0.5f);
            ck_tampa(base, junta, 0.0112f, 6, ck_Y, CK_LUVA);           /* junta arredondada */
            ck_cilindro(junta, ponta, 0.0110f, 0.0098f, 6, 0, 2 * PI_F, ck_Y, CK_LUVA, 0.3f, 1.0f, 0.5f);
            pvr_prim(&hdr_ck_camurca, sizeof(hdr_ck_camurca));
            for(int e = 0; e < 3; e++) p2[e] = ponta[e] + (ponta[e] - junta[e]) * 0.35f;
            ck_cilindro(ponta, p2, 0.0098f, 0.0072f, 6, 0, 2 * PI_F, ck_Y, 0xFF8E9096, 0.2f, 0.5f, 0.3f);
            ck_tampa(ponta, p2, 0.0072f, 6, ck_Y, 0xFF8E9096);
        }
        #undef PG
    }
    /* ---- ANTEBRAÇOS: do cotovelo (dentro do cockpit) até o pulso, na frente do volante ---- */
    for(int l = -1; l <= 1; l += 2) {
        float W[3], E[3], W0[3], Wr[3];
        ck_braco_pulso(l, W);
        {   /* pulso com o volante reto: o cotovelo acompanha só uma parte do movimento */
            float sX[3], sY[3];
            memcpy(sX, ck_X, 12); memcpy(sY, ck_Y, 12);
            float Yw[3];
            for(int e = 0; e < 3; e++) Yw[e] = ck_U[e] * 0.955f + ck_F[e] * 0.296f;
            memcpy(ck_X, ck_R, 12); memcpy(ck_Y, Yw, 12);   /* (centro do volante continua em ck_C) */
            ck_braco_pulso(l, Wr);
            memcpy(ck_X, sX, 12); memcpy(ck_Y, sY, 12);
        }
        ck_w(l * 0.155f, 0.470f, 0.470f, E);           /* cotovelo baixo, dentro do cockpit (o braço sobe de baixo) */
        for(int e = 0; e < 3; e++) E[e] += (W[e] - Wr[e]) * 0.40f;
        memcpy(W0, W, 12);
        {   /* punho da luva (cor de detalhe): anel curto alinhado ao antebraço */
            float P1[3], P0[3];
            for(int e = 0; e < 3; e++) { P0[e] = W[e] + (W[e] - E[e]) * 0.06f; P1[e] = W[e] + (E[e] - W[e]) * 0.11f; }
            pvr_prim(&hdr_ck_luva, sizeof(hdr_ck_luva));
            float Pm[3];
            for(int e = 0; e < 3; e++) Pm[e] = P0[e] + (P1[e] - P0[e]) * 0.55f;
            ck_cilindro(P0, Pm, 0.031f, 0.0305f, 8, 0, 2 * PI_F, ck_U, CK_LUVA_DET, 0.2f, 1.0f, 0.3f);   /* faixa clara */
            ck_cilindro(Pm, P1, 0.0305f, 0.030f, 8, 0, 2 * PI_F, ck_U, CK_PUNHO, 0.2f, 1.0f, 0.3f);
        }
        pvr_prim(&hdr_ck_tecido, sizeof(hdr_ck_tecido));
        uint32_t cb = cor_mistura(eu->cores[C_PRINC], 0xFFC8C8C8, 0.15f);
        /* músculo do antebraço: grosso perto do cotovelo, "barriga" no terço, fino no pulso */
        float M[3];
        for(int e = 0; e < 3; e++) M[e] = E[e] + (W0[e] - E[e]) * 0.38f + ck_U[e] * 0.006f;
        /* um tubo só (anel do meio compartilhado): sem fresta na junção */
        ck_tubo3(E, M, W0, 0.036f, 0.037f, 0.026f, 8, 0, 2 * PI_F, ck_U, cb, 0.12f, 2.0f, 4.0f, 0.50f, 1.0f, 0.78f);
        /* faixa do macacão (cor secundária) do lado de fora */
        float fora[3];
        for(int e = 0; e < 3; e++) fora[e] = ck_R[e] * l + ck_U[e] * 0.6f;
        ck_norm(fora);
        ck_tubo3(E, M, W0, 0.0367f, 0.0377f, 0.0266f, 2, -0.22f, 0.22f, fora, eu->cores[C_SEC], 0.1f, 0.5f, 4.0f, 0.55f, 1.0f, 0.8f);
    }
    /* ---- GOTAS escorrendo para trás no verniz das laterais (chuva) ---- */
    if(molhado > 0.15f) {
        pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
        int ng = (int)(36 * limitar(molhado, 0, 1));
        uint32_t cg = cor_mistura(cor_ceu, 0xFFFFFFFF, 0.55f);
        for(int g = 0; g < ng; g++) {
            int l = (g & 1) ? 1 : -1;
            float fx = 0.21f + 0.20f * aleat(g * 7 + 1);
            float vel = 0.25f + 0.5f * aleat(g * 7 + 3) + eu->v * 0.012f;
            float fz = fmodf(aleat(g * 7 + 2) * 1.0f - t_clima * vel, 1.0f); if(fz < 0) fz += 1.0f;
            float z = 0.92f - fz * 0.95f;
            float yb = fx < 0.26f ? 0.652f : 0.652f - (fx - 0.26f) * 0.34f;
            yb += z > 0.55f ? -0.050f * (z - 0.55f) / 0.38f : 0;
            float w[4][3], tam = 0.004f + 0.003f * aleat(g * 7 + 4), cau = 0.012f + eu->v * 0.0004f;
            ck_w(l * (fx - tam), yb + 0.002f, z, w[0]); ck_w(l * (fx + tam), yb + 0.002f, z, w[1]);
            ck_w(l * (fx + tam * 0.4f), yb + 0.002f, z - cau, w[2]); ck_w(l * (fx - tam * 0.4f), yb + 0.002f, z - cau, w[3]);
            uint32_t cc[4] = { cg, cg, cor_escala(cg, 0.85f), cor_escala(cg, 0.85f) };
            ck_poli(w, 4, cc);
        }
    }
    /* ---- RETROVISORES: retângulo de cantos arredondados (carenagem na cor do carro,
     * moldura escura, vidro com a imagem de trás), no alto, ao lado dos pneus ---- */
    for(int l = -1; l <= 1; l += 2) {
        const float mx = l * 0.430f, my = 0.705f, mz = 0.930f, hw = 0.088f, hh = 0.040f, prof = 0.040f;
        pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
        /* haste dupla até o alto do sidepod */
        ck_w(mx - l * 0.040f, my - hh, mz + 0.01f, A); ck_w(l * 0.300f, 0.620f, mz + 0.02f, B);
        ck_cilindro(A, B, 0.008f, 0.010f, 4, 0, 2 * PI_F, ck_F, 0xFF2A2C30, 0.4f, 0, 0);
        ck_w(mx - l * 0.010f, my - hh, mz + 0.03f, A); ck_w(l * 0.330f, 0.610f, mz + 0.05f, B);
        ck_cilindro(A, B, 0.006f, 0.008f, 4, 0, 2 * PI_F, ck_F, 0xFF2A2C30, 0.4f, 0, 0);
        /* contorno arredondado (12 pontos) */
        float cx[12], cy[12];
        for(int i = 0; i < 12; i++) {
            float a = i * 2 * PI_F / 12;
            float ca = fcos(a), sa = fsin(a);
            float ex = (ca > 0 ? 1 : -1) * powf(fabsf(ca), 0.30f), ey = (sa > 0 ? 1 : -1) * powf(fabsf(sa), 0.30f);
            cx[i] = mx + ex * hw; cy[i] = my + ey * hh;
        }
        /* lateral da carenagem (profundidade), iluminada pela normal de cada trecho */
        for(int i = 0; i < 12; i++) {
            int j = (i + 1) % 12;
            float w[4][3], n[3];
            ck_w(cx[i], cy[i], mz, w[0]); ck_w(cx[j], cy[j], mz, w[1]);
            ck_w(cx[j], cy[j], mz + prof, w[2]); ck_w(cx[i], cy[i], mz + prof, w[3]);
            float nx = (cx[i] + cx[j]) * 0.5f - mx, ny = ((cy[i] + cy[j]) * 0.5f - my) * 2.2f;
            for(int e = 0; e < 3; e++) n[e] = ck_R[e] * nx + ck_U[e] * ny;
            ck_norm(n);
            uint32_t c = ck_luz(n, eu->cores[C_PRINC], 0.9f);
            uint32_t cc[4] = { ck_verniz(w[0], n, c), ck_verniz(w[1], n, c), cor_escala(c, 0.8f), cor_escala(c, 0.8f) };
            ck_poli(w, 4, cc);
        }
        /* frente da carenagem virada para o piloto: moldura (anel) escura */
        {
            float nf[3] = { -ck_F[0], -ck_F[1], -ck_F[2] };
            uint32_t cm = ck_luz(nf, 0xFF1A1B1F, 0.4f), cr = ck_luz(nf, eu->cores[C_PRINC], 0.6f);
            for(int i = 0; i < 12; i++) {
                int j = (i + 1) % 12;
                float w[4][3];
                ck_w(cx[i], cy[i], mz, w[0]); ck_w(cx[j], cy[j], mz, w[1]);
                ck_w(mx + (cx[j] - mx) * 0.86f, my + (cy[j] - my) * 0.80f, mz + 0.004f, w[2]);
                ck_w(mx + (cx[i] - mx) * 0.86f, my + (cy[i] - my) * 0.80f, mz + 0.004f, w[3]);
                uint32_t cc[4] = { cr, cr, cm, cm };
                ck_poli(w, 4, cc);
            }
        }
        /* vidro (imagem do retrovisor), recuado na moldura */
        pvr_prim(&hdr_ck_vidro, sizeof(hdr_ck_vidro));
        {
            float u0 = l < 0 ? 0.0f : 0.5f, u1 = u0 + 0.5f;
            uint32_t cv = cor_escala(0xFFE8ECF0, limitar(0.78f + 0.3f * brilho, 0, 1.05f));
            if(molhado > 0.05f) cv = cor_mistura(cv, 0xFF9CA4AC, limitar(molhado, 0, 1) * 0.45f);   /* embaçado na chuva */
            float w[8][3], uv[8][2]; uint32_t cc[8];
            for(int k = 0; k < 8; k++) {                     /* octógono arredondado do vidro */
                int i = (k * 12) / 8;
                float gx = mx + (cx[i] - mx) * 0.86f, gy = my + (cy[i] - my) * 0.80f;
                ck_w(gx, gy, mz + 0.005f, w[k]);
                uv[k][0] = u0 + (u1 - u0) * (0.5f + (gx - mx) / (2 * hw * 0.86f));
                uv[k][1] = 0.5f - (gy - my) / (2 * hh * 0.80f);
                cc[k] = cv;
            }
            ck_poli_t(w, uv, 8, cc);
        }
    }
    /* ---- o que muda na face do volante: LEDs acesos e o visor ---- */
    pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
    {
        int acesas = (int)((rpm - 0.55f) / 0.43f * 10.0f + 0.5f);
        int pisca = rpm > 0.97f && ((int)(t_clima * 12.0f) & 1);
        for(int i = 0; i < 10 && !pisca && i < acesas; i++) {
            uint32_t c = i < 3 ? 0xFF40FF60 : i < 7 ? 0xFFFF3A28 : 0xFF4A80FF;
            float lx = -50 + i * 11.1f, ly = -51;
            ck_circ(lx, ly, 3.6f, 0.0022f, cor_mistura(c, 0xFFFFFFFF, 0.25f));
        }
        ck_leds = pisca ? 0 : (acesas > 10 ? 10 : acesas);
        float kl = limitar(0.55f + 0.45f * brilho, 0.5f, 1.0f);
        uint32_t on = 0xFF16221A, off = cor_escala(0xFF6E8862, kl);
        static int marcha_ant; static float t_troca; static int lado_troca;
        if(marcha != marcha_ant) { lado_troca = marcha > marcha_ant ? 1 : -1; marcha_ant = marcha; t_troca = 0.30f; }
        if(t_troca > 0) t_troca -= 1.0f / 60.0f;
        int inv = t_troca > 0 && ((int)(t_troca * 20.0f) & 1);
        if(inv) {                               /* troca: o número da marcha pisca invertido */
            ck_ret(-31, -35, -11, -7, 0.0020f, on);
            ck_digito(marcha <= 0 ? 10 : marcha, -28, -32, 14, 22, 3, 0.0022f, cor_escala(0xFFB8D4A8, kl), off);
        } else ck_digito(marcha <= 0 ? 10 : marcha, -28, -32, 14, 22, 3, 0.0022f, on, off);
        if(t_troca > 0) {                       /* borboleta do lado da troca acende (+ direita, - esquerda) */
            float bx = lado_troca * 100.0f;
            ck_ret(bx - 14, -68, bx + 14, -62, 0.0024f, lado_troca > 0 ? 0xFF40FF60 : 0xFFFF6030);
        }
        int v = (int)(kmh + 0.5f); if(v > 999) v = 999;
        for(int k = 0; k < 3; k++) {
            int dg = k == 0 ? v / 100 : k == 1 ? (v / 10) % 10 : v % 10;
            if(k == 0 && v < 100) dg = -1;
            if(k == 1 && v < 10) dg = -1;
            ck_digito(dg, 2 + k * 9.5f, -31, 7, 12, 1.7f, 0.0022f, on, off);
        }
        for(int k = 0; k < 6; k++)
            if(rpm > 0.55f + k * 0.075f) ck_ret(2 + k * 4.8f, -14, 5.5f + k * 4.8f, -11, 0.0022f, on);
    }
    brilho = brilho_sal;
}
/* lista translúcida: sombra das luvas na face do volante e brilho dos LEDs acesos */
/* visão DE DENTRO DO CAPACETE: a borda de cima da viseira e as laterais do
 * capacete aparecendo bem desfocadas nos cantos (como nas câmeras de capacete) */
static void desenhar_capacete(void) {
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    /* borda de cima: faixa escura curvada (mais baixa nos cantos) */
    const int N = 16;
    for(int i = 0; i < N; i++) {
        float x0 = 640.0f * i / N, x1 = 640.0f * (i + 1) / N;
        float c0 = (x0 - 320) / 320, c1 = (x1 - 320) / 320;
        float y0 = 6 + 26 * c0 * c0, y1 = 6 + 26 * c1 * c1;
        enviar_2d(x0, 0, 33.0f, 0xE0060708, 0); enviar_2d(x1, 0, 33.0f, 0xE0060708, 0);
        enviar_2d(x0, y0, 33.0f, 0xB0060708, 0); enviar_2d(x1, y1, 33.0f, 0xB0060708, 0);
        enviar_2d(x0, y0 + 16, 33.0f, 0x00060708, 0); enviar_2d(x1, y1 + 16, 33.0f, 0x00060708, 1);
    }
    /* laterais do capacete nos cantos de cima (forro escuro) */
    for(int l = 0; l < 2; l++) {
        float xb = l ? 640.0f : 0.0f, xi = l ? 580.0f : 60.0f;
        enviar_2d(xb, 0, 33.0f, 0xD0060708, 0); enviar_2d(xi, 0, 33.0f, 0x00060708, 0);
        enviar_2d(xb, 230, 33.0f, 0x00060708, 0); enviar_2d(xi, 40, 33.0f, 0x00060708, 1);
    }
    /* reflexo fraco na viseira (faixa clara inclinada) */
    uint32_t a = (uint32_t)(10 + 10 * brilho) << 24 | 0xE8F0FF;
    enviar_2d(380, 20, 33.0f, 0x00E8F0FF, 0); enviar_2d(470, 20, 33.0f, a, 0);
    enviar_2d(300, 200, 33.0f, 0x00E8F0FF, 0); enviar_2d(350, 200, 33.0f, 0x00E8F0FF, 1);
}
static void desenhar_cockpit3d_tr(void) {
    if(!ck_ok) return;
#ifdef CK_CAPACETE
    desenhar_capacete();                 /* desligado: o usuário preferiu sem a borda da viseira */
#else
    (void)desenhar_capacete;
#endif
    mat_load(&mat_cam);
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    for(int l = -1; l <= 1; l += 2) {               /* sombra suave das mãos sobre a placa */
        float w[4][3];
        ck_vw(l * 74.0f, 60.0f, 0.0006f, w[0]); ck_vw(l * 92.0f, 60.0f, 0.0006f, w[1]);
        ck_vw(l * 92.0f, -16.0f, 0.0006f, w[2]); ck_vw(l * 74.0f, -16.0f, 0.0006f, w[3]);
        const uint32_t cc[4] = { 0x00000000, 0x70000000, 0x70000000, 0x00000000 };
        ck_poli(w, 4, cc);
    }
    if(ck_leds > 0) {                                /* brilho que "vaza" dos LEDs */
        pvr_prim(&hdr_brilho_luz, sizeof(hdr_brilho_luz));
        for(int i = 0; i < ck_leds; i++) {
            uint32_t c = i < 3 ? 0x9040FF60 : i < 7 ? 0x90FF3A28 : 0x904A80FF;
            float lx = -50 + i * 11.1f, ly = 51, r = 9.0f;
            float w[4][3];
            ck_vw(lx - r, ly + r, 0.003f, w[0]); ck_vw(lx + r, ly + r, 0.003f, w[1]);
            ck_vw(lx + r, ly - r, 0.003f, w[2]); ck_vw(lx - r, ly - r, 0.003f, w[3]);
            const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
            const uint32_t cc[4] = { c, c, c, c };
            ck_poli_t(w, uv, 4, cc);
        }
    }
}
/* face do volante (lista de recorte) */
static void desenhar_cockpit3d_pt(void) {
    if(!ck_ok) return;
    mat_load(&mat_cam);
    pvr_prim(&hdr_ck_face, sizeof(hdr_ck_face));
    float w[4][3];
    ck_vw(-150, 80, 0.0f, w[0]); ck_vw(150, 80, 0.0f, w[1]); ck_vw(150, -80, 0.0f, w[2]); ck_vw(-150, -80, 0.0f, w[3]);
    float n[3] = { ck_N[0], ck_N[1], ck_N[2] };
    uint32_t c = ck_luz(n, 0xFFFFFFFF, 0.0f);
    c = cor_mistura(c, 0xFFFFFFFF, 0.25f);
    const uint32_t cc[4] = { c, c, c, c };
    const float uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    ck_poli_t(w, uv, 4, cc);
}
#endif

typedef struct { uint16_t *buf; pvr_ptr_t vram; pvr_poly_hdr_t hdr; int w, h; uint32_t hash; } painel_t;
static painel_t txt_topo, txt_baixo, txt_centro;

static void painel_criar(painel_t *p, int w, int h) {
    p->w = w; p->h = h; p->hash = 1;
    p->buf = memalign(32, w * h * 2);
    p->vram = pvr_mem_malloc(w * h * 2);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED,
                     w, h, p->vram, PVR_FILTER_NONE);
    pvr_poly_compile(&p->hdr, &cxt);
}

/* Escreve até 2 linhas; só reenvia a textura quando o texto muda. */
static void painel_texto(painel_t *p, const char *l1, const char *l2, int centralizar) {
    uint32_t h = 2166136261u;
    for(const char *c = l1; c && *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    h ^= 0x55;
    for(const char *c = l2; c && *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    if(h == p->hash) return;
    p->hash = h;
    memset(p->buf, 0, p->w * p->h * 2);
    bfont_set_foreground_color(0xFFFF);
    const char *ls[2] = { l1, l2 };
    for(int i = 0; i < 2; i++) {
        if(!ls[i] || 4 + (i + 1) * 26 > p->h) continue;
        int x = centralizar ? (p->w - 12 * (int)strlen(ls[i])) / 2 : 4;
        if(x < 0) x = 0;
        bfont_draw_str(p->buf + (4 + i * 26) * p->w + x, p->w, false, ls[i]);
    }
    pvr_txr_load(p->buf, p->vram, p->w * p->h * 2);
}

static void painel_desenhar(painel_t *p, float x, float y, uint32_t cor) {
    pvr_vertex_t v;
    pvr_prim(&p->hdr, sizeof(p->hdr));
    float xs[4] = { x, x, x + p->w, x + p->w }, ys[4] = { y + p->h, y, y + p->h, y };
    float us[4] = { 0, 0, 1, 1 }, vs[4] = { 1, 0, 1, 0 };
    for(int i = 0; i < 4; i++) {
        v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = xs[i] * XS; v.y = ys[i]; v.z = 100.0f; v.u = us[i]; v.v = vs[i];
        v.argb = cor; v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}

/* Faixa escura translúcida atrás dos textos. */
/* TONS POR CLIMA ("color grading"): um quad na tela inteira que multiplica a
 * cena (entardecer dourado, chuva azulada e fria, nublado acinzentado) */
static pvr_poly_hdr_t hdr_grade;
static void desenhar_grade(void) {
    float r = 1, g = 1, b = 1;
    float tt = limitar(tarde, 0, 1); tt = tt * tt * (3 - 2 * tt);
    float nb = limitar(nuvens, 0, 1) * (1 - chuva);
    r *= 1 - 0.00f * tt; g *= 1 - 0.07f * tt; b *= 1 - 0.17f * tt;                 /* dourado */
    r *= 1 - 0.13f * chuva; g *= 1 - 0.08f * chuva; b *= 1 - 0.01f * chuva;         /* frio */
    r *= 1 - 0.05f * nb; g *= 1 - 0.04f * nb; b *= 1 - 0.01f * nb;                  /* acinzentado */
    b *= 1 - 0.03f * (1 - tt) * (1 - nb) * (1 - chuva);                              /* sol: levemente quente */
    if(r > 0.995f && g > 0.995f && b > 0.995f) return;
    uint32_t c = 0xFF000000 | (uint32_t)(r * 255) << 16 | (uint32_t)(g * 255) << 8 | (uint32_t)(b * 255);
    pvr_prim(&hdr_grade, sizeof(hdr_grade));
    pvr_vertex_t v;
    float xs[4] = { 0, 0, 640, 640 }, ys[4] = { 480, 0, 480, 0 };
    for(int i = 0; i < 4; i++) {
        v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = xs[i] * XS; v.y = ys[i]; v.z = 35.0f; v.u = v.v = 0; v.argb = c; v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}
/* VINHETA (câmera de TV): bordas da tela um pouco mais escuras */
static void desenhar_vinheta(void) {
    pvr_prim(&hdr_grade, sizeof(hdr_grade));
    const float X0 = 0, X1 = 640, Y0 = 0, Y1 = 480, mx = 150, my = 110;
    const uint32_t B = 0xFFFFFFFF, E = 0xFFB4B4B8;
    /* 4 trapézios: borda (escura) -> miolo (neutro) */
    const float Q[4][4][2] = {
        { { X0, Y0 }, { X1, Y0 }, { X1 - mx, Y0 + my }, { X0 + mx, Y0 + my } },
        { { X0 + mx, Y1 - my }, { X1 - mx, Y1 - my }, { X1, Y1 }, { X0, Y1 } },
        { { X0, Y0 }, { X0 + mx, Y0 + my }, { X0 + mx, Y1 - my }, { X0, Y1 } },
        { { X1 - mx, Y0 + my }, { X1, Y0 }, { X1, Y1 }, { X1 - mx, Y1 - my } } };
    const uint32_t C[4][4] = { { E, E, B, B }, { B, B, E, E }, { E, B, B, E }, { B, E, E, B } };
    for(int k = 0; k < 4; k++) {
        enviar_2d(Q[k][0][0], Q[k][0][1], 34.0f, C[k][0], 0);
        enviar_2d(Q[k][1][0], Q[k][1][1], 34.0f, C[k][1], 0);
        enviar_2d(Q[k][3][0], Q[k][3][1], 34.0f, C[k][3], 0);
        enviar_2d(Q[k][2][0], Q[k][2][1], 34.0f, C[k][2], 1);
    }
}
/* LINHAS DE VELOCIDADE (o "desfoque de movimento" da época): riscos claros
 * saindo do centro para as bordas acima de ~250 km/h */
static void desenhar_velocidade(float v) {
    float k = limitar((v - 62.0f) / 25.0f, 0, 1);
    if(k <= 0.02f) return;
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    for(int i = 0; i < 18; i++) {
        float a = i * 2 * PI_F / 18 + aleat(i * 7) * 0.3f;
        float ca = fcos(a), sa = fsin(a);
        float fase = fmodf(t_clima * 2.2f + aleat(i * 13), 1.0f);
        float r0 = 230.0f + 190.0f * fase, r1 = r0 + 70.0f + 60.0f * k;
        float w = 1.2f;
        uint32_t al = (uint32_t)(60 * k * (1.0f - fase)) << 24 | 0xFFFFFF;
        float cx = 320, cy = 230;
        enviar_2d(cx + ca * r0 - sa * w, cy + sa * r0 * 0.75f + ca * w, 36.0f, al, 0);
        enviar_2d(cx + ca * r0 + sa * w, cy + sa * r0 * 0.75f - ca * w, 36.0f, al, 0);
        enviar_2d(cx + ca * r1 - sa * w, cy + sa * r1 * 0.75f + ca * w, 36.0f, al & 0xFFFFFF, 0);
        enviar_2d(cx + ca * r1 + sa * w, cy + sa * r1 * 0.75f - ca * w, 36.0f, al & 0xFFFFFF, 1);
    }
}
static void faixa_escura(float x, float y, float w, float h) {
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    pvr_vertex_t v;
    float xs[4] = { x, x, x + w, x + w }, ys[4] = { y + h, y, y + h, y };
    for(int i = 0; i < 4; i++) {
        v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = xs[i] * XS; v.y = ys[i]; v.z = 90.0f; v.u = v.v = 0;
        v.argb = 0x90000000; v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}

/* ======================================================================== */
/* HUD PRÓPRIO, MINIMAPA, REPLAY E TELA DE TÍTULO                           */
/* ======================================================================== */
/* FONTE PRÓPRIA: 5x7 desenhada aqui, ampliada 2x, inclinada (itálico de
 * corrida) e com contorno escuro, numa textura 256x64 (células 16x16,
 * ASCII 32..95). Cada letra = 1 quad. */
static const uint8_t FONTE[64][7] = {
    {0},                                              /* ' ' */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04},             /* ! */
    {0x0A,0x0A,0,0,0,0,0},                            /* " */
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},             /* # */
    {0}, {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, {0},   /* $ % & */
    {0x0C,0x04,0x08,0,0,0,0},                         /* ' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02},             /* ( */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08},             /* ) */
    {0,0x04,0x15,0x0E,0x15,0x04,0},                   /* * */
    {0,0x04,0x04,0x1F,0x04,0x04,0},                   /* + */
    {0,0,0,0,0x0C,0x04,0x08},                         /* , */
    {0,0,0,0x1F,0,0,0},                               /* - */
    {0,0,0,0,0,0x0C,0x0C},                            /* . */
    {0,0x01,0x02,0x04,0x08,0x10,0},                   /* / */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0,0x0C,0x0C,0,0x0C,0x0C,0},                      /* : */
    {0,0x0C,0x0C,0,0x0C,0x04,0x08},                   /* ; */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02},             /* < */
    {0,0,0x1F,0,0x1F,0,0},                            /* = */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08},             /* > */
    {0x0E,0x11,0x01,0x02,0x04,0,0x04},                /* ? */
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E},             /* @ */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},   /* A B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},   /* C D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},   /* E F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},   /* G H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},   /* I J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},   /* K L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},   /* M N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},   /* O P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},   /* Q R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},   /* S T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},   /* U V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},   /* W X */
    {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},   /* Y Z */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, {0,0x10,0x08,0x04,0x02,0x01,0},          /* [ \ */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, {0x04,0x0A,0x11,0,0,0,0},                /* ] ^ */
    {0,0,0,0,0,0,0x1F} };                                                         /* _ */
static pvr_poly_hdr_t hdr_fonte, hdr_mapa;
static int hud_ok;
static float mapa_x0, mapa_z0, mapa_esc;
#ifndef MAPA_ESPELHO
#define MAPA_ESPELHO 0          /* 1 inverte o mapa na vertical (Interlagos corre no sentido anti-horário) */
#endif
static void mapa_pt(float wx, float wz, float *u, float *v) {
    *u = (12.0f + (wx - mapa_x0) * mapa_esc) / 256.0f;
    *v = (12.0f + (wz - mapa_z0) * mapa_esc) / 256.0f;
    if(MAPA_ESPELHO) *v = 1.0f - *v;
}
static inline uint16_t argb4(float a, float c) {
    int A = (int)(limitar(a, 0, 1) * 15 + 0.5f), C = (int)(limitar(c, 0, 1) * 15 + 0.5f);
    return (uint16_t)(A << 12 | C << 8 | C << 4 | C);
}
static void criar_hud(void) {
    /* FONTE em alta: cada letra 5x7 vira TRAÇOS (cápsulas ligando os pontos
     * vizinhos), desenhados com borda suave numa célula 32x32; 8x8 células
     * numa textura 256x256 com MIPMAP (texto pequeno fica liso, sem serrilhar) */
    static float seg[80][4];
    memset(tex_px, 0, 256 * 256 * 2);
    for(int c = 0; c < 64; c++) {
        int ns = 0;
#define FL(b, r) ((b) >= 0 && (b) < 5 && (r) >= 0 && (r) < 7 && (FONTE[c][r] & (0x10 >> (b))))
#define FX(b, r) (5.5f + (b) * 4.3f + (6 - (r)) * 0.72f)
#define FY(r) (4.5f + (r) * 3.9f)
        for(int r = 0; r < 7; r++)
            for(int b = 0; b < 5; b++) {
                if(!FL(b, r) || ns > 76) continue;
                int liga = 0;
                const int D[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { -1, 1 } };
                for(int d = 0; d < 4; d++) {
                    int b2 = b + D[d][0], r2 = r + D[d][1];
                    if(!FL(b2, r2)) continue;
                    if(d >= 2 && (FL(b + D[d][0], r) || FL(b, r + 1))) continue;   /* diagonal só sem atalho reto */
                    seg[ns][0] = FX(b, r); seg[ns][1] = FY(r); seg[ns][2] = FX(b2, r2); seg[ns][3] = FY(r2); ns++;
                    liga = 1;
                }
                if(!liga) { seg[ns][0] = seg[ns][2] = FX(b, r); seg[ns][1] = seg[ns][3] = FY(r); ns++; }
            }
        int cx = (c & 7) * 32, cy = (c >> 3) * 32;
        for(int y = 0; y < 32; y++)
            for(int x = 0; x < 32; x++) {
                float px = x + 0.5f, py = y + 0.5f, dm = 99;
                for(int i = 0; i < ns; i++) {
                    float ax = seg[i][0], ay = seg[i][1], dx = seg[i][2] - ax, dy = seg[i][3] - ay;
                    float l2 = dx * dx + dy * dy, t = l2 > 0 ? limitar(((px - ax) * dx + (py - ay) * dy) / l2, 0, 1) : 0;
                    float ex = ax + dx * t - px, ey = ay + dy * t - py, d = ex * ex + ey * ey;
                    if(d < dm) dm = d;
                }
                dm = sqrtf(dm);
                float cv = limitar(2.15f - dm + 0.5f, 0, 1), o = limitar(4.1f - dm + 0.5f, 0, 1);
                tex_px[(cy + y) * 256 + cx + x] = argb4(fmaxf(cv, o * 0.85f), cv);
            }
#undef FL
#undef FX
#undef FY
    }
    mip_4444 = 1;
    pvr_ptr_t t = tex_subir_mip(256);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 256, 256, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
    pvr_poly_compile(&hdr_fonte, &cxt);
    /* MINIMAPA 256x256: o traçado (das fatias do jogo), linha com borda suave */
    float x1 = -1e9f, z1 = -1e9f;
    mapa_x0 = mapa_z0 = 1e9f;
    for(int k = 0; k < NF; k++) {
        mapa_x0 = fminf(mapa_x0, F[k].x); x1 = fmaxf(x1, F[k].x);
        mapa_z0 = fminf(mapa_z0, F[k].z); z1 = fmaxf(z1, F[k].z);
    }
    mapa_esc = 232.0f / fmaxf(x1 - mapa_x0, z1 - mapa_z0);
    float ox = (232.0f - (x1 - mapa_x0) * mapa_esc) * 0.5f, oz = (232.0f - (z1 - mapa_z0) * mapa_esc) * 0.5f;
    mapa_x0 -= ox / mapa_esc; mapa_z0 -= oz / mapa_esc;
    static float pt[400][2];
    static float dmin[256 * 256];
    int np = 0;
    for(int k = 0; k < NF && np < 399; k += (NF / 380 + 1)) {
        float u, v; mapa_pt(F[k].x, F[k].z, &u, &v);
        pt[np][0] = u * 256; pt[np][1] = v * 256; np++;
    }
    pt[np][0] = pt[0][0]; pt[np][1] = pt[0][1]; np++;
    for(int i = 0; i < 256 * 256; i++) dmin[i] = 99.0f;
    for(int i = 0; i + 1 < np; i++) {               /* cada trecho só mexe na sua caixa */
        float ax = pt[i][0], ay = pt[i][1], bx = pt[i + 1][0], by = pt[i + 1][1];
        int x0 = (int)(fminf(ax, bx) - 7), x1i = (int)(fmaxf(ax, bx) + 7);
        int y0 = (int)(fminf(ay, by) - 7), y1i = (int)(fmaxf(ay, by) + 7);
        if(x0 < 0) x0 = 0;
        if(y0 < 0) y0 = 0;
        if(x1i > 255) x1i = 255;
        if(y1i > 255) y1i = 255;
        float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy + 1e-6f;
        for(int y = y0; y <= y1i; y++)
            for(int x = x0; x <= x1i; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float tt = limitar(((px - ax) * dx + (py - ay) * dy) / l2, 0, 1);
                float ex = ax + dx * tt - px, ey = ay + dy * tt - py, d = sqrtf(ex * ex + ey * ey);
                if(d < dmin[y * 256 + x]) dmin[y * 256 + x] = d;
            }
    }
    for(int i = 0; i < 256 * 256; i++) {
        float d = dmin[i];
        float linha = limitar(3.0f - d, 0, 1), borda = limitar(5.6f - d, 0, 1);
        tex_px[i] = argb4(fmaxf(linha, borda * 0.75f), linha);
    }
    t = tex_subir_mip(256);
    mip_4444 = 0;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 256, 256, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
    cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
    pvr_poly_compile(&hdr_mapa, &cxt);
    hud_ok = 1;
}
static void hud_quad_tex(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z, uint32_t cor) {
    pvr_vertex_t v;
    const float xs[4] = { x0, x0, x1, x1 }, ys[4] = { y1, y0, y1, y0 };
    const float us[4] = { u0, u0, u1, u1 }, vs[4] = { v1, v0, v1, v0 };
    for(int i = 0; i < 4; i++) {
        v.flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v.x = xs[i] * XS; v.y = ys[i]; v.z = z; v.u = us[i]; v.v = vs[i];
        v.argb = cor; v.oargb = 0;
        pvr_prim(&v, sizeof(v));
    }
}
/* texto com a fonte própria; alinha 0 = esquerda, 1 = centro, 2 = direita */
static void hud_texto(float x, float y, float esc, uint32_t cor, const char *s, int alinha) {
    int n = (int)strlen(s);
    float av = 11.0f * esc;
    if(alinha == 1) x -= n * av * 0.5f;
    else if(alinha == 2) x -= n * av;
    pvr_prim(&hdr_fonte, sizeof(hdr_fonte));
    for(int i = 0; i < n; i++, x += av) {
        int c = (uint8_t)s[i];
        if(c >= 'a' && c <= 'z') c -= 32;
        if(c < 32 || c > 95) c = '?';
        if(c == ' ') continue;
        c -= 32;
        float u0 = (c & 7) / 8.0f, v0 = (c >> 3) / 8.0f;
        hud_quad_tex(x, y, x + 16 * esc, y + 16 * esc, u0, v0, u0 + 1 / 8.0f, v0 + 1 / 8.0f, 110.0f, cor);
    }
}
static void hud_caixa(float x, float y, float w, float h, uint32_t cor) {
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    uint32_t c2 = (cor & 0xFF000000) | (cor_escala(cor | 0xFF000000, 0.55f) & 0xFFFFFF);
    enviar_2d(x, y + h, 104, c2, 0); enviar_2d(x, y, 104, cor, 0);
    enviar_2d(x + w, y + h, 104, c2, 0); enviar_2d(x + w, y, 104, cor, 1);
}
static void hud_ret_z(float x, float y, float w, float h, uint32_t cor, float z) {
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    enviar_2d(x, y + h, z, cor, 0); enviar_2d(x, y, z, cor, 0);
    enviar_2d(x + w, y + h, z, cor, 0); enviar_2d(x + w, y, z, cor, 1);
}
static void hud_ret(float x, float y, float w, float h, uint32_t cor) { hud_ret_z(x, y, w, h, cor, 106); }
static void hud_minimapa(float x, float y, float tam) {
    hud_caixa(x - 3, y - 3, tam + 6, tam + 6, 0x70101418);
    pvr_prim(&hdr_mapa, sizeof(hdr_mapa));
    hud_quad_tex(x, y, x + tam, y + tam, 0, 0, 1, 1, 108.0f, 0xE0FFFFFF);
    /* marcadores POR CIMA do mapa (z maior); o jogador por cima de todos */
    for(int j = N_CARROS - 1; j >= 0; j--) {
        const fatia_t *f = fatia_em(carros[j].s);
        float u, v; mapa_pt(f->x, f->z, &u, &v);
        float r = j == 0 ? 3.2f : 2.4f, z = j == 0 ? 116.0f : 112.0f + j * 0.1f;
        float px = x + u * tam, py = y + v * tam;
        hud_ret_z(px - r - 1, py - r - 1, 2 * r + 2, 2 * r + 2, 0xFF000000, z);
        hud_ret_z(px - r, py - r, 2 * r, 2 * r, j == 0 ? 0xFFFFE040 : (carros[j].cores[C_PRINC] | 0xFF000000), z + 0.05f);
    }
}
static void fmt_tempo(char *s, size_t n, float t);
static const char *nome_trecho(float s);
static float aviso_volta;          /* mostra "BAIXO = REPLAY" depois de uma volta */
static void desenhar_hud_novo(const carro_t *eu, int posicao, int cockpit, const char *centro, float largada, int fps) {
    char t1[40], t2[24];
    const uint32_t BR = 0xFFFFFFFF, CZ = 0xFFB8C0C8, AM = 0xFFFFD840;
    /* posição */
    hud_caixa(10, 8, 81, 40, 0x90101418);
    hud_texto(15, 10, 0.56f, CZ, "POS", 0);
    snprintf(t1, sizeof(t1), "%d", posicao);
    hud_texto(15, 19, 1.66f, posicao == 1 ? AM : BR, t1, 0);
    snprintf(t1, sizeof(t1), "/%d", N_CARROS);
    hud_texto(15 + (posicao >= 10 ? 2 : 1) * 18.3f + 2, 29, 0.95f, CZ, t1, 0);
    /* volta e tempos */
    hud_caixa(95, 8, 193, 40, 0x90101418);
    snprintf(t1, sizeof(t1), "VOLTA %d", eu->volta < 0 ? 1 : eu->volta + 1);
    hud_texto(101, 10, 0.65f, CZ, t1, 0);
    fmt_tempo(t2, sizeof(t2), eu->melhor);
    snprintf(t1, sizeof(t1), "MELHOR %s", t2);
    hud_texto(283, 10, 0.65f, AM, t1, 2);
    fmt_tempo(t2, sizeof(t2), eu->t_volta);
    hud_texto(101, 23, 1.26f, BR, t2, 0);
    /* FPS */
    snprintf(t1, sizeof(t1), "%d FPS", fps);
    hud_texto(630, 10, 0.62f, fps >= 58 ? 0xFF80F0A0 : fps >= 30 ? 0xFFFFD840 : 0xFFFF5040, t1, 2);
    /* velocidade, marcha e conta-giros (o cockpit já tem o volante com isso) */
    if(!cockpit) {
        hud_caixa(508, 411, 122, 59, 0x90101418);
        float r = rotacao(eu);
        int acesos = (int)(16.0f * limitar((r - 0.22f) / 0.78f, 0, 1) + 0.5f);
        for(int i = 0; i < 16; i++) {
            uint32_t c = i < 10 ? 0xFF30E060 : i < 13 ? 0xFFF0D020 : 0xFFF03020;
            if(i >= acesos) c = 0x50404448;
            hud_ret(513 + i * 7.0f, 415, 5.4f, 6.3f, c);
        }
        hud_caixa(513, 426, 29, 38, 0xC0000000);
        snprintf(t1, sizeof(t1), "%d", eu->marcha < 1 ? 1 : eu->marcha);
        hud_texto(528, 429, 1.9f, acesos >= 14 ? 0xFFFF5030 : BR, t1, 1);
        snprintf(t1, sizeof(t1), "%d", (int)(eu->v * 3.6f));
        hud_texto(625, 426, 1.75f, BR, t1, 2);
        hud_texto(625, 455, 0.58f, CZ, "KM/H", 2);
        hud_minimapa(16, 394, 76);
    } else hud_minimapa(554, 72, 74);
    /* trecho e clima */
    snprintf(t1, sizeof(t1), "%s - %s", nome_trecho(eu->s), texto_tempo());
    hud_texto(320, cockpit ? 68 : 460, 0.62f, 0xFFFFE070, t1, 1);
    /* contagem / avisos */
    if(centro && centro[0]) {
        int num = largada > 0;
        int vai = !num && largada > -1.0f;
        float e = num ? 3.6f : vai ? 3.2f : 1.35f;
        hud_texto(320, num || vai ? 175 : 200, e, num ? 0xFFFF3020 : vai ? 0xFF30F060 : BR, centro, 1);
    }
    if(aviso_volta > 0 && !cockpit) hud_texto(320, 84, 0.78f, (((int)(aviso_volta * 3)) & 1) ? AM : BR, "VOLTA COMPLETA  -  BAIXO = REPLAY", 1);
}

/* ---------------- REPLAY: os últimos segundos gravados e mostrados com câmeras de TV ---------------- */
#define RP_HZ 30
#define RP_N (RP_HZ * 26)
#define RP_TAM ((int)offsetof(carro_t, cores))
static uint8_t rp_buf[RP_N][N_CARROS][RP_TAM] __attribute__((aligned(32)));
static int rp_cab, rp_qtd, rp_div, rp_ini, rp_len;
static int replay_on;
static float rp_t;
static carro_t rp_vivo[N_CARROS];
static void replay_gravar(void) {
    if(replay_on || ++rp_div < 2) return;
    rp_div = 0;
    for(int i = 0; i < N_CARROS; i++) memcpy(rp_buf[rp_cab][i], &carros[i], RP_TAM);
    rp_cab = (rp_cab + 1) % RP_N;
    if(rp_qtd < RP_N) rp_qtd++;
}
static int replay_iniciar(float seg) {
    int n = (int)(seg * RP_HZ);
    if(n > rp_qtd) n = rp_qtd;
    if(n < RP_HZ * 4 || replay_on) return 0;
    memcpy(rp_vivo, carros, sizeof(carros));
    rp_ini = (rp_cab - n + RP_N) % RP_N; rp_len = n; rp_t = 0; replay_on = 1;
    return 1;
}
static void replay_fim(void) {
    if(!replay_on) return;
    memcpy(carros, rp_vivo, sizeof(carros));
    replay_on = 0;
}
/* põe os carros na posição gravada (interpolando entre as amostras de 30 Hz) */
static int replay_quadro(float dt) {
    rp_t += dt * RP_HZ;
    if(rp_t >= rp_len - 1) { replay_fim(); return 0; }
    int a = (int)rp_t; float f = rp_t - a;
    int ia = (rp_ini + a) % RP_N, ib = (rp_ini + a + 1) % RP_N;
    for(int i = 0; i < N_CARROS; i++) {
        carro_t *c = &carros[i];
        memcpy(c, rp_buf[ia][i], RP_TAM);
        const carro_t *B = (const carro_t *)rp_buf[ib][i];
        float ds = B->s - c->s;
        if(ds < -COMPR * 0.5f) ds += COMPR;
        if(ds > COMPR * 0.5f) ds -= COMPR;
        c->s = fmodf(c->s + ds * f + COMPR, COMPR);
        c->x += (B->x - c->x) * f; c->psi += (B->psi - c->psi) * f;
        c->v += (B->v - c->v) * f; c->volante += (B->volante - c->volante) * f;
    }
    return 1;
}
/* CÂMERAS DE TV: postos fixos na beira da pista a cada 200 m (o carro vem,
 * passa e a próxima assume), com zoom que mantém o carro do mesmo tamanho;
 * de vez em quando a câmera do helicóptero ou a câmera em cima do carro */
static void replay_camera(const carro_t *c, float *s_ref, int *sentido, float *dist) {
    float P[3], R[3], U[3], Fr[3], pos[3], alvo[3];
    base_carro(c, P, R, U, Fr);
    int k = (int)floorf((c->s + 70.0f) / 200.0f);
    int tipo = k % 5;
    for(int e = 0; e < 3; e++) alvo[e] = P[e] + U[e] * 0.5f + Fr[e] * 1.0f;
    if(tipo == 4) {                     /* helicóptero */
        ponto_pista(c->s - 55.0f, c->x * 0.5f + 40.0f, pos, 0, 0);
        pos[1] += 42.0f;
        FOCO = 900.0f; PERTO = 1.0f;
        *s_ref = c->s - 70.0f; *sentido = 1; *dist = dist_visao;
    } else if(tipo == 2) {              /* câmera em cima do carro (T-cam) */
        for(int e = 0; e < 3; e++) { pos[e] = P[e] - Fr[e] * 0.35f + U[e] * 1.30f; alvo[e] = P[e] + Fr[e] * 14.0f + U[e] * 0.5f; }
        FOCO = 330.0f; PERTO = 0.2f;
        *s_ref = c->s; *sentido = 1; *dist = dist_visao;
    } else {                            /* posto fixo na beira */
        float s_cam = fmodf(k * 200.0f + COMPR, COMPR);
        /* do lado de DENTRO da curva (fora ficam brita, arquibancadas e muros);
         * na reta dos boxes, do lado oposto ao pit */
        float cv = fatia_em(fmodf(s_cam + 30.0f, COMPR))->curva;
        float lado = fabsf(cv) > 1.0f / 400.0f ? (cv > 0 ? 1.0f : -1.0f) : ((k & 1) ? 1.0f : -1.0f);
        if(s_cam > 3850.0f || s_cam < 400.0f) lado = 1.0f;
        ponto_pista(s_cam, lado * 13.5f, pos, 0, 0);
        pos[1] += 4.0f + 2.5f * (k % 3);
        float dx = P[0] - pos[0], dy = P[1] - pos[1], dz = P[2] - pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        FOCO = limitar(d * 16.0f, 380.0f, 2400.0f); PERTO = 1.0f;
        float ds = fmodf(c->s - s_cam + COMPR * 1.5f, COMPR) - COMPR * 0.5f;
        if(ds < 0) { *s_ref = s_cam + 30.0f; *sentido = -1; }     /* o carro vem vindo */
        else { *s_ref = s_cam - 30.0f; *sentido = 1; }            /* já passou */
        *dist = 520.0f;
    }
    camera_olhar(pos, alvo);
}
static void desenhar_hud_replay(const carro_t *eu, float t) {
    hud_caixa(10, 8, 150, 42, 0x90101418);
    if(((int)(t * 2.0f)) & 1) hud_ret(20, 21, 14, 14, 0xFFFF2020);
    hud_texto(42, 16, 1.5f, 0xFFFFFFFF, "REPLAY", 0);
    hud_caixa(150, 416, 340, 46, 0xA0101418);
    hud_ret(150, 416, 6, 46, eu->cores[C_PRINC] | 0xFF000000);
    char t1[48];
    snprintf(t1, sizeof(t1), "#7  JOGADOR  %d KM/H", (int)(eu->v * 3.6f));
    hud_texto(166, 421, 1.1f, 0xFFFFFFFF, t1, 0);
    snprintf(t1, sizeof(t1), "INTERLAGOS - %s", nome_trecho(eu->s));
    hud_texto(166, 443, 0.8f, 0xFFFFE070, t1, 0);
    hud_texto(630, 8, 0.7f, 0xFFB8C0C8, "A = VOLTAR", 2);
}
/* ---------------- TELA DE TÍTULO ---------------- */
static void desenhar_titulo(float t, int modo_clima_atual) {
    pvr_prim(&hdr_tr2d, sizeof(hdr_tr2d));
    enviar_2d(0, 205, 103, 0x00000000, 0); enviar_2d(0, 58, 103, 0xA0000000, 0);
    enviar_2d(640, 205, 103, 0x00000000, 0); enviar_2d(640, 58, 103, 0xA0000000, 1);
    hud_texto(320, 64, 2.6f, 0xFFFFFFFF, "CLASSIC RACING", 1);
    hud_texto(323, 104, 4.2f, 0xFFFFD840, "GT", 1);
    hud_ret(150, 174, 340, 3, 0xFFFFD840);
    hud_texto(320, 182, 1.1f, 0xFFE0E4E8, "DC EDITION  -  INTERLAGOS 1999", 1);
    char t1[40];
    snprintf(t1, sizeof(t1), "<  CLIMA: %s  >", NOME_MODO[modo_clima_atual]);
    hud_caixa(170, 352, 300, 30, 0x90101418);
    hud_texto(320, 359, 1.0f, 0xFFFFFFFF, t1, 1);
    if(((int)(t * 1.6f)) & 1) hud_texto(320, 398, 1.7f, 0xFFFFFFFF, "APERTE START", 1);
    hud_texto(320, 452, 0.7f, 0xFF9098A0, "FEITO PARA DREAMCAST  -  TRACADO: OPENSTREETMAP (ODBL)", 1);
}
static void fmt_tempo(char *s, size_t n, float t) {
    if(t <= 0) snprintf(s, n, "-:--.--");
    else snprintf(s, n, "%d:%05.2f", (int)t / 60, (double)fmodf(t, 60));
}

static const char *nome_trecho(float s) {
    const pista_osm_t *P = &PISTA_INTERLAGOS;
    const char *nm = "RETA DOS BOXES";
    for(int i = 0; i < P->n_curvas; i++)
        if(P->curvas[i].seg <= s) nm = P->curvas[i].nome;
    return nm;
}

/* ======================================================================== */
/* PRINCIPAL                                                                 */
/* ======================================================================== */

/* ======================================================================== */
/* TEXTURAS DO CENÁRIO (geradas por código, sem nada copiado)               */
/* ======================================================================== */
static uint16_t cinza565(int v) {
    if(v < 0) v = 0;
    if(v > 255) v = 255;
    return (uint16_t)((v >> 3) << 11 | (v >> 2) << 5 | (v >> 3));
}
static void hdr_textura(pvr_poly_hdr_t *h, pvr_ptr_t t, int w, int hgt) {
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, w, hgt, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
    pvr_poly_compile(h, &cxt);
}
/* MIPMAPS: a textura (N x N, em tex_px) + versões reduzidas pela metade até
 * 1x1, todas twiddled, no layout do PowerVR (1x1 no byte 6, depois 2x2, 4x4...).
 * À distância o chip usa a versão menor: some o "chuvisco" da grama, do
 * asfalto e da torcida, e o desenho fica mais leve para o chip. */
static inline uint32_t twid2(uint32_t x, uint32_t y) {
    uint32_t a = 0;
    for(int b = 0; b < 10; b++) a |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return a;
}
static int mip_suave;
static uint16_t suave_buf[128 * 128];
static pvr_ptr_t tex_subir_mip(int n) {
    static uint16_t buf[3 + (1024 * 1024 * 4 / 3) / 16 + 64] __attribute__((aligned(32)));   /* até 256x256 */
    static uint16_t nivel[256 * 256], menor[128 * 128];
    int total = 3;
    for(int s = 1; s <= n; s <<= 1) total += s * s;
    memset(buf, 0, ((total * 2 + 31) & ~31));
    /* nível n = tex_px (stride n) */
    for(int i = 0; i < n * n; i++) nivel[i] = tex_px[i];
    int s = n, off = total - n * n;
    for(;;) {
        for(int y = 0; y < s; y++) for(int x = 0; x < s; x++) buf[off + twid2(x, y)] = nivel[y * s + x];
        if(s == 1) break;
        int h = s / 2;
        for(int y = 0; y < h; y++)
            for(int x = 0; x < h; x++) {
                int r = 0, g = 0, b = 0, a = 0;
                if(mip_4444) {
                    for(int k = 0; k < 4; k++) {
                        uint16_t c = nivel[(2 * y + (k >> 1)) * s + 2 * x + (k & 1)];
                        int ca = c >> 12;
                        a += ca; r += ((c >> 8) & 15) * ca; g += ((c >> 4) & 15) * ca; b += (c & 15) * ca;
                    }
                    if(a) { r = (r + a / 2) / a; g = (g + a / 2) / a; b = (b + a / 2) / a; }   /* cor pesada pelo alfa */
                    menor[y * h + x] = (uint16_t)(((a + 2) / 4) << 12 | r << 8 | g << 4 | b);
                    continue;
                }
                for(int k = 0; k < 4; k++) {
                    uint16_t c = nivel[(2 * y + (k >> 1)) * s + 2 * x + (k & 1)];
                    r += c >> 11; g += (c >> 5) & 63; b += c & 31;
                }
                menor[y * h + x] = (uint16_t)(((r + 2) / 4) << 11 | ((g + 2) / 4) << 5 | ((b + 2) / 4));
            }
        if(mip_suave && !mip_4444 && h <= 128) {
            /* níveis reduzidos SUAVES (torcida): borra na horizontal e tira
             * saturação; de longe vira uma massa colorida que não pisca */
            for(int y = 0; y < h; y++)
                for(int x = 0; x < h; x++) {
                    int r = 0, g = 0, b = 0;
                    for(int k = -1; k <= 1; k++) {
                        uint16_t c = menor[y * h + ((x + k) & (h - 1))];
                        int w = k ? 1 : 2;
                        r += (c >> 11) * w; g += ((c >> 5) & 63) * w; b += (c & 31) * w;
                    }
                    r /= 4; g /= 4; b /= 4;
                    r = r * 2 * 10 / 20; g = g * 10 / 10; b = b * 2 * 10 / 20;   /* só borra (sem tirar cor) */
                    suave_buf[y * h + x] = (uint16_t)((r > 31 ? 31 : r) << 11 | (g > 63 ? 63 : g) << 5 | (b > 31 ? 31 : b));
                }
            for(int i = 0; i < h * h; i++) menor[i] = suave_buf[i];
        }
        for(int i = 0; i < h * h; i++) nivel[i] = menor[i];
        s = h; off -= s * s;
    }
    int bytes = (total * 2 + 31) & ~31;
    pvr_ptr_t t = pvr_mem_malloc(bytes);
    pvr_txr_load(buf, t, bytes);
    return t;
}
static int mip_vies = PVR_MIPBIAS_NORMAL;       /* >NORMAL: usa antes a versão menor (menos serrilhado) */
static int mip_mod;      /* 1: recebe sombra dos volumes modificadores */
static void hdr_textura_mip(pvr_poly_hdr_t *h, int n) {
    pvr_ptr_t t = tex_subir_mip(n);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, n, n, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
    cxt.txr.mipmap = PVR_MIPMAP_ENABLE;
    cxt.txr.mipmap_bias = mip_vies;
    if(mip_spec) cxt.gen.specular = PVR_SPECULAR_ENABLE;
    if(mip_mod) { cxt.gen.modifier_mode = PVR_MODIFIER_CHEAP_SHADOW; cxt.fmt.modifier = PVR_MODIFIER_ENABLE; }
    pvr_poly_compile(h, &cxt);
}
/* TEXTURAS PINTADAS NO PC (gerar_texturas.py): asfalto 256, concreto 128, torcida 256 */
__asm__(".section .rodata\n.balign 32\n.global _tex_bin\n_tex_bin:\n.incbin \"texturas.bin\"\n.text\n");
static pvr_ptr_t ultima_tex_bin;
/* textura VQ com mipmaps vinda do texturas.bin (entre ofs e fim) */
static void hdr_vq_mip(pvr_poly_hdr_t *h, int ofs, int fim, int n, int mod) {
    int bytes = (fim - ofs) & ~31;
    pvr_ptr_t t = pvr_mem_malloc(bytes);
    pvr_txr_load(tex_bin + ofs, t, bytes);
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE, n, n, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
    cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
    if(mod) { cxt.gen.modifier_mode = PVR_MODIFIER_CHEAP_SHADOW; cxt.fmt.modifier = PVR_MODIFIER_ENABLE; }
    pvr_poly_compile(h, &cxt);
}
static void hdr_bin(pvr_poly_hdr_t *h, int ofs, int w, int hh, int fmt4444, int lista, int fog, int alfa_mod, int soma) {
    memcpy(tex_px, tex_bin + ofs, w * hh * 2);
    pvr_ptr_t t = tex_subir(w, hh);
    ultima_tex_bin = t;
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, lista, (fmt4444 ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_RGB565) | PVR_TXRFMT_TWIDDLED, w, hh, t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = fog ? PVR_FOG_TABLE : PVR_FOG_DISABLE;
    if(alfa_mod) cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    if(soma == 1) { cxt.blend.src = PVR_BLEND_ONE; cxt.blend.dst = PVR_BLEND_ONE; }
    if(soma == 2) { cxt.blend.src = PVR_BLEND_SRCALPHA; cxt.blend.dst = PVR_BLEND_ONE; }
    if(soma == 3) cxt.gen.specular = PVR_SPECULAR_ENABLE;      /* névoa da distância pela cor somada */
    pvr_poly_compile(h, &cxt);
}
static void criar_texturas_novas(void) {
    hdr_bin(&hdr_cidade, TEXB_CIDADE, 512, 64, 1, PVR_LIST_TR_POLY, 0, 1, 3);
    hdr_bin(&hdr_cidade_luz, TEXB_CIDADE_LUZ, 512, 64, 0, PVR_LIST_TR_POLY, 0, 0, 1);
    hdr_bin(&hdr_marcas, TEXB_MARCAS, 32, 128, 1, PVR_LIST_TR_POLY, 1, 1, 0);
    hdr_bin(&hdr_roda_borr, TEXB_RODA, 64, 64, 0, PVR_LIST_OP_POLY, 1, 0, 0);
    hdr_bin(&hdr_sombra_carro, TEXB_SOMBRA_CARRO, 64, 64, 1, PVR_LIST_TR_POLY, 1, 1, 0);
    hdr_bin(&hdr_nuvem, TEXB_NUVEM, 64, 64, 1, PVR_LIST_TR_POLY, 1, 1, 0);
    tex_nuvem_ptr = ultima_tex_bin;
    hdr_bin(&hdr_brilho_luz, TEXB_NUVEM, 64, 64, 1, PVR_LIST_TR_POLY, 0, 1, 2);
    static const int OF[6] = { TEXB_FACHADA0, TEXB_FACHADA1, TEXB_FACHADA2, TEXB_FACHADA3, TEXB_FACHADA4, TEXB_FACHADA5 };
    for(int t = 0; t < 6; t++) {                 /* fachadas: com mipmap (somem o serrilhado de longe) */
        memcpy(tex_px, tex_bin + OF[t], 128 * 128 * 2);    /* 128x128: detalhe por janela de perto */
        mip_spec = t == 1;                       /* vidro: reflexo do céu (cor somada) */
        hdr_textura_mip(&hdr_fach[t], 128);
        mip_spec = 0;
    }
    hdr_bin(&hdr_jan_luz, TEXB_JANELA_LUZ, 64, 64, 0, PVR_LIST_TR_POLY, 0, 0, 1);
    hdr_bin(&hdr_bairros, TEXB_BAIRROS, 512, 32, 1, PVR_LIST_TR_POLY, 0, 1, 3);
    hdr_bin(&hdr_borda, TEXB_BORDA, 64, 32, 1, PVR_LIST_TR_POLY, 1, 1, 0);
    hdr_bin(&hdr_faixas, TEXB_FAIXAS, 128, 64, 1, PVR_LIST_PT_POLY, 1, 1, 0);
    hdr_bin(&hdr_nuv_mov, TEXB_NUVENS_MOV, 256, 64, 1, PVR_LIST_TR_POLY, 0, 1, 0);
    hdr_bin(&hdr_grelha, TEXB_GRELHA, 32, 16, 1, PVR_LIST_TR_POLY, 1, 1, 0);
    hdr_bin(&hdr_bairros_luz, TEXB_BAIRROS_LUZ, 512, 32, 0, PVR_LIST_TR_POLY, 0, 0, 1);
    hdr_bin(&hdr_transito, TEXB_TRANSITO, 256, 8, 0, PVR_LIST_TR_POLY, 0, 0, 1);
}
static void criar_texturas_cenario(void) {
    /* ASFALTO: granulado fino (tons de cinza claros: a cor vem do vértice) */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            uint32_t h = h32((uint32_t)(y * 64 + x) * 2654435761u + 17);
            int v = 226 + (int)(h % 30) - 18;
            if((h >> 8) % 100 < 4) v -= 40;                 /* pedrinhas escuras */
            if((h >> 16) % 100 < 2) v += 20;                /* pedrinhas claras */
            tex_px[y * 64 + x] = cinza565(v);
        }
    hdr_textura(&hdr_asfalto, tex_subir(64, 64), 64, 64);

    /* GRAMA: manchas e "tufos" (traços verticais) */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            uint32_t h = h32((uint32_t)(y * 64 + x) * 40503u + 99);
            uint32_t g = h32((uint32_t)((y >> 3) * 8 + (x >> 3)) * 977u + 5);   /* manchas de 8x8 */
            int v = 214 + (int)(g % 26) - 13 + (int)(h % 36) - 18;
            if(((x * 7 + (y >> 2) * 3) % 11) == 0) v -= 22;  /* tufos */
            tex_px[y * 64 + x] = cinza565(v);
        }
#ifdef VQ_ANTIGO
    hdr_textura_mip(&hdr_grama, 64);
#else
    hdr_vq_mip(&hdr_grama, TEXB_GRAMA_VQ, TEXB_BRITA_VQ, 256, 0);          /* grama 256 em VQ */
#endif

    /* TORCIDA em alta (128x128 = 4 m x 6 m de rampa): 8 fileiras, gente de
     * 8 px (25 cm) com cabelo/boné, rosto, camisa, braços; uns de pé com os
     * braços para cima, outros com bandeirinhas e cachecóis */
    {
        static const uint32_t CAMISA[10] = { 0x18B848, 0xFFE020, 0x2060E8, 0xFFFFFF, 0xF02828, 0xFF9010, 0xF8E838,
                                             0x9848D0, 0x10C0C0, 0x30D060 };   /* cores vivas (muito verde e amarelo) */
        static const uint32_t PELE[5] = { 0xE8B896, 0xC68A64, 0x8A5A3C, 0xF0CCAA, 0x6E4630 };
        static const uint32_t CABELO[5] = { 0x2A1E16, 0x101010, 0x6A4A2A, 0xC8A060, 0x3A3A3A };
        for(int y = 0; y < 128; y++)
            for(int x = 0; x < 128; x++) {
                int yy = y & 15;
                tex_px[y * 128 + x] = yy >= 14 ? rgb565(0xB8BCC2) : yy >= 12 ? rgb565(0x585C62) : rgb565(0x8A8E94);
            }
        for(int fil = 0; fil < 8; fil++)
            for(int col = 0; col < 16; col++) {
                uint32_t h = h32((uint32_t)(fil * 16 + col) * 7919u + 11);
                if(h % 25 < 1) continue;                               /* lugar vazio (raro) */
                int x0 = col * 8 + (int)((h >> 3) & 1), y0 = fil * 16 + (int)((h >> 4) & 1);
                uint16_t cam = rgb565(CAMISA[(h >> 8) % 10]), pel = rgb565(PELE[(h >> 12) % 5]);
                uint16_t cab = (h >> 15) % 5 == 0 ? rgb565(CAMISA[(h >> 18) % 10]) : rgb565(CABELO[(h >> 18) % 5]);   /* boné */
                #define PX(xx, yy, c) do { int _x = (xx), _y = (yy); if(_x >= 0 && _x < 128 && _y >= 0 && _y < 128) tex_px[_y * 128 + _x] = (c); } while(0)
                for(int y = 0; y < 2; y++) for(int x = 2; x < 6; x++) PX(x0 + x, y0 + y, cab);        /* cabelo */
                for(int y = 2; y < 5; y++) for(int x = 2; x < 6; x++) PX(x0 + x, y0 + y, pel);        /* rosto */
                for(int y = 5; y < 13; y++) for(int x = 0; x < 8; x++) PX(x0 + x, y0 + y, cam);       /* camisa */
                int pose = (h >> 22) % 8;
                if(pose == 0) {                                        /* braços para cima */
                    for(int y = -2; y < 6; y++) { PX(x0, y0 + y, pel); PX(x0 + 7, y0 + y, pel); }
                } else if(pose == 1) {                                 /* bandeirinha */
                    uint16_t b1 = rgb565(CAMISA[(h >> 25) % 10]);
                    for(int y = -5; y < 6; y++) PX(x0 + 7, y0 + y, rgb565(0x303030));
                    for(int y = -5; y < -1; y++) for(int x = 8; x < 12; x++) PX(x0 + x, y0 + y, b1);
                } else if(pose == 2) {                                 /* cachecol */
                    for(int x = 1; x < 7; x++) PX(x0 + x, y0 + 5, rgb565(CAMISA[(h >> 26) % 10]));
                } else {                                               /* braços do lado */
                    for(int y = 6; y < 11; y++) { PX(x0, y0 + y, pel); PX(x0 + 7, y0 + y, pel); }
                }
                #undef PX
            }
        memcpy(tex_px, tex_bin + TEXB_TORCIDA, 256 * 256 * 2);   /* versão do PC, resolução dupla */
        /* vista de lado (sempre rasante, da pista) a torcida virava chuvisco
         * piscando: os níveis reduzidos do mipmap são suavizados */
#ifndef TORCIDA_16BIT
        {   /* PALETA de 8 bits (como as multidões dos jogos da época): metade da
             * memória e leitura mais leve para o chip; cadeia de mipmaps pronta do PC */
            const uint16_t *pal = (const uint16_t *)(tex_bin + TEXB_TORCIDA_PAL);
            pvr_set_pal_format(PVR_PAL_RGB565);
            for(int i = 0; i < 256; i++) pvr_set_pal_entry(i, pal[i]);
            memcpy(pal_torcida, pal, sizeof(pal_torcida));
            int bytes = (3 + 256 * 256 + 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8 + 4 * 4 + 2 * 2 + 1 + 31) & ~31;
            pvr_ptr_t t = pvr_mem_malloc(bytes);
            pvr_txr_load(tex_bin + TEXB_TORCIDA_IDX, t, bytes);
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(0) | PVR_TXRFMT_TWIDDLED,
                             256, 256, t, PVR_FILTER_BILINEAR);
            cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
            cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_1_50;   /* um pouco mais suave: não cintila de lado */
            pvr_poly_compile(&hdr_torcida, &cxt);
            cxt.txr.mipmap_bias = PVR_MIPBIAS_3_75;                 /* LONGE: bem mais suave (sem chuvisco em movimento) */
            pvr_poly_compile(&hdr_torcida_longe, &cxt);
            pvr_ptr_t t2 = pvr_mem_malloc(bytes);                  /* 2º quadro: braços, bandeiras, pulinhos */
            pvr_txr_load(tex_bin + TEXB_TORCIDA_IDX2, t2, bytes);
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(0) | PVR_TXRFMT_TWIDDLED,
                             256, 256, t2, PVR_FILTER_BILINEAR);
            cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
            cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_1_50;
            pvr_poly_compile(&hdr_torcida2, &cxt);
        }
#else
        mip_suave = 1; mip_vies = PVR_MIPBIAS_0_50;     /* mais nítida de lado (arquibancada vista em ângulo) */
        hdr_textura_mip(&hdr_torcida, 256);
        mip_suave = 0; mip_vies = PVR_MIPBIAS_NORMAL;
#endif
    }
    /* JANELAS (64x64 = 4x4 janelas): parede branca (a cor vem do vértice),
     * vidro escuro com caixilho, algumas com cortina clara */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            int cx = x & 15, cy = y & 15;
            uint32_t h = h32((uint32_t)((y >> 4) * 4 + (x >> 4)) * 31u + 7);
            int v = 250;
            if(cx >= 3 && cx <= 12 && cy >= 3 && cy <= 11) {
                v = (h % 5 == 0) ? 170 : 78 + (int)(h % 25);                     /* vidro / cortina */
                if(cx == 7 || cx == 8 || cy == 3 || cy == 11 || cx == 3 || cx == 12) v = 215;   /* caixilho */
            }
            if(cy == 13) v = 205;                                                 /* peitoril */
            tex_px[y * 64 + x] = cinza565(v);
        }
    hdr_textura_mip(&hdr_janelas, 64);
    /* ÁGUA: ondinhas (traços claros) sobre o tom médio */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            uint32_t h = h32((uint32_t)(y * 64 + x) * 911u + 23);
            float w = sinf(x * 0.39f + sinf(y * 0.2f) * 2.0f) * sinf(y * 0.59f);
            int v = 200 + (int)(w * 22) + (int)(h % 10);
            if(w > 0.8f) v = 255;
            tex_px[y * 64 + x] = cinza565(v);
        }
    hdr_textura_mip(&hdr_agua, 64);
    /* BANDEIRAS dos países campeões da F1 (128x64, 8 de 32x32):
     * Brasil, Reino Unido, Alemanha, Finlândia / Áustria, Argentina, França, Itália */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 128; x++) {
            int pa = (y >> 5) * 4 + (x >> 5), u = x & 31, v = y & 31;
            uint32_t c = 0xFFFFFF;
            switch(pa) {
                case 0: {                                              /* Brasil */
                    c = 0x009C3B;
                    if(abs(u - 16) * 22 / 16 + abs(v - 16) < 13) c = 0xFFDF00;
                    if((u - 16) * (u - 16) + (v - 16) * (v - 16) < 30) c = 0x002776;
                    break; }
                case 1: {                                              /* Reino Unido */
                    c = 0x012169;
                    int d1 = abs(u - v), d2 = abs(u + v - 31);
                    if(d1 < 4 || d2 < 4) c = 0xFFFFFF;
                    if(d1 < 2 || d2 < 2) c = 0xC8102E;
                    if(abs(u - 16) < 5 || abs(v - 16) < 5) c = 0xFFFFFF;
                    if(abs(u - 16) < 3 || abs(v - 16) < 3) c = 0xC8102E;
                    break; }
                case 2: c = v < 11 ? 0x101010 : v < 21 ? 0xDD0000 : 0xFFCE00; break;   /* Alemanha */
                case 3: c = (abs(v - 16) < 4 || abs(u - 11) < 4) ? 0x003580 : 0xFFFFFF; break;   /* Finlândia */
                case 4: c = (v >= 11 && v < 21) ? 0xFFFFFF : 0xC8102E; break;          /* Áustria */
                case 5: c = (v >= 11 && v < 21) ? 0xFFFFFF : 0x74ACDF;                  /* Argentina */
                        if((u - 16) * (u - 16) + (v - 16) * (v - 16) < 10) c = 0xF6B40E; break;
                case 6: c = u < 11 ? 0x002395 : u < 21 ? 0xFFFFFF : 0xED2939; break;   /* França */
                default: c = u < 11 ? 0x009246 : u < 21 ? 0xFFFFFF : 0xCE2B37; break;  /* Itália */
            }
            tex_px[y * 128 + x] = rgb565(c);
        }
    hdr_textura(&hdr_bandeiras, tex_subir(128, 64), 128, 64);

    /* PILHA DE PNEUS (64x32 = 4 pneus x 2 fileiras, 2,4 m x 1,2 m): banda
     * pintada clara (a cor vem do vértice), borracha escura entre eles,
     * miolo escuro; a 2ª fileira deslocada meio pneu */
    for(int y = 0; y < 32; y++)
        for(int x = 0; x < 64; x++) {
            int fil = y >> 4, xx = (x + fil * 8) & 15, yy = y & 15;
            float dx = xx - 7.5f, dy = yy - 7.5f, r2 = dx * dx + dy * dy;
            int v = 40;                                           /* vão entre pneus */
            if(r2 < 56) v = 235 - (int)(fabsf(dy) * 4);           /* banda pintada */
            if(r2 < 20) v = 55;                                   /* miolo */
            if(r2 >= 56 && r2 < 64) v = 70;                       /* borda de borracha */
            tex_px[y * 64 + x] = cinza565(v);
        }
    hdr_textura(&hdr_pneus, tex_subir(64, 32), 64, 32);
    /* CONCRETO: placas de 4 m com junta escura, manchas e uma faixa de
     * sujeira embaixo */
    for(int y = 0; y < 32; y++)
        for(int x = 0; x < 64; x++) {
            uint32_t h = h32((uint32_t)(y * 64 + x) * 7741u + 9);
            int v = 228 + (int)(h % 18) - 9;
            if(x < 2) v = 150;                                    /* junta */
            if(y > 26) v -= 35;                                   /* sujeira */
            if(y < 2) v = 245;                                    /* borda de cima */
            tex_px[y * 64 + x] = cinza565(v);
        }
#ifdef VQ_ANTIGO
    memcpy(tex_px, tex_bin + TEXB_CONCRETO, 128 * 128 * 2);   /* versão do PC (escorridos, marcas de pneu) */
    hdr_textura_mip(&hdr_concreto, 128);
#else
    hdr_vq_mip(&hdr_concreto, TEXB_CONCRETO_VQ, TEXB_RELEVO_CONCRETO, 256, 0);   /* concreto 256 em VQ (poros, bolhas) */
#endif

    /* PLACAS DE PUBLICIDADE (256x128 = 8 placas de 128x32): marcas
     * INVENTADAS, no estilo dos patrocinadores do fim dos anos 90 (cores
     * fortes, letra inclinada, um símbolo simples). Nenhuma marca real. */
    {
        typedef struct { const char *nome; uint32_t fundo, letra, detalhe; uint8_t estilo; } marca_t;
        static const marca_t M[8] = {
            { "NITRAX",   0xC81E28, 0xFFFFFF, 0xF0C820, 0 },   /* faixa embaixo */
            { "ROTA",     0x141416, 0xF0C820, 0xF0C820, 1 },   /* rodinha à esquerda */
            { "VOAR",     0x1E4FA8, 0xFFFFFF, 0xFFFFFF, 2 },   /* asa à esquerda */
            { "CREDIX",   0x0E5A34, 0xFFFFFF, 0xD8B040, 3 },   /* quadrado dourado */
            { "HORIX",    0x101012, 0xD8B040, 0xD8B040, 4 },   /* moldura fina */
            { "GELO+",    0x6EC8F0, 0xFFFFFF, 0x14306E, 5 },   /* letra com contorno */
            { "VELTRO",   0xC8CCD2, 0x14205A, 0xC81E28, 6 },   /* risco vermelho */
            { "SINALCOM", 0x5A2A8C, 0xFFFFFF, 0xF08A1E, 7 },   /* bolinha laranja */
        };
        static uint16_t txt[128 * 32];
        for(int b = 0; b < 8; b++) {
            const marca_t *m = &M[b];
            int ox = (b & 1) * 128, oy = (b >> 1) * 32;
            uint16_t fu = rgb565(m->fundo), le = rgb565(m->letra), de = rgb565(m->detalhe);
            uint16_t so = rgb565(cor_escala(m->fundo | 0xFF000000, 0.5f));
            for(int y = 0; y < 32; y++) for(int x = 0; x < 128; x++) tex_px[(oy + y) * 256 + ox + x] = fu;
            #define PP(xx, yy, c) do { int _x = (xx), _y = (yy); if(_x >= 0 && _x < 128 && _y >= 0 && _y < 32) tex_px[(oy + _y) * 256 + ox + _x] = (c); } while(0)
            /* símbolo */
            int tx0 = 0;
            switch(m->estilo) {
                case 0: for(int x = 6; x < 122; x++) { PP(x, 28, de); PP(x, 29, de); } break;
                case 1: for(int y = 0; y < 32; y++) for(int x = 0; x < 32; x++) {
                            int r2 = (x - 15) * (x - 15) + (y - 15) * (y - 15);
                            if(r2 < 144 && r2 > 64) PP(x + 3, y, de);
                            if(r2 < 12) PP(x + 3, y, de);
                        }
                        tx0 = 18; break;
                case 2: for(int y = 0; y < 16; y++) for(int x = 0; x < 26 - y; x++) PP(6 + x + y, 8 + y, de);
                        tx0 = 16; break;
                case 3: for(int y = 6; y < 26; y++) for(int x = 6; x < 26; x++) PP(x, y, ((x + y) & 7) < 4 ? de : fu);
                        tx0 = 14; break;
                case 4: for(int x = 2; x < 126; x++) { PP(x, 2, de); PP(x, 29, de); }
                        for(int y = 2; y < 30; y++) { PP(2, y, de); PP(125, y, de); } break;
                case 6: for(int y = 0; y < 32; y++) for(int x = 0; x < 5; x++) PP(108 + x + (31 - y) / 3, y, de); break;
                case 7: for(int y = 0; y < 32; y++) for(int x = 0; x < 32; x++)
                            if((x - 15) * (x - 15) + (y - 15) * (y - 15) < 40) PP(110 + x - 15, y, de);
                        break;
                default: break;
            }
            /* nome com a fonte da BIOS (12x24), INCLINADO (itálico de corrida) */
            memset(txt, 0, sizeof(txt));
            bfont_set_foreground_color(0xFFFF);
            int w = 12 * (int)strlen(m->nome);
            int x0 = (128 - w) / 2 + tx0 / 2;
            if(x0 < 1) x0 = 1;
            bfont_draw_str(txt + 4 * 128 + x0, 128, false, m->nome);
            for(int y = 0; y < 32; y++)
                for(int x = 0; x < 128; x++) {
                    if(!txt[y * 128 + x]) continue;
                    int xi = x + (16 - y) / 4;                       /* inclinação */
                    if(m->estilo == 5) {                              /* contorno escuro */
                        PP(xi + 1, y, de); PP(xi - 1, y, de); PP(xi, y + 1, de); PP(xi, y - 1, de);
                    } else PP(xi + 1, y + 1, so);                     /* sombra */
                }
            for(int y = 0; y < 32; y++)
                for(int x = 0; x < 128; x++)
                    if(txt[y * 128 + x]) PP(x + (16 - y) / 4, y, le);
            #undef PP
        }
        /* placa 9: "FEITO PARA DREAMCAST" — homenagem só com TEXTO (fonte
         * genérica, sem logo nem cores da marca), em 256x64 na linha 128 */
        {
            uint16_t fu = rgb565(0x1C1E24), le = rgb565(0xF2F2F2), fi = rgb565(0x8A9098);
            for(int y = 128; y < 256; y++) for(int x = 0; x < 256; x++) tex_px[y * 256 + x] = fu;
            for(int x = 6; x < 250; x++) { tex_px[(128 + 8) * 256 + x] = fi; tex_px[(128 + 55) * 256 + x] = fi; }
            static uint16_t t2[256 * 64];
            memset(t2, 0, sizeof(t2));
            bfont_set_foreground_color(0xFFFF);
            const char *frase = "FEITO PARA DREAMCAST";
            bfont_draw_str(t2 + 20 * 256 + (256 - 12 * (int)strlen(frase)) / 2, 256, false, frase);
            for(int y = 0; y < 64; y++)
                for(int x = 0; x < 256; x++)
                    if(t2[y * 256 + x]) {
                        int xi = x + (32 - y) / 5;
                        if(xi >= 0 && xi < 256) tex_px[(128 + y) * 256 + xi] = le;
                    }
        }
        hdr_textura(&hdr_placas_muro, tex_subir(256, 256), 256, 256);
    }

    /* NUVEM (64x64 ARGB4444): soma de bolhas com borda macia, base achatada
     * e mais escura embaixo */
    {
        static const float B[7][3] = { { 32, 34, 17 }, { 18, 38, 12 }, { 46, 38, 12 }, { 26, 24, 12 },
                                       { 40, 25, 11 }, { 10, 42, 8 }, { 54, 42, 8 } };
        for(int y = 0; y < 64; y++)
            for(int x = 0; x < 64; x++) {
                float a = 0;
                for(int b = 0; b < 7; b++) {
                    float dx = x - B[b][0], dy = y - B[b][1], r = B[b][2];
                    float k = 1.0f - (dx * dx + dy * dy) / (r * r);
                    if(k > a) a = k;
                }
                a = limitar(a * 1.6f, 0, 1);
                if(y > 44) a *= limitar((54 - y) / 10.0f, 0, 1);        /* base achatada */
                uint32_t h = h32((uint32_t)(y * 64 + x) * 131u);
                int lum = 15 - (y > 30 ? (y - 30) / 6 : 0) - (int)(h % 2);
                if(lum < 9) lum = 9;
                int al = (int)(a * 15.0f);
                tex_px[y * 64 + x] = (uint16_t)(al << 12 | lum << 8 | lum << 4 | lum);
            }
        pvr_ptr_t t = tex_subir(64, 64);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 64, 64, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_nuvem, &cxt);
    }
    /* ALAMBRADO (32x32 ARGB4444): losangos de arame, poste à esquerda, cabo no alto */
    {
        for(int y = 0; y < 32; y++)
            for(int x = 0; x < 32; x++) {
                int a = 0, l = 8;
                if(((x + y) & 3) == 0 || ((x - y) & 3) == 0) { a = 7; l = 10; }   /* arame */
                if(x < 2) { a = 15; l = 6; }                                       /* poste */
                if(y < 1) { a = 13; l = 7; }                                       /* cabo */
                tex_px[y * 32 + x] = (uint16_t)(a << 12 | l << 8 | l << 4 | l);
            }
        pvr_ptr_t t = tex_subir(32, 32);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 32, 32, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        pvr_poly_compile(&hdr_cerca, &cxt);
    }

    /* GRAMA CORTADA (128x128 = 16 m): listras do cortador ao longo da pista
     * (metade clara, metade escura) + granulado */
    for(int y = 0; y < 128; y++)
        for(int x = 0; x < 128; x++) {
            uint32_t h = h32((uint32_t)(y * 128 + x) * 40503u + 99);
            uint32_t g = h32((uint32_t)((y >> 3) * 16 + (x >> 3)) * 977u + 5);
            int faixa = (y < 64) ? 18 : -14;
            int v = 214 + faixa + (int)(g % 20) - 10 + (int)(h % 30) - 15;
            if(((x * 7 + (y >> 2) * 3) % 13) == 0) v -= 16;
            tex_px[y * 128 + x] = cinza565(v);
        }
    mip_mod = 1; hdr_textura_mip(&hdr_corte, 128); mip_mod = 0;
    /* BRITA (64x64 = 4 m): pedrisco claro e escuro (a cor vem do vértice) */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            uint32_t h = h32((uint32_t)((y >> 1) * 64 + (x >> 1)) * 2246822519u + 7);
            int v = 205 + (int)(h % 50) - 25;
            if((h >> 8) % 7 == 0) v -= 45;
            if((h >> 12) % 9 == 0) v += 30;
            tex_px[y * 64 + x] = cinza565(v);
        }
#ifdef VQ_ANTIGO
    memcpy(tex_px, tex_bin + TEXB_BRITA, 128 * 128 * 2);   /* brita do PC: pedrinhas com luz e sombra */
    mip_mod = 1; hdr_textura_mip(&hdr_brita, 128); mip_mod = 0;
#else
    hdr_vq_mip(&hdr_brita, TEXB_BRITA_VQ, TEXB_CONCRETO_VQ, 256, 1);        /* brita 256 em VQ */
#endif
    /* ASFALTO 256x256 (16 m): granulado + REMENDOS (retângulos mais escuros ou
     * claros) + rachaduras finas */
    for(int y = 0; y < 256; y++)
        for(int x = 0; x < 256; x++) {
            uint32_t h = h32((uint32_t)(y * 256 + x) * 2654435761u + 17);
            int v = 226 + (int)(h % 30) - 18;
            if((h >> 8) % 100 < 4) v -= 40;
            if((h >> 16) % 100 < 2) v += 20;
            tex_px[y * 256 + x] = cinza565(v);
        }
    for(int r = 0; r < 9; r++) {
        uint32_t h = h32((uint32_t)r * 7919u + 3);
        int x0 = h % 256, y0 = (h >> 8) % 256, w = 20 + (int)((h >> 16) % 50), hh = 14 + (int)((h >> 24) % 40);
        int dv = (r & 1) ? -22 : 12;
        for(int y = y0; y < y0 + hh; y++)
            for(int x = x0; x < x0 + w; x++) {
                int xx = x & 255, yy = y & 255;
                int borda = (x == x0 || x == x0 + w - 1 || y == y0 || y == y0 + hh - 1);
                uint16_t c = tex_px[yy * 256 + xx];
                int g = ((c >> 5) & 63) * 255 / 63 + (borda ? -30 : dv);
                tex_px[yy * 256 + xx] = cinza565(g);
            }
    }
    for(int c = 0; c < 5; c++) {                       /* rachaduras */
        uint32_t h = h32((uint32_t)c * 131u + 11);
        float x = h % 256, y = (h >> 8) % 256, ang = ((h >> 16) % 628) * 0.01f;
        for(int k = 0; k < 70; k++) {
            ang += (((h32(c * 1000 + k) % 100) / 100.0f) - 0.5f) * 0.6f;
            x += cosf(ang); y += sinf(ang);
            int xx = (int)x & 255, yy = (int)y & 255;
            tex_px[yy * 256 + xx] = cinza565(150);
        }
    }
    {
#ifndef ASFALTO_256
        {   /* ASFALTO 512x512 em VQ (compressão do PowerVR) COM mipmaps: o dobro
             * da resolução ocupando ~88 KB (antes 256x256 = 170 KB) */
            int bytes = TEXB_RELEVO_ASFALTO - TEXB_ASFALTO_VQ;
            pvr_ptr_t t = pvr_mem_malloc(bytes);
            pvr_txr_load(tex_bin + TEXB_ASFALTO_VQ, t, bytes);
            pvr_poly_cxt_t cxt;
            pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE,
                             512, 512, t, PVR_FILTER_BILINEAR);
            cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
            cxt.txr.mipmap = PVR_MIPMAP_ENABLE; cxt.txr.mipmap_bias = PVR_MIPBIAS_NORMAL;
#ifndef SEM_BRILHO_ASF
            cxt.gen.specular = PVR_SPECULAR_ENABLE;
#endif
            cxt.gen.modifier_mode = PVR_MODIFIER_CHEAP_SHADOW; cxt.fmt.modifier = PVR_MODIFIER_ENABLE;
            pvr_poly_compile(&hdr_asfalto, &cxt);
        }
#else
        memcpy(tex_px, tex_bin + TEXB_ASFALTO, 256 * 256 * 2);
#ifndef SEM_BRILHO_ASF
        mip_spec = 1;
#endif
        mip_mod = 1; hdr_textura_mip(&hdr_asfalto, 256); mip_mod = 0;
        mip_spec = 0;
#endif
    }
    /* ZEBRA (32x32): tinta gasta, sulco de relevo no meio e sujeira nas bordas */
    for(int y = 0; y < 32; y++)
        for(int x = 0; x < 32; x++) {
            uint32_t h = h32((uint32_t)(y * 32 + x) * 977u + 41);
            int v = 238 + (int)(h % 16) - 8;
            if(x == 15 || x == 16) v -= 40;                       /* relevo */
            if(x < 2 || x > 29) v -= 30;
            if((h >> 8) % 23 == 0) v -= 50;                       /* tinta descascada */
            tex_px[y * 32 + x] = cinza565(v);
        }
    mip_mod = 1; hdr_textura_mip(&hdr_zebra, 32); mip_mod = 0;
    /* FOLHAGEM (64x64): tufos claros/escuros */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            uint32_t h = h32((uint32_t)((y >> 2) * 16 + (x >> 2)) * 2654435761u + 23);
            uint32_t h2 = h32((uint32_t)(y * 64 + x) * 40503u);
            int v = 215 + (int)(h % 50) - 25 + (int)(h2 % 20) - 10;
            if(((x + (h & 3)) & 3) == 0 && ((y + (h >> 2 & 3)) & 3) == 0) v -= 45;   /* vãos entre folhas */
            tex_px[y * 64 + x] = cinza565(v);
        }
    hdr_textura_mip(&hdr_folhas, 64);
    /* FIBRA DE CARBONO (32x32): trama 2x2 em sarja */
    for(int y = 0; y < 32; y++)
        for(int x = 0; x < 32; x++) {
            int cx = x >> 2, cy = y >> 2;
            int dir = ((cx + cy) & 1);
            int pos = dir ? (x & 3) : (y & 3);
            int v = 150 + (dir ? 70 : 30) - pos * 12;
            tex_px[y * 32 + x] = cinza565(v);
        }
    hdr_textura_mip(&hdr_carbono, 32);
    /* LETRAS DO PNEU (64x32): "RODAX" (marca fictícia) branco na borracha */
    {
        uint16_t bor = rgb565(0x2A2A2E), le = rgb565(0xE8E8E8);
        static uint16_t tt[64 * 32];
        for(int i = 0; i < 64 * 32; i++) tex_px[i] = bor;
        memset(tt, 0, sizeof(tt));
        bfont_set_foreground_color(0xFFFF);
        bfont_draw_str(tt + 4 * 64 + 2, 64, false, "RODAX");
        for(int i = 0; i < 64 * 32; i++) if(tt[i]) tex_px[i] = le;
        pvr_ptr_t t = tex_subir(64, 32);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 64, 32, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_pneu_letras, &cxt);
    }
    /* SOMBRA (64x64 ARGB4444): retângulo de cantos arredondados com borda esfumaçada */
    {
        for(int y = 0; y < 64; y++)
            for(int x = 0; x < 64; x++) {
                float dx = fabsf(x - 31.5f) / 31.5f, dy = fabsf(y - 31.5f) / 31.5f;
                float ex = limitar((dx - 0.55f) / 0.45f, 0, 1), ey = limitar((dy - 0.70f) / 0.30f, 0, 1);
                float d = sqrtf(ex * ex + ey * ey);
                int a = (int)(15.0f * limitar(1.0f - d, 0, 1));
                tex_px[y * 64 + x] = (uint16_t)(a << 12);
            }
        pvr_ptr_t t = tex_subir(64, 64);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 64, 64, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        pvr_poly_compile(&hdr_sombra, &cxt);
    }
    /* DÍGITOS PINTADOS (256x32 ARGB4444): 0-9 brancos, fundo transparente;
     * o canto direito é branco sólido (para as linhas) */
    {
        static uint16_t tt[256 * 32];
        memset(tt, 0, sizeof(tt));
        bfont_set_foreground_color(0xFFFF);
        for(int d = 0; d < 10; d++) {
            char c[2] = { (char)('0' + d), 0 };
            bfont_draw_str(tt + 4 * 256 + d * 16 + 2, 256, false, c);
        }
        for(int y = 0; y < 32; y++)
            for(int x = 0; x < 256; x++) {
                int a = tt[y * 256 + x] ? 15 : 0;
                if(x >= 248) a = 15;
                tex_px[y * 256 + x] = (uint16_t)(a << 12 | 0x0FFF);
            }
        pvr_ptr_t t = tex_subir(256, 32);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED, 256, 32, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        pvr_poly_compile(&hdr_digitos, &cxt);
    }

    /* BRILHO: textura branca com ESPECULAR ligado (a cor de brilho de cada
     * vértice é somada por cima: reflexo do sol na pintura dos carros) */
    {
        /* AMBIENTE (64x64): céu claro em cima, faixa brilhante no horizonte,
         * silhuetas e chão escuro embaixo (é MULTIPLICADO pela cor da pintura) */
        for(int y = 0; y < 64; y++)
            for(int x = 0; x < 64; x++) {
                float vv = y / 63.0f;
                int r, g, b;
                if(vv < 0.47f) { float k = vv / 0.47f; r = (int)(225 + 30 * k); g = (int)(232 + 23 * k); b = 255; }
                else if(vv < 0.53f) { r = g = b = 255; }
                else { float k = (vv - 0.53f) / 0.47f; r = g = b = (int)(205 - 85 * k); b += 6; }
                /* silhuetas (árvores/prédios) logo abaixo do horizonte */
                uint32_t h = h32((uint32_t)(x >> 2) * 2654435761u);
                if(vv >= 0.53f && vv < 0.53f + (h % 6) * 0.012f) { r = g = 150; b = 160; }
                if(b > 255) b = 255;
                tex_px[y * 64 + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
            }
        pvr_ptr_t t = tex_subir(64, 64);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 64, 64, t, PVR_FILTER_BILINEAR);
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        cxt.gen.specular = PVR_SPECULAR_ENABLE;
        pvr_poly_compile(&hdr_brilho, &cxt);
        /* TINTA (64x64 = 1 m): quase branca, leve variação, emenda de painel e rebites */
        for(int y = 0; y < 64; y++)
            for(int x = 0; x < 64; x++) {
                uint32_t hh = h32((uint32_t)(y * 64 + x) * 40503u + 3);
                int v = 246 + (int)(hh % 7) - 3 - (y > 40 ? (y - 40) / 4 : 0);
                if(x == 0 || y == 0) v = 205;                               /* emenda */
                if((x == 3 || x == 60) && (y & 7) == 4) v = 214;             /* rebites */
                tex_px[y * 64 + x] = cinza565(v);
            }
        t = tex_subir(64, 64);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 64, 64, t, PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
        pvr_poly_compile(&hdr_tinta, &cxt);
    }
    /* (torcida antiga, 64x64 — substituída acima) */
    if(0) {
        static const uint32_t CAMISA[8] = { 0x2E9A48, 0xF0C820, 0x2D5FB0, 0xEDEDED, 0xD03030, 0xF08A1E, 0x202020, 0x7A3FA0 };
        static const uint32_t PELE[4] = { 0xE8B896, 0xC68A64, 0x8A5A3C, 0xF0CCAA };
        for(int y = 0; y < 64; y++)
            for(int x = 0; x < 64; x++)
                tex_px[y * 64 + x] = (y & 15) < 13 ? rgb565(0x6E7278) : rgb565(0x9AA0A6);   /* degrau + borda */
        for(int fil = 0; fil < 4; fil++)
            for(int col = 0; col < 16; col++) {
                uint32_t h = h32((uint32_t)(fil * 16 + col) * 7919u + 3);
                if(h % 10 < 2) continue;                      /* lugar vazio */
                int x0 = col * 4, y0 = fil * 16 + 1 + (int)((h >> 4) & 1);
                uint16_t cam = rgb565(CAMISA[(h >> 8) & 7]), pel = rgb565(PELE[(h >> 12) & 3]);
                for(int y = 0; y < 4; y++) for(int x = 1; x < 3; x++) tex_px[(y0 + y) * 64 + x0 + x] = pel;    /* cabeça */
                for(int y = 4; y < 11; y++) for(int x = 0; x < 4; x++) tex_px[(y0 + y) * 64 + x0 + x] = cam;   /* camisa */
                if((h >> 20) % 6 == 0)                       /* braço levantado / bandeira */
                    for(int y = -3; y < 4; y++) tex_px[(y0 + y + 64) % 64 * 64 + x0 + 3] = cam;
            }
        hdr_textura(&hdr_torcida, tex_subir(64, 64), 64, 64);
    }

    /* PLACAS DE PUBLICIDADE: 4 desenhos abstratos em faixas de 16 px (nenhuma
     * marca nem texto): divisas, círculos, listras diagonais e ondas */
    for(int y = 0; y < 64; y++)
        for(int x = 0; x < 64; x++) {
            int d = y >> 4, yy = y & 15;
            uint32_t c;
            if(yy == 0 || yy == 15) c = 0xF0F0F0;                                   /* moldura */
            else if(d == 0) c = (((x + (yy < 8 ? yy : 15 - yy)) >> 2) % 3 == 0) ? 0xFFFFFF : 0xC81E28;   /* divisas */
            else if(d == 1) { int cx = (x & 15) - 8, cy = yy - 8; c = cx * cx + cy * cy < 22 ? 0xF0C820 : 0x1E3C9C; }
            else if(d == 2) c = (((x + yy) >> 2) & 1) ? 0xF2F2F2 : 0x1E8A3C;          /* listras */
            else { float w = 8 + 3.5f * sinf(x * 0.39f); c = fabsf(yy - w) < 2.2f ? 0xF08A1E : 0x18181C; }
            tex_px[y * 64 + x] = rgb565(c);
        }
    hdr_textura(&hdr_placas, tex_subir(64, 64), 64, 64);
}

/* MARCAS DE PNEU: faixas escuras no asfalto antes das curvas de frenagem
 * forte (pares de rodas, 1,5 m entre si), pré-calculadas no mundo. */
#define MAX_MARCAS 400
static float marca_p[MAX_MARCAS][4][3];
static float marca_s[MAX_MARCAS];
static int n_marcas;
static void montar_marcas(void) {
    const pista_osm_t *P = &PISTA_INTERLAGOS;
    for(int f = 0; f < P->n_frenagens; f++)
        for(int r = 0; r < 4; r++) {
            uint32_t h = h32((uint32_t)(f * 8 + r) * 2654435761u);
            float lat0 = -3.0f + r * 1.9f + ((h % 100) - 50) * 0.01f;
            float s_ini = (float)P->frenagens[f] - 95.0f + (h >> 8) % 30, s_fim = (float)P->frenagens[f] - 8.0f;
            for(int roda = -1; roda <= 1; roda += 2)
                for(float s0 = s_ini; s0 < s_fim && n_marcas < MAX_MARCAS; s0 += 12.0f) {
                    float s1 = s0 + 12.0f < s_fim ? s0 + 12.0f : s_fim;
                    float l0 = lat0 + roda * 0.75f + sinf(s0 * 0.05f + r) * 0.25f;
                    float l1 = lat0 + roda * 0.75f + sinf(s1 * 0.05f + r) * 0.25f;
                    float a[3], b[3], c[3], d[3];
                    ponto_pista(fmodf(s0 + COMPR, COMPR), l0 - 0.13f, a, 0, 0); ponto_pista(fmodf(s0 + COMPR, COMPR), l0 + 0.13f, b, 0, 0);
                    ponto_pista(fmodf(s1 + COMPR, COMPR), l1 + 0.13f, c, 0, 0); ponto_pista(fmodf(s1 + COMPR, COMPR), l1 - 0.13f, d, 0, 0);
                    float *q[4] = { a, b, c, d };
                    for(int v = 0; v < 4; v++) { memcpy(marca_p[n_marcas][v], q[v], 12); marca_p[n_marcas][v][1] += 0.02f; }
                    marca_s[n_marcas++] = fmodf(s0 + COMPR, COMPR);
                }
        }
}
static void desenhar_marcas(void) {
    uint32_t c = cor_escala(cor_mistura(0xFF34373C, 0xFF2A2D32, molhado), brilho);
    float s0 = carros[0].s;
    for(int i = 0; i < n_marcas; i++) {
        float ds = fmodf(marca_s[i] - s0 + COMPR + 30.0f, COMPR);
        if(ds > 230.0f) continue;                          /* de 30 m atrás a 200 m à frente */
        vv_t q[4];
        for(int v = 0; v < 4; v++) para_vista(marca_p[i][v][0], marca_p[i][v][1], marca_p[i][v][2], c, &q[v]);
        if(fora_tela(&q[0]) & fora_tela(&q[1]) & fora_tela(&q[2]) & fora_tela(&q[3])) continue;
        poligono(q, 4);
    }
}

/* PINTURA NO CHÃO (lista translúcida, textura com alfa): grid de largada
 * (colchetes + número da posição), linhas de entrada/saída do pit e o
 * número de cada garagem no piso. Quads fixos no mundo. */
#define MAX_PINT 160
static float pint_p[MAX_PINT][4][3], pint_uv[MAX_PINT][4];   /* uv: u0, v0, u1, v1 */
static uint32_t pint_cor[MAX_PINT];
static int n_pint;
static void pint_quad(float s0, float l0, float s1, float l1, float esp, uint32_t cor, const float *uv, int no_chao_box) {
    if(n_pint >= MAX_PINT) return;
    float a[3], b[3], fr[3], dir[3];
    ponto_pista(fmodf(s0 + COMPR, COMPR), l0, a, fr, dir);
    ponto_pista(fmodf(s1 + COMPR, COMPR), l1, b, 0, 0);
    float dx = b[0] - a[0], dz = b[2] - a[2], L = sqrtf(dx * dx + dz * dz) + 1e-4f;
    float nx = -dz / L * esp, nz = dx / L * esp;
    float ya = a[1] + 0.03f, yb = b[1] + 0.03f;
    if(no_chao_box) { ya += 0.0f; yb += 0.0f; }
    float q[4][3] = { { a[0] - nx, ya, a[2] - nz }, { a[0] + nx, ya, a[2] + nz }, { b[0] + nx, yb, b[2] + nz }, { b[0] - nx, yb, b[2] - nz } };
    memcpy(pint_p[n_pint], q, sizeof(q));
    if(uv) memcpy(pint_uv[n_pint], uv, 16); else { pint_uv[n_pint][0] = 0.98f; pint_uv[n_pint][1] = 0.2f; pint_uv[n_pint][2] = 0.99f; pint_uv[n_pint][3] = 0.3f; }
    pint_cor[n_pint++] = cor;
}
static void pint_numero(float s, float lat, int n, float alt) {   /* número pintado, lido de quem chega */
    char txt[4]; int len = snprintf(txt, sizeof(txt), "%d", n);
    float larg = alt * 0.55f;
    for(int i = 0; i < len; i++) {
        int d = txt[i] - '0';
        float uv[4] = { d * 0.0625f + 0.002f, 0.02f, d * 0.0625f + 0.060f, 0.98f };
        float l0 = lat + (i - (len - 1) * 0.5f) * larg;
        /* quad ao longo da pista: de s (base do número) até s+alt (topo) */
        if(n_pint >= MAX_PINT) return;
        float a[3], b[3], c[3], d2[3];
        ponto_pista(fmodf(s + COMPR, COMPR), l0 - larg * 0.5f, a, 0, 0);
        ponto_pista(fmodf(s + COMPR, COMPR), l0 + larg * 0.5f, b, 0, 0);
        ponto_pista(fmodf(s + alt + COMPR, COMPR), l0 + larg * 0.5f, c, 0, 0);
        ponto_pista(fmodf(s + alt + COMPR, COMPR), l0 - larg * 0.5f, d2, 0, 0);
        float *pp[4] = { a, b, c, d2 };
        for(int v = 0; v < 4; v++) { memcpy(pint_p[n_pint][v], pp[v], 12); pint_p[n_pint][v][1] += 0.03f; }
        memcpy(pint_uv[n_pint], uv, 16);
        pint_cor[n_pint++] = 0xFFFFFFFF;
    }
}
static void montar_pintura(void) {
    n_pint = 0;
    const uint32_t BR = 0xF0F2F2F2, AM = 0xF0F0C820;
    /* grid: colchete na frente de cada posição + número */
    for(int pos = 0; pos < N_CARROS; pos++) {
        float s0 = COMPR - 12.0f - pos * 8.0f, x = (pos & 1) ? 3.0f : -3.0f;
        pint_quad(s0 + 3.4f, x - 1.2f, s0 + 3.4f, x + 1.2f, 0.10f, BR, 0, 0);       /* barra da frente */
        pint_quad(s0 + 3.4f, x - 1.2f, s0 + 2.4f, x - 1.2f, 0.10f, BR, 0, 0);       /* pernas */
        pint_quad(s0 + 3.4f, x + 1.2f, s0 + 2.4f, x + 1.2f, 0.10f, BR, 0, 0);
        pint_numero(s0 + 3.7f, x, pos + 1, 1.1f);
    }
    /* linha de entrada do pit (antes da reta) e de saída (depois da 1ª curva) */
    for(int i = 0; i < 8; i++) {
        pint_quad(4030.0f + i * 9.0f, -7.3f - i * 0.45f, 4039.0f + i * 9.0f, -7.75f - i * 0.45f, 0.10f, BR, 0, 0);
        pint_quad(560.0f + i * 10.0f, -10.9f + i * 0.45f, 570.0f + i * 10.0f, -10.45f + i * 0.45f, 0.10f, BR, 0, 0);
    }
    /* faixa amarela de limite de velocidade na entrada do pit */
    pint_quad(4100.0f, -8.0f, 4100.0f, -21.5f, 0.25f, AM, 0, 0);
    /* número de cada garagem no piso */
    int g = 1;
    for(float s = 4045.0f; s < COMPR + 35.0f && g <= 24; s += 10.0f, g++)
        pint_numero(s - 1.0f, -25.0f, g, 1.6f);
}
/* POSTES ACESOS no entardecer / temporal: halo somado na luminária */
#define MAX_LUZES 16
static float luz_p[MAX_LUZES][3];
static int n_luzes;
static void montar_luzes(void) {
    n_luzes = 0;
    for(float s = 3990.0f; s < COMPR + 60.0f && n_luzes < MAX_LUZES; s += 45.0f) {
        ponto_pista(fmodf(s, COMPR), 25.0f, luz_p[n_luzes], 0, 0);
        luz_p[n_luzes][1] += 13.9f;
        n_luzes++;
    }
}
static void desenhar_luzes_tr(void) {
    float tt = limitar(tarde, 0, 1); tt = tt * tt * (3 - 2 * tt);
    float k = limitar(tt + 0.55f * pesada + 0.25f * nuvens * chuva, 0, 1);
    if(k < 0.04f || !n_luzes) return;
    pvr_prim(&hdr_brilho_luz, sizeof(hdr_brilho_luz));
    uint32_t c = ((uint32_t)(k * 235) << 24) | 0xFFD9A0;
    for(int i = 0; i < n_luzes; i++) {
        if(dist2_cam(luz_p[i]) > 500.0f * 500.0f) continue;
        vv_t m;
        para_vista(luz_p[i][0], luz_p[i][1], luz_p[i][2], 0, &m);
        if(m.z > -PERTO - 3.0f) continue;
        for(int halo = 0; halo < 2; halo++) {          /* miolo forte + halo largo */
            float sz = halo ? 3.4f : 1.3f;
            uint32_t cc = halo ? (((uint32_t)(k * 120) << 24) | 0xFFC070) : c;
            vvt_t q[4] = { { m.x - sz, m.y - sz, m.z, 0, 1, cc, 0 }, { m.x + sz, m.y - sz, m.z, 1, 1, cc, 0 },
                           { m.x + sz, m.y + sz, m.z, 1, 0, cc, 0 }, { m.x - sz, m.y + sz, m.z, 0, 0, cc, 0 } };
            poligono_tex(q, 4);
        }
    }
}
/* MARCAS DE FRENAGEM: riscos de borracha antes de cada freada forte (onde a
 * velocidade-limite cai muito), 2-3 "carros" por zona, cada um com as duas
 * rodas; começam escuras e somem aos poucos. Camada fina sobre o asfalto. */
#define MAX_FREIO 360
static float freio_p[MAX_FREIO][4][3], freio_v[MAX_FREIO][2];
static uint8_t freio_a[MAX_FREIO];
static int n_freio;
static void montar_marcas_freio(void) {
    n_freio = 0;
    int ultima = -1000;
    for(int k = 0; k < NF; k++) {
        if(k - ultima < 45) continue;
        float vmin = 1e9f;
        for(int j = 1; j <= 25; j++) { float v = F[(k + j) % NF].vlim; if(v < vmin) vmin = v; }
        if(F[k].vlim - vmin < 14.0f) continue;
        ultima = k;
        for(int c = 0; c < 4; c++) {
            uint32_t sem = k * 17u + c * 5u;
            float s0 = k * (float)PASSO + 4.0f + aleat(sem) * 34.0f, comp = 10.0f + aleat(sem + 1) * 22.0f;
            float lat = borracha_lat[k] + (aleat(sem + 2) - 0.5f) * 3.2f;
            uint8_t alfa = (uint8_t)(55 + 65 * aleat(sem + 3));
            int nseg = (int)(comp / 6.0f) + 1;
            for(int l = -1; l <= 1; l += 2) {
                if(aleat(sem + 20 + l) < 0.25f) continue;          /* às vezes só uma roda travou */
                float lt = lat + l * 0.78f + (aleat(sem + 9 + l) - 0.5f) * 0.1f;
                for(int g = 0; g < nseg && n_freio < MAX_FREIO; g++) {
                    float sa = s0 + comp * g / nseg, sb = s0 + comp * (g + 1) / nseg;
                    float (*q)[3] = freio_p[n_freio];
                    ponto_pista(fmodf(sa, COMPR), lt - 0.17f, q[0], 0, 0);
                    ponto_pista(fmodf(sa, COMPR), lt + 0.17f, q[1], 0, 0);
                    ponto_pista(fmodf(sb, COMPR), lt + 0.17f, q[2], 0, 0);
                    ponto_pista(fmodf(sb, COMPR), lt - 0.17f, q[3], 0, 0);
                    for(int v = 0; v < 4; v++) q[v][1] += 0.025f;
                    freio_v[n_freio][0] = g / (float)nseg; freio_v[n_freio][1] = (g + 1) / (float)nseg;
                    freio_a[n_freio++] = alfa;
                }
            }
        }
    }
}
#define MAX_BOLA 420
static float bola_p[MAX_BOLA][3], bola_r[MAX_BOLA];
static int n_bola;
static void montar_bolinhas(void) {
    n_bola = 0;
    int ultima = -1000;
    for(int k = 0; k < NF && n_bola < MAX_BOLA - 30; k++) {
        if(k - ultima < 45) continue;
        float vmin = 1e9f; int kmin = k;
        for(int j = 1; j <= 25; j++) { float v = F[(k + j) % NF].vlim; if(v < vmin) { vmin = v; kmin = (k + j) % NF; } }
        if(F[k].vlim - vmin < 14.0f) continue;
        ultima = k;
        float sg = borracha_lat[kmin] > 0 ? -1.0f : 1.0f;          /* para fora da trajetória */
        for(int b = 0; b < 30; b++) {
            uint32_t sem = kmin * 31u + b * 7u;
            float s0 = kmin * (float)PASSO - 20.0f + aleat(sem) * 70.0f;
            float lat = borracha_lat[kmin] + sg * (2.4f + aleat(sem + 1) * 3.6f);
            lat = limitar(lat, -6.6f, 6.6f);
            ponto_pista(fmodf(s0 + COMPR, COMPR), lat, bola_p[n_bola], 0, 0);
            bola_p[n_bola][1] += 0.02f;
            bola_r[n_bola++] = 0.18f + aleat(sem + 2) * 0.30f;
        }
    }
}
/* BORDA IRREGULAR entre a grama e a pista (terra, areia, tufos avançando) e
 * GRELHAS de drenagem: decalques por fatia, desenhados só nas fatias visíveis */
#define MAX_BORDA 2400
static float borda_p[MAX_BORDA][4][3], borda_u[MAX_BORDA][2];
static uint8_t borda_g[MAX_BORDA];             /* 1 = grelha */
static uint16_t borda_ini[MAX_FATIAS + 1];
static void montar_bordas(void) {
    int n = 0;
    for(int k = 0; k < NF; k++) {
        borda_ini[k] = (uint16_t)n;
        if(k % 2) continue;                        /* um quad a cada 8 m */
        for(int l = -1; l <= 1; l += 2) {
            if(n >= MAX_BORDA - 2) break;
            if(F[k].box && l < 0) continue;
            float s0 = k * (float)PASSO, s1 = s0 + 2 * PASSO;
            float (*q)[3] = borda_p[n];
            float a = l * 8.0f, b = l * 9.9f;
            ponto_pista(fmodf(s0, COMPR), a, q[0], 0, 0); ponto_pista(fmodf(s1, COMPR), a, q[1], 0, 0);
            ponto_pista(fmodf(s1, COMPR), b, q[2], 0, 0); ponto_pista(fmodf(s0, COMPR), b, q[3], 0, 0);
            for(int v = 0; v < 4; v++) q[v][1] += 0.04f - (v >= 2 ? 0.05f : 0.0f);
            borda_u[n][0] = s0 / 12.0f + l * 0.37f; borda_u[n][1] = s1 / 12.0f + l * 0.37f;
            borda_g[n++] = 0;
            if((k % 22) == (l > 0 ? 0 : 11)) {     /* grelha a cada ~90 m, lados alternados */
                float (*g)[3] = borda_p[n];
                ponto_pista(fmodf(s0 + 2.0f, COMPR), l * 7.75f, g[0], 0, 0); ponto_pista(fmodf(s0 + 3.0f, COMPR), l * 7.75f, g[1], 0, 0);
                ponto_pista(fmodf(s0 + 3.0f, COMPR), l * 8.25f, g[2], 0, 0); ponto_pista(fmodf(s0 + 2.0f, COMPR), l * 8.25f, g[3], 0, 0);
                for(int v = 0; v < 4; v++) g[v][1] += 0.045f;
                borda_u[n][0] = 0; borda_u[n][1] = 1;
                borda_g[n++] = 1;
            }
        }
    }
    borda_ini[NF] = (uint16_t)n;
}
static void desenhar_bordas_tr(void) {
    uint32_t cl = 0xFF000000 | (uint32_t)(limitar(brilho * (0.85f + 0.15f * luz_dir), 0, 1) * 255) * 0x010101u;
    for(int pass = 0; pass < 2; pass++) {
        pvr_prim(pass ? &hdr_grelha : &hdr_borda, sizeof(pvr_poly_hdr_t));
        int nj = nvis < 40 ? nvis : 40;            /* só as fatias perto (~160 m) */
        for(int j = 0; j < nj; j++) {
            int k = vis[j].k;
            for(int i = borda_ini[k]; i < borda_ini[k + 1]; i++) {
                if(borda_g[i] != pass) continue;
                vvt_t t[4]; int fora = 0;
                for(int v = 0; v < 4; v++) {
                    vv_t q;
                    para_vista(borda_p[i][v][0], borda_p[i][v][1], borda_p[i][v][2], 0, &q);
                    t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].c = cl; t[v].o = 0;
                    t[v].u = (v == 0 || v == 3) ? borda_u[i][0] : borda_u[i][1];
                    t[v].v = v < 2 ? 0.0f : 1.0f;
                    fora |= q.z > -PERTO;
                }
                if(!fora) poligono_tex(t, 4);
            }
        }
    }
}
/* FAIXAS DA TORCIDA penduradas na mureta das arquibancadas (balançam com o vento) */
#define MAX_FAIXA 90
static float faixa_p[MAX_FAIXA][4][3], faixa_o[MAX_FAIXA][2], faixa_fase[MAX_FAIXA];
static uint8_t faixa_t[MAX_FAIXA];
static int n_faixa;
static void montar_faixas(void) {
    n_faixa = 0;
    for(int q = 0; q < n_arqs && n_faixa < MAX_FAIXA; q++) {
        const arq_t *a = &arqs[q];
        uint32_t h = h32((uint32_t)q * 2654435761u + 5);
        int nf = a->meio > 9.0f ? 2 : 1;
        for(int k = 0; k < nf && n_faixa < MAX_FAIXA; k++) {
            if((h >> (k * 3)) % 3 == 0) continue;          /* nem toda arquibancada tem */
            float u = nf == 1 ? 0.0f : (k ? 0.45f : -0.45f);
            u += ((h >> (8 + k * 4)) % 7 - 3) * 0.04f;
            float L = limitar(a->meio * 0.55f, 2.5f, 5.0f);
            /* amarrada no guarda-corpo da frente (acima do muro da pista) */
            float ofs = -0.12f;
            float cx = a->f[0] + a->t[0] * u * a->meio + a->o[0] * ofs, cz = a->f[2] + a->t[2] * u * a->meio + a->o[2] * ofs;
            float y1 = a->f[1] + a->h0 + 0.98f, y0 = y1 - L * 0.22f;
            float (*p)[3] = faixa_p[n_faixa];
            p[0][0] = cx - a->t[0] * L; p[0][1] = y0; p[0][2] = cz - a->t[2] * L;
            p[1][0] = cx + a->t[0] * L; p[1][1] = y0; p[1][2] = cz + a->t[2] * L;
            p[2][0] = cx + a->t[0] * L; p[2][1] = y1; p[2][2] = cz + a->t[2] * L;
            p[3][0] = cx - a->t[0] * L; p[3][1] = y1; p[3][2] = cz - a->t[2] * L;
            faixa_o[n_faixa][0] = -a->o[0]; faixa_o[n_faixa][1] = -a->o[2];
            faixa_fase[n_faixa] = (h % 628) * 0.01f;
            faixa_t[n_faixa++] = (uint8_t)((h >> 20) & 3);
        }
    }
}
static void desenhar_faixas_tr(void) {
    if(!n_faixa) return;
    pvr_prim(&hdr_faixas, sizeof(hdr_faixas));
    uint32_t cl = 0xFF000000 | (uint32_t)(limitar((luz_base + luz_dir * 0.6f) * brilho * 1.1f, 0, 1) * 255) * 0x010101u;
    for(int i = 0; i < n_faixa; i++) {
        if(dist2_cam(faixa_p[i][0]) > 170.0f * 170.0f) continue;
        float bal = 0.07f * fsin(t_clima * 2.3f + faixa_fase[i]);            /* a barra de baixo balança */
        float v0 = faixa_t[i] * 0.25f + 0.005f, v1 = v0 + 0.24f;
        static const float U[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
        vvt_t t[4]; int fora = 0;
        for(int v = 0; v < 4; v++) {
            vv_t q;
            float dx = v < 2 ? faixa_o[i][0] * bal : 0, dz = v < 2 ? faixa_o[i][1] * bal : 0;
            para_vista(faixa_p[i][v][0] + dx, faixa_p[i][v][1], faixa_p[i][v][2] + dz, 0, &q);
            t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = v < 2 ? v1 : v0; t[v].c = cl; t[v].o = 0;
            fora |= q.z > -PERTO;
        }
        if(!fora) poligono_tex(t, 4);
    }
}
static void desenhar_marcas_tr(void) {
    desenhar_bordas_tr();
    if(n_bola) {
        pvr_prim(&hdr_nuvem, sizeof(hdr_nuvem));
        uint32_t c = ((uint32_t)(150 * (1.0f - 0.5f * molhado)) << 24) | 0x141416;
        for(int i = 0; i < n_bola; i++) {
            if(dist2_cam(bola_p[i]) > 90.0f * 90.0f) continue;
            const float *P = bola_p[i]; float r = bola_r[i];
            static const float U[4] = { 0, 1, 1, 0 }, V[4] = { 0, 0, 1, 1 }, DX[4] = { -1, 1, 1, -1 }, DZ[4] = { -1, -1, 1, 1 };
            vvt_t t[4]; int fora = 0;
            for(int v = 0; v < 4; v++) {
                vv_t q;
                para_vista(P[0] + DX[v] * r, P[1], P[2] + DZ[v] * r, 0, &q);
                t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = V[v]; t[v].c = c; t[v].o = 0;
                fora |= q.z > -PERTO;
            }
            if(!fora) poligono_tex(t, 4);
        }
    }
    if(!n_freio) return;
    pvr_prim(&hdr_marcas, sizeof(hdr_marcas));
    float k = (1.0f - 0.55f * molhado) * (0.75f + 0.25f * brilho);
    for(int i = 0; i < n_freio; i++) {
        if(dist2_cam(freio_p[i][0]) > 170.0f * 170.0f) continue;
        vvt_t t[4];
        static const float U[4] = { 0, 1, 1, 0 };
        uint32_t c = ((uint32_t)(freio_a[i] * k) << 24) | 0x0E0E10;
        int fora = 0;
        for(int v = 0; v < 4; v++) {
            vv_t q;
            para_vista(freio_p[i][v][0], freio_p[i][v][1], freio_p[i][v][2], 0, &q);
            t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = freio_v[i][v < 2 ? 0 : 1]; t[v].c = c; t[v].o = 0;
            fora |= q.z > -PERTO;
        }
        if(fora) continue;
        poligono_tex(t, 4);
    }
}
static void desenhar_pintura_tr(void) {
    if(!n_pint) return;
    pvr_prim(&hdr_digitos, sizeof(hdr_digitos));
    for(int i = 0; i < n_pint; i++) {
        float dx = pint_p[i][0][0] - cam_p[0], dz = pint_p[i][0][2] - cam_p[2];
        if(dx * dx + dz * dz > 260.0f * 260.0f) continue;
        vvt_t t[4];
        const float U[4] = { pint_uv[i][0], pint_uv[i][2], pint_uv[i][2], pint_uv[i][0] };
        const float V[4] = { pint_uv[i][3], pint_uv[i][3], pint_uv[i][1], pint_uv[i][1] };
        uint32_t c = (pint_cor[i] & 0xFF000000) | (cor_escala(pint_cor[i], brilho * (1.0f - 0.3f * molhado)) & 0xFFFFFF);
        int fora = 0;
        for(int v = 0; v < 4; v++) {
            vv_t q;
            para_vista(pint_p[i][v][0], pint_p[i][v][1], pint_p[i][v][2], 0, &q);
            t[v].x = q.x; t[v].y = q.y; t[v].z = q.z; t[v].u = U[v]; t[v].v = V[v]; t[v].c = c;
            fora |= q.z > -PERTO;
        }
        (void)fora;
        poligono_tex(t, 4);
    }
}

/* FISCAIS: um em cada posto, ao lado da casinha. Agitam a bandeira
 * AMARELA quando tem carro fora da pista ou parado logo adiante; senão
 * ficam com a bandeira abaixada. */
static void desenhar_fiscais(void) {
    for(int i = 0; i < n_postos; i++) {
        float s = postos_s[i], lat = postos_l[i] * 13.3f;
        float p[3], fr[3], dir[3];
        ponto_pista(s - 2.0f, lat, p, fr, dir);
        if(dist2_cam(p) > 150.0f * 150.0f || !na_campo(p, 4.0f)) continue;
        int amarela = 0, azul = 0;
        for(int c = 0; c < N_CARROS; c++) {
            float ds = fmodf(carros[c].s - s + COMPR, COMPR);
            if(ds < 220.0f && (carros[c].fora || carros[c].v < 12.0f) && t_clima > 6.0f) amarela = 1;
            /* AZUL: carro chegando no posto com outro (uma volta a mais) logo atrás */
            float dsc = fmodf(s - carros[c].s + COMPR, COMPR);
            if(dsc < 120.0f)
                for(int d = 0; d < N_CARROS; d++) {
                    if(d == c || carros[d].volta <= carros[c].volta) continue;
                    float g = fmodf(carros[c].s - carros[d].s + COMPR, COMPR);
                    if(g < 45.0f) azul = 1;
                }
        }
        /* VERDE: alguns segundos depois que o perigo passou */
        static float verde_ate[20];
        static uint8_t tinha_amarela[20];
        if(tinha_amarela[i] && !amarela) verde_ate[i] = t_clima + 4.0f;
        tinha_amarela[i] = (uint8_t)amarela;
        int verde = !amarela && t_clima < verde_ate[i];
        float g = chao_preciso(s - 2.0f, lat);
        float R[3] = { dir[0], 0, dir[2] }, Fr[3] = { dir[2], 0, -dir[0] };
        float cb[3] = { p[0], g + 0.65f, p[2] }, ca[3] = { p[0], g + 1.42f, p[2] };
        desenhar_caixa(cb, R, Fr, 0.22f, 0.62f, 0.14f, 0xFFF07A1C, 0xFFF07A1C);     /* macacão laranja */
        desenhar_caixa(ca, R, Fr, 0.11f, 0.12f, 0.11f, 0xFFE0B08C, 0xFFF2F2F2);     /* cabeça + capacete */
        /* bandeira: mastro na mão e o pano */
        int agita = amarela || azul || verde;
        float ang = agita ? 0.9f + 0.6f * sinf(t_clima * 9.0f + i) : -0.6f;
        float base[3] = { p[0] + R[0] * 0.28f, g + 1.0f, p[2] + R[2] * 0.28f };
        float ponta[3] = { base[0] + R[0] * 0.9f * cosf(ang) * 0.4f, base[1] + 0.9f * sinf(ang), base[2] + R[2] * 0.9f * cosf(ang) * 0.4f };
        uint32_t cp = amarela ? 0xFFFFD820 : azul ? 0xFF1E5AD8 : verde ? 0xFF20B040 : 0xFFE8E8E8;
        /* pano de 1,1 x 0,75 m em 3 faixas que ondulam */
        for(int f = 0; f < 3; f++) {
            float a0 = f / 3.0f, a1 = (f + 1) / 3.0f;
            float o0 = sinf(t_clima * 12.0f + i * 2 - f * 1.4f) * 0.16f * a0, o1 = sinf(t_clima * 12.0f + i * 2 - (f + 1) * 1.4f) * 0.16f * a1;
            uint32_t cf = cor_escala(cp, 0.82f + 0.18f * (f & 1));
            vv_t q[4];
            para_vista(ponta[0] + Fr[0] * 1.1f * a0, ponta[1] + o0, ponta[2] + Fr[2] * 1.1f * a0, cf, &q[0]);
            para_vista(ponta[0] + Fr[0] * 1.1f * a1, ponta[1] + o1, ponta[2] + Fr[2] * 1.1f * a1, cf, &q[1]);
            para_vista(ponta[0] + Fr[0] * 1.1f * a1, ponta[1] - 0.75f + o1, ponta[2] + Fr[2] * 1.1f * a1, cf, &q[2]);
            para_vista(ponta[0] + Fr[0] * 1.1f * a0, ponta[1] - 0.75f + o0, ponta[2] + Fr[2] * 1.1f * a0, cf, &q[3]);
            poligono(q, 4);
        }
        vv_t m[4];
        para_vista(base[0], base[1], base[2], 0xFF303030, &m[0]); para_vista(base[0] + 0.03f, base[1], base[2] + 0.03f, 0xFF303030, &m[1]);
        para_vista(ponta[0] + 0.03f, ponta[1], ponta[2] + 0.03f, 0xFF303030, &m[2]); para_vista(ponta[0], ponta[1], ponta[2], 0xFF303030, &m[3]);
        poligono(m, 4);
    }
}

/* FAÍSCAS: o assoalho raspa no chão nas compressões (fundo de descida) */
#define N_FAISCAS 96
static float fa_p[N_FAISCAS][3], fa_v[N_FAISCAS][3], fa_vida[N_FAISCAS];
static int fa_prox;
static void faiscas_atualizar(float dt) {
    for(int i = 0; i < N_FAISCAS; i++) {
        if(fa_vida[i] <= 0) continue;
        fa_vida[i] -= dt;
        fa_v[i][1] -= 9.8f * dt;
        for(int e = 0; e < 3; e++) fa_p[i][e] += fa_v[i][e] * dt;
    }
    for(int c = 0; c < N_CARROS; c++) {
        const carro_t *cr = &carros[c];
        if(cr->v < 48.0f || cr->fora) continue;
        int k = (int)(fmodf(cr->s + COMPR, COMPR) / PASSO) % NF;
        float dr = (F[(k + 1) % NF].rampa - F[(k + NF - 1) % NF].rampa) / (2.0f * PASSO);
        if(cr->v * cr->v * dr < 9.0f) continue;              /* compressão forte (g vertical) */
        float P[3], R[3], U[3], Fr[3];
        base_carro(cr, P, R, U, Fr);
        if(dist2_cam(P) > 120.0f * 120.0f) continue;
        for(int n = 0; n < 2; n++) {
            int i = fa_prox; fa_prox = (fa_prox + 1) % N_FAISCAS;
            uint32_t h = h32((uint32_t)(t_clima * 1000) + i * 77 + c);
            float lado = ((h & 255) / 255.0f - 0.5f) * 0.9f;
            for(int e = 0; e < 3; e++) fa_p[i][e] = P[e] + Fr[e] * (-1.1f - 0.6f * ((h >> 8 & 255) / 255.0f)) + R[e] * lado;
            fa_p[i][1] = P[1] + 0.06f;
            float vel = cr->v * (0.55f + 0.2f * ((h >> 16 & 255) / 255.0f));
            for(int e = 0; e < 3; e++) fa_v[i][e] = Fr[e] * vel + R[e] * lado * 6.0f;
            fa_v[i][1] = 1.0f + 2.5f * ((h >> 24) / 255.0f);
            fa_vida[i] = 0.18f + 0.18f * ((h >> 4 & 15) / 15.0f);
        }
    }
}
static void desenhar_faiscas_tr(void) {
    pvr_prim(&hdr_fumaca, sizeof(hdr_fumaca));
    for(int i = 0; i < N_FAISCAS; i++) {
        if(fa_vida[i] <= 0) continue;
        vv_t a, b;
        para_vista(fa_p[i][0], fa_p[i][1], fa_p[i][2], 0, &a);
        para_vista(fa_p[i][0] - fa_v[i][0] * 0.025f, fa_p[i][1] - fa_v[i][1] * 0.025f, fa_p[i][2] - fa_v[i][2] * 0.025f, 0, &b);
        if(a.z > -0.5f || b.z > -0.5f) continue;
        uint32_t al = (uint32_t)(limitar(fa_vida[i] / 0.25f, 0, 1) * 255) << 24;
        float esp = 0.025f;
        vv_t q[4] = { a, b, b, a };
        q[0].y += esp; q[1].y += esp; q[2].y -= esp; q[3].y -= esp;
        q[0].c = al | 0xFFF4C0; q[1].c = al | 0xFF9A30; q[2].c = al | 0xFF9A30; q[3].c = al | 0xFFF4C0;
        poligono(q, 4);
    }
}

/* Helicóptero da TV: circula sobre o trecho onde está o jogador. */
static void desenhar_helicoptero(void) {
    /* O centro do voo é um ponto da PISTA 70 m à frente do jogador (não a
     * direção do carro: antes ele girava junto quando o carro virava). O
     * helicóptero persegue esse ponto com velocidade limitada e circula. */
    static float hc[3], t_ant = -1;
    float alvo[3], fr[3], dir[3];
    ponto_pista(fmodf(carros[0].s + 110.0f, COMPR), 0, alvo, fr, dir);
    float dt = t_ant < 0 ? 0 : t_clima - t_ant;
    if(dt < 0 || dt > 0.5f) dt = 0;
    if(t_ant < 0) memcpy(hc, alvo, 12);
    t_ant = t_clima;
    for(int e = 0; e < 3; e += 2) {
        float d = alvo[e] - hc[e];
        float passo = limitar(d * 1.8f * dt, -120.0f * dt, 120.0f * dt);   /* acompanha os carros (até 430 km/h) */
        hc[e] += passo;
    }
    hc[1] += (alvo[1] - hc[1]) * limitar(0.5f * dt, 0, 1);
    float th = t_clima * 0.11f;
    float c[3] = { hc[0] + cosf(th) * 60.0f, hc[1] + 45.0f + 4.0f * sinf(t_clima * 0.3f), hc[2] + sinf(th) * 60.0f };
    if(!na_campo(c, 15.0f)) return;
    float hr[3] = { cosf(th), 0, sinf(th) }, hf[3] = { -sinf(th), 0, cosf(th) };   /* voa na tangente do círculo */
    static const float UP[3] = { 0, 1, 0 };
    (void)UP;
    desenhar_caixa(c, hr, hf, 0.85f, 0.85f, 1.7f, 0xFF1E3A6E, 0xFFE8E8E8);                 /* cabine */
    float nz[3] = { c[0] + hf[0] * 1.6f, c[1] + 0.1f, c[2] + hf[2] * 1.6f };
    desenhar_caixa(nz, hr, hf, 0.7f, 0.55f, 0.35f, 0xFF20242C, 0xFF20242C);                 /* vidro */
    float cauda[3] = { c[0] - hf[0] * 3.6f, c[1] + 0.35f, c[2] - hf[2] * 3.6f };
    desenhar_caixa(cauda, hr, hf, 0.2f, 0.22f, 2.1f, 0xFF1E3A6E, 0xFFE8E8E8);               /* cauda */
    float der[3] = { c[0] - hf[0] * 5.6f, c[1] + 0.9f, c[2] - hf[2] * 5.6f };
    desenhar_caixa(der, hr, hf, 0.06f, 0.7f, 0.35f, 0xFFE8E8E8, 0xFFE8E8E8);                /* deriva */
    for(int l = -1; l <= 1; l += 2) {                                                       /* esquis */
        float e[3] = { c[0] + hr[0] * l * 0.8f, c[1] - 1.25f, c[2] + hr[2] * l * 0.8f };
        desenhar_caixa(e, hr, hf, 0.05f, 0.05f, 1.6f, 0xFF30343A, 0xFF30343A);
    }
    /* rotor: duas pás girando rápido */
    float ang = t_clima * 27.0f;
    for(int k = 0; k < 2; k++) {
        float a = ang + k * PI_F / 2;
        float rr[3] = { cosf(a), 0, sinf(a) }, rf[3] = { -sinf(a), 0, cosf(a) };
        float m[3] = { c[0], c[1] + 1.05f, c[2] };
        desenhar_caixa(m, rr, rf, 5.2f, 0.03f, 0.14f, 0xFF404448, 0xFF404448);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    {   /* opacos, MODIFICADORES opacos (sombras de verdade), translúcidos e
         * RECORTE ("punch-through": árvores e faixas, sem custo de ordenação) */
        pvr_init_params_t pp = { .opb_sizes = {
#ifdef OPB32
                                 PVR_BINSIZE_32,
#else
                                 PVR_BINSIZE_16,
#endif
                                 PVR_BINSIZE_16, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
                                 .vertex_buf_size = 512 * 1024, .opb_overflow_count = 3,
#ifdef DMA_VERTICES
                                 .dma_enabled = 1,
#endif
#ifndef SEM_FSAA
                                 .fsaa_enabled = 1
#endif
                               };
        pvr_init(&pp);
#ifdef DMA_VERTICES
        {   /* buffers de DMA na RAM (o dobro do que um quadro usa: frente e fundo) */
            static const pvr_list_t L[5] = { PVR_LIST_OP_POLY, PVR_LIST_OP_MOD, PVR_LIST_TR_POLY, PVR_LIST_TR_MOD, PVR_LIST_PT_POLY };
            static const int T[5] = { 1024 * 1024, 64 * 1024, 512 * 1024, 128, 128 * 1024 };
            for(int i = 0; i < 5; i++) pvr_set_vertbuf(L[i], memalign(32, T[i]), T[i]);
        }
#endif
#ifndef SEM_FSAA
        XS = 2.0f;
#endif
        PVR_SET(PVR_PT_ALPHA_REF, 0x50);         /* recorte: alfa abaixo de ~30% some */
    }
    pvr_set_bg_color(0.78f, 0.82f, 0.84f);

    float nl = 1.0f / sqrtf(LUZ[0] * LUZ[0] + LUZ[1] * LUZ[1] + LUZ[2] * LUZ[2]);
    LUZ[0] *= nl; LUZ[1] *= nl; LUZ[2] *= nl;

    som_iniciar();
    montar_pista();
    montar_terreno();
    montar_carro();
    montar_cenario();
    montar_nuvens();
    montar_pocas();
    iniciar_carros();
    if(modo_clima == CLIMA_CHUVA || modo_clima == CLIMA_PESADA) { nuvens = 1; chuva = 1; molhado = 1; }   /* já começa encharcado */
    if(modo_clima == CLIMA_PESADA) pesada = 1;
    if(modo_clima == CLIMA_SOL) nuvens = 0.05f;
    if(modo_clima == CLIMA_ENSOLARADO) { nuvens = 0; manha = 1; }
    if(modo_clima == CLIMA_ENTARDECER) { nuvens = 0.1f; tarde = 1; }
    printf("pista: %d fatias, %.0f m | predios %d, arvores %d\n", NF, (double)COMPR, n_predios, n_arvores);

    /* cabeçalhos: mundo COM neblina de tabela; longe e 2D sem neblina */
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.fog_type = PVR_FOG_TABLE;
    pvr_poly_compile(&hdr_mundo, &cxt);
    criar_texturas_carro();
    criar_texturas_cenario();
    criar_texturas_novas();
    criar_textura_arvores();
#if SKYBOX
    carregar_ceu();
    montar_anel();
    assar_luz();
    montar_luzes();
    montar_marcas_freio();
    montar_bolinhas();
    montar_bordas();
    ligar_arquibancadas();
    montar_faixas();
    montar_tipos_predio();
    grade_montar(&gr_p, n_predios, pos_predio);
    grade_montar(&gr_a, n_arvores, pos_arvore);
    montar_nuv_tab();
    for(int k = 0; k < NF; k++) nuv_f[k] = 255;
#endif
    montar_marcas();
    montar_pintura();
    cxt.gen.fog_type = PVR_FOG_DISABLE;
    pvr_poly_compile(&hdr_longe, &cxt);
    pvr_poly_compile(&hdr_2d, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    pvr_poly_compile(&hdr_tr2d, &cxt);
    {
        pvr_poly_cxt_t cg;
        pvr_poly_cxt_col(&cg, PVR_LIST_TR_POLY);
        cg.blend.src = PVR_BLEND_DESTCOLOR; cg.blend.dst = PVR_BLEND_ZERO;    /* cor da tela x cor do quad */
        cg.gen.fog_type = PVR_FOG_DISABLE; cg.gen.culling = PVR_CULLING_NONE;
        pvr_poly_compile(&hdr_grade, &cg);
    }
    cxt.gen.fog_type = PVR_FOG_TABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_fumaca, &cxt);
    pvr_fog_table_color(1.0f, ((COR_NEBLINA >> 16) & 0xFF) / 255.0f, ((COR_NEBLINA >> 8) & 0xFF) / 255.0f,
                        (COR_NEBLINA & 0xFF) / 255.0f);
    pvr_fog_table_linear(NEBLINA_INI, NEBLINA_FIM);

    tex_espelho = pvr_mem_malloc(ESP_W * ESP_H * 2);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED,
                     ESP_W, ESP_H, tex_espelho, PVR_FILTER_BILINEAR);
    pvr_poly_compile(&hdr_espelho, &cxt);

    tex_telao = pvr_mem_malloc(256 * 128 * 2);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 256, 128, tex_telao, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_TABLE;
    pvr_poly_compile(&hdr_telao, &cxt);
    tex_refl = pvr_mem_malloc(256 * 256 * 2);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 256, 256, tex_refl, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE; cxt.gen.fog_type = PVR_FOG_DISABLE;
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    pvr_poly_compile(&hdr_refl, &cxt);
    painel_criar(&txt_topo, 512, 64);
    painel_criar(&txt_baixo, 512, 32);
    painel_criar(&txt_centro, 256, 32);

    int camera = CAM_INI;
    float largada = S_INI >= 0 ? 0.0f : 3.5f;       /* contagem regressiva */
    float volante_j = 0;
    uint32_t botoes_ant = 0;
    int fps = 0, quadros = 0, vert_med = 0;
    uint64_t t_fps = timer_ms_gettime64();
    float t_demo = 0;
#ifndef SEM_CARRO_OBJ
    criar_carro_obj();
    if(obj_ok) obj_atribuir();
    else
#endif
    assar_carros_tex();
    criar_hud();
#ifdef COCKPIT_2D
    criar_volante_tex();
#else
    criar_cockpit3d();
#endif
    int quadro_n = 0;
    /* TELA DE TÍTULO antes da corrida (pulada nos testes com S_INI) */
#ifdef SEM_TITULO
    int titulo = 0;
#else
    int titulo = S_INI < 0;
#endif
    float t_titulo = 0, t_replay = 0;
    int cam_antes = camera, volta_ant = -1;
    float aviso_clima = 0, aviso_det = 0;
    int espelho_pronto = 0;
    const float dt = 1.0f / 60.0f;

    for(;;) {
        /* ---------------- entrada ---------------- */
        float acel = 0, freio = 0, eixo = 0;
        static int eixo_analogico;
        uint32_t b = 0;
        maple_device_t *cont = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        if(cont) {
            cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
            if(st) {
                b = st->buttons;
                if(abs(st->joyx) > 12) {
                    /* curva do analógico: fino perto do centro, total no fim */
                    float e = limitar((abs(st->joyx) - 12) / 114.0f, 0, 1);
                    e = e * (0.35f + 0.65f * e);
                    eixo = st->joyx > 0 ? e : -e;
                    eixo_analogico = 1;
                } else eixo_analogico = 0;
                if(b & CONT_DPAD_LEFT)  { eixo = -1; eixo_analogico = 0; }
                if(b & CONT_DPAD_RIGHT) { eixo = 1;  eixo_analogico = 0; }
                /* acelerador analógico no gatilho R (A = pé no fundo) */
                acel = limitar((st->rtrig - 10) / 220.0f, 0, 1);
                if(b & CONT_A) acel = 1.0f;
                freio = (b & CONT_B) ? 1.0f : st->ltrig / 255.0f;
                if((b & CONT_START) && (b & CONT_A) && (b & CONT_B) && (b & CONT_X) && (b & CONT_Y)) break;
            }
        }
        uint32_t apertou = b & ~botoes_ant;
        botoes_ant = b;
        if(titulo) {
            /* título: START começa, esquerda/direita escolhem o clima */
            t_titulo += 1.0f / 60.0f;
            if(apertou & CONT_DPAD_RIGHT) { modo_clima = (modo_clima + 1) % N_MODOS_CLIMA; som_bip(0); }
            if(apertou & CONT_DPAD_LEFT) { modo_clima = (modo_clima + N_MODOS_CLIMA - 1) % N_MODOS_CLIMA; som_bip(0); }
#ifndef SEM_CARRO_OBJ
            if(obj_ok && (apertou & (CONT_DPAD_UP | CONT_DPAD_DOWN))) {    /* carro do jogador: original ou uma das 11 equipes */
                int nv = OBJ_NPINT + 1;
                jogador_obj = (jogador_obj + ((apertou & CONT_DPAD_DOWN) ? 1 : nv - 1)) % nv;
                if(jogador_obj == 0) {                     /* volta às cores originais do jogador */
                    const pintura_t *pp = &PINTURAS[0];
                    carros[0].cores[C_PRINC] = pp->princ; carros[0].cores[C_SEC] = pp->sec; carros[0].cores[C_TERC] = pp->terc;
                    carros[0].cores[C_NARIZ] = pp->nariz; carros[0].cores[C_MOTOR] = pp->motor; carros[0].cores[C_ASA] = pp->asa;
                }
                obj_atribuir();
                som_bip(0);
            }
#endif
#ifdef DEMO_AUTO
            if(t_titulo > 7.0f) apertou |= CONT_START;
#endif
            if(apertou & CONT_START) { titulo = 0; camera = cam_antes; som_bip(1); }
            apertou = 0;
        } else if(replay_on) {
            if(apertou & (CONT_A | CONT_B | CONT_START)) { replay_fim(); camera = cam_antes; }
            apertou = 0;
        } else if(apertou & CONT_DPAD_DOWN) {
            if(replay_iniciar(20.0f)) { cam_antes = camera; camera = 0; t_replay = 0; aviso_volta = 0; }
            apertou = 0;
        }
        if(titulo) camera = 0;
        if(apertou & CONT_Y) camera ^= 1;
        if(apertou & CONT_X) { modo_clima = (modo_clima + 1) % N_MODOS_CLIMA; aviso_clima = 2.0f; aviso_det = 0; som_bip(0); }
        if(apertou & CONT_START) { detalhe_alto ^= 1; aviso_det = 2.0f; aviso_clima = 0; som_bip(0); }

        /* QUADROS QUE ATRASAM: a física roda em passos fixos de 1/60 s; se o
         * desenho de um quadro demorou 2 (ou 3) vblanks, rodamos 2 (ou 3)
         * passos — o jogo cai para 30/20 fps mas NÃO fica em câmera lenta. */
        int n_passos;
        {
            static uint64_t t_ant;
            static int64_t acum;                  /* tempo real ainda não simulado (µs) */
            uint64_t ag = timer_us_gettime64();
            if(!t_ant) t_ant = ag - 16667;
            acum += (int64_t)(ag - t_ant);
            t_ant = ag;
            n_passos = (int)(acum / 16667);
            if(n_passos < 1) n_passos = 1;
            if(n_passos > 4) n_passos = 4;        /* travou muito: não tenta recuperar tudo */
            acum -= (int64_t)n_passos * 16667;
            if(acum < -16667) acum = -16667;
            if(acum > 16667) acum = 16667;
        }
        const float dt_quadro = dt * n_passos;
        if(aviso_det > 0) aviso_det -= dt_quadro;
#ifdef TESTE_LENTO
        thd_sleep(20);                  /* teste: força quadros de ~30 fps */
#endif
        for(int passo = 0; passo < n_passos; passo++) {   /* ======== passos de física ======== */
        clima_atualizar(dt);
        if(aviso_clima > 0) aviso_clima -= dt;

        /* ---------------- física (60 Hz fixo) ---------------- */
        {
            int ant = luzes_acesas;
            luzes_acesas = largada > 0 ? (int)((3.5f - largada) / 0.6f) + 1 : 0;
            if(luzes_acesas > 5) luzes_acesas = 5;
            if(luzes_acesas > ant) som_bip(0);
            if(largada > 0 && largada - dt <= 0) som_bip(1);
        }
        if(titulo || replay_on) { }               /* título / replay: a corrida fica parada */
        else if(largada > 0) largada -= dt;
        else {
            /* VÁCUO: atrás de outro carro (até 45 m, mesma linha) o ar já vem
             * "aberto": menos arrasto (ganha velocidade na reta), mas o ar
             * turbulento tira um pouco de aderência nas curvas. */
            for(int i = 0; i < N_CARROS; i++) {
                carro_t *c = &carros[i];
                float vac = 0;
                for(int j = 0; j < N_CARROS; j++) {
                    if(j == i) continue;
                    float ds = fmodf(carros[j].s - c->s + COMPR, COMPR);
                    if(ds > 4.5f && ds < 45.0f && fabsf(carros[j].x - c->x) < 2.2f && carros[j].v > 25.0f) {
                        float k = 1.0f - (ds - 4.5f) / 40.5f;
                        if(k > vac) vac = k;
                    }
                }
                c->vacuo += (vac - c->vacuo) * limitar(4.0f * dt, 0, 1);
            }
            for(int i = 0; i < N_CARROS; i++) {
                carro_t *c = &carros[i];
                float a, f, om;
                c->t_volta += dt;
                if(i == 0) {
#ifdef DEMO_AUTO
                    ia_pilotar(c, &a, &f, &om);
                    t_demo += dt;
                    if(t_demo > 9.0f) { t_demo = 0;
#ifndef CAM_FIXA
                        camera ^= 1;
#endif
                    }
#else
                    /* volante digital suavizado; analógico vai direto */
                    if(eixo_analogico) volante_j += (eixo - volante_j) * limitar(30.0f * dt, 0, 1);
                    else {
                        /* direcional: vira em ~0,22 s até o fim, volta rápido ao centro */
                        int volta = fabsf(eixo) < fabsf(volante_j) || eixo * volante_j < 0;
                        float taxa = volta ? 8.0f : 4.5f;
                        volante_j += limitar(eixo - volante_j, -taxa * dt, taxa * dt);
                    }
                    c->volante = volante_j;
                    /* rotação pedida: forte em baixa, ~0.55 rad/s em alta */
                    float ganho = limitar(30.0f / (c->v + 1.0f), 0.72f, 1.6f);
                    om = volante_j * ganho;
                    a = acel; f = freio;
                    c->psi -= c->psi * 0.40f * dt;     /* o carro se alinha um pouco sozinho */
#endif
                }
                else ia_pilotar(c, &a, &f, &om);
                c->acel_in = a;
                fisica(c, a, f, om, dt);
            }
            colisoes();
            for(int i = 0; i < N_CARROS; i++) soltar_fumaca(&carros[i], quadro_n);
            replay_gravar();
        }
        (void)t_demo;
        atualizar_fumaca(dt);
        faiscas_atualizar(dt);
        if(!replay_on) atualizar_rastros(&carros[0]);
        quadro_n++;

        /* ---------------- som ---------------- */
        {
            carro_t *j = &carros[0];
            som_motor(largada > 0 ? 0.25f + 0.2f * acel : rotacao(j),
                      largada > 0 ? acel : j->acel_in,
                      j->derrapando ? limitar(j->deriva * 1.3f, 0, 1) : 0.0f,
                      j->fora && j->v > 3 ? limitar(j->v / 40.0f, 0, 1) : 0.0f);
            /* rival mais próximo: volume pela distância, estéreo pelo lado */
            int perto = -1; float dmin = 1e9f, dsp = 0, dsl = 0;
            for(int i = 1; i < N_CARROS; i++) {
                float ds = fmodf(carros[i].s - j->s + COMPR * 1.5f, COMPR) - COMPR * 0.5f;
                float dx = carros[i].x - j->x, d = sqrtf(ds * ds + dx * dx);
                if(d < dmin) { dmin = d; perto = i; dsp = dx; dsl = ds; }
            }
            float vol = limitar(1.0f - dmin / 90.0f, 0, 1);
            /* Doppler: velocidade com que o rival se afasta (+) ou se aproxima (-) */
            float v_afasta = (carros[perto].v - j->v) * (dsl >= 0 ? 1.0f : -1.0f) * fabsf(dsl) / (dmin + 0.5f);
            float dop = limitar(343.0f / (343.0f + v_afasta), 0.85f, 1.18f);
            som_rival(rotacao(&carros[perto]), vol * vol, 0.5f + limitar(dsp / 8.0f, -1, 1) * 0.4f, dop);
            float sm = j->s;
            som_chuva(chuva * (1.0f + 0.6f * pesada), molhado * (1.0f + 0.4f * pesada) * limitar(j->v / 50.0f, 0, 1));
            som_torcida((sm > 3950.0f || sm < 40.0f || (sm > 360.0f && sm < 490.0f)) ? 1.0f : 0.15f);
            if(batida_jogador > 0) { som_batida(batida_jogador); batida_jogador = 0; }
            som_atualizar();
        }
        }   /* ======== fim dos passos de física ======== */

        if(replay_on) {
            t_replay += dt_quadro;
            if(!replay_quadro(dt_quadro)) camera = cam_antes;
        }
        if(aviso_volta > 0) aviso_volta -= dt_quadro;
        if(!replay_on && !titulo && carros[0].volta != volta_ant) {
            if(carros[0].volta >= 1) {
                aviso_volta = 5.0f;
#ifdef DEMO_AUTO
                if(replay_iniciar(20.0f)) { cam_antes = camera; camera = 0; t_replay = 0; aviso_volta = 0; }
#endif
            }
            volta_ant = carros[0].volta;
        }
#ifdef REPLAY_TESTE
        {   /* teste: replay automático depois de REPLAY_TESTE segundos de corrida */
            static float tr; static int feito;
            if(!titulo && !replay_on) tr += dt_quadro;
            if(!feito && tr > REPLAY_TESTE && replay_iniciar(20.0f)) { feito = 1; cam_antes = camera; camera = 0; t_replay = 0; }
        }
#endif
        carro_t *eu = &carros[0];
        int vert_espelho = 0;
        if(camera == 0) espelho_pronto = 0;

        /* ---------------- retrovisor (render-to-texture) ---------------- */
        vert_quadro = 0;
        PF(9);                       /* entrada + física + som */
        pvr_wait_ready();
        PF(0);                       /* esperando o PowerVR */

        /* atualizado a cada 2 quadros (30 Hz basta num espelho): a textura
         * guarda a imagem anterior no quadro em que não renderizamos */
#ifdef SEM_ESPELHO
        if(0) {                          /* teste: sem a cena extra do retrovisor */
#else
        if(camera == 1 && ((quadro_n % (eu->v < 8.0f ? 6 : 3)) == 0 || !espelho_pronto)) {   /* retrovisor a 20 Hz (10 Hz parado) */
#endif
            espelho_pronto = 1;
            float P[3], R[3], U[3], Fr[3];
            base_carro(eu, P, R, U, Fr);
            float pos[3], alvo[3];
            for(int e = 0; e < 3; e++) {
                pos[e] = P[e] - Fr[e] * 0.3f + U[e] * 1.05f;
                alvo[e] = pos[e] - Fr[e] * 10.0f - U[e] * 0.25f;
            }
            camera_olhar(pos, alvo);
            CX = ESP_W / 2; CY = ESP_H * 0.42f; FOCO = 150.0f; PERTO = 0.3f; ESPELHAR = -1.0f;
            pvr_scene_begin_rtt(tex_espelho, ESP_W, ESP_H, ESP_W);
            pvr_list_begin(PVR_LIST_OP_POLY);
            desenhar_fundo_ceu();
            pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
            montar_visiveis_dir(eu->s, -1, 140.0f);   /* espelho pequeno: alcance curto */
            desenhar_terreno(160.0f);
            desenhar_pista();
            desenhar_barreiras();
            n_cerca = 0;
            n_detalhados = 99;   /* no espelho (256x64) o modelo simples basta */
            for(int i = 1; i < N_CARROS; i++) desenhar_carro(&carros[i], 0);
            pvr_list_finish();
            pvr_scene_finish();
            CX = 320; CY = 240; ESPELHAR = 1.0f;
            vert_espelho = vert_quadro;
        }
        else if((quadro_n % 6) == 2 && telao_q[0][0] != 0) {
            /* TELÃO: câmera de TV na reta filmando os carros que chegam (10 Hz) */
            float tc[3], ta[3];
            ponto_pista(fmodf(TELAO_S - 90.0f + COMPR, COMPR), 16.0f, tc, 0, 0); tc[1] += 6.0f;
            ponto_pista(fmodf(TELAO_S - 230.0f + COMPR, COMPR), 0.0f, ta, 0, 0);
            if(dist2_cam(telao_q[0]) < 300.0f * 300.0f) {
                camera_olhar(tc, ta);
                CX = 128; CY = 64; FOCO = 190.0f; PERTO = 0.5f; ESPELHAR = 1.0f;
                pvr_scene_begin_rtt(tex_telao, 256, 128, 256);
                pvr_list_begin(PVR_LIST_OP_POLY);
                desenhar_fundo_ceu();
                pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
                montar_visiveis_dir(fmodf(TELAO_S - 90.0f + COMPR, COMPR), -1, 260.0f);
                desenhar_terreno(300.0f);
                desenhar_pista();
                desenhar_barreiras();
                n_cerca = 0; n_detalhados = 99;
                for(int i = 0; i < N_CARROS; i++) desenhar_carro(&carros[i], 0);
                pvr_list_finish();
                pvr_scene_finish();
                CX = 320; CY = 240; ESPELHAR = 1.0f;
                telao_pronto = 1;
            }
        }
#ifndef SEM_REFLEXO
        else if((quadro_n % 3) == 1 && molhado > 0.30f && camera == 0) {
            /* REFLEXO: a câmera do quadro anterior espelhada no plano da pista
             * (desenha céu e carros; a imagem sai de cabeça para baixo) */
            const fatia_t *fe = fatia_em(eu->s);
            float yref = fe->y + 0.02f;
            float pos[3] = { cam_p[0], 2 * yref - cam_p[1], cam_p[2] }, alvo[3];
            for(int e = 0; e < 3; e++) alvo[e] = pos[e] + (e == 1 ? -cam_f[e] : cam_f[e]) * 10.0f;
            float foco_sal = FOCO;
            camera_olhar(pos, alvo);
            CX = 128; CY = 240 * 0.4f; FOCO = foco_sal * 0.4f; PERTO = 0.5f; ESPELHAR = 1.0f;
            refl_cy = CY; refl_foco = FOCO;
            memcpy(refl_P, cam_p, 12); memcpy(refl_R, cam_r, 12); memcpy(refl_U, cam_u, 12); memcpy(refl_F, cam_f, 12);
            pvr_scene_begin_rtt(tex_refl, 256, 256, 256);
            pvr_list_begin(PVR_LIST_OP_POLY);
            {   /* fundo: o céu inteiro (a câmera espelhada olha para cima: zênite em cima) */
                pvr_prim(&hdr_2d, sizeof(hdr_2d));
                uint32_t cz = cor_escala(cor_ceu, 0.85f), ch = cor_neb;
                enviar_2d(0, 256, 0.00001f, ch, 0); enviar_2d(0, 0, 0.00001f, cz, 0);
                enviar_2d(256, 256, 0.00001f, ch, 0); enviar_2d(256, 0, 0.00001f, cz, 1);
            }
            desenhar_fundo_ceu();
            pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
            n_cerca = 0; n_detalhados = 99;
            for(int i = 1; i < N_CARROS; i++) desenhar_carro(&carros[i], 0);   /* rivais (o próprio carro fica de fora) */
            pvr_list_finish();
            pvr_scene_finish();
            CX = 320; CY = 240; FOCO = foco_sal; ESPELHAR = 1.0f;
            refl_ativo = 1;
        }
#endif
        if(molhado < 0.25f || camera != 0) refl_ativo = 0;
        PF(1);                       /* retrovisor */

        /* ---------------- câmera ---------------- */
        /* SENSAÇÃO DE VELOCIDADE: o campo de visão abre (FOCO menor), a câmera
         * de trás desce e fica um pouco para trás, e tudo trepida de leve —
         * mais na zebra e muito mais na grama. */
        static float vel_cam;
        vel_cam += (limitar(eu->v / 85.0f, 0, 1) - vel_cam) * limitar(dt_quadro * 2.5f, 0, 1);
        float kv = vel_cam * vel_cam;
        float tq = quadro_n * (1.0f / 60.0f);
        float na_zebra = (fatia_em(eu->s)->zebra && fabsf(eu->x) > 7.2f && !eu->fora) ? 1.0f : 0.0f;
        float trep = 0.0035f * kv + 0.010f * na_zebra * limitar(eu->v / 40.0f, 0, 1)
                   + (eu->fora ? 0.022f * limitar(eu->v / 30.0f, 0, 1) : 0.0f);
        float ondas = sinf(tq * 71.0f) * 0.6f + sinf(tq * 113.0f) * 0.4f;
        trep += 0.05f * baque;                      /* toque em outro carro: tranco leve */
#ifndef COCKPIT_2D
        ck_trep = trep;
#endif
        baque -= baque * limitar(dt_quadro * 6.0f, 0, 1);
        if(camera == 0) {
            /* atrás do carro: a câmera anda PELA PISTA (s - 7 m), então acompanha a curva */
            float pos[3], alvo[3];
            ponto_pista(eu->s - 7.5f - 0.8f * kv, eu->x * 0.85f, pos, 0, 0);
            ponto_pista(eu->s + 12.0f, eu->x * 0.7f, alvo, 0, 0);
            pos[1] += 2.4f - 0.35f * kv + trep * 0.5f * ondas; alvo[1] += 0.9f;
            camera_olhar(pos, alvo);
            FOCO = 400.0f - 80.0f * kv; PERTO = 0.5f;
#ifdef CAM_ORBITA
            {   /* teste: câmera girando em volta do carro do jogador */
                float P[3], R[3], U[3], Fr[3];
#ifndef CAM_ALVO
#define CAM_ALVO 0              /* teste: qual carro a câmera orbita */
#endif
                base_carro(&carros[CAM_ALVO], P, R, U, Fr);
                float t = quadro_n * (2 * PI_F / 600.0f), d = CAM_ORBITA;
                for(int e = 0; e < 3; e++) {
#ifndef CAM_MIRA
#define CAM_MIRA 0.35f          /* teste: altura do ponto que a câmera olha */
#endif
                    pos[e] = P[e] + R[e] * cosf(t) * d + Fr[e] * sinf(t) * d + U[e] * (d * 0.35f + CAM_MIRA);
                    alvo[e] = P[e] + U[e] * CAM_MIRA;
                }
                camera_olhar(pos, alvo);
                FOCO = 420.0f; PERTO = 0.3f;
            }
#endif
#ifdef CAM_MURO
            {   /* teste: câmera de frente para o muro (CAM_LADO = -1 esquerda, 1 direita) */
                float pp[3], aa[3];
                ponto_pista(CAM_MURO, CAM_LADO * 6.0f, pp, 0, 0); ponto_pista(CAM_MURO + 14.0f, CAM_LADO * 24.5f, aa, 0, 0);
                pp[1] += 1.5f; aa[1] = pp[1] - 1.5f;
                camera_olhar(pp, aa);
                FOCO = 330.0f; PERTO = 0.3f;
            }
#endif
#ifdef CAM_PLACA
            {   /* teste: câmera parada olhando uma placa na grama (lado direito) */
                float pp[3], aa[3];
                ponto_pista(CAM_PLACA - 9.0f, 9.0f, pp, 0, 0); ponto_pista(CAM_PLACA, 14.5f, aa, 0, 0);
                pp[1] += 1.6f; aa[1] += 1.0f;
                camera_olhar(pp, aa);
                FOCO = 330.0f; PERTO = 0.3f;
            }
#endif
#ifdef CAM_BOX
            {   /* teste: câmera parada no pit lane, olhando as garagens */
                float pp[3], aa[3];
                ponto_pista(CAM_BOX, -11.0f, pp, 0, 0); ponto_pista(CAM_BOX + 18.0f, -27.0f, aa, 0, 0);
                pp[1] += 2.2f; aa[1] += 2.0f;
                camera_olhar(pp, aa);
                FOCO = 330.0f; PERTO = 0.3f;
            }
#endif
#ifdef OLHAR_SOL
            {   /* teste: câmera de trás virada para o lado do sol (horizontal) */
                float hl = sqrtf(LUZ[0] * LUZ[0] + LUZ[2] * LUZ[2]);
                alvo[0] = pos[0] + LUZ[0] / hl * 50; alvo[1] = pos[1] + 4.0f; alvo[2] = pos[2] + LUZ[2] / hl * 50;
                camera_olhar(pos, alvo);
            }
#endif
        } else {
            /* cockpit: olho do piloto, olhando para onde o carro aponta */
            float P[3], R[3], U[3], Fr[3];
            base_carro(eu, P, R, U, Fr);
            float pos[3], alvo[3];
#ifdef COCKPIT_2D
            for(int e = 0; e < 3; e++) {
                pos[e] = P[e] + Fr[e] * 0.05f + U[e] * (1.02f + trep * ondas);
                alvo[e] = pos[e] + Fr[e] * 10.0f - U[e] * (0.45f - trep * 3.0f * sinf(tq * 89.0f));
            }
            camera_olhar(pos, alvo);
            FOCO = 330.0f - 50.0f * kv; PERTO = 0.08f;
#else
            /* olho do piloto DENTRO do capacete (0,76 m do chão, sobre o banco):
             * daqui o próprio carro forma o "V" do cockpit, como nas câmeras reais */
            for(int e = 0; e < 3; e++) {
                pos[e] = P[e] + Fr[e] * 0.40f + U[e] * (0.728f + trep * ondas * 0.6f);
                alvo[e] = pos[e] + Fr[e] * 10.0f - U[e] * (1.30f - trep * 3.0f * sinf(tq * 89.0f));
            }
            {   /* força G lateral: a cabeça vai para FORA da curva e o olhar procura a tangência */
                static float gl;
                float g = limitar(eu->volante * limitar(eu->v / 35.0f, 0, 1.2f), -1.2f, 1.2f);
                gl += (g - gl) * limitar(dt_quadro * 4.0f, 0, 1);
                for(int e = 0; e < 3; e++) {
                    pos[e] -= R[e] * gl * 0.030f;
                    pos[e] -= U[e] * fabsf(gl) * 0.008f;
                    alvo[e] += R[e] * gl * 0.85f;
                }
            }
            camera_olhar(pos, alvo);
            FOCO = 270.0f - 35.0f * kv; PERTO = 0.04f;
#endif
        }

        float s_vis = camera == 0 ? eu->s - 7.5f : eu->s, dist_vis = dist_visao;
        int sent_vis = 1;
        if(titulo) {
            /* título: a câmera gira devagar em volta do carro no grid */
            float P[3], R[3], U[3], Fr[3], pos[3], alvo[3];
            base_carro(eu, P, R, U, Fr);
            float an = t_titulo * 0.22f + 2.2f, d = 6.3f;
            for(int e = 0; e < 3; e++) {
                pos[e] = P[e] + R[e] * fcos(an) * d + Fr[e] * fsin(an) * d + U[e] * 1.5f;
                alvo[e] = P[e] + U[e] * 0.55f;
            }
            camera_olhar(pos, alvo);
            FOCO = 400.0f; PERTO = 0.3f;
            s_vis = eu->s - 35.0f;
        } else if(replay_on) replay_camera(eu, &s_vis, &sent_vis, &dist_vis);
        /* ---------------- desenho ---------------- */
        PF(10);                      /* câmera */
        pvr_scene_begin();
        pvr_list_begin(PVR_LIST_OP_POLY);
        PF(11);                      /* início da cena */
        desenhar_fundo_ceu();
        pvr_prim(&hdr_longe, sizeof(hdr_longe));
        desenhar_longe();
        PF(12);                      /* céu + longe */
        pvr_prim(&hdr_mundo, sizeof(hdr_mundo));
        montar_visiveis_dir(fmodf(s_vis + COMPR, COMPR), sent_vis, dist_vis);
        PF(8);                       /* fatias visíveis */
        desenhar_terreno(dist_visao + 40.0f);
        PF(2);
        desenhar_pista();
        desenhar_zebra_dentes();
        desenhar_saias();
        desenhar_marcas();
        desenhar_largada();
        desenhar_barreiras();
        PF(3);
        {   /* onde está o jogador (torcida se agita quando ele passa) */
            float Pf[3], Rf[3], Uf[3], Ff[3];
            base_carro(eu, Pf, Rf, Uf, Ff);
            foco[0] = Pf[0]; foco[1] = Pf[1]; foco[2] = Pf[2];
        }
        desenhar_cenario();
        desenhar_helicoptero();
        desenhar_fiscais();
        desenhar_telao();
        desenhar_luzes();
        PF(4);
        n_detalhados = 1;   /* o jogador sempre detalhado */
        escolher_detalhados();
        gravar_sombra = 1; n_sombras = 0;
        {
            float brilho_base = brilho;
            for(int i = N_CARROS - 1; i >= 0; i--) {
                float P[3], R[3], U[3], Fr[3];
                base_carro(&carros[i], P, R, U, Fr);
                brilho = brilho_base * sombra_estrutura_no_carro(P);
                {   /* sombra assada da pista (copas, arquibancadas) também no carro */
                    int kf = (int)(fmodf(carros[i].s + COMPR, COMPR) / PASSO) % NF, ib = 4;
                    float dmin = 1e9f;
                    for(int q = 1; q < NPTS - 1; q++) { float d = fabsf(LAT[q] - carros[i].x); if(d < dmin) { dmin = d; ib = q; } }
                    brilho *= 0.50f + 0.50f * (somb[kf][ib] / 255.0f);
                }
                desenhar_carro(&carros[i], camera == 1 && i == 0);
            }
            brilho = brilho_base;
        }
        gravar_sombra = 0;
        PF(5);
        if(camera == 1) {
#ifndef SEM_VOLANTE
            /* o volante desenhado segue o esterço com inércia (sem trancos) */
            static float giro_vis;
            giro_vis += (eu->volante * 0.95f - giro_vis) * limitar(dt_quadro * 7.0f, 0, 1);
#ifdef COCKPIT_2D
            desenhar_volante(giro_vis, eu->marcha, eu->v * 3.6f, rotacao(eu), 0xFFE6E4DE, eu->cores[C_PRINC],
                             eu->cores[C_SEC]);
#else
            desenhar_cockpit3d(eu, giro_vis, eu->marcha, eu->v * 3.6f, rotacao(eu));
#endif
#endif
#if !defined(PREVIA_COCKPIT) && defined(COCKPIT_2D)
            desenhar_espelhos();
#endif
        }
        pvr_list_finish();
        pvr_list_begin(PVR_LIST_OP_MOD);      /* sombras dos carros (volumes) */
        desenhar_sombras_mv();
        pvr_list_finish();
        pvr_list_begin(PVR_LIST_PT_POLY);     /* recorte: árvores e faixas */
        desenhar_arvores_tr();
        desenhar_faixas_tr();
#ifdef PREVIA_COCKPIT
        if(0) {
#else
        if(camera == 1 && !titulo && !replay_on) {
#endif
#ifdef COCKPIT_2D
#ifndef SEM_VOLANTE
            desenhar_volante_pt(eu->cores[C_PRINC]);
#endif
            desenhar_espelhos_pt();
#else
            desenhar_cockpit3d_pt();
#endif
        }
        pvr_list_finish();
        som_atualizar();             /* 2a alimentação do áudio no quadro: evita falhas no buffer */
        PF(6);

        /* HUD */
        char l1[64], l2[64], tv[16], tm[16];
        int posicao = 1;
        for(int i = 1; i < N_CARROS; i++) {
            float a = carros[i].volta * COMPR + carros[i].s, m = eu->volta * COMPR + eu->s;
            if(a > m) posicao++;
        }
        fmt_tempo(tv, sizeof(tv), eu->t_volta);
        fmt_tempo(tm, sizeof(tm), eu->melhor);
        snprintf(l1, sizeof(l1), "POS %d/%d  VOLTA %d  %s", posicao, N_CARROS, eu->volta < 0 ? 1 : eu->volta + 1, tv);
        snprintf(l2, sizeof(l2), "%3d KM/H  %da  MELHOR %s  %dfps %dv", (int)(eu->v * 3.6f), eu->marcha, tm, fps, vert_med);
#ifdef DIAG_ARV
        snprintf(l2, sizeof(l2), "%s", diag_txt);
#endif
#ifdef DIAG_ARQ
        snprintf(l2, sizeof(l2), "ARQ %d DUP %d", n_arqs, n_arq_dup);
#endif
#ifdef DIAG_VRAM
        snprintf(l2, sizeof(l2), "VRAM LIVRE %lu KB  CEU %lu", (unsigned long)(pvr_mem_available() / 1024), (unsigned long)(vram_livre / 1024));
#endif
#ifdef TESTE_LENTO
        {   /* teste: relógio do console desde o início x tempo de jogo acumulado */
            static uint64_t t0; static float tj; if(!t0) t0 = timer_ms_gettime64();
            tj += dt_quadro;
            snprintf(l2, sizeof(l2), "REL %.1f JOGO %.1f %dfps", (double)((timer_ms_gettime64() - t0) / 1000.0f), (double)tj, fps);
        }
#endif
        /* o cronômetro muda todo quadro, e redesenhar + reenviar a textura de
         * 64 KB custa caro: o painel de cima é atualizado a 15 Hz */
#ifdef HUD_ANTIGO
        if((quadro_n & 3) == 0) painel_texto(&txt_topo, l1, l2, 0);
#endif
        snprintf(l1, sizeof(l1), "%s  -  %s", nome_trecho(eu->s), texto_tempo());
#ifdef PERFIL
        snprintf(l1, sizeof(l1), "%s", pf_txt);
#ifdef DIAG_CARRO
        if(++car_q >= 60) {
            snprintf(car_txt, sizeof(car_txt), "J%d R%d/%d M%d/%d S%d/%d f%d", (int)(car_ac[1] / 600), (int)(car_ac[3] / 600), car_n[3] / 60,
                     (int)(car_ac[4] / 600), car_n[4] / 60, (int)(car_ac[0] / 600), car_n[0] / 60, n_lista[LST_JOGADOR]);
            memset(car_ac, 0, sizeof(car_ac)); memset(car_n, 0, sizeof(car_n)); car_q = 0;
        }
        snprintf(l1, sizeof(l1), "%s", car_txt);
#endif
#ifdef DIAG_CEN
        if(++cen_n >= 60) {
            snprintf(cen_txt, sizeof(cen_txt), "PRE%d t0:%d t1:%d t7:%d t8:%d o:%d", (int)(cen_ac[1] / 600), dg_tipo[0] / 60, dg_tipo[1] / 60, dg_tipo[7] / 60, dg_tipo[8] / 60, (dg_tipo[2] + dg_tipo[3] + dg_tipo[4] + dg_tipo[5] + dg_tipo[9]) / 60); memset(dg_tipo, 0, sizeof(dg_tipo));
            memset(cen_ac, 0, sizeof(cen_ac)); cen_n = 0;
        }
        snprintf(l1, sizeof(l1), "%s", cen_txt);
#endif
#endif
#ifdef DIAG_LOD
        snprintf(l1, sizeof(l1), "DET %d  MIUDAS %d  SIMPLES %d", carro_nf - nf_simples, n_miudas, nf_simples);
#endif
#ifdef DIAG_SOM
        {   /* clip = amostras saturadas/s, gap = maior intervalo entre pedidos (ms),
               ped = maior pedido (amostras), us = maior custo de um pedido */
            static int dc, dg, dp, du; static uint64_t dt_ult;
            uint64_t ag = timer_ms_gettime64();
            if(ag - dt_ult >= 1000) { void som_diag(int*, int*, int*, int*); som_diag(&dc, &dg, &dp, &du); dt_ult = ag; }
            snprintf(l1, sizeof(l1), "CLIP%d GAP%d PED%d US%d", dc, dg, dp, du);
        }
#endif
        painel_texto(&txt_baixo, l1, NULL, 1);
        if(largada > 3.0f) l1[0] = 0;
        else if(largada > 0) snprintf(l1, sizeof(l1), "%d", (int)largada + 1);
        else if(largada > -1.0f) snprintf(l1, sizeof(l1), "VAI!");
        else if(aviso_clima > 0) snprintf(l1, sizeof(l1), "CLIMA: %s", NOME_MODO[modo_clima]);
        else if(aviso_det > 0) snprintf(l1, sizeof(l1), "DETALHE: %s", detalhe_alto ? "ALTO" : "NORMAL");
        else l1[0] = 0;
        if(largada > -1.0f && largada <= 0) largada -= dt_quadro;
        painel_texto(&txt_centro, l1, NULL, 1);

        pvr_list_begin(PVR_LIST_TR_POLY);
        cam_s_ref = s_vis;
#if SKYBOX
        desenhar_skybox_tr();
#ifndef DBG_C1
        desenhar_nuvens_mov_tr();
#endif
#ifndef DBG_C2
        desenhar_cidade_tr();
        desenhar_nevoa_urbana_tr();
#endif
        desenhar_bairros_tr();
#endif
        desenhar_detalhe_tr();
        desenhar_marcas_grama_tr();
        desenhar_agua_zebra_tr();
        desenhar_reflexo_tr();
        desenhar_relevo_tr();
        desenhar_luzes_tr();
        desenhar_janelas_tr();
        desenhar_marcas_tr();
        desenhar_sombras_tr();
        desenhar_faiscas_tr();
        desenhar_reflexos_tr();
        desenhar_pintura_tr();
        desenhar_cercas_tr();
        desenhar_sol();
        desenhar_pocas();
        desenhar_rastros();
        desenhar_fumaca();
        desenhar_calor_tr();
        desenhar_brilhos_tr();
        desenhar_chuva(dt_quadro);
        desenhar_gotas_tela(dt_quadro, camera == 1, eu->v);
#ifdef DBG_CTEX
        if(ctex_ok[1]) {   /* mostra a textura assada do carro 2 */
            static pvr_poly_hdr_t hd; static int ok;
            if(!ok) { pvr_poly_cxt_t cx; pvr_poly_cxt_txr(&cx, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256, ctex_tex[1], PVR_FILTER_NONE);
                      cx.gen.fog_type = PVR_FOG_DISABLE; cx.txr.mipmap = PVR_MIPMAP_ENABLE; pvr_poly_compile(&hd, &cx); ok = 1; }
            pvr_prim(&hd, sizeof(hd));
            pvr_vertex_t v;
            const float X[4] = { 0, 0, 512, 512 }, Y[4] = { 470, 0, 470, 0 }, Uu[4] = { 0, 0, 1, 1 }, Vv[4] = { 1, 0, 1, 0 };
            for(int k = 0; k < 4; k++) { v.flags = k == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX; v.x = X[k] * XS; v.y = Y[k]; v.z = 60; v.u = Uu[k]; v.v = Vv[k]; v.argb = 0xFFFFFFFF; v.oargb = 0; pvr_prim(&v, sizeof(v)); }
        }
#endif
        desenhar_grade();
        desenhar_vinheta();
#ifndef VOLANTE_ANTIGO
#ifdef COCKPIT_2D
        if(camera == 1 && !titulo && !replay_on) { desenhar_brilho_espelhos(); desenhar_sombra_bracos(); }
#else
        if(camera == 1 && !titulo && !replay_on) desenhar_cockpit3d_tr();
#endif
#endif
        animar_paleta_torcida(t_clima);
        if(!titulo && !replay_on) desenhar_velocidade(eu->v);
#ifdef HUD_ANTIGO
        faixa_escura(0, 6, 640, 58);
        painel_desenhar(&txt_topo, 16, 6, 0xFFFFFFFF);
        painel_desenhar(&txt_baixo, 64, camera == 1 ? 66 : 440, 0xFFFFE070);
        if(l1[0] && (largada > -1.0f || aviso_clima > 0 || aviso_det > 0)) painel_desenhar(&txt_centro, 192, 200, 0xFFFFFFFF);
#else
        if(titulo) desenhar_titulo(t_titulo, modo_clima);
#ifndef SEM_CARRO_OBJ
        if(titulo && obj_ok) {
            char tc[40];
            if(jogador_obj == 0) snprintf(tc, sizeof(tc), "CARRO: CLASSIC GT  (CIMA/BAIXO)");
            else snprintf(tc, sizeof(tc), "CARRO: EQUIPE %02d  (CIMA/BAIXO)", jogador_obj);
            hud_caixa(170, 318, 300, 26, 0x90101418);
            hud_texto(320, 324, 0.85f, 0xFFFFD840, tc, 1);
        }
#endif
        else if(replay_on) desenhar_hud_replay(eu, t_replay);
        else desenhar_hud_novo(eu, posicao, camera == 1, (largada > -1.0f || aviso_clima > 0 || aviso_det > 0) ? l1 : "", largada, fps);
#if defined(PERFIL) || defined(DIAG_CEN) || defined(DIAG_CARRO) || defined(DIAG_LOD) || defined(DIAG_SOM)
        painel_desenhar(&txt_baixo, 64, 96, 0xFFFFE070);       /* diagnóstico */
#endif
#endif
        pvr_list_finish();

        pvr_scene_finish();
        PF(7);                       /* HUD + lista translúcida */

        quadros++;
        uint64_t agora = timer_ms_gettime64();
        if(agora - t_fps >= 1000) {
            fps = quadros; quadros = 0; t_fps = agora;
            vert_med = vert_quadro;
#ifdef PERFIL
            /* esp, retrov, terreno, pista, cenário, carros, volante, TR, câm, física */
            int q = fps > 0 ? fps : 1;
            snprintf(pf_txt, sizeof(pf_txt), "E%d R%d T%d P%d C%d K%d V%d X%d M%d F%d c%d b%d l%d",
                     (int)(pf_ac[0] / q / 100), (int)(pf_ac[1] / q / 100), (int)(pf_ac[2] / q / 100),
                     (int)(pf_ac[3] / q / 100), (int)(pf_ac[4] / q / 100), (int)(pf_ac[5] / q / 100),
                     (int)(pf_ac[6] / q / 100), (int)(pf_ac[7] / q / 100), (int)(pf_ac[8] / q / 100),
                     (int)(pf_ac[9] / q / 100), (int)(pf_ac[10] / q / 100), (int)(pf_ac[11] / q / 100),
                     (int)(pf_ac[12] / q / 100));
            memset(pf_ac, 0, sizeof(pf_ac));
#endif
            printf("fps %d | vert %d (retrovisor %d) | s %.0f\n", fps, vert_quadro, vert_espelho, (double)eu->s);
        }
    }
    return 0;
}
