# RELEVO REAL em volta de Interlagos -> relevo.h (grade em coordenadas do jogo).
# Fonte: tiles "Terrarium" do AWS Open Data (SRTM/NASA, domínio público), os
# mesmos que o gerar_pista_osm.py já usa para a altura da pista.
# O jogo integra o rumo de cada metro do traçado do OSM; aqui o mesmo traçado
# é reconstruído e encaixado (rotação + translação, mínimos quadrados) na
# projeção do OSM, para saber a latitude/longitude de cada ponto do jogo.
#   python gerar_relevo.py [previa.png]
import os, re, sys, math, json
import numpy as np

AQUI = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(AQUI, "..", "..", "ferramentas"))
import gerar_pista_osm as g

CEL = 40.0          # m por célula
MARGEM = 2700.0     # m além do traçado (cobre o anel do horizonte a 1,7 km)


def ler_header():
    t = open(os.path.join(AQUI, "pista_interlagos.h"), encoding="utf-8").read()
    rumo = [int(v) for v in re.search(r"INTERLAGOS_RUMO\[\d+\] = \{(.*?)\};", t, re.S).group(1).replace("\n", " ").split(",") if v.strip()]
    pares = re.search(r"INTERLAGOS_PISTA\[\d+\]\[2\] = \{(.*?)\};", t, re.S).group(1)
    alt = [float(b) for a, b in re.findall(r"\{\s*([-\d.eEf]+)f?\s*,\s*([-\d.eE]+)f?\s*\}", pares)]
    return np.array(rumo, float), np.array(alt) / (200.0 * 1.6)


def main():
    previa = sys.argv[1] if len(sys.argv) > 1 else None
    cfg = g.CIRCUITOS["interlagos"]
    dados = json.load(open(os.path.join(g.PASTA, "interlagos_raw.json"), encoding="utf-8"))
    latlon, _ = g.encadear(dados, cfg["trechos"], cfg.get("ignorar_nomes", ()))
    proj = g.Projecao(latlon)
    pts, total, _ = g.reamostrar([proj.xy(*p) for p in latlon], g.METROS_SEG)
    P = np.array(pts)                                   # leste, norte (m)
    rumo, alt_jogo = ler_header()
    n = len(rumo)
    assert n == len(P), (n, len(P))
    # traçado como o jogo monta (sem a suavização, que não desloca nada)
    th = rumo * math.pi / 1800.0
    x = np.concatenate([[0], np.cumsum(np.cos(th))])[:n]
    z = np.concatenate([[0], np.cumsum(-np.sin(th))])[:n]
    fx, fz = x[-1] + math.cos(th[-1]), z[-1] - math.sin(th[-1])
    i = np.arange(n)
    x = x - fx * i / n; z = z - fz * i / n
    J = np.stack([x, -z], 1)                            # jogo em (leste, norte)
    # encaixe rígido J -> P
    cj, cp = J.mean(0), P.mean(0)
    H = (J - cj).T @ (P - cp)
    U, S, Vt = np.linalg.svd(H)
    R = (U @ Vt).T
    if np.linalg.det(R) < 0:
        Vt[-1] *= -1; R = (U @ Vt).T
    erro = np.sqrt((((J - cj) @ R.T + cp - P) ** 2).sum(1))
    print(f"encaixe: erro médio {erro.mean():.1f} m, máx {erro.max():.1f} m")

    x0, x1 = x.min() - MARGEM, x.max() + MARGEM
    z0, z1 = z.min() - MARGEM, z.max() + MARGEM
    nx, nz = int((x1 - x0) / CEL) + 1, int((z1 - z0) / CEL) + 1
    gx = x0 + np.arange(nx) * CEL; gz = z0 + np.arange(nz) * CEL
    GX, GZ = np.meshgrid(gx, gz)
    Jg = np.stack([GX.ravel(), -GZ.ravel()], 1)
    Pg = (Jg - cj) @ R.T + cp
    ll = [proj.latlon(px, py) for px, py in Pg]
    h = np.array(g.relevo_terrarium(ll, os.path.join(g.PASTA, "interlagos_relevo_grade.json"), z=14), float).reshape(nz, nx)
    # tira ruído de 1 pixel (3x3)
    hp = np.pad(h, 1, mode="edge")
    h = sum(hp[a:a + nz, b:b + nx] for a in range(3) for b in range(3)) / 9.0
    # mesma referência de altura do jogo (altura 0 = linha de chegada suavizada)
    ii = np.clip(((x - x0) / CEL).astype(int), 0, nx - 1); jj = np.clip(((z - z0) / CEL).astype(int), 0, nz - 1)
    ofs = np.median(h[jj, ii] - alt_jogo)
    h -= ofs
    dif = h[jj, ii] - alt_jogo
    print(f"grade {nx}x{nz} ({nx*CEL/1000:.1f} x {nz*CEL/1000:.1f} km), relevo {h.min():.0f}..{h.max():.0f} m; "
          f"pista vs SRTM: {np.abs(dif).mean():.1f} m em média")
    # REPRESAS: água no SRTM é perfeitamente plana; células baixas e planas
    hp = np.pad(h, 1, mode="edge")
    viz = np.stack([hp[a:a + nz, b:b + nx] for a in range(3) for b in range(3)])
    plano = (viz.max(0) - viz.min(0)) < 0.35
    baixo = h < np.percentile(h, 25)
    cand = h[plano & baixo]
    nivel = float(np.median(cand)) if cand.size > 150 else -9999.0
    print(f"represas: {cand.size} células planas e baixas -> nível {nivel:.1f} m")
    with open(os.path.join(AQUI, "relevo.h"), "w", encoding="utf-8") as f:
        f.write("/* gerado por gerar_relevo.py — relevo real (SRTM/NASA via AWS Terrarium), em decímetros,\n"
                " * na mesma referência de altura da pista (0 = linha de chegada) */\n")
        f.write(f"#define REL_X0 {x0:.1f}f\n#define REL_Z0 {z0:.1f}f\n#define REL_CEL {CEL:.1f}f\n")
        f.write(f"#define REL_NX {nx}\n#define REL_NZ {nz}\n#define REL_AGUA {nivel:.1f}f\n")
        f.write(f"static const short RELEVO[{nz}][{nx}] = {{\n")
        for j in range(nz):
            f.write("    {" + ",".join(str(int(round(v * 10))) for v in h[j]) + "},\n")
        f.write("};\n")
    if previa:
        from PIL import Image
        v = (h - h.min()) / (h.max() - h.min())
        img = np.stack([v * 255] * 3, -1).astype(np.uint8)
        for a, b in zip(ii, jj):
            img[b, a] = (255, 40, 40)
        Image.fromarray(img).resize((nx * 3, nz * 3), Image.NEAREST).save(previa)


if __name__ == "__main__":
    main()
