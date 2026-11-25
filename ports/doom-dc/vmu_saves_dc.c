/*
 * vmu_saves_dc.c — SAVES do Doom no memory card (VMU) do Dreamcast.
 *
 * O Doom salva com fopen/fwrite/rename numa pasta ("savegamedir"). No
 * Dreamcast não há disco gravável: o CD é só leitura, e a VMU tem um sistema
 * de arquivos próprio, minúsculo (128 KB, blocos de 512 bytes, ~200 livres),
 * cujos arquivos levam um cabeçalho com ícone para aparecer no gerenciador
 * de memória da BIOS.
 *
 * ESTRATÉGIA (sem alterar o código do jogo):
 *
 *   1. O Doom salva na RAM, em /ram/ (o RAM disk do KOS). Para isso
 *      "embrulhamos" duas funções com o --wrap do linker (ver Makefile):
 *        M_GetSaveGameDir() -> devolve "/ram/" (o RAM disk não tem mkdir,
 *                              então não dá para usar a subpasta .savegame/)
 *        rename()           -> o RAM disk não implementa rename; dentro de
 *                              /ram copiamos o arquivo e apagamos o antigo.
 *   2. No BOOT, copiamos os saves da VMU para /ram/doomsavN.dsg.
 *   3. Durante o jogo, 1x por segundo, conferimos um hash de cada
 *      /ram/doomsavN.dsg. Se mudou, o jogador acabou de salvar: COMPRIMIMOS
 *      e gravamos na VMU como DOOMSAVN — numa THREAD, porque gravar na VMU
 *      é lento (o 1º teste, sem compressão e na thread principal, congelou
 *      o jogo por 11 s e ocupou 122 dos ~200 blocos).
 *
 * Cabeçalho VMS: no KOS 2.3 o fs_vmu monta o cabeçalho sozinho. Passamos
 * descrição + ícone com fs_ioctl(IOCTL_VMU_SET_HDR) e escrevemos só os dados;
 * na leitura, fs_read já devolve os dados sem o cabeçalho.
 *
 * Licença: GPL v2 (parte do port do doomgeneric).
 */

#include <kos.h>
#include <dc/fs_vmu.h>
#include <dc/vmu_pkg.h>
#include <dc/vmufs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_SLOTS    6                    /* o menu do Doom tem 6 posições */
#define APP_ID     "DOOMGENERIC-DC"
#define TAM_DESCR  24                   /* SAVESTRINGSIZE do Doom */
#define MAGICO     "DCSV"               /* nosso formato: "DCSV" + tamanho + LZSS */

static char              resumo[256];   /* o que aconteceu no boot (para repetir no log) */
static char              dir_vmu[16];   /* ex.: "/vmu/a1" */
static maple_device_t   *vmu;
static uint32_t          assinatura[N_SLOTS];
static volatile int      gravando;      /* thread de gravação ocupada? */

/* ------------------------------------------------------------------------ */
/* Utilidades de arquivo                                                     */
/* ------------------------------------------------------------------------ */

static uint8_t *ler_tudo(const char *caminho, size_t *tam) {
    file_t f = fs_open(caminho, O_RDONLY);
    if(f < 0) return NULL;
    size_t n = fs_total(f);
    uint8_t *buf = malloc(n ? n : 1);
    if(buf && fs_read(f, buf, n) != (ssize_t)n) { free(buf); buf = NULL; }
    fs_close(f);
    if(buf) *tam = n;
    return buf;
}

static int gravar_tudo(const char *caminho, const void *dados, size_t tam) {
    file_t f = fs_open(caminho, O_WRONLY | O_TRUNC);
    if(f < 0) return -1;
    ssize_t n = fs_write(f, dados, tam);
    fs_close(f);
    return n == (ssize_t)tam ? 0 : -1;
}

/* FNV-1a: "assinatura" barata para saber se o arquivo mudou */
static uint32_t hash(const uint8_t *d, size_t n) {
    uint32_t h = 2166136261u;
    for(size_t i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h ^ (uint32_t)n;
}

/* ------------------------------------------------------------------------ */
/* Compressão LZSS                                                           */
/* ------------------------------------------------------------------------ */
/* Ideia: em vez de repetir bytes que já apareceram, escrevemos "copie N
 * bytes de D posições atrás". Saves do Doom têm muitos zeros e estruturas
 * repetidas (um "thinker" por monstro, porta, plataforma...), então
 * comprimem bem. Formato:
 *   1 byte de FLAGS para cada grupo de 8 itens (bit=1: literal, bit=0: cópia)
 *   literal = 1 byte
 *   cópia   = 2 bytes: distância-1 (12 bits) | comprimento-3 (4 bits)
 *             -> janela de 4 KB, cópias de 3 a 18 bytes
 * Para achar repetições rápido (o SH-4 não é um PC!), usamos uma tabela hash
 * dos 3 próximos bytes com uma corrente de posições anteriores. */

#define JANELA    4096
#define MIN_COPIA 3
#define MAX_COPIA 18
#define BITS_HASH 12

static int cabeca[1 << BITS_HASH];
static int anterior[JANELA];

static inline unsigned hash3(const uint8_t *p) {
    return ((p[0] << 8) ^ (p[1] << 4) ^ p[2]) & ((1 << BITS_HASH) - 1);
}

/* 'saida' precisa de pelo menos n + n/8 + 16 bytes. Devolve o tamanho. */
static size_t lzss_comprimir(const uint8_t *in, size_t n, uint8_t *saida) {
    for(int i = 0; i < (1 << BITS_HASH); i++) cabeca[i] = -1;
    size_t o = 0, pos = 0;

    while(pos < n) {
        size_t pos_flags = o++;
        uint8_t flags = 0;

        for(int bit = 0; bit < 8 && pos < n; bit++) {
            int melhor = 0, dist = 0;
            if(pos + MIN_COPIA <= n) {
                int max = (n - pos < MAX_COPIA) ? (int)(n - pos) : MAX_COPIA;
                int cand = cabeca[hash3(in + pos)];
                for(int tentativas = 32; cand >= 0 && pos - cand <= JANELA && tentativas; tentativas--) {
                    int l = 0;
                    while(l < max && in[cand + l] == in[pos + l]) l++;
                    if(l > melhor) { melhor = l; dist = (int)(pos - cand); if(l == max) break; }
                    cand = anterior[cand % JANELA];
                }
            }

            int avancar;
            if(melhor >= MIN_COPIA) {                      /* cópia */
                uint16_t v = (uint16_t)(((dist - 1) << 4) | (melhor - MIN_COPIA));
                saida[o++] = v & 0xFF;
                saida[o++] = v >> 8;
                avancar = melhor;
            }
            else {                                         /* literal */
                flags |= 1 << bit;
                saida[o++] = in[pos];
                avancar = 1;
            }
            for(int k = 0; k < avancar; k++, pos++) {      /* registra no hash */
                if(pos + MIN_COPIA <= n) {
                    unsigned h = hash3(in + pos);
                    anterior[pos % JANELA] = cabeca[h];
                    cabeca[h] = (int)pos;
                }
            }
        }
        saida[pos_flags] = flags;
    }
    return o;
}

static int lzss_descomprimir(const uint8_t *in, size_t n, uint8_t *saida, size_t tam_saida) {
    size_t i = 0, o = 0;
    while(i < n && o < tam_saida) {
        uint8_t flags = in[i++];
        for(int bit = 0; bit < 8 && i < n && o < tam_saida; bit++) {
            if(flags & (1 << bit)) {
                saida[o++] = in[i++];
            }
            else {
                if(i + 1 >= n) return -1;
                uint16_t v = in[i] | (in[i + 1] << 8);
                i += 2;
                size_t dist = (v >> 4) + 1, len = (v & 15) + MIN_COPIA;
                if(dist > o) return -1;                    /* dado corrompido */
                for(size_t k = 0; k < len && o < tam_saida; k++, o++)
                    saida[o] = saida[o - dist];
            }
        }
    }
    return o == tam_saida ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* Ícone da VMU (32x32, 16 cores) — desenho original gerado em código        */
/* ------------------------------------------------------------------------ */
/* 4 bits por pixel (2 pixels por byte, o da esquerda no nibble alto) +
 * paleta de 16 cores ARGB4444. Aparece no gerenciador de memória da BIOS. */

static uint8_t  icone[512];
static uint16_t paleta[16] = {
    0xF111, /* 0 fundo quase preto */
    0xFF80, /* 1 laranja           */
    0xFC22, /* 2 vermelho          */
    0xFFFF, /* 3 branco            */
    0xF555, /* 4 cinza             */
};

static void gerar_icone(void) {
    for(int y = 0; y < 32; y++) {
        for(int x = 0; x < 32; x++) {
            int c = 0, dx = x - 16, dy = y - 15;
            int losango = abs(dx) + abs(dy);
            if(x < 2 || y < 2 || x > 29 || y > 29) c = 1;   /* borda laranja    */
            else if(losango <= 7)                  c = 2;   /* losango vermelho */
            else if(losango <= 9)                  c = 3;   /* contorno branco  */
            else if(y >= 26 && x >= 6 && x < 26)   c = 4;   /* etiqueta cinza   */
            int i = y * 16 + x / 2;
            if(x & 1) icone[i] = (icone[i] & 0xF0) | c;
            else      icone[i] = (icone[i] & 0x0F) | (c << 4);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* VMU -> RAM (no boot)                                                      */
/* ------------------------------------------------------------------------ */

static int achar_vmu(void) {
    vmu = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if(!vmu) {
        printf("VMU: nenhum memory card encontrado - saves ficam só na RAM\n");
        return 0;
    }
    snprintf(dir_vmu, sizeof(dir_vmu), "/vmu/%c%d", 'a' + vmu->port, vmu->unit);
    printf("VMU: usando %s (%d blocos livres)\n", dir_vmu, vmufs_free_blocks(vmu));
    return 1;
}

static void restaurar_slot(int slot) {
    char nome_vmu[32], nome_ram[32];
    snprintf(nome_vmu, sizeof(nome_vmu), "%s/DOOMSAV%d", dir_vmu, slot);
    snprintf(nome_ram, sizeof(nome_ram), "/ram/doomsav%d.dsg", slot);

    size_t tam;
    uint8_t *arq = ler_tudo(nome_vmu, &tam);      /* já vem sem o cabeçalho VMS */
    if(!arq) return;

    uint32_t tam_orig;
    if(tam < 8 || memcmp(arq, MAGICO, 4) != 0) {
        printf("VMU: %s não é um save deste port, ignorado\n", nome_vmu);
        free(arq);
        return;
    }
    memcpy(&tam_orig, arq + 4, 4);
    uint8_t *save = malloc(tam_orig);
    if(save && lzss_descomprimir(arq + 8, tam - 8, save, tam_orig) == 0 &&
       gravar_tudo(nome_ram, save, tam_orig) == 0) {
        assinatura[slot] = hash(save, tam_orig);
        printf("VMU: slot %d restaurado (%lu -> %lu bytes) \"%.24s\"\n", slot,
               (unsigned long)tam, (unsigned long)tam_orig, (const char *)save);
        size_t u = strlen(resumo);
        snprintf(resumo + u, sizeof(resumo) - u, " [slot %d: %lu->%lu bytes \"%.24s\"]", slot,
                 (unsigned long)tam, (unsigned long)tam_orig, (const char *)save);
    }
    else {
        printf("VMU: slot %d corrompido, ignorado\n", slot);
    }
    free(save);
    free(arq);
}

/* ------------------------------------------------------------------------ */
/* RAM -> VMU (numa thread)                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    int      slot;
    uint8_t *dados;
    size_t   tam;
    uint32_t h;
} trabalho_t;

static void gravar_slot_na_vmu(int slot, const uint8_t *dados, size_t tam) {
    uint64_t t0 = timer_ms_gettime64();
    char nome_vmu[32], descr[TAM_DESCR + 1];
    snprintf(nome_vmu, sizeof(nome_vmu), "%s/DOOMSAV%d", dir_vmu, slot);

    /* 1) Comprime: "DCSV" + tamanho original + LZSS */
    uint8_t *pacote = malloc(8 + tam + tam / 8 + 16);
    memcpy(pacote, MAGICO, 4);
    uint32_t t32 = (uint32_t)tam;
    memcpy(pacote + 4, &t32, 4);
    size_t tam_pac = 8 + lzss_comprimir(dados, tam, pacote + 8);
    uint64_t t1 = timer_ms_gettime64();

    /* 2) Cabeçalho VMS: os primeiros 24 bytes do save são a descrição digitada */
    memcpy(descr, dados, TAM_DESCR);
    descr[TAM_DESCR] = 0;
    for(int i = 0; i < TAM_DESCR && descr[i]; i++)
        if(descr[i] < 32 || descr[i] > 126) descr[i] = ' ';

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.desc_short, "DOOM DC SAVE");
    snprintf(pkg.desc_long, sizeof(pkg.desc_long), "%d: %s", slot, descr);
    strcpy(pkg.app_id, APP_ID);
    pkg.icon_cnt = 1;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    memcpy(pkg.icon_pal, paleta, sizeof(paleta));
    pkg.icon_data = icone;

    /* 3) Cabe? (cabeçalho 128 bytes + ícone 512 + dados). Conta também os
     *    blocos que o save antigo deste slot vai liberar. */
    int blocos = (int)((128 + 512 + tam_pac + 511) / 512);
    int livres = vmufs_free_blocks(vmu);
    file_t antigo = fs_open(nome_vmu, O_RDONLY);
    if(antigo >= 0) { livres += (int)((fs_total(antigo) + 128 + 512 + 511) / 512); fs_close(antigo); }

    if(blocos > livres) {
        printf("VMU: sem espaço! save precisa de %d blocos, há %d livres\n", blocos, livres);
    }
    else {
        /* 4) Grava: o fs_vmu monta o cabeçalho com o que passamos no ioctl */
        file_t f = fs_open(nome_vmu, O_WRONLY | O_TRUNC);
        int ok = 0;
        if(f >= 0) {
            fs_ioctl(f, IOCTL_VMU_SET_HDR, &pkg);
            ok = fs_write(f, pacote, tam_pac) == (ssize_t)tam_pac;
            ok = (fs_close(f) == 0) && ok;     /* a escrita real acontece no close */
        }
        printf("VMU: slot %d %s: %lu -> %lu bytes (%d blocos). LZSS %lu ms, VMU %lu ms. \"%s\"\n",
               slot, ok ? "gravado" : "FALHOU", (unsigned long)tam, (unsigned long)tam_pac, blocos,
               (unsigned long)(t1 - t0), (unsigned long)(timer_ms_gettime64() - t1), descr);
    }
    free(pacote);
}

static void *thread_gravar(void *arg) {
    trabalho_t *t = arg;
    gravar_slot_na_vmu(t->slot, t->dados, t->tam);
    assinatura[t->slot] = t->h;
    free(t->dados);
    free(t);
    gravando = 0;
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* Interface usada pelo doomgeneric_dc.c                                     */
/* ------------------------------------------------------------------------ */

void VMU_Iniciar(void) {
    gerar_icone();
    if(!achar_vmu()) { strcpy(resumo, " sem VMU"); return; }
    snprintf(resumo, sizeof(resumo), " %s, %d blocos livres;", dir_vmu, vmufs_free_blocks(vmu));
    for(int s = 0; s < N_SLOTS; s++)
        restaurar_slot(s);
}

/* Repete no log o que aconteceu no boot (o começo do log some da janela serial). */
void VMU_ImprimirResumo(void) {
    printf("VMU (boot):%s\n", resumo);
}

/* Chamar periodicamente (ex.: 1x por segundo). Não bloqueia: a gravação
 * roda numa thread, uma de cada vez. */
void VMU_Sincronizar(void) {
    if(!vmu || gravando) return;
    for(int s = 0; s < N_SLOTS; s++) {
        char nome_ram[32];
        snprintf(nome_ram, sizeof(nome_ram), "/ram/doomsav%d.dsg", s);
        size_t tam;
        uint8_t *d = ler_tudo(nome_ram, &tam);
        if(!d) continue;
        uint32_t h = hash(d, tam);
        if(h != assinatura[s]) {                   /* o jogador salvou neste slot */
            trabalho_t *t = malloc(sizeof(trabalho_t));
            t->slot = s; t->dados = d; t->tam = tam; t->h = h;
            gravando = 1;
            thd_create(1, thread_gravar, t);       /* 1 = thread "destacada" */
            return;                                /* um slot por vez */
        }
        free(d);
    }
}

/* ------------------------------------------------------------------------ */
/* Funções "embrulhadas" com -Wl,--wrap=... (ver Makefile)                   */
/* ------------------------------------------------------------------------ */

char *__wrap_M_GetSaveGameDir(char *iwadname) {
    (void)iwadname;
    printf("Saves: usando /ram/ (sincronizado com a VMU)\n");
    return strdup("/ram/");
}

int __real_rename(const char *de, const char *para);

int __wrap_rename(const char *de, const char *para) {
    if(strncmp(de, "/ram/", 5) == 0 && strncmp(para, "/ram/", 5) == 0) {
        size_t tam;
        uint8_t *d = ler_tudo(de, &tam);
        if(!d) return -1;
        fs_unlink(para);
        int r = gravar_tudo(para, d, tam);
        free(d);
        if(r == 0) fs_unlink(de);
        return r;
    }
    return __real_rename(de, para);
}
