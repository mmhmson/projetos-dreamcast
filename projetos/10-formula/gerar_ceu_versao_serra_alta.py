# Gera o céu pintado (skybox) do 10-formula: ceu.bin, embutido no jogo.
# Estilo dos céus de corrida do Dreamcast: degradê saturado, CÚMULOS feitos
# de "bolhas" (cada uma com luz própria: lado do sol claro, sombra azulada,
# base reta), contorno brilhante nas nuvens contra o sol, halo do sol pintado
# no céu, cirros, e um horizonte em camadas (serras, cidade, morros, árvores).
# Tudo gerado por código, nada copiado.
#
# Variantes (RGB565 little-endian, linear; o jogo "twiddla" ao carregar):
#   0 limpo      3072x256 (3 texturas de 1024), volta inteira, sol pintado
#   1 fechado    2048x256 (2 texturas), repetido 2x na volta (sem sol)
#   2 entardecer 3072x256 (3 texturas), volta inteira, sol baixo alaranjado
# A direção do sol TEM de bater com o jogo (LUZ e desenhar_sol em main.c).
#
# ARTE À MÃO: se existir arte_ceu_limpo.png / arte_ceu_fechado.png /
# arte_ceu_tarde.png nesta pasta, ela é usada no lugar do céu gerado (é
# redimensionada para o tamanho da variante; o horizonte fica na linha 232
# de 256 e a imagem precisa "emendar" nas bordas esquerda/direita).
#   python gerar_ceu.py [pasta_previa]
import sys, os, math
import numpy as np

H, HOR = 256, 232
W = 2048                                   # largura da variante sendo gerada
AQUI = os.path.dirname(os.path.abspath(__file__))
TAM = {0: 4096, 1: 4096, 2: 3072}      # todas com a volta inteira (geografia real)
ARTE = {0: "arte_ceu_limpo.png", 1: "arte_ceu_fechado.png", 2: "arte_ceu_tarde.png"}
# sol: ângulo no mundo = atan2(LUZ z, LUZ x); altura = atan(tan) (igual a desenhar_sol)
SOL = {2: (math.atan2(-0.174, -0.985), 0.196 / 0.98)}    # entardecer: sol a OESTE (só ele tem o sol pintado)


def ss(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def hexc(c):
    return np.array([(c >> 16) & 255, (c >> 8) & 255, c & 255], np.float32)


def mix(a, b, t):
    t = np.asarray(t, np.float32)
    if t.ndim == 2:
        t = t[..., None]
    return a + (b - a) * t


class Ruido:
    """fbm de valor, periódico em x (largura W da variante)."""
    def __init__(self, semente, celulas, amps, ymax):
        self.oct = []
        for k, (c, a) in enumerate(zip(celulas, amps)):
            rs = np.random.RandomState(semente * 131 + k)
            nx, ny = W // c, int(ymax / c) + 3
            self.oct.append((c, a, rs.rand(ny, nx).astype(np.float32)))

    def __call__(self, X, Y):
        soma = 0
        for c, a, L in self.oct:
            gx, gy = X / c, Y / c
            ix, iy = np.floor(gx).astype(np.int64), np.floor(gy).astype(np.int64)
            fx, fy = gx - ix, gy - iy
            fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
            nx, ny = L.shape[1], L.shape[0]
            x0, x1 = ix % nx, (ix + 1) % nx
            iy = np.clip(iy, 0, ny - 2)
            a_, b_, c_, d_ = L[iy, x0], L[iy, x1], L[iy + 1, x0], L[iy + 1, x1]
            soma = soma + a * ((a_ + (b_ - a_) * fx) * (1 - fy) + (c_ + (d_ - c_) * fx) * fy)
        return soma


def linha(semente, celulas, amps):
    r = Ruido(semente, celulas, amps, 8)
    x = np.arange(W, dtype=np.float32)
    return r(x, np.zeros_like(x))


def elev_linha(row):
    """elevação (rad) de uma linha da textura (cilindro: y/R = 0.55*(1-row/HOR))"""
    return np.arctan(0.55 * (1.0 - row / HOR))


def borrar(A, r, wrap=True):
    """3 passadas de média móvel (≈ gaussiana) em x (periódico) e y"""
    for _ in range(3):
        k = 2 * r + 1
        P = np.concatenate([A[:, -r:], A, A[:, :r]], axis=1) if wrap else np.pad(A, ((0, 0), (r, r)), mode='edge')
        c = np.cumsum(np.pad(P, ((0, 0), (1, 0))), axis=1)
        A = (c[:, k:] - c[:, :-k]) / k
        P = np.pad(A, ((r, r), (0, 0)), mode='edge')
        c = np.cumsum(np.pad(P, ((1, 0), (0, 0))), axis=0)
        A = (c[k:, :] - c[:-k, :]) / k
    return A


def cumulos(col, C, xsol, el_sol, n_nuvens, semente):
    """Cúmulos: bolhas viram um MAPA DE ALTURA (hemisférios), suavizado para
    as bolhas se fundirem; a luz sai das normais desse volume."""
    Hh, Ww = H, W
    Z = np.zeros((Hh, Ww), np.float32)
    BASE = np.zeros_like(Z); ALT = np.ones_like(Z)
    rs = np.random.RandomState(semente)
    nuvens = []
    for k in range(n_nuvens):
        t = rs.rand() ** 1.5
        yb = 214 - t * 135                                # linha da base (perto do horizonte = longe)
        esc = 0.12 + 0.88 * ((HOR - yb) / 170.0) ** 1.2
        w = (60 + 240 * esc) * (0.6 + 0.7 * rs.rand())
        h = w * (0.32 + 0.16 * rs.rand()) * (0.75 + 0.25 * esc)
        h = min(h, yb - 8)
        nuvens.append((rs.rand() * Ww, yb, w, h))
    for (xc, yb, w, h) in nuvens:
        bolhas = []
        nb = max(3, int(w / (h * 0.6)))
        for i in range(nb):                                # fileira da base
            fx = (i + 0.5) / nb - 0.5
            r = h * (0.30 + 0.10 * rs.rand()) * (1.0 - 0.45 * abs(fx) * 2) + 2
            bolhas.append((xc + fx * (w - 2 * r) + rs.randn() * r * 0.15, yb - r * 0.55, r))
        for i in range(int(nb * 1.4)):                     # corpo e topo arredondado
            fx = rs.rand() - 0.5
            perfil = max(0.0, 1.0 - (fx * 2) ** 2)
            r = h * (0.22 + 0.20 * rs.rand()) * (0.55 + 0.45 * perfil) + 1.5
            cy = yb - h * perfil * (0.45 + 0.25 * rs.rand()) - r * 0.25
            if cy - r < yb - h - 2:
                cy = yb - h - 2 + r
            bolhas.append((xc + fx * w * 0.8, cy, r))
        for (cx, cy, r) in bolhas:
            for desloc in (0, Ww, -Ww):
                c = cx + desloc
                x0, x1 = int(max(0, c - r - 2)), int(min(Ww, c + r + 3))
                y0, y1 = int(max(0, cy - r - 2)), int(min(Hh, min(cy + r + 3, yb + 3)))
                if x0 >= x1 or y0 >= y1:
                    continue
                X = np.arange(x0, x1, dtype=np.float32)[None, :]
                Y = np.arange(y0, y1, dtype=np.float32)[:, None]
                d2 = (X - c) ** 2 + (Y - cy) ** 2
                hz = np.sqrt(np.clip(r * r - d2, 0, None))
                sub = Z[y0:y1, x0:x1]
                m = hz > sub
                sub[m] = hz[m]
                BASE[y0:y1, x0:x1][m] = yb
                ALT[y0:y1, x0:x1][m] = h
    Y = np.arange(Hh, dtype=np.float32)[:, None] * np.ones((1, Ww), np.float32)
    X = np.ones((Hh, 1), np.float32) * np.arange(Ww, dtype=np.float32)[None, :]
    corte = np.clip((BASE + 1.0 - Y) / 2.0, 0, 1)            # base reta
    alfa = np.clip(Z / 1.6, 0, 1) * corte
    Zs = borrar(Z, 2)                                         # funde as bolhas
    gx = (np.roll(Zs, -1, axis=1) - np.roll(Zs, 1, axis=1)) * 0.5
    gy = np.zeros_like(Zs); gy[1:-1] = (Zs[2:] - Zs[:-2]) * 0.5
    inv = 1.0 / np.sqrt(gx * gx + gy * gy + 1.0)
    NX, NY, NZ = -gx * inv, -gy * inv, inv
    th = ((xsol - X + Ww / 2) % Ww - Ww / 2) / Ww * 2 * np.pi   # >0: sol à direita
    ce, se = math.cos(el_sol), math.sin(el_sol)
    Lx, Ly, Lz = ce * np.sin(th), -se, ce * (-np.cos(th))
    lamb = NX * Lx + NY * Ly + NZ * Lz
    luz = np.clip(0.42 + 0.75 * lamb, 0, 1)
    luz *= 0.80 + 0.20 * np.clip((BASE - Y) / (0.35 * ALT + 1), 0, 1)   # base mais escura
    cn = mix(C['ns'], C['nl'], luz)
    cn = mix(cn, C['nq'], np.clip((luz - 0.72) * 2.5, 0, 1))
    contra = np.clip(-np.cos(th), 0, 1) ** 2 * np.exp(-(th / 0.9) ** 2)   # nuvem na frente do sol
    aro = (1 - NZ) ** 1.5 * contra
    cn = mix(cn, C['nq'] * 1.04, np.clip(aro * 1.8, 0, 1))
    e = np.clip(1.0 - Y / HOR, 0, 1)
    cn = mix(cn, col, ((1 - e) ** 4) * 0.55)                  # longe: some na névoa
    col[:] = mix(col, cn, alfa * 0.98)



# ---------------- SERRAS REAIS (SRTM via tiles AWS Terrarium) ----------------
# Para cada direção a partir do centro do autódromo, o ângulo de elevação
# máximo do relevo em duas faixas: 2-10 km (morros) e 10-45 km (serras:
# Cantareira, Jaraguá, Serra do Mar...). Com a curvatura da Terra.
def perfil_serras(Wout):
    import sys, json, urllib.request
    cache = os.path.join(AQUI, "serras_perfil.npz")
    if os.path.exists(cache):
        d = np.load(cache)
        A, B = d["A"], d["B"]
    else:
        sys.path.insert(0, os.path.join(AQUI, "..", "..", "ferramentas"))
        import gerar_pista_osm as gp
        from PIL import Image
        cfg = gp.CIRCUITOS["interlagos"]
        dados = json.load(open(os.path.join(gp.PASTA, "interlagos_raw.json"), encoding="utf-8"))
        latlon, _ = gp.encadear(dados, cfg["trechos"], cfg.get("ignorar_nomes", ()))
        lat0 = sum(p[0] for p in latlon) / len(latlon); lon0 = sum(p[1] for p in latlon) / len(latlon)
        z = 11; n = 2 ** z
        def gxy(lat, lon):
            x = (lon + 180) / 360 * n * 256
            y = (1 - np.log(np.tan(np.radians(lat)) + 1 / np.cos(np.radians(lat))) / np.pi) / 2 * n * 256
            return x, y
        NA, ND = 2048, 420
        ang = np.arange(NA) / NA * 2 * np.pi
        dist = np.linspace(800, 45000, ND)
        k = np.cos(np.radians(lat0))
        # ângulo do jogo: 0 leste, 90 SUL
        lat = lat0 - (np.sin(ang)[:, None] * dist[None, :]) / 110540.0
        lon = lon0 + (np.cos(ang)[:, None] * dist[None, :]) / (111320.0 * k)
        px, py = gxy(lat, lon)
        tx0, tx1 = int(px.min() // 256), int(px.max() // 256)
        ty0, ty1 = int(py.min() // 256), int(py.max() // 256)
        pasta = os.path.join(gp.PASTA, "tiles")
        os.makedirs(pasta, exist_ok=True)
        M = np.zeros(((ty1 - ty0 + 1) * 256, (tx1 - tx0 + 1) * 256), np.float32)
        for ty in range(ty0, ty1 + 1):
            for tx in range(tx0, tx1 + 1):
                arq = os.path.join(pasta, f"{z}_{tx}_{ty}.png")
                if not os.path.exists(arq):
                    url = f"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{tx}/{ty}.png"
                    open(arq, "wb").write(urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "dc-estudo/1.0"}), timeout=60).read())
                im = np.asarray(Image.open(arq).convert("RGB"), np.float32)
                M[(ty - ty0) * 256:(ty - ty0 + 1) * 256, (tx - tx0) * 256:(tx - tx0 + 1) * 256] = im[..., 0] * 256 + im[..., 1] + im[..., 2] / 256 - 32768
        print(f"  serras: {(tx1-tx0+1)*(ty1-ty0+1)} tiles z{z}")
        def amostra(px, py):
            x = px - tx0 * 256; y = py - ty0 * 256
            x0 = np.clip(np.floor(x).astype(int), 0, M.shape[1] - 2); y0 = np.clip(np.floor(y).astype(int), 0, M.shape[0] - 2)
            fx = x - x0; fy = y - y0
            return (M[y0, x0] * (1 - fx) + M[y0, x0 + 1] * fx) * (1 - fy) + (M[y0 + 1, x0] * (1 - fx) + M[y0 + 1, x0 + 1] * fx) * fy
        h = amostra(px, py)
        cx, cy = gxy(np.array(lat0), np.array(lon0))
        hcam = float(amostra(cx, cy)) + 4.0
        queda = dist ** 2 / (2 * 6371000.0) * 0.87                 # curvatura (com refração)
        e = np.arctan((h - hcam - queda[None, :]) / dist[None, :])
        A = e[:, dist >= 10000].max(1); B = e[:, (dist >= 2000) & (dist < 10000)].max(1)
        np.savez(cache, A=A, B=B)
        print(f"  serras: longe {np.degrees(A.min()):.2f}..{np.degrees(A.max()):.2f} graus, perto {np.degrees(B.min()):.2f}..{np.degrees(B.max()):.2f}")
    # tira agulhas (antenas, prédios altos e falhas do radar): mediana circular
    # de 15 amostras e depois uma suavização leve (serras paulistas são arredondadas)
    def filtra(P):
        n = len(P)
        idx = (np.arange(n)[:, None] + np.arange(-7, 8)[None, :]) % n
        P = np.median(P[idx], 1)
        for _ in range(3):
            P = (np.roll(P, 1) + 2 * P + np.roll(P, -1)) / 4
        return P
    A, B = filtra(A), filtra(B)
    xs = np.arange(Wout) / Wout * len(A)
    i0 = np.floor(xs).astype(int) % len(A); i1 = (i0 + 1) % len(A); f = xs - np.floor(xs)
    return A[i0] * (1 - f) + A[i1] * f, B[i0] * (1 - f) + B[i1] * f


def gerar(v):
    global W
    W = TAM[v]
    PAL = {
        0: dict(zen=0x1C4FB8, mei=0x4C8EE0, hor=0xFFFFFF, hsol=0xFFF6E2, nl=0xFFFFFF, ns=0x8E9EC0, nq=0xFFF4DC,
                s1=0x9DBBE4, s2=0x6F98C4, cid=0xC9BBA2, s4=0x5F9672, s5=0x356848, th=0.53),
        1: dict(zen=0x58626E, mei=0x8A949E, hor=0xFFFFFF, hsol=0xFFFFFF, nl=0xE4E8EC, ns=0x6A7480, nq=0xECECEC,
                s1=0xAEB8C4, s2=0x8C9CAC, cid=0xB4ACA0, s4=0x6E8A78, s5=0x4C6652, th=0.40),
        2: dict(zen=0x22306C, mei=0x7A64A6, hor=0xF2B4A4, hsol=0xFFC064, nl=0xFFB47C, ns=0x5C4C7E, nq=0xFFE4A4,
                s1=0xB08AAC, s2=0x7C5E8C, cid=0x5E4C6E, s4=0x4C3C5E, s5=0x30263E, th=0.53),
    }[v]
    C = {k: hexc(c) for k, c in PAL.items() if k != 'th'}
    th = PAL['th']
    y = np.arange(H, dtype=np.float32)[:, None]
    x = np.arange(W, dtype=np.float32)[None, :]
    X = np.broadcast_to(x, (H, W)).astype(np.float32)
    e = np.clip(1.0 - y / HOR, 0, 1) * np.ones((1, W), np.float32)
    FY = (y / (0.30 + 0.70 * np.clip(1.0 - y / HOR, 0, 1))) * np.ones((1, W), np.float32)
    Y_ = y * np.ones((1, W), np.float32)

    tem_sol = v in SOL
    xsol = el_sol = 0
    g1 = np.zeros_like(X)
    if tem_sol:
        ang, tan_el = SOL[v]
        xsol = (ang % (2 * np.pi)) / (2 * np.pi) * W
        el_sol = math.atan(tan_el)
        dth = ((xsol - X + W / 2) % W - W / 2) / W * 2 * np.pi             # ângulo até o sol (rad)
        perto_sol = np.exp(-(dth / (1.1 if v == 2 else 0.9)) ** 2)
    else:
        perto_sol = np.zeros_like(X)

    # degradê: horizonte -> meio -> zênite; no entardecer o lado do sol é laranja
    HORC = mix(np.broadcast_to(C['hor'], (H, W, 3)), C['hsol'], perto_sol * (0.9 if v == 2 else 0.35))
    MEI = C['mei'] if v != 2 else mix(np.broadcast_to(C['mei'], (H, W, 3)), hexc(0xE0866E), perto_sol * 0.8)
    t1 = ss(0, 1, e / (0.38 if v == 2 else 0.30))[..., None]
    t2 = ss(0, 1, (e - 0.30) / 0.70)[..., None]
    col = np.where((e < 0.30)[..., None], HORC + (MEI - HORC) * t1, MEI + (C['zen'] - MEI) * t2)

    # halo do sol pintado no céu (atrás das nuvens)
    if tem_sol:
        del_el = elev_linha(y * np.ones((1, W))) - el_sol
        dist = np.sqrt(dth ** 2 + del_el ** 2)
        g1 = np.exp(-(dist / 0.07) ** 2)
        g2 = np.exp(-(dist / (0.32 if v == 2 else 0.24)) ** 2)
        col = mix(col, C['hsol'], np.clip(g1 * 0.9 + g2 * (0.55 if v == 2 else 0.35), 0, 1))

    # cirros (ruído esticado só na vertical: contínuo na volta)
    rn = Ruido(100 + v, [256, 128, 64, 32, 16], [0.47, 0.25, 0.145, 0.095, 0.04], 3700)
    nc = rn(X + 777, FY * 3.5 + 900)
    ci = ss(0.56, 0.72, nc) * ss(0.30, 0.55, e) * (0.25 if v == 1 else 0.45) * (0.4 if v in (0, 1) and os.environ.get("FOTOS", "0") == "1" else 1.0)
    col = mix(col, C['nl'], ci)

    # NUVENS DAS FOTOS do usuário (limpo e fechado): só a forma vem da foto;
    # cor e sombreamento em faixas são os do jogo. FOTOS=0 volta ao procedural.
    usar_fotos = v in (0, 1) and os.environ.get("FOTOS", "0") == "1"
    if usar_fotos:
        import fotos
        fx = fotos.faixa_nuvens(v == 1, W, HOR)
        usar_fotos = fx is not None
    bruta = usar_fotos and os.environ.get("CEU_FOTO_BRUTA") == "1"
    if bruta:                                 # FOTOS COMO ESTÃO, só misturadas no horizonte
        rgb = np.zeros((H, W, 3)); rgb[:HOR] = fotos.faixa_rgb(v == 1, W, HOR)
        col = mix(col, rgb, ss(0.03, 0.20, e))
    elif usar_fotos:
        alfa, lz = fotos.estilizar_nuvens(*fx, niveis=4, corte=0.18 if v == 1 else 0.40)
        A = np.zeros((H, W)); Lz = np.zeros((H, W))
        A[:HOR] = alfa; Lz[:HOR] = lz
        A *= ss(0.02, 0.12, e)                                    # some perto do horizonte
        if v == 0:
            A *= 1 - 0.75 * ss(0.55, 0.95, e)                     # céu limpo: azul aberto no alto
        cn = mix(C['ns'], C['nl'], Lz)
        cn = mix(cn, C['nq'], np.clip((Lz - 0.80) * 3.0, 0, 1))
        col = mix(col, cn, A * 0.97)
    # NUVENS: a fórmula aprovada pelo usuário (todas as variantes)
    n = rn(X, FY)
    n_cima = rn(X, FY - 7)
    luz = np.clip(0.55 + (n - n_cima) * 5.0, 0, 1)
    larg = 0.12 - 0.085 * luz
    banda = ss(0.02, 0.10, e) * (1 - ss(0.90 if v == 1 else 0.70, 1.05 if v == 1 else 0.95, e))
    d = ss(th, th + larg, n) * banda * (n > th - 0.02)
    cn = mix(C['ns'], C['nl'], luz)
    cn = mix(cn, C['nq'], np.clip((luz - 0.75) * 2.0, 0, 1))
    if not usar_fotos:
        col = mix(col, cn, d * 0.96)
    if tem_sol:
        col = mix(col, C['hsol'], np.clip(g1 * 0.55, 0, 1))   # brilho do sol sobre as bordas próximas
    if tem_sol:
        # DISCO do sol pintado (atrás das faixas de nuvem)
        disco = ss(0.062, 0.052, dist)
        cdis = mix(hexc(0xFFB458), hexc(0xFFF4C8), ss(0.058, 0.0, dist))
        col = mix(col, cdis, disco)
        # FAIXAS DE NUVEM passando pela metade de baixo do sol (~40% coberto):
        # silhueta em contraluz perto do sol, borda acesa dourada; longe do
        # sol, iluminadas de rosa como as outras nuvens
        yrow = HOR * (1.0 - tan_el / 0.55)
        rf = Ruido(333, [128, 64, 32, 16], [0.5, 0.25, 0.15, 0.1], 10)
        onda = rf(X, np.zeros_like(X))
        perto = np.exp(-(dist / 0.16) ** 2)
        for (dr, esp, ext, sem) in ((6, 5.0, 0.60, 1), (19, 4.2, 0.80, 2), (31, 6.0, 1.00, 3), (-12, 2.2, 0.35, 4)):
            ond2 = rf(X * 1.7 + sem * 500, np.zeros_like(X))
            centro = yrow + dr + (onda - 0.5) * 6.0
            meia = esp * (0.7 + 0.6 * ond2)
            t = np.abs(Y_ - centro) / meia
            ponta = ss(ext, ext * 0.55, np.abs(dth)) * ss(0.25, 0.45, ond2 + 0.2)
            m = ss(1.15, 0.65, t) * ponta
            borda = ss(0.35, 1.0, t) * m
            c_lado = mix(C['nl'] * 0.92, hexc(0x6A4466), perto * 0.85)          # contraluz perto do sol
            c_lado = mix(c_lado, hexc(0xFFD27A), np.clip(borda * perto * 1.3, 0, 1))   # borda acesa
            col = mix(col, c_lado, np.clip(m * 0.97, 0, 1))

    # névoa no horizonte
    col = mix(col, HORC, ((1 - e) ** 3) * 0.75)

    # ---------------- horizonte em camadas ----------------
    r = (HOR - y) * np.ones((1, W), np.float32)
    def camada(alt, cor, nevoa0, nevoa1):
        m = r < alt[None, :]
        t = nevoa0 + (nevoa1 - nevoa0) * np.clip(1 - r / (alt[None, :] + 1), 0, 1)
        c = mix(np.broadcast_to(cor, col.shape), HORC, t)
        return m, c
    # ---- GEOGRAFIA REAL em volta de Interlagos (ângulo no mundo: 0 leste,
    # 90 sul, 180 oeste, 270 norte). Norte: São Paulo (prédios, torres da
    # Paulista) e a Cantareira atrás; sul: Serra do Mar; represas Billings
    # (sudeste) e Guarapiranga (sudoeste); leste/oeste: bairros baixos.
    # O céu fechado repete 2x a volta: lá a geografia vale "pela metade".
    ANG = np.arange(W, dtype=np.float32) / W * 2 * np.pi
    def g(c_graus, larg_graus):
        d = (ANG - math.radians(c_graus) + np.pi) % (2 * np.pi) - np.pi
        return np.exp(-(d / math.radians(larg_graus)) ** 2)
    gN, gS, gPaul = g(270, 55), g(90, 70), g(272, 10)
    gBill, gGuar = g(45, 14), g(140, 16)
    agua = np.clip(np.maximum(gBill, gGuar) * 2.2 - 0.9, 0, 1)
    rs = np.random.RandomState(500 + v)
    # SERRAS REAIS: silhueta verdadeira (SRTM), com exagero vertical constante
    # (a forma e a posição dos picos são reais; a altura é "de jogo")
    eA, eB = perfil_serras(W)
    pxA = np.tan(np.maximum(eA, -0.002)) * HOR / 0.55
    pxB = np.tan(np.maximum(eB, -0.002)) * HOR / 0.55
    K = 50.0 / max(np.percentile(pxA, 98), 1e-3)                    # serra mais alta ~50 px
    det = linha(11 + v, [32, 16, 8, 4], [0.4, 0.3, 0.2, 0.1])       # textura fina da crista
    h1 = 8 + K * np.maximum(pxA, 0) + 3 * det
    h2 = np.clip(4 + K * pxB + 2 * linha(21 + v, [16, 8, 4], [0.5, 0.3, 0.2]), 0, None) * 0.9
    h4 = (8 + 22 * linha(41 + v, [128, 64, 32, 16, 8, 4], [0.42, 0.26, 0.15, 0.09, 0.05, 0.03])) * (1 - 0.75 * agua)
    xx = np.arange(W, dtype=np.float32)
    arv = linha(51 + v, [16, 8, 4, 2], [0.4, 0.3, 0.2, 0.1])
    h5 = (3 + 9 * linha(61 + v, [64, 32, 16], [0.5, 0.3, 0.2]) + 6 * arv) * (1 - 0.85 * agua)
    # SERRA DO FUNDO: tom próprio (azul-acinzentado esverdeado, mata vista de
    # longe), menos névoa e encostas sombreadas pela inclinação do perfil —
    # antes era quase da cor do céu e se confundia com ele
    S1 = {0: hexc(0xA3B3BE), 1: hexc(0x949EA6), 2: C['s1']}[v]          # fundo: claro, mas cinza (não azul-céu)
    S2 = {0: hexc(0x58767C), 1: hexc(0x66726F), 2: C['s2']}[v]          # serra da frente: verde-azulado escuro
    for alt, cor, a0, a1 in ((h1, S1, 0.10, 0.35), (h2, S2, 0.04, 0.32)):
        m, c = camada(alt, cor, a0, a1)
        if True:
            dx = (np.roll(alt, -10) - np.roll(alt, 10)) / 20.0          # inclinação da encosta (suave)
            for _ in range(3): dx = (np.roll(dx, -4) + dx + np.roll(dx, 4)) / 3
            sh = np.clip(1.0 + dx * 0.25, 0.84, 1.14)[None, :, None]
            topo = np.clip(1 - (alt[None, :] - r) / 3.0, 0, 1)[..., None]  # crista levemente acesa
            c = c * sh * (1.0 + 0.06 * topo)
        col = np.where(m[..., None], c, col)
    rm = Ruido(70 + v, [8, 4, 2], [0.5, 0.3, 0.2], 300)
    mata = rm(X, y * np.ones((1, W)))
    m2 = r < h2[None, :]
    col = np.where(m2[..., None], mix(col, S2 * 0.80, np.clip((mata - 0.45) * 0.6, 0, 1) * ss(0, 12, h2[None, :] - r)), col)
    # névoa NOS VALES (entre as serras e os morros): mais forte no fechado, dourada no entardecer
    neb_vale = {0: 0.14, 1: 0.28, 2: 0.42}[v]
    col = mix(col, HORC, np.clip(np.exp(-r / 14.0) * neb_vale * (r < h2[None, :] + 4), 0, 1))
    for alt, cor, a0, a1 in ((h4, C['s4'], 0.03, 0.30),):
        m, c = camada(alt, cor, a0, a1)
        col = np.where(m[..., None], c, col)
    # cidade: densa e alta ao norte, torres da Paulista, bairros baixos no resto
    hc = np.zeros(W, np.float32)
    tom = np.zeros(W, np.int64)
    ant = []
    xc = 0
    while xc < W:
        dens = 0.10 + 0.85 * gN[xc]
        if agua[xc] > 0.2 or rs.rand() > dens:
            xc += rs.randint(3, 10)
            continue
        w = rs.randint(4, 13)
        alt = 3 + (5 + 20 * gN[xc]) * rs.rand()
        if gPaul[xc] > 0.3 and rs.rand() < 0.5:          # torres altas e finas
            w = rs.randint(3, 6)
            alt = 30 + 22 * rs.rand()
            if rs.rand() < 0.18:
                ant.append((xc + w // 2, int(alt) + rs.randint(6, 12)))
        elif rs.rand() < 0.05 * (0.3 + gN[xc]):
            ant.append((xc + w // 2, int(alt) + rs.randint(6, 12)))
        hc[xc:xc + w] = alt
        tom[xc:xc + w] = rs.randint(0, 4)
        xc += w + (1 if rs.rand() < 0.3 else 0)
    TONS = [hexc(0xE0D6C4), hexc(0xC2B094), hexc(0xEDEAE4), hexc(0xB09C84)]
    if v == 2:
        TONS = [C['cid'], C['cid'] * 0.85, C['cid'] * 1.1, C['cid'] * 0.95]
    topo_c = h4 + hc
    mc = (r < topo_c[None, :]) & (hc[None, :] > 0)
    base = np.stack(TONS)[tom]
    longe_cid = 0.08 + 0.10 * (1 - gN)                        # bairros mais longe = mais névoa
    cc = mix(np.broadcast_to(base[None, :, :], col.shape), HORC,
             longe_cid[None, :] + 0.22 * np.clip((topo_c[None, :] - r) / (hc[None, :] + 1), 0, 1))
    jan = ((xx[None, :].astype(np.int64) % 3) == 1) & ((y.astype(np.int64) % 2) == 0)
    if v == 2:   # entardecer: janelas acesas
        acesa = jan & (np.random.RandomState(3).rand(H, W) < 0.35)
        cc = np.where(acesa[..., None], hexc(0xFFD27A), np.where(jan[..., None], cc * 0.85, cc))
    else:
        cc = np.where(jan[..., None], mix(cc, HORC, 0.35), cc)
    col = np.where(mc[..., None], cc, col)
    for (xa, ta) in ant:
        if xa >= W:
            continue
        ta = int(ta + h4[xa])
        for yy in range(max(0, HOR - ta), HOR - int(h4[xa])):
            col[yy, xa] = mix(C['s4'], HORC[yy, xa], 0.35)
        col[max(0, HOR - ta), xa] = np.array([230, 40, 30], np.float32)
    rd = Ruido(80 + v, [6, 3], [0.6, 0.4], 300)
    cop = rd(X, y * np.ones((1, W)))
    m4 = (r < h4[None, :]) & ~mc
    col = np.where(m4[..., None], mix(col, C['s4'] * 0.72, np.clip((cop - 0.55) * 2.5, 0, 0.6)), col)
    col = np.where(m4[..., None], mix(col, C['s4'] * 1.18, np.clip((0.40 - cop) * 2.5, 0, 0.4)), col)
    rsc = np.random.RandomState(900 + v)
    for _ in range(int(420 * W / 2048)):
        xc = rsc.randint(0, W)
        top = int(h4[xc]) - 2
        if top < 2:
            continue
        rr = rsc.randint(1, top)
        yc = HOR - rr
        if mc[yc, xc]:
            continue
        tel = hexc(0xB8603C) if rsc.rand() < 0.7 else hexc(0xE8E2D6)
        if v == 2:
            tel = hexc(0xFFD27A) if rsc.rand() < 0.4 else C['s4'] * 0.7
        col[yc, xc] = mix(tel, HORC[yc, xc], 0.15)
        if xc + 1 < W:
            col[yc, xc + 1] = mix(tel, HORC[yc, xc + 1], 0.15)
    # REPRESAS: faixa de água rente ao horizonte, refletindo o céu (+ o sol no entardecer)
    ceu_ref = mix(C['mei'] if v != 2 else hexc(0xE0866E), HORC, 0.45)
    for yy in range(HOR - 5, HOR):
        fr = (yy - (HOR - 5)) / 5.0
        cw = mix(ceu_ref[yy] if ceu_ref.ndim == 3 else np.broadcast_to(ceu_ref, (W, 3)), HORC[yy], 0.3 + 0.4 * fr)
        brilho = ((xx.astype(np.int64) * 7 + yy * 3) % 11 == 0) * 0.35
        cw = cw + (255.0 - cw) * brilho[:, None]
        a2 = agua[:, None] * np.ones((1, 3))
        col[yy] = col[yy] * (1 - a2) + cw * a2
    for alt, cor, a0, a1 in ((h5, C['s5'], 0.0, 0.18),):
        m, c = camada(alt, cor, a0, a1)
        col = np.where(m[..., None], c, col)
    # abaixo do horizonte: BRANCO -> no jogo vira exatamente a cor da névoa
    col[HOR:] = 255.0
    col[:3] = C['zen']
    return col, int(PAL['zen'])


def para_565(col):
    Hh, Ww = col.shape[:2]
    B = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32)
    dd = (B[(np.arange(Hh) % 4)[:, None], (np.arange(Ww) % 4)[None, :]] + 0.5) / 16.0
    R = np.clip(col[..., 0] + dd * 8, 0, 255).astype(np.uint16)
    G = np.clip(col[..., 1] + dd * 4, 0, 255).astype(np.uint16)
    Bc = np.clip(col[..., 2] + dd * 8, 0, 255).astype(np.uint16)
    return ((R >> 3) << 11) | ((G >> 2) << 5) | (Bc >> 3)


def dec565(px):
    r = ((px >> 11) & 31) * 255 / 31; g = ((px >> 5) & 63) * 255 / 63; b = (px & 31) * 255 / 31
    return np.stack([r, g, b], -1).astype(np.float32)


def twid(x, y):
    """endereço "twiddled" do PVR (bits de y nas posições pares, de x nas ímpares)"""
    a = 0
    for b in range(10):
        a |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
    return a


def vq_textura(px, semente=1):
    """Compressão VQ do PowerVR: 256 blocos 2x2 (codebook de 2 KB, RGB565) +
    1 índice por bloco, em ordem twiddled. Textura 1024x256 -> 67.584 bytes."""
    Hh, Ww = px.shape
    rgb = dec565(px)
    X = rgb.reshape(Hh // 2, 2, Ww // 2, 2, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 12)
    rs = np.random.RandomState(semente)
    C = X[rs.choice(len(X), 256, replace=False)].copy()
    for _ in range(14):
        d = (X * X).sum(1)[:, None] - 2 * X @ C.T + (C * C).sum(1)[None, :]
        lab = d.argmin(1)
        cnt = np.bincount(lab, minlength=256)
        for j in range(12):
            sm = np.bincount(lab, weights=X[:, j], minlength=256)
            C[:, j] = np.where(cnt > 0, sm / np.maximum(cnt, 1), C[:, j])
        vazio = np.where(cnt == 0)[0]
        if len(vazio):
            pior = d[np.arange(len(X)), lab].argsort()[-len(vazio):]
            C[vazio] = X[pior]
    d = (X * X).sum(1)[:, None] - 2 * X @ C.T + (C * C).sum(1)[None, :]
    lab = d.argmin(1).astype(np.uint8)
    # codebook: texels de cada bloco em ordem twiddled (sup-esq, inf-esq, sup-dir, inf-dir)
    Cb = C.reshape(256, 2, 2, 3)                    # [entrada, dy, dx, rgb]
    ordem = [(0, 0), (1, 0), (0, 1), (1, 1)]
    cb = np.zeros((256, 4), np.uint16)
    for k, (dy, dx) in enumerate(ordem):
        c = np.clip(np.round(Cb[:, dy, dx]), 0, 255).astype(np.uint16)
        cb[:, k] = ((c[:, 0] >> 3) << 11) | ((c[:, 1] >> 2) << 5) | (c[:, 2] >> 3)
    # índices (Ww/2 x Hh/2), twiddled em blocos quadrados ao longo de x
    iw, ih = Ww // 2, Hh // 2
    idx = lab.reshape(ih, iw)
    out = np.zeros(iw * ih, np.uint8)
    T = np.array([[twid(x, y) for x in range(ih)] for y in range(ih)], np.int64)
    for b in range(iw // ih):
        out[b * ih * ih + T.ravel()] = idx[:, b * ih:(b + 1) * ih].ravel()
    return cb.astype('<u2').tobytes() + out.tobytes()


def main():
    global W
    previa = sys.argv[1] if len(sys.argv) > 1 else None
    dados, zens = [], []
    for v in range(3):
        arte = os.path.join(AQUI, ARTE[v])
        if os.path.exists(arte):
            from PIL import Image
            W = TAM[v]
            col = np.asarray(Image.open(arte).convert("RGB").resize((W, H), Image.LANCZOS), np.float32).copy()
            zen = int(col[1, :, 0].mean()) << 16 | int(col[1, :, 1].mean()) << 8 | int(col[1, :, 2].mean())
            print("usando arte à mão:", ARTE[v])
        else:
            col, zen = gerar(v)
        zens.append(zen)
        px = para_565(col)
        for h in range(TAM[v] // 1024):
            dados.append(vq_textura(px[:, h * 1024:(h + 1) * 1024], 1 + v * 8 + h))
        if previa:
            from PIL import Image
            Image.fromarray(np.clip(col, 0, 255).astype(np.uint8)).save(os.path.join(previa, f"ceu_{v}.png"))
    open(os.path.join(AQUI, "ceu.bin"), "wb").write(b"".join(dados))
    print("ceu.bin:", sum(len(d) for d in dados), "bytes; zenites", [hex(z) for z in zens])


if __name__ == "__main__":
    main()
