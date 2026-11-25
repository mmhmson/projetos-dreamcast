/* som.h — áudio sintetizado do protótipo de fórmula. */
#ifndef SOM_H
#define SOM_H

void som_iniciar(void);
void som_atualizar(void);                                   /* 1x por quadro */
void som_motor(float rpm, float acelerador, float derrapagem, float na_grama);  /* tudo 0..1 */
void som_rival(float rpm, float volume, float pan, float doppler); /* rival mais próximo; pan 0 = esq., 1 = dir.; doppler = fator de tom */
void som_torcida(float volume);                             /* 0..1 */
void som_bip(int largada);                                  /* 0: bip das luzes, 1: largada */
void som_batida(float volume);                              /* contato/muro */

void som_chuva(float chuva, float agua_pneu);                /* 0..1 cada */
void som_trovao(void);

#endif
