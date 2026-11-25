# Recalcula a ALTURA da pista (2ª coluna de INTERLAGOS_PISTA em pista_interlagos.h)
# a partir do relevo SRTM já baixado (cache do gerar_pista_osm.py), com filtros
# menores: o gerador original usava mediana de 7 amostras (168 m) + suavização
# de ~90 m, que apagava as quebras curtas (Mergulho, queda antes do S do Senna).
#   python ajustar_altura.py [--previa arq.png] [--aplicar]
import os, sys, re, json, math
import numpy as np

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(AQUI, "..", "..", "ferramentas"))
import gerar_pista_osm as g

MEDIANA = 3        # amostras de 24 m (só tira picos isolados)
RAIO = 40          # m, suavização gaussiana (sigma = raio/2)


def main():
    h = open(os.path.join(AQUI, "pista_interlagos.h"), encoding="utf-8").read()
    m = re.search(r"(INTERLAGOS_PISTA\[(\d+)\]\[2\] = \{)(.*?)(\};)", h, re.S)
    n = int(m.group(2))
    pares = re.findall(r"\{\s*([-\d.]+)f\s*,\s*([-\d.]+)f\s*\}", m.group(3))
    assert len(pares) == n
    antigo = np.array([float(b) for a, b in pares]) / 320.0
    alt = json.load(open(os.path.join(g.PASTA, "interlagos_relevo.json"), encoding="utf-8"))
    M = len(alt)
    k = MEDIANA // 2
    alt = [sorted(alt[(i + j) % M] for j in range(-k, k + 1))[k] for i in range(M)]
    passo = n / M                                  # ~24 m por amostra
    seg = []
    for i in range(n):
        a = i / passo; j = int(a) % M; t = a - int(a)
        seg.append(alt[j] * (1 - t) + alt[(j + 1) % M] * t)
    seg = g.suavizar_circular(seg, RAIO)
    novo = np.array(seg) - seg[0]
    r_a = (np.roll(antigo, -25) - np.roll(antigo, 25)) / 50 * 100
    r_n = (np.roll(novo, -25) - np.roll(novo, 25)) / 50 * 100
    print(f"antigo: {antigo.min():.1f}..{antigo.max():.1f} m, rampa máx {np.abs(r_a).max():.1f}%")
    print(f"novo:   {novo.min():.1f}..{novo.max():.1f} m, rampa máx {np.abs(r_n).max():.1f}%")
    for s, nm in ((300, "antes do Senna"), (437, "S do Senna"), (744, "Sol"), (1512, "Lago"), (2122, "Ferradura"),
                  (2233, "Laranjinha"), (2550, "Pinheirinho"), (2716, "Bico de Pato"), (3016, "Mergulho"),
                  (3150, "meio do Mergulho"), (3277, "Juncao"), (3648, "Subida"), (4034, "Arquibancadas")):
        print(f"  {nm:18s} s={s:5d}  antigo {antigo[s]:6.1f} m ({r_a[s]:+5.1f}%)   novo {novo[s]:6.1f} m ({r_n[s]:+5.1f}%)")
    if "--previa" in sys.argv:
        from PIL import Image, ImageDraw
        W, H = 1400, 420
        im = Image.new("RGB", (W, H), (20, 22, 28)); d = ImageDraw.Draw(im)
        lo, hi = min(antigo.min(), novo.min()), max(antigo.max(), novo.max())
        Y = lambda v: H - 40 - (v - lo) / (hi - lo) * (H - 80)
        for hh in range(int(lo) // 5 * 5, int(hi) + 5, 5):
            d.line((0, Y(hh), W, Y(hh)), fill=(50, 50, 60)); d.text((2, Y(hh) - 12), f"{hh} m", fill=(150, 150, 150))
        for arr, cor in ((antigo, (120, 120, 120)), (novo, (120, 230, 120))):
            for i in range(0, n - 2, 2):
                d.line((i * W / n, Y(arr[i]), (i + 2) * W / n, Y(arr[i + 2])), fill=cor, width=2)
        for s, nm in ((437, "SENNA"), (744, "SOL"), (1512, "LAGO"), (2122, "FERRADURA"), (2233, "LARANJ."), (2550, "PINHEIR."),
                      (2716, "BICO"), (3016, "MERGULHO"), (3277, "JUNCAO"), (3730, "SUBIDA"), (4034, "ARQ.")):
            x = s * W / n; d.line((x, 20, x, H - 30), fill=(80, 80, 130)); d.text((x + 2, H - 20), nm, fill=(255, 220, 120))
        d.text((10, 4), "cinza: atual   verde: novo", fill=(220, 220, 220))
        im.save(sys.argv[sys.argv.index("--previa") + 1])
    if "--aplicar" in sys.argv:
        corpo = ",\n".join(f"    {{{a}f, {v * 320:.1f}f}}" for (a, b), v in zip(pares, novo))
        h2 = h[:m.start(3)] + "\n" + corpo + "\n" + h[m.end(3):]
        open(os.path.join(AQUI, "pista_interlagos.h"), "w", encoding="utf-8").write(h2)
        print("pista_interlagos.h atualizado")


if __name__ == "__main__":
    main()
