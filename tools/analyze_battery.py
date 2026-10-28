# Checks the battery % against the COUNTED charge (mAh), straight from a
# card energy CSV (the old energia.csv, or Sistema/energia/energia-YYYY-MM-DD.csv).
# Run directly: python3 analyze_battery.py [file.csv]
# For each discharge segment: how much the voltage curve says it dropped
# versus how much the mAh count says - and how many times the recorded %
# ROSE while discharging.
import sys

CAP = 1306.8   # TAB5_CAPACITY_MAH
R = 0.268      # TAB5_INTERNAL_R
CURVE = [(4155,100),(4076,98),(4063,96),(4053,94),(4049,92),(4044,90),(4037,88),(4031,86),(4023,84),(4018,82),
         (4014,80),(4009,78),(4005,76),(4001,74),(3995,72),(3989,70),(3982,68),(3973,66),(3965,64),(3957,62),
         (3950,60),(3945,58),(3938,56),(3933,54),(3928,52),(3923,50),(3916,48),(3910,46),(3903,44),(3895,42),
         (3886,40),(3879,38),(3868,36),(3863,34),(3856,32),(3850,30),(3843,28),(3836,26),(3828,24),(3819,22),
         (3810,20),(3801,18),(3794,16),(3786,14),(3778,12),(3766,10),(3744,8),(3701,6),(3645,4),(3557,2),(3302,0)]

def curve(v, ma):
    mv = (v - ma / 1000 * R) * 500
    if mv >= CURVE[0][0]: return 100.0
    if mv <= CURVE[-1][0]: return 0.0
    for (m0, p0), (m1, p1) in zip(CURVE, CURVE[1:]):
        if mv >= m1: return p1 + (mv - m1) * (p0 - p1) / (m0 - m1)

path = sys.argv[1] if len(sys.argv) > 1 else "energia.csv"
samples = []   # (t, V, mA, recorded pct); None = reboot
for line in open(path):
    f = line.strip().split(",")
    if f[0] == "BOOT": samples.append(None)
    elif f[0] in ("ENE", "ENE2"): samples.append((int(f[1]), float(f[2]), float(f[3]), int(f[5])))
    elif f[0] == "ENE3" and f[1] != "0": samples.append((int(f[1]), float(f[3]), float(f[4]), int(f[6])))

segments, current = [], []
for s in samples + [None]:
    if s and s[2] < -20 and (not current or s[0] - current[-1][0] <= 130):
        current.append(s); continue
    if len(current) >= 20: segments.append(current)
    current = [s] if (s and s[2] < -20) else []

print(f"{len(segments)} discharge segment(s) in {path}\n")
print("  hours | curve: from -> to (drop) | counted | difference | recorded % rose")
for seg in segments:
    mah = sum(-(b[2] + a[2]) / 2 * (b[0] - a[0]) / 3600 for a, b in zip(seg, seg[1:]))
    c0, c1 = curve(seg[0][1], seg[0][2]), curve(seg[-1][1], seg[-1][2])
    rose = sum(1 for a, b in zip(seg, seg[1:]) if b[3] > a[3])
    print(f"  {(seg[-1][0]-seg[0][0])/3600:5.1f} | {c0:5.1f} -> {c1:5.1f} ({c0-c1:5.1f}) | {mah/CAP*100:6.1f}  | {(c0-c1)-mah/CAP*100:+6.1f}    | {rose} time(s)")
