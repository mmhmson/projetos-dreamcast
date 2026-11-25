# 10-formula — protótipo do jogo novo de fórmula:
# pista em MALHA 3D low poly no PowerVR + física 2.5D em coordenadas da pista.

TARGET = formula.elf
OBJS   = main.o som.o
KOS_CFLAGS += -O2

# make DEMO=1 -> piloto automático (para testar/filmar sem controle)
ifdef DEMO
KOS_CFLAGS += -DDEMO_AUTO
endif
# make OPC="-DCAM_INI=1 -DS_INI=430" -> muda constantes para testes
ifdef OPC
KOS_CFLAGS += $(OPC)
endif

# make ESTUDO=1 -> build SÓ PARA ESTUDO, com a gravação de motor de estudo/
# (estudo/motor_gravado.h, gerado por estudo/estudo.ps1). Sai em outro .elf
# e outro .o, para a gravação nunca entrar no formula.elf/formula.cdi normal.
ifdef ESTUDO
TARGET = formula-estudo.elf
OBJS   = main.o som_estudo.o
endif

all: rm-elf $(TARGET)

include $(KOS_BASE)/Makefile.rules

# texturas geradas no PC (não vão para o git): criadas na 1ª compilação
texturas.bin texturas.h:
	py -I gerar_texturas.py

main.o: main.c pista_interlagos.h pista_osm.h som.h ceu.bin texturas.bin texturas.h relevo.h $(wildcard carro_obj.h)

som_estudo.o: som.c som.h estudo/motor_gravado.h
	kos-cc $(CFLAGS) -DSOM_GRAVADO -Iestudo -c som.c -o som_estudo.o

$(TARGET): $(OBJS)
	kos-cc -o $(TARGET) $(OBJS) -lm

clean: rm-elf
	-rm -f $(OBJS) som_estudo.o formula-estudo.elf
	-rm -rf build-cd

rm-elf:
	-rm -f $(TARGET)

.PHONY: all clean rm-elf
