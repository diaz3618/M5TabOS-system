#!/usr/bin/env python3
# Gera main/tab5_emoji_dados.cpp com TODO emoji de UM SO ponto de codigo -
# 1391 na versao 17.0 do Unicode, 1357 com desenho no Twemoji. Arte do Twemoji (twemoji.twitter.com,
# CC-BY 4.0). Roda uma vez, offline depois disso - o firmware nunca baixa nada
# em tempo de execucao, so os bytes ja convertidos que este script deixa
# gravados no repositorio.
#
#   pip3 install --user Pillow
#   ./generate_emoji.py
#
# "UM SO PONTO DE CODIGO": o seletor de estilo (U+FE0F) NAO conta - a propria
# biblioteca de tela ja o descarta antes de qualquer emoji chegar ao callback
# (ver LGFXBase.cpp). O que fica de fora sao bandeiras (pares de indicador
# regional), tons de pele e sequencias com ZWJ (familia, profissoes) - cada
# uma e VARIOS pontos de codigo colados numa imagem so, e o hook do
# LovyanGFX dispara um callback POR CODIGO. Da para fazer, mas e outro
# projeto: juntar N chamadas de callback numa imagem composta.
#
# FORMATO COM PALETA: cada emoji guarda as suas cores (no maximo 256, e
# nenhum passa de 237) e um indice de 1 byte por pixel - 0,95MB em vez de
# 1,49MB do RGB565 cru, com a mesma imagem pixel a pixel. O indice 0 e sempre
# o fundo transparente.
#
# Ver main/tab5_emoji.hpp para o formato da tabela gerada e main/tab5_emoji.cpp
# para o callback que a consome (busca BINARIA - a tabela tem que continuar
# ORDENADA por codigo, e este script ja garante isso).

import os
import re
import subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed

TAM = 24
SENTINELA = 0xF81F  # RGB565 de magenta puro - nunca aparece em emoji de verdade
FONTE_URL = "https://unicode.org/Public/emoji/latest/emoji-test.txt"
IMG_BASE = "https://raw.githubusercontent.com/twitter/twemoji/master/assets/72x72"

AQUI = os.path.dirname(os.path.abspath(__file__))
AQUI = os.path.dirname(AQUI)   # a RAIZ do projeto - o script mora em tools/
CACHE = os.path.join(AQUI, ".emoji_cache")
SAIDA = os.path.join(AQUI, "main", "tab5_emoji_dados.cpp")

PADRAO_LINHA = re.compile(
    r'^([0-9A-Fa-f ]+);\s*(\S+)\s*#\s*(\S+)\s*E[\d.]+\s*(.*)$'
)


def buscar_lista_oficial():
    """Devolve [(codigo, nome_em_ingles), ...] ordenada por codigo - so os
    de UM ponto de codigo, fully-qualified (a forma "completa" e canonica de
    cada emoji; minimally-qualified/unqualified sao variantes do mesmo)."""
    r = subprocess.run(["curl", "-sf", FONTE_URL], capture_output=True, text=True, check=True)
    entradas = {}
    for linha in r.stdout.splitlines():
        if not linha or linha.startswith('#'):
            continue
        m = PADRAO_LINHA.match(linha)
        if not m:
            continue
        codes_s, status, _emoji_char, nome = m.groups()
        if status != 'fully-qualified':
            continue
        codigos = [c for c in codes_s.strip().split(' ') if c.upper() != 'FE0F']
        if len(codigos) != 1:
            continue   # bandeira, tom de pele ou sequencia ZWJ - fora do escopo
        cp = int(codigos[0], 16)
        entradas[cp] = nome.strip()
    return sorted(entradas.items())


def baixar_um(cp):
    # curl, e nao urllib: o Python deste Mac nao tem o certificado raiz
    # configurado (erro classico do instalador python.org), e curl usa o
    # certificado do sistema, que ja funciona.
    nome_arquivo = f"{cp:x}"
    caminho = os.path.join(CACHE, f"{nome_arquivo}.png")
    if os.path.exists(caminho):
        return cp, caminho, None
    url = f"{IMG_BASE}/{nome_arquivo}.png"
    r = subprocess.run(["curl", "-sf", "-o", caminho, url])
    if r.returncode != 0:
        if os.path.exists(caminho):
            os.remove(caminho)   # curl pode deixar arquivo vazio/parcial no erro
        return cp, None, f"HTTP falhou para U+{cp:04X}"
    return cp, caminho, None


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def trocar_bytes(v):
    # O LovyanGFX le imagem uint16_t como RGB565 com os bytes TROCADOS
    # (swap565_t) quando setSwapBytes nao esta ligado - e o caso do sistema.
    # Gravar ja trocado faz a cor chegar certa sem mexer no estado da tela.
    return ((v & 0xFF) << 8) | (v >> 8)


def converter(caminho):
    from PIL import Image
    img = Image.open(caminho).convert("RGBA").resize((TAM, TAM), Image.LANCZOS)
    px = img.load()
    valores = []
    for y in range(TAM):
        for x in range(TAM):
            r, g, b, a = px[x, y]
            if a < 128:
                v = SENTINELA
            else:
                v = rgb565(r, g, b)
                if v == SENTINELA:
                    v ^= 0x0001   # nunca colidir com a sentinela de verdade
            valores.append(trocar_bytes(v))
    return valores


def paletizar(valores):
    """Pixels RGB565 (ja trocados) -> (paleta, indices). A sentinela vira o
    indice 0 - o 'transparente' do desenho -, as outras cores entram na
    ordem em que aparecem."""
    sentinela = trocar_bytes(SENTINELA)
    paleta = [sentinela]
    pos = {sentinela: 0}
    indices = []
    for v in valores:
        if v not in pos:
            pos[v] = len(paleta)
            paleta.append(v)
        indices.append(pos[v])
    if len(paleta) > 256:
        raise ValueError(f"{len(paleta)} cores - nao cabe em indice de 1 byte")
    return paleta, indices


def escrever(itens):
    """itens = [(codigo, nome, pixels RGB565 ja trocados), ...] em ordem de
    codigo. Grava SAIDA."""
    linhas = [
        "// GERADO por generate_emoji.py a partir da lista oficial de emoji do",
        "// Unicode (emoji-test.txt) e da arte do Twemoji (CC-BY 4.0,",
        "// twemoji.twitter.com). Nao editar a mao - rode o script de novo.",
        "//",
        "// ORDENADA POR CODIGO DE PROPOSITO: tab5_emoji.cpp busca aqui por",
        "// busca BINARIA, nao varredura - com mais de mil entradas isso importa.",
        "//",
        "// Cada emoji: uma paleta RGB565 (bytes trocados, indice 0 = transparente)",
        "// e um indice de 1 byte por pixel.",
        '#include "tab5_emoji.hpp"',
        "",
    ]
    tabela = []
    total = 0
    for cp, nome, valores in itens:
        paleta, indices = paletizar(valores)
        arr = f"s_emoji_{cp:x}"
        pal = f"s_paleta_{cp:x}"
        linhas.append(f"// U+{cp:04X} - {nome}")
        linhas.append(f"static const uint16_t {pal}[{len(paleta)}] = {{")
        for k in range(0, len(paleta), 16):
            linhas.append("  " + ",".join(f"0x{v:04X}" for v in paleta[k:k + 16]) + ",")
        linhas.append("};")
        linhas.append(f"static const uint8_t {arr}[{TAM}*{TAM}] = {{")
        for k in range(0, len(indices), 24):
            linhas.append("  " + ",".join(str(v) for v in indices[k:k + 24]) + ",")
        linhas.append("};")
        tabela.append((cp, arr, pal))
        total += len(indices) + 2 * len(paleta)

    linhas.append("")
    linhas.append(f"const int TAB5_EMOJI_LADO = {TAM};")
    linhas.append("")
    linhas.append("// ORDENADA por codigo - nao embaralhar. Ver o aviso no topo.")
    linhas.append("const tab5_emoji_t TAB5_EMOJI_TABELA[] = {")
    for cp, arr, pal in tabela:
        linhas.append(f"  {{ 0x{cp:X}, {arr}, {pal} }},")
    linhas.append("};")
    linhas.append(f"const int TAB5_EMOJI_N = {len(tabela)};")

    with open(SAIDA, "w") as f:
        f.write("\n".join(linhas) + "\n")

    print(f"gravado {SAIDA}: {len(tabela)} emoji, {TAM}x{TAM} com paleta, "
          f"{total} bytes ({total/1024/1024:.2f} MB)")


def main():
    os.makedirs(CACHE, exist_ok=True)

    print("buscando a lista oficial de emoji (unicode.org)...")
    lista = buscar_lista_oficial()
    print(f"{len(lista)} emoji de um so ponto de codigo")

    print("baixando (20 ao mesmo tempo, cache em .emoji_cache/)...")
    caminhos = {}
    falhas = []
    with ThreadPoolExecutor(max_workers=20) as ex:
        futuros = [ex.submit(baixar_um, cp) for cp, _ in lista]
        feitos = 0
        for fut in as_completed(futuros):
            cp, caminho, erro = fut.result()
            feitos += 1
            if erro:
                falhas.append((cp, erro))
            else:
                caminhos[cp] = caminho
            if feitos % 200 == 0:
                print(f"  {feitos}/{len(lista)}...")

    if falhas:
        print(f"AVISO: {len(falhas)} sem imagem no Twemoji (pulados):")
        for cp, erro in falhas[:20]:
            print(f"  U+{cp:04X}: {erro}")
        if len(falhas) > 20:
            print(f"  ... e mais {len(falhas) - 20}")

    itens = []
    for i, (cp, nome) in enumerate(lista):
        caminho = caminhos.get(cp)
        if not caminho:
            continue
        itens.append((cp, nome, converter(caminho)))
        if (i + 1) % 200 == 0:
            print(f"  convertido {i + 1}/{len(lista)}...")

    escrever(itens)


if __name__ == "__main__":
    main()
