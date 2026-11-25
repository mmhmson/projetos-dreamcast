#!/usr/bin/env bash
#
# mkcdi.sh — transforma um .elf do KallistiOS numa imagem .cdi "selfboot"
# (MIL-CD) que roda em emulador, em GDEMU ou gravada num CD-R.
#
# Uso (dentro do shell do DreamSDK, na pasta do projeto):
#     mkcdi.sh programa.elf "TITULO DO JOGO"
#
# ============================ COMO O DREAMCAST DÁ BOOT ============================
#
#  1. A BIOS lê os primeiros 16 setores (32 KB) da última sessão do disco:
#     é o IP.BIN ("Initial Program"). Ele tem:
#       - um cabeçalho de texto (título, região, periféricos, nome do executável);
#       - o código de segurança da Sega + a tela de licença com o logo;
#       - o bootstrap que carrega o executável.
#  2. O IP.BIN carrega o executável indicado (1ST_READ.BIN) para 0x8C010000
#     e salta para ele.
#  3. Em discos MIL-CD (CD-R comuns, como os de homebrew), a BIOS espera que o
#     1ST_READ.BIN esteja "SCRAMBLED": embaralhado em blocos, um esquema antigo
#     anti-cópia. Se não estiver embaralhado, o console trava na tela preta.
#
#  Por que "-C 0,11702" no mkisofs? Um CD-R de Dreamcast tem 2 sessões: uma
#  de áudio curta e a de dados começando no setor 11702. O ISO precisa ser
#  gerado sabendo que vai morar nessa posição (endereços absolutos dos arquivos).
#  O cdi4dc então monta a imagem Audio/Data no formato DiscJuggler (.cdi).
# ================================================================================

set -euo pipefail

ELF="${1:?uso: mkcdi.sh programa.elf \"TITULO\"}"
TITULO="${2:-HOMEBREW}"
NOME="$(basename "$ELF" .elf)"
TMP="build-cd"

echo "==> Gerando ${NOME}.cdi a partir de ${ELF}"
rm -rf "$TMP"
mkdir -p "$TMP/disco"

# 1) ELF -> binário "cru" (só bytes que vão para a memória, sem cabeçalho ELF).
#    -R .stack remove a seção da pilha, que não precisa ir para o disco.
echo "--> [1/5] objcopy: ELF -> binário"
"${KOS_OBJCOPY:-sh-elf-objcopy}" -R .stack -O binary "$ELF" "$TMP/$NOME.bin"

# 2) Embaralha o binário para o boot de MIL-CD.
echo "--> [2/5] scramble: binário -> 1ST_READ.BIN"
scramble "$TMP/$NOME.bin" "$TMP/disco/1ST_READ.BIN"

# 2b) Se o projeto tem uma pasta cd/, o conteúdo dela vai para a raiz do disco
#     (acessível no Dreamcast como /cd/...).
if [ -d cd ]; then
    echo "--> [2b]  copiando pasta cd/ para o disco"
    cp -r cd/. "$TMP/disco/"
    find "$TMP/disco" -name '.copiado' -delete
fi

# 3) Gera o setor de boot com os nossos dados.
echo "--> [3/5] makeip: gerando IP.BIN"
makeip -f -g "$TITULO" -c "ESTUDOS DC" -n "T0000" "$TMP/IP.BIN" > /dev/null

# 4) Monta o sistema de arquivos ISO9660 com o IP.BIN no setor de boot (-G),
#    já posicionado para a 2ª sessão (-C 0,11702).
echo "--> [4/5] mkisofs: montando ISO"
mkisofs -quiet -C 0,11702 -V "${NOME^^}" -G "$TMP/IP.BIN" -r -J -l \
        -o "$TMP/$NOME.iso" "$TMP/disco"

# 5) ISO -> CDI (Audio/Data, formato aceito pelo GDEMU e emuladores).
echo "--> [5/5] cdi4dc: ISO -> CDI"
cdi4dc "$TMP/$NOME.iso" "$NOME.cdi" > /dev/null

echo "==> Pronto: $(pwd)/${NOME}.cdi ($(du -h "${NOME}.cdi" | cut -f1))"
