/*
 * SDL_mixer.h VAZIO — "shim" de compatibilidade do port para Dreamcast.
 *
 * O i_sound.c original faz #include <SDL_mixer.h> quando FEATURE_SOUND está
 * ligado, mas não usa nada dele (só comentários citam o SDL). Em vez de editar
 * o código original, damos a ele um cabeçalho vazio com o mesmo nome.
 * O Makefile coloca esta pasta (compat/) no caminho de includes.
 */
#ifndef DC_SHIM_SDL_MIXER_H
#define DC_SHIM_SDL_MIXER_H
#endif
