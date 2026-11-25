/*
 * i_sound_dc.c — EFEITOS SONOROS do Doom no chip de som AICA do Dreamcast.
 *
 * O Doom fala com o som por um "módulo" (sound_module_t, em i_sound.h).
 * Aqui implementamos esse módulo usando o gerenciador de efeitos do KOS,
 * exatamente como no projeto 08-audio:
 *
 *   1. O som está DENTRO do WAD, no formato DMX (da biblioteca de som do Doom
 *      original):
 *          bytes 0-1  formato (3)
 *          bytes 2-3  taxa de amostragem (geralmente 11025 Hz)
 *          bytes 4-7  nº de amostras
 *          depois     PCM de 8 bits SEM sinal, com 16 bytes "de enchimento"
 *                     no começo e no fim (o DMX os ignorava; nós também)
 *   2. Na 1ª vez que um som toca, convertemos para 8 bits COM sinal (é o que
 *      o AICA espera: basta inverter o bit mais alto, x ^ 0x80) e enviamos
 *      para a RAM de som com snd_sfx_load_raw_buf(). Fica lá para sempre:
 *      das próximas vezes, tocar custa só um comando para o ARM7.
 *   3. Cada "canal" do Doom vira um canal do AICA reservado para nós.
 *   4. Quando um monstro anda, o Doom chama UpdateSoundParams: mandamos um
 *      comando AICA_CH_CMD_UPDATE mudando volume e pan do canal AO VIVO.
 *
 * Música: ver i_music_dc.c (tocador de MIDI/MUS + sintetizador por stream).
 *
 * Licença: GPL v2 (parte do port do doomgeneric).
 */

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/aica_comm.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "doomtype.h"
#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define N_CANAIS 16                     /* o Doom usa 8 por padrão (snd_channels) */

/* Variáveis de configuração que o i_sound.c espera encontrar (normalmente
 * definidas no módulo SDL, que não usamos). O AICA faz a conversão de taxa
 * de amostragem no hardware, então não precisamos de libsamplerate. */
int   use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

typedef struct {
    sfxhnd_t hnd;                       /* som na RAM do AICA (0 = falhou) */
    uint32_t duracao_ms;
} som_dc_t;

static int      canal_aica[N_CANAIS];   /* canal do AICA de cada canal do Doom */
static uint64_t termina_em[N_CANAIS];   /* para SoundIsPlaying */
static boolean  usar_prefixo;
static unsigned bytes_na_ram_de_som;

/* ------------------------------------------------------------------------ */

static boolean DC_Init(boolean use_sfx_prefix) {
    usar_prefixo = use_sfx_prefix;
    /* Liga o AICA (envia o driver do ARM7) E prepara o sistema de stream,
     * que a música (i_music_dc.c) vai usar. snd_stream_init() chama snd_init(). */
    snd_stream_init();
    for(int i = 0; i < N_CANAIS; i++) {
        canal_aica[i] = snd_sfx_chn_alloc();  /* reserva canais só para o Doom */
        termina_em[i] = 0;
    }
    printf("I_SoundDC: AICA pronto, %d canais reservados\n", N_CANAIS);
    return true;
}

static void DC_Shutdown(void) {
    for(int i = 0; i < N_CANAIS; i++)
        if(canal_aica[i] >= 0) snd_sfx_chn_free(canal_aica[i]);
    snd_sfx_unload_all();
}

static int DC_GetSfxLumpNum(sfxinfo_t *sfx) {
    char nome[9];
    if(sfx->link)                       /* sons "linkados" usam o lump de outro */
        sfx = sfx->link;
    if(usar_prefixo)
        M_snprintf(nome, sizeof(nome), "ds%s", DEH_String(sfx->name));
    else
        M_StringCopy(nome, DEH_String(sfx->name), sizeof(nome));
    return W_GetNumForName(nome);
}

/* Converte o som DMX do WAD e o envia para a RAM de som (uma vez só). */
static som_dc_t *carregar_som(sfxinfo_t *sfx) {
    if(sfx->driver_data)
        return sfx->driver_data;

    som_dc_t *s = malloc(sizeof(som_dc_t));
    s->hnd = 0;
    s->duracao_ms = 0;
    sfx->driver_data = s;               /* mesmo se falhar: não tenta de novo */

    int lump = sfx->lumpnum;
    const uint8_t *d = W_CacheLumpNum(lump, PU_STATIC);
    unsigned tam_lump = W_LumpLength(lump);

    if(tam_lump >= 8 && d[0] == 3 && d[1] == 0) {
        uint32_t taxa = d[2] | (d[3] << 8);
        uint32_t n = d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24);
        if(n <= tam_lump - 8 && n > 48) {
            const uint8_t *amostras = d + 8 + 16;   /* pula cabeçalho + enchimento */
            n -= 32;
            uint32_t n4 = (n + 3) & ~3u;            /* múltiplo de 4 bytes */
            int8_t *buf = memalign(32, n4);
            for(uint32_t i = 0; i < n; i++)
                buf[i] = (int8_t)(amostras[i] ^ 0x80);   /* sem sinal -> com sinal */
            for(uint32_t i = n; i < n4; i++)
                buf[i] = 0;

            s->hnd = snd_sfx_load_raw_buf((char *)buf, n4, taxa, 8, 1);
            s->duracao_ms = n * 1000 / taxa;
            free(buf);
            if(s->hnd) {
                bytes_na_ram_de_som += n4;
                printf("[som] ds%-6s %5lu Hz %6lu bytes -> AICA (total %u KB)\n", sfx->name,
                       (unsigned long)taxa, (unsigned long)n4, bytes_na_ram_de_som / 1024);
            }
            else
                printf("I_SoundDC: RAM de som cheia? falhou %s\n", sfx->name);
        }
    }
    W_ReleaseLumpNum(lump);
    return s;
}

static inline int pan_do_doom(int sep) {
    /* Doom: sep 0 = esquerda, 127 = centro, 254 = direita. AICA: 0..255 */
    int p = sep * 255 / 254;
    return p < 0 ? 0 : (p > 255 ? 255 : p);
}

static inline int vol_do_doom(int vol) {
    /* Doom: 0..127. AICA: 0..255 */
    int v = vol * 2;
    return v > 255 ? 255 : v;
}

static int DC_StartSound(sfxinfo_t *sfx, int channel, int vol, int sep) {
    if(channel < 0 || channel >= N_CANAIS || canal_aica[channel] < 0)
        return -1;
    som_dc_t *s = carregar_som(sfx);
    if(!s->hnd)
        return -1;

    snd_sfx_stop(canal_aica[channel]);
    snd_sfx_play_chn(canal_aica[channel], s->hnd, vol_do_doom(vol), pan_do_doom(sep));
    termina_em[channel] = timer_ms_gettime64() + s->duracao_ms;
    return channel;
}

/* Muda volume/pan de um som QUE JÁ ESTÁ TOCANDO (ex.: o monstro andou).
 * O sfxmgr não tem função pública para isso, então mandamos o comando
 * direto para o driver do ARM7, como o próprio KOS faz em snd_stream_volume(). */
static void DC_UpdateSoundParams(int channel, int vol, int sep) {
    if(channel < 0 || channel >= N_CANAIS || canal_aica[channel] < 0)
        return;
    AICA_CMDSTR_CHANNEL(tmp, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = canal_aica[channel];
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_VOL | AICA_CH_UPDATE_SET_PAN;
    chan->vol = vol_do_doom(vol);
    chan->pan = pan_do_doom(sep);
    snd_sh4_to_aica(tmp, cmd->size);
}

static void DC_StopSound(int channel) {
    if(channel < 0 || channel >= N_CANAIS || canal_aica[channel] < 0)
        return;
    snd_sfx_stop(canal_aica[channel]);
    termina_em[channel] = 0;
}

static boolean DC_SoundIsPlaying(int channel) {
    if(channel < 0 || channel >= N_CANAIS)
        return false;
    return timer_ms_gettime64() < termina_em[channel];
}

static void DC_Update(void) {
    /* nada: o AICA mixa os canais sozinho, sem ajuda do SH-4 */
}

static void DC_CacheSounds(sfxinfo_t *sounds, int num_sounds) {
    /* Carregamos sob demanda (na 1ª vez que tocam) para não alongar a
     * inicialização lendo dezenas de sons do CD. */
    (void)sounds; (void)num_sounds;
}

static snddevice_t dispositivos_sfx[] = {
    SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32,
};

sound_module_t DG_sound_module = {
    dispositivos_sfx,
    sizeof(dispositivos_sfx) / sizeof(dispositivos_sfx[0]),
    DC_Init,
    DC_Shutdown,
    DC_GetSfxLumpNum,
    DC_Update,
    DC_UpdateSoundParams,
    DC_StartSound,
    DC_StopSound,
    DC_SoundIsPlaying,
    DC_CacheSounds,
};

/* A música (DG_music_module) está em i_music_dc.c. */

/* Para o HUD/depuração: quanto da RAM de som os efeitos já ocupam. */
unsigned I_SoundDC_BytesUsados(void) { return bytes_na_ram_de_som; }
