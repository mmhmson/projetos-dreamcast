/*
 * w_file_dc.c — leitura do WAD com CACHE DE BLOCOS, para o CD do Dreamcast.
 *
 * Substitui o w_file_stdc.c do doomgeneric (mesma interface: a "classe"
 * stdc_wad_file com abrir / fechar / ler), sem alterar o código do jogo.
 *
 * O PROBLEMA: o Doom foi feito para disco rígido. Na inicialização ele lê
 * MILHARES de pedacinhos espalhados pelo WAD (cabeçalhos de cada textura,
 * sprite, etc.): um fseek + fread para cada um. Num HD de 1993 isso já era
 * aceitável; num leitor de CD cada salto é uma BUSCA MECÂNICA (dezenas a
 * centenas de ms). Resultado medido no Flycast: R_Init levando minutos.
 *
 * A SOLUÇÃO (a mesma de jogos comerciais de CD): ler o disco em BLOCOS
 * GRANDES (64 KB) e guardá-los numa cache na RAM. Pedidos pequenos e
 * próximos passam a ser atendidos da memória. Quando a cache enche,
 * descartamos o bloco usado há mais tempo (LRU = Least Recently Used).
 *
 * Custo: N_BLOCOS × 64 KB de RAM (aqui 2 MB dos 16 MB do Dreamcast).
 *
 * DETALHE IMPORTANTE (descoberto lendo o fs_iso9660.c do KOS): o KOS só usa
 * o caminho RÁPIDO de leitura do GD-ROM (DMA em modo "stream") quando o
 * buffer de destino está ALINHADO EM 32 BYTES e a leitura começa no início
 * de um setor. Com malloc() comum ou fread() (que passa pelo buffer interno
 * do stdio), ele cai no caminho lento, um setor de 2 KB por vez. Por isso
 * usamos fs_read() do KOS direto e memalign(32, ...).
 *
 * Licença: GPL v2 (parte do port do doomgeneric).
 */

#include <kos/fs.h>
#include <malloc.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "m_misc.h"
#include "w_file.h"
#include "z_zone.h"

#define TAM_BLOCO   (64 * 1024)
#define N_BLOCOS    32                  /* 32 × 64 KB = 2 MB de cache */

typedef struct {
    int      bloco;                     /* índice do bloco no arquivo (-1 = vazio) */
    uint32_t usado_em;                  /* "relógio" do último uso (para o LRU) */
    uint32_t tamanho;                   /* bytes válidos (o último bloco é menor) */
    uint8_t *dados;
} entrada_cache_t;

typedef struct {
    wad_file_t      wad;
    file_t          arq;                /* arquivo do VFS do KOS (sem stdio) */
    entrada_cache_t cache[N_BLOCOS];
    uint32_t        relogio;
    uint32_t        acertos, faltas;    /* estatística */
} dc_wad_file_t;

extern wad_file_class_t stdc_wad_file;

static wad_file_t *W_DC_OpenFile(char *path) {
    file_t f = fs_open(path, O_RDONLY);
    if(f < 0)
        return NULL;

    dc_wad_file_t *r = Z_Malloc(sizeof(dc_wad_file_t), PU_STATIC, 0);
    memset(r, 0, sizeof(*r));
    r->wad.file_class = &stdc_wad_file;
    r->wad.mapped = NULL;
    r->wad.length = (unsigned int)fs_total(f);
    r->arq = f;
    for(int i = 0; i < N_BLOCOS; i++)
        r->cache[i].bloco = -1;
    /* A cache fica FORA da zona do Doom (é nossa), alinhada em 32 bytes
     * para o KOS poder ler do GD-ROM por DMA direto nela. */
    for(int i = 0; i < N_BLOCOS; i++)
        r->cache[i].dados = memalign(32, TAM_BLOCO);
    printf("W_DC: cache de leitura do CD: %d blocos x %d KB\n", N_BLOCOS, TAM_BLOCO / 1024);
    return &r->wad;
}

static void W_DC_CloseFile(wad_file_t *wad) {
    dc_wad_file_t *r = (dc_wad_file_t *)wad;
    printf("W_DC: cache: %lu acertos, %lu leituras do CD\n",
           (unsigned long)r->acertos, (unsigned long)r->faltas);
    for(int i = 0; i < N_BLOCOS; i++)
        free(r->cache[i].dados);
    fs_close(r->arq);
    Z_Free(r);
}

/* Devolve o bloco pedido, lendo do CD se ele não estiver na cache. */
static entrada_cache_t *obter_bloco(dc_wad_file_t *r, int bloco) {
    entrada_cache_t *mais_velho = &r->cache[0];

    for(int i = 0; i < N_BLOCOS; i++) {
        entrada_cache_t *e = &r->cache[i];
        if(e->bloco == bloco) {                 /* ACERTO: já está na RAM */
            e->usado_em = ++r->relogio;
            r->acertos++;
            return e;
        }
        if(e->usado_em < mais_velho->usado_em)
            mais_velho = e;
    }

    /* FALTA: lê 64 KB do CD para o lugar do bloco usado há mais tempo */
    r->faltas++;
    fs_seek(r->arq, (off_t)bloco * TAM_BLOCO, SEEK_SET);   /* início de setor */
    ssize_t n = fs_read(r->arq, mais_velho->dados, TAM_BLOCO);
    mais_velho->tamanho = n > 0 ? (uint32_t)n : 0;
    mais_velho->bloco = bloco;
    mais_velho->usado_em = ++r->relogio;

    if((r->faltas & 63) == 0)
        printf("[W_DC: %lu blocos lidos do CD, %lu%% de acerto]\n", (unsigned long)r->faltas,
               (unsigned long)(100ull * r->acertos / (r->acertos + r->faltas)));
    return mais_velho;
}

size_t W_StdC_Read(wad_file_t *wad, unsigned int offset, void *buffer, size_t buffer_len) {
    dc_wad_file_t *r = (dc_wad_file_t *)wad;
    uint8_t *dst = buffer;
    size_t lido = 0;

    while(lido < buffer_len) {
        unsigned int pos = offset + (unsigned int)lido;
        int bloco = pos / TAM_BLOCO;
        uint32_t dentro = pos % TAM_BLOCO;

        entrada_cache_t *e = obter_bloco(r, bloco);
        if(dentro >= e->tamanho)
            break;                              /* fim do arquivo */

        size_t n = e->tamanho - dentro;
        if(n > buffer_len - lido)
            n = buffer_len - lido;
        memcpy(dst + lido, e->dados + dentro, n);
        lido += n;
    }
    return lido;
}

/* Mesmo nome da classe original: o resto do Doom não percebe a troca. */
wad_file_class_t stdc_wad_file = {
    W_DC_OpenFile,
    W_DC_CloseFile,
    W_StdC_Read,
};
