# Port do Doom (doomgeneric + Freedoom) para Dreamcast.
#
#   ../fontes-baixadas/doomgeneric/  código do jogo (GPL v2), SEM alterações
#   doomgeneric_dc.c                 camada de plataforma do Dreamcast (este port)
#   ../fontes-baixadas/freedoom/     dados livres do jogo (BSD), vão para o CD

SRC   = ../fontes-baixadas/doomgeneric/doomgeneric
WAD   = ../fontes-baixadas/freedoom/extraido/freedoom-0.13.0/freedoom1.wad
BUILD = build

TARGET = doom.elf

# Mesma lista do Makefile original do doomgeneric, trocando o arquivo de
# plataforma (doomgeneric_xlib.o) pelo nosso (doomgeneric_dc.o).
DOOM_OBJS = dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop \
	d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom \
	i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config \
	m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter \
	p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight p_spec \
	p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane r_segs r_sky \
	r_things sha1 sounds statdump st_lib st_stuff s_sound tables v_video wi_stuff \
	w_checksum w_file w_main w_wad z_zone i_input i_video doomgeneric mus2mid

# Arquivos do port (substituem/complementam os originais):
#   doomgeneric_dc.c  camada de plataforma (vídeo, controle, tempo)
#   w_file_dc.c       no lugar de w_file_stdc.c: leitura do WAD com cache de CD
#   i_sound_dc.c      módulo de som: efeitos do WAD no AICA
#   i_music_dc.c      módulo de música: tocador MIDI/MUS + sintetizador (stream)
#   compat/           "shims" de cabeçalhos que o código original inclui à toa
#   vmu_saves_dc.c    saves: Doom salva em /ram/, sincronizado com a VMU
# (mus2mid, acima, é do doomgeneric: converte música MUS para MIDI)
PORT_OBJS = doomgeneric_dc w_file_dc i_sound_dc i_music_dc vmu_saves_dc

# --wrap: chamadas a M_GetSaveGameDir/rename vão para __wrap_M_GetSaveGameDir /
# __wrap_rename (em vmu_saves_dc.c), sem editar o código original do jogo.
WRAPS = -Wl,--wrap=M_GetSaveGameDir -Wl,--wrap=rename

OBJS = $(addprefix $(BUILD)/,$(addsuffix .o,$(DOOM_OBJS) $(PORT_OBJS)))

# 320x200 = resolução nativa do Doom; -O2 para o SH-4.
# FEATURE_SOUND liga o som (o doomgeneric então procura DG_sound_module).
# MOSTRAR_FPS mostra o painel de FPS e CPU da música (remova para esconder).
DOOM_CFLAGS = -O2 -DNORMALUNIX -D_DEFAULT_SOURCE -DFEATURE_SOUND -DMOSTRAR_FPS \
	-DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -Icompat -I$(SRC) -w

# Teste automático dos saves:  make TESTE=1 (salva sozinho) / make TESTE=2 (carrega)
ifdef TESTE
DOOM_CFLAGS += -DTESTE_VMU=$(TESTE)
endif

all: rm-elf $(TARGET) cd/freedoom1.wad

include $(KOS_BASE)/Makefile.rules

$(BUILD)/%.o: $(SRC)/%.c
	@mkdir -p $(BUILD)
	kos-cc $(DOOM_CFLAGS) -c $< -o $@

# Arquivos do port (nesta pasta): com avisos ligados (-Wall)
$(BUILD)/%.o: %.c
	@mkdir -p $(BUILD)
	kos-cc $(DOOM_CFLAGS) -Wall -c $< -o $@

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) $(WRAPS) -lm

# O WAD vai para a raiz do CD (o mkcdi.sh copia a pasta cd/)
cd/freedoom1.wad: $(WAD)
	@mkdir -p cd
	cp $< $@

clean: rm-elf
	-rm -rf $(BUILD) cd build-cd

rm-elf:
	-rm -f $(TARGET)

.PHONY: all clean rm-elf
