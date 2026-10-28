# Confere a % da bateria contra a carga CONTADA (mAh), direto de um CSV de
# energia do cartao (energia.csv antigo ou Sistema/energia/energia-AAAA-MM-DD.csv).
# Roda direto: python3 analyze_battery.py [arquivo.csv]
# Para cada trecho de descarga: quanto a curva de tensao diz que caiu contra
# quanto a contagem de mAh diz - e quantas vezes a % gravada SUBIU descarregando.
import sys

CAP = 1306.8   # TAB5_CAPACIDADE_MAH
R = 0.268      # TAB5_R_INTERNA
CURVA = [(4155,100),(4076,98),(4063,96),(4053,94),(4049,92),(4044,90),(4037,88),(4031,86),(4023,84),(4018,82),
         (4014,80),(4009,78),(4005,76),(4001,74),(3995,72),(3989,70),(3982,68),(3973,66),(3965,64),(3957,62),
         (3950,60),(3945,58),(3938,56),(3933,54),(3928,52),(3923,50),(3916,48),(3910,46),(3903,44),(3895,42),
         (3886,40),(3879,38),(3868,36),(3863,34),(3856,32),(3850,30),(3843,28),(3836,26),(3828,24),(3819,22),
         (3810,20),(3801,18),(3794,16),(3786,14),(3778,12),(3766,10),(3744,8),(3701,6),(3645,4),(3557,2),(3302,0)]

def curva(v, ma):
    mv = (v - ma / 1000 * R) * 500
    if mv >= CURVA[0][0]: return 100.0
    if mv <= CURVA[-1][0]: return 0.0
    for (m0, p0), (m1, p1) in zip(CURVA, CURVA[1:]):
        if mv >= m1: return p1 + (mv - m1) * (p0 - p1) / (m0 - m1)

arq = sys.argv[1] if len(sys.argv) > 1 else "energia.csv"
amostras = []   # (t, V, mA, pct gravado); None = reinicio
for linha in open(arq):
    f = linha.strip().split(",")
    if f[0] == "BOOT": amostras.append(None)
    elif f[0] in ("ENE", "ENE2"): amostras.append((int(f[1]), float(f[2]), float(f[3]), int(f[5])))
    elif f[0] == "ENE3" and f[1] != "0": amostras.append((int(f[1]), float(f[3]), float(f[4]), int(f[6])))

trechos, atual = [], []
for a in amostras + [None]:
    if a and a[2] < -20 and (not atual or a[0] - atual[-1][0] <= 130):
        atual.append(a); continue
    if len(atual) >= 20: trechos.append(atual)
    atual = [a] if (a and a[2] < -20) else []

print(f"{len(trechos)} trechos de descarga em {arq}\n")
print("  horas | curva: de -> ate (queda) | contado | diferenca | a % gravada subiu")
for t in trechos:
    mah = sum(-(b[2] + a[2]) / 2 * (b[0] - a[0]) / 3600 for a, b in zip(t, t[1:]))
    c0, c1 = curva(t[0][1], t[0][2]), curva(t[-1][1], t[-1][2])
    subiu = sum(1 for a, b in zip(t, t[1:]) if b[3] > a[3])
    print(f"  {(t[-1][0]-t[0][0])/3600:5.1f} | {c0:5.1f} -> {c1:5.1f} ({c0-c1:5.1f}) | {mah/CAP*100:6.1f}  | {(c0-c1)-mah/CAP*100:+6.1f}    | {subiu} vez(es)")
