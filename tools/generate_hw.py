#!/usr/bin/env python3
# Gera main/tab5_mesh_hw_dados.cpp a partir do enum HardwareModel oficial do
# Meshtastic (meshtastic/protobufs, mesh.proto). Roda uma vez, offline depois
# disso - o firmware nunca busca nada em tempo de execucao.
#
# POR QUE ISTO EXISTE: a tabela anterior foi digitada a mao, com uns 12
# modelos que pareciam prováveis. Numa malha de verdade (a que motivou isto,
# ) apareceram "modelo 110", "modelo 88", "modelo 81" - nos reais
# que a tabela adivinhada nao cobria. A fonte oficial tem 149 modelos; gerar
# a tabela inteira de la elimina a adivinhacao.
#
#   ./generate_hw.py
#
# Ver main/tab5_mesh.cpp (mesh_hw_nome) para onde a tabela e usada.

import re
import subprocess
import os

URL = "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto"
AQUI = os.path.dirname(os.path.abspath(__file__))
AQUI = os.path.dirname(AQUI)   # a RAIZ do projeto - o script mora em tools/
SAIDA = os.path.join(AQUI, "main", "tab5_mesh_hw_dados.cpp")


def titulo(nome_enum: str) -> str:
    # HELTEC_WIRELESS_TRACKER -> "Heltec Wireless Tracker". Sem dicionario de
    # excecoes de proposito: e derivado, nao adivinhado - a mesma regra que
    # motivou reescrever este arquivo.
    partes = nome_enum.split("_")
    saida = []
    for p in partes:
        if p.isdigit() or re.match(r'^[0-9]', p):
            saida.append(p)               # "T1000", "900" ficam como estao
        else:
            saida.append(p.capitalize())
    return " ".join(saida)


def main():
    r = subprocess.run(["curl", "-sf", URL], capture_output=True, text=True, check=True)
    txt = r.stdout

    m = re.search(r'enum HardwareModel \{(.*?)\n\}', txt, re.S)
    if not m:
        raise SystemExit("nao achei o enum HardwareModel no mesh.proto - o formato mudou?")

    entradas = re.findall(r'\n\s*([A-Z0-9_]+)\s*=\s*(\d+)\s*;', m.group(1))

    tabela = {}   # codigo -> nome bonito
    for nome_enum, codigo_s in entradas:
        codigo = int(codigo_s)
        if nome_enum == "UNSET":
            continue   # 0 e "sem info" - tratado a parte, nao vira nome
        if nome_enum == "PRIVATE_HW":
            tabela[codigo] = "privado"
            continue
        tabela[codigo] = titulo(nome_enum)

    linhas = [
        "// GERADO por generate_hw.py a partir do HardwareModel oficial do",
        "// Meshtastic (meshtastic/protobufs, mesh.proto). Nao editar a mao -",
        "// rode o script de novo se a lista oficial crescer.",
        '#include "tab5_mesh.hpp"',
        "",
        "// 256 ponteiros, a maioria nulos - endereca direto pelo byte do codigo,",
        "// sem busca. O nulo E o dado: mesh_hw_nome() o devolve para quem chama",
        "// decidir como mostrar um numero que a tabela oficial ainda nao tem.",
        "const char* const TAB5_MESH_HW_NOMES[256] = {",
    ]
    for codigo in range(256):
        nome = tabela.get(codigo)
        if nome is None:
            linhas.append(f"  [{codigo}] = nullptr,")
        else:
            linhas.append(f'  [{codigo}] = "{nome}",')
    linhas.append("};")

    with open(SAIDA, "w") as f:
        f.write("\n".join(linhas) + "\n")

    print(f"gravado {SAIDA}: {len(tabela)} modelos conhecidos de {len(entradas)} no enum")


if __name__ == "__main__":
    main()
