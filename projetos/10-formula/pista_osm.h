/* pista_osm.h — formato das pistas geradas do OpenStreetMap
 * (ferramentas/gerar_pista_osm.py gera um pista_<nome>.h por circuito). */
#ifndef PISTA_OSM_H
#define PISTA_OSM_H

typedef struct { int seg; const char *nome; } osm_curva_t;
typedef struct { int ini, fim, lado; } osm_trecho_t;      /* lado: -1 esquerda, +1 direita */
/* prédio real do OSM: segmento do centro, lateral (m, + = direita), largura
 * (perpendicular à pista), profundidade (ao longo), altura; tipo 0 prédio,
 * 1 arquibancada, 2 hangar */
typedef struct { int seg; float lateral, larg, prof, alt; int tipo; } osm_predio_t;
#define AMB_AGUA_ESQ 1
#define AMB_AGUA_DIR 2
#define AMB_MATA_ESQ 4
#define AMB_MATA_DIR 8

typedef struct {
    int                 n_seg;
    const float       (*pista)[2];      /* curva e altura de cada segmento (1 m)      */
    int                 n_mapa;
    const float       (*mapa)[2];       /* contorno 0..1 para o minimapa               */
    float               mapa_w, mapa_h;
    int                 n_curvas;
    const osm_curva_t  *curvas;         /* nomes das curvas/trechos                    */
    int                 n_boxes;
    const osm_trecho_t *boxes;          /* onde o pit lane corre ao lado da pista      */
    int                 hotel_ini, hotel_fim;   /* prédio sobre a pista (-1 = não tem) */
    int                 n_frenagens;
    const int          *frenagens;      /* segmento onde começa cada curva forte após reta */
    const unsigned char *ambiente;      /* bits AMB_* por segmento (água/mata reais)    */
    int                 n_predios;
    const osm_predio_t *predios;        /* ordenados por segmento                       */
    const short        *rumo;           /* direção real (0,1°; 0 = leste, 900 = norte)   */
} pista_osm_t;

#endif
