"""Converte o carro .obj (modelo_obj/Car.obj, 11 pinturas TeamXX.png) para o jogo:
- carro_obj.h: triângulos (posição, UV, normal) já na escala/eixos do jogo,
  parte de cada triângulo (0 carroceria, 1..4 rodas no padrão do jogo, 5 capacete),
  centro das rodas e as 2 cores principais de cada pintura (HUD/reflexos/cockpit)
- texturas (VQ com mipmap) entram no texturas.bin pelo gerar_texturas.py (obj_texturas())

Eixos do jogo: x = direita, y = cima (chão = 0), z = frente; origem entre os eixos.
O modelo vem maior e mais largo (carro dos anos 2000): escala 0,85 e largura x0,80
para a bitola estreita de 1998 e o entre-eixos de 3,1 m do jogo."""
import os
import numpy as np
from PIL import Image

AQUI = os.path.dirname(os.path.abspath(__file__))
PASTA = os.path.join(AQUI, "modelo_obj")
S, SX = 0.85, 0.80


def ler():
    V, T, N, F = [], [], [], []
    for ln in open(os.path.join(PASTA, "Car.obj"), encoding="utf-8", errors="ignore"):
        p = ln.split()
        if not p: continue
        if p[0] == "v": V.append([float(x) for x in p[1:4]])
        elif p[0] == "vt": T.append([float(x) for x in p[1:3]])
        elif p[0] == "vn": N.append([float(x) for x in p[1:4]])
        elif p[0] == "f":
            f = []
            for c in p[1:]:
                k = c.split("/")
                f.append((int(k[0]) - 1, int(k[1]) - 1, int(k[2]) - 1))
            for i in range(1, len(f) - 1):
                F.append((f[0], f[i], f[i + 1]))
    return np.array(V), np.array(T), np.array(N), F


def componentes(V, F):
    par = list(range(len(V)))
    def r(a):
        while par[a] != a:
            par[a] = par[par[a]]; a = par[a]
        return a
    for f in F:
        for c in f[1:]:
            par[r(c[0])] = r(f[0][0])
    return [r(f[0][0]) for f in F]


def converter():
    V, T, N, F = ler()
    comp = componentes(V, F)
    grupos = {}
    for k, cc in enumerate(comp): grupos.setdefault(cc, []).append(k)
    zc = None
    parte = [0] * len(F)
    rodas = []
    for cc, fs in grupos.items():
        vs = sorted(set(c[0] for k in fs for c in F[k]))
        P = V[vs]
        mn, mx = P.min(0), P.max(0)
        if len(fs) == 60:                                   # roda (cilindro)
            rodas.append(((mn + mx) / 2, fs))
        elif len(fs) == 28 and mn[1] > 0.3:                 # capacete do piloto
            for k in fs: parte[k] = 5
    frente = [w for w in rodas if w[0][2] > 0]
    tras = [w for w in rodas if w[0][2] < 0]
    zc = (np.mean([w[0][2] for w in frente]) + np.mean([w[0][2] for w in tras])) / 2
    ymin = V[:, 1].min()
    def tr(p):
        return np.array([p[0] * S * SX, (p[1] - ymin) * S, (p[2] - zc) * S])
    centros = {}
    for c, fs in rodas:                                     # padrão do jogo: 1 = esq. frente, 2 = dir. frente, 3 = esq. trás, 4 = dir. trás
        idx = (1 if c[0] < 0 else 2) if c[2] > 0 else (3 if c[0] < 0 else 4)
        for k in fs: parte[k] = idx
        centros[idx] = tr(c)
    tri = []
    for k, f in enumerate(F):
        cs = []
        for (vi, ti, ni) in f:
            p = tr(V[vi])
            n = N[ni] / np.array([S * SX, S, S]); n /= np.linalg.norm(n) + 1e-9
            u, v = T[ti]
            cs.append((p, (u, 1.0 - v), n, vi, ni))
        # ordem dos cantos coerente com a normal (o teste de "costas" no jogo usa a ordem)
        fn = np.cross(cs[1][0] - cs[0][0], cs[2][0] - cs[0][0])
        if fn @ (cs[0][2] + cs[1][2] + cs[2][2]) < 0:
            cs = [cs[0], cs[2], cs[1]]
        tri.append((cs, parte[k]))
    tri.sort(key=lambda t: t[1])                            # carroceria, rodas 1..4, capacete
    return tri, centros


def cores_pinturas(tri):
    """as 2 cores mais presentes na carroceria de cada pintura (k-médias simples)"""
    uvs = np.array([c[1] for t, p in tri if p == 0 for c in t])
    res = []
    for i in range(11):
        img = Image.open(os.path.join(PASTA, f"Team{i + 1:02d}.png")).convert("RGBA")
        a = np.asarray(img, np.float32)
        h, w = a.shape[:2]
        pts = []
        rs = np.random.RandomState(1)
        for _ in range(3000):                               # amostras dentro dos triângulos da carroceria
            t, p = tri[rs.randint(len(tri))]
            if p != 0: continue
            b = rs.dirichlet((1, 1, 1))
            u = sum(b[j] * t[j][1][0] for j in range(3)); v = sum(b[j] * t[j][1][1] for j in range(3))
            px = a[int(np.clip(v * h, 0, h - 1)), int(np.clip(u * w, 0, w - 1))]
            if px[3] > 128: pts.append(px[:3])
        X = np.array(pts)
        C = X[rs.choice(len(X), 4, replace=False)].copy()
        for _ in range(15):
            lab = ((X[:, None, :] - C[None]) ** 2).sum(2).argmin(1)
            for k in range(4):
                if (lab == k).any(): C[k] = X[lab == k].mean(0)
        cont = np.bincount(lab, minlength=4)
        ordem = np.argsort(-cont)
        c1 = C[ordem[0]]
        c2 = next((C[o] for o in ordem[1:] if np.abs(C[o] - c1).sum() > 120), C[ordem[1]])
        res.append(tuple(int(x) for x in c1) + tuple(int(x) for x in c2))
    return res


def escrever_h(tri, centros, cores):
    # vértices ÚNICOS (posição + normal) por parte: cada um é transformado uma vez por carro
    vid, verts, vparte, idx = {}, [], [], []
    for t, p in tri:
        ii = []
        for c in t:
            chave = (p, c[3], c[4])
            if chave not in vid:
                vid[chave] = len(verts); verts.append((c[0], c[2])); vparte.append(p)
            ii.append(vid[chave])
        idx.append(ii)
    ordem = sorted(range(len(verts)), key=lambda i: vparte[i])
    novo = {o: k for k, o in enumerate(ordem)}
    verts = [verts[o] for o in ordem]; vparte = [vparte[o] for o in ordem]
    idx = [[novo[i] for i in ii] for ii in idx]
    NL = chr(10)
    L = []
    L.append("/* gerado por conv_obj.py a partir de modelo_obj/Car.obj (eixos e escala do jogo) */")
    L.append(f"#define OBJ_NTRI {len(tri)}")
    L.append(f"#define OBJ_NV {len(verts)}")
    L.append("#define OBJ_NPINT 11")
    tini = [sum(1 for t in tri if t[1] < p) for p in range(7)]
    vini = [sum(1 for q in vparte if q < p) for p in range(7)]
    L.append("static const int OBJ_PARTE_INI[7] = { " + ", ".join(map(str, tini)) + " };   /* triângulos: 0 carroceria, 1..4 rodas, 5 capacete */")
    L.append("static const int OBJ_VPARTE_INI[7] = { " + ", ".join(map(str, vini)) + " };  /* vértices por parte */")
    L.append(f"static const float OBJ_V[{len(verts)}][6] = {{   /* x, y, z, nx, ny, nz */")
    for pp, nn in verts:
        L.append("  {" + ", ".join(f"{x:.4f}f" for x in list(pp) + list(nn)) + "},")
    L.append("};")
    L.append(f"static const uint16_t OBJ_T[{len(tri)}][3] = {{")
    L.append(("," + NL).join("  {" + ", ".join(map(str, ii)) + "}" for ii in idx))
    L.append("};")
    L.append(f"static const float OBJ_UV[{len(tri)}][3][2] = {{")
    L.append(("," + NL).join("  {" + ", ".join(f"{{{c[1][0]:.4f}f, {c[1][1]:.4f}f}}" for c in t) + "}" for t, p in tri))
    L.append("};")
    L.append("static const float OBJ_RODA[5][3] = { {0,0,0}, " + ", ".join(
        "{" + ", ".join(f"{x:.4f}f" for x in centros[i]) + "}" for i in range(1, 5)) + " };")
    L.append("static const uint32_t OBJ_COR[11][2] = {")
    for c in cores:
        L.append(f"  {{ 0xFF{c[0]:02X}{c[1]:02X}{c[2]:02X}, 0xFF{c[3]:02X}{c[4]:02X}{c[5]:02X} }},")
    L.append("};")
    open(os.path.join(AQUI, "carro_obj.h"), "w", encoding="utf-8").write(NL.join(L) + NL)
    print("carro_obj.h:", len(tri), "triângulos,", len(verts), "vértices únicos", tini, vini)


def obj_texturas(textura_vq_mip):
    """as 11 pinturas em VQ com mipmap (o transparente é preenchido pela cor vizinha)"""
    from scipy import ndimage as nd
    out = []
    for i in range(11):
        a = np.asarray(Image.open(os.path.join(PASTA, f"Team{i + 1:02d}.png")).convert("RGBA"), np.float32)
        rgb, al = a[..., :3], a[..., 3] > 128
        if (~al).any():
            _, (iy, ix) = nd.distance_transform_edt(~al, return_indices=True)
            rgb = rgb[iy, ix]
        out.append(textura_vq_mip(rgb))
    return out


if __name__ == "__main__":
    tri, centros = converter()
    escrever_h(tri, centros, cores_pinturas(tri))
