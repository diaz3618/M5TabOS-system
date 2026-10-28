# Gera as fontes SUAVIZADAS do M5tabOS.
#
# Por que: o sistema desenhava todo texto com UMA fonte bitmap (FreeSans 12pt,
# 29 px, 1 bit por pixel) esticada ou encolhida por fracoes (0,88x, 0,63x...).
# Escala fracionaria de bitmap duplica umas linhas de pixel e some com outras:
# a letra sai serrilhada e desigual - a barra de escrita (0,88x) era o pior
# caso. Aqui cada tamanho nasce NATIVO, desenhado pelo FreeType (via Pillow)
# com antialiasing de 8 bits, no formato VLW que o LovyanGFX desenha misturando
# a borda da letra com o fundo.
#
# Fonte: Roboto (licenca Apache 2.0). Regular para o corpo, Medium para o
# negrito. Com os acentos do portugues (a fonte bitmap antiga nao tinha).
#
# Rodar: python3 tools/generate_fonts.py   (gera main/fonts/soft_fonts.cpp)
import os
from PIL import ImageFont

AQUI = os.path.dirname(os.path.abspath(__file__))
AQUI = os.path.dirname(AQUI)   # a RAIZ do projeto - o script mora em tools/
SAIDA = os.path.join(AQUI, "main", "fonts", "soft_fonts.cpp")

FAMILIAS = [
    ("corpo",   os.path.join(AQUI, "tools", "Roboto-Regular.ttf")),
    ("negrito", os.path.join(AQUI, "tools", "Roboto-Medium.ttf")),
]

# ALTURAS DE LINHA em pixels (o que d.fontHeight() devolve). O sistema pede
# tamanho no "estilo antigo" N (altura = N*8 px, herdado da Font0); a tabela
# cobre de 12 a 40 px de 2 em 2 e os grandes de 4 em 4 - o pedido cai no mais
# proximo, a no maximo 1 px (2 px nos grandes) do que foi pedido.
ALTURAS = list(range(12, 42, 2)) + [44, 48, 52, 56]

# ASCII imprimivel + o que o portugues usa + alguns simbolos uteis.
CARACTERES = [chr(c) for c in range(0x20, 0x7F)]
CARACTERES += list("¡ª«°±²³µ·º»¿ÀÁÂÃÄÇÈÉÊËÍÎÏÑÒÓÔÕÖÚÛÜàáâãäçèéêëíîïñòóôõöùúûü×÷")
CARACTERES += list("–—‘’“”•…←↑→↓")


def metricas_ascii(fonte):
    # Como o LovyanGFX calcula a altura da linha: maior subida e maior descida
    # entre os glifos ASCII (fora o espaco) - os acentos maiusculos ficam fora
    # da conta dele, e aqui tambem.
    sobe = desce = 0
    for c in range(0x21, 0x7F):
        esq, topo, dir_, base = fonte.getbbox(chr(c), anchor="ls")
        sobe = max(sobe, -topo)
        desce = max(desce, base)
    return sobe, desce


def fonte_para_altura(caminho, alvo):
    # Procura o tamanho em pixels cuja linha (subida+descida) da o alvo.
    melhor = None
    for dezimos in range(60, 800):
        tam = dezimos / 10.0
        f = ImageFont.truetype(caminho, tam)
        s, d = metricas_ascii(f)
        if s + d >= alvo:
            melhor = f
            break
    return melhor


def vlw(fonte):
    glifos = []
    for ch in sorted(set(CARACTERES), key=ord):
        if ch != " " and fonte.getbbox(ch, anchor="ls")[2] == 0 and ord(ch) > 0x7E:
            continue
        avanco = int(round(fonte.getlength(ch)))
        if ch == " ":
            glifos.append((ord(ch), 0, 0, avanco, 0, 0, b""))
            continue
        mascara, (ox, oy) = fonte.getmask2(ch, mode="L", anchor="ls")
        w, h = mascara.size
        if w == 0 or h == 0:
            glifos.append((ord(ch), 0, 0, avanco, 0, 0, b""))
            continue
        dados = bytes(mascara)
        # topo do bitmap acima da linha de base (positivo) e recuo a esquerda
        glifos.append((ord(ch), h, w, avanco, -oy, ox, dados))

    sobe, desce = metricas_ascii(fonte)
    out = bytearray()

    def i32(v):
        out.extend((v & 0xFFFFFFFF).to_bytes(4, "big"))

    i32(len(glifos))       # quantidade de glifos
    i32(11)                # versao do codificador (o LovyanGFX ignora)
    i32(sobe + desce)      # "tamanho" - vira yAdvance, recalculado pelos glifos
    i32(0)
    i32(sobe)              # subida
    i32(desce)             # descida
    for (u, h, w, av, dy, dx, _) in glifos:
        for v in (u, h, w, av, dy, dx, 0):
            i32(v)
    for g in glifos:
        out.extend(g[6])
    return bytes(out), len(glifos), sobe + desce


def main():
    os.makedirs(os.path.dirname(SAIDA), exist_ok=True)
    linhas = [
        "// GERADO por generate_fonts.py - nao edite a mao.",
        "// Roboto (Apache 2.0), antialiasing de 8 bits, formato VLW (LovyanGFX).",
        "#include \"soft_fonts.hpp\"",
        "",
    ]
    tabela = {nome: [] for nome, _ in FAMILIAS}
    total = 0
    for nome, caminho in FAMILIAS:
        for alvo in ALTURAS:
            f = fonte_para_altura(caminho, alvo)
            dados, n, altura = vlw(f)
            total += len(dados)
            simbolo = f"s_{nome}_{alvo}"
            linhas.append(f"static const uint8_t {simbolo}[{len(dados)}] = {{")
            for k in range(0, len(dados), 32):
                linhas.append("  " + ",".join(str(b) for b in dados[k:k + 32]) + ",")
            linhas.append("};")
            tabela[nome].append((alvo, simbolo, len(dados), altura))
            print(f"{nome:8s} alvo {alvo:3d}px -> linha {altura:3d}px, {n} glifos, {len(dados)/1024:6.1f} KB")

    for nome, itens in tabela.items():
        linhas.append(f"const fonte_suave_t FONTES_{nome.upper()}[] = {{")
        for alvo, simbolo, tam, altura in itens:
            linhas.append(f"  {{ {altura}, {simbolo}, {tam} }},")
        linhas.append("};")
        linhas.append(f"const int FONTES_{nome.upper()}_N = {len(itens)};")
    with open(SAIDA, "w") as arq:
        arq.write("\n".join(linhas) + "\n")
    print(f"total {total/1024:.0f} KB -> {SAIDA}")


main()
