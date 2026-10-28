// O OBJETO DE DESENHO DO SISTEMA (name historico: "screen virtual").
//
// Um LovyanGFX comum cujo panel e one RASTREADOR: anota cada rectangle que
// qualquer primitiva of M5GFX tocou e o entrega on compositor
// (comp_invalidar). Home, barras e todos os apps desenham aqui, sem saber of
// nada disso.
//
// MODO DIRETO (COMPOSITOR.md): o rastreador escreve in PAGINA DE TRAS of
// panel. A primeira escrita of cada iteration abre o frame (comp_abrir: espera a
// troca anterior e sincroniza), e so entao o pixel e escrito. Ate a etapa 3a
// isto era one canvas of 1,84 MB in PSRAM que o compositor copiava para a
// pagina a cada frame - a copia exited, e a memoria tambem.
//
// O que continua valendo, sem mudar uma line of app:
//   - nenhum drawing acontece in pagina que o panel esta varrendo;
//   - as areas mudadas sao descobertas SOZINHAS.
// O que changed: o content NAO sobrevive mais on sleep (as paginas morrem
// com o panel). Ao wake, o core redesenha everything - como ja fazia.

#pragma once

#include <M5Unified.h>

// Liga o objeto of drawing as paginas of panel e o torna o alvo of sistema
// (tab5_screen_set_target). Chamar uma vez, of tarefa of interface.
bool virtual_screen_start(void);

// O alvo of drawing. Enquanto ele nao existir, e o M5.Display.
lgfx::LovyanGFX& virtual_screen(void);

// Copia a rotation of M5.Display (o Mesh abre em paisagem). O touch continua
// sendo convertido pelo M5.Display; aqui so precisa draw igual.
void virtual_screen_follow_rotation(void);

// O rectangle (x, y, w, h), em coordenadas of drawing, vai ser INTEIRO
// sobrescrito por quem chamou, ainda neste frame - o que a pagina of tras
// tinha of atrasado ali nao precisa vir of frente. E o que o fillRect ja diz
// sozinho; serve para quem cobre com imagem (pushSprite) em vez of color.
void virtual_screen_cover(int x, int y, int w, int h);

// Pede a screen inteira no next frame (qualquer tarefa).
void virtual_screen_invalidate_everything(void);

// Devolve a surface of DMA (sistema_superficie) of app que fechou. Chamar
// depois of ao_fechar() - quem escrevia nela (a camera) ja parou.
void virtual_screen_release_surface(void);
