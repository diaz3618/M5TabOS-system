// O OBJETO DE DESENHO DO SISTEMA (nome historico: "tela virtual").
//
// Um LovyanGFX comum cujo painel e um RASTREADOR: anota cada retangulo que
// qualquer primitiva do M5GFX tocou e o entrega ao compositor
// (comp_invalidar). Home, barras e todos os apps desenham aqui, sem saber de
// nada disso.
//
// MODO DIRETO (COMPOSITOR.md): o rastreador escreve na PAGINA DE TRAS do
// painel. A primeira escrita de cada volta abre o quadro (comp_abrir: espera a
// troca anterior e sincroniza), e so entao o pixel e escrito. Ate a etapa 3a
// isto era um canvas de 1,84 MB na PSRAM que o compositor copiava para a
// pagina a cada quadro - a copia saiu, e a memoria tambem.
//
// O que continua valendo, sem mudar uma linha de app:
//   - nenhum desenho acontece na pagina que o painel esta varrendo;
//   - as areas mudadas sao descobertas SOZINHAS.
// O que mudou: o conteudo NAO sobrevive mais ao repouso (as paginas morrem
// com o painel). Ao acordar, o core redesenha tudo - como ja fazia.

#pragma once

#include <M5Unified.h>

// Liga o objeto de desenho as paginas do painel e o torna o alvo do sistema
// (tab5_tela_definir_alvo). Chamar uma vez, da tarefa de interface.
bool tela_virtual_iniciar(void);

// O alvo de desenho. Enquanto ele nao existir, e o M5.Display.
lgfx::LovyanGFX& tela_virtual(void);

// Copia a rotacao do M5.Display (o Mesh abre em paisagem). O toque continua
// sendo convertido pelo M5.Display; aqui so precisa desenhar igual.
void tela_virtual_seguir_rotacao(void);

// O retangulo (x, y, w, h), em coordenadas de desenho, vai ser INTEIRO
// sobrescrito por quem chamou, ainda neste quadro - o que a pagina de tras
// tinha de atrasado ali nao precisa vir da frente. E o que o fillRect ja diz
// sozinho; serve para quem cobre com imagem (pushSprite) em vez de cor.
void tela_virtual_cobrir(int x, int y, int w, int h);

// Pede a tela inteira no proximo quadro (qualquer tarefa).
void tela_virtual_invalidar_tudo(void);

// Devolve a superficie de DMA (sistema_superficie) do app que fechou. Chamar
// depois do ao_fechar() - quem escrevia nela (a camera) ja parou.
void tela_virtual_soltar_superficie(void);
