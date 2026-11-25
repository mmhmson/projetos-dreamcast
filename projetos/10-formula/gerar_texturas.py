# Gera texturas detalhadas do cenário no PC (texturas.bin, embutido no jogo).
# Tudo gerado por código (nada copiado). Tons claros/cinza: a cor final vem
# do vértice no jogo (clima, luz). Formato: RGB565 little-endian, linear.
#   asfalto 256x256 | concreto 128x128 | torcida 256x256
#   python gerar_texturas.py [pasta_previa]
import sys, os
import numpy as np

AQUI = os.path.dirname(os.path.abspath(__file__))
rs = np.random.RandomState(42)


def ruido(n, celulas, amps, semente):
    """fbm de valor periódico (n x n)"""
    r = np.random.RandomState(semente)
    out = np.zeros((n, n), np.float32)
    for c, a in zip(celulas, amps):
        g = n // c
        L = r.rand(g, g).astype(np.float32)
        y = np.arange(n) / c; x = np.arange(n) / c
        iy = np.floor(y).astype(int); ix = np.floor(x).astype(int)
        fy = (y - iy)[:, None]; fx = (x - ix)[None, :]
        fy = fy * fy * (3 - 2 * fy); fx = fx * fx * (3 - 2 * fx)
        y0, y1 = iy % g, (iy + 1) % g; x0, x1 = ix % g, (ix + 1) % g
        a00 = L[np.ix_(y0, x0)]; a01 = L[np.ix_(y0, x1)]; a10 = L[np.ix_(y1, x0)]; a11 = L[np.ix_(y1, x1)]
        out += a * ((a00 * (1 - fx) + a01 * fx) * (1 - fy) + (a10 * (1 - fx) + a11 * fx) * fy)
    return out


def teto_nuvens(N=256):
    """nuvens vistas DE BAIXO (camada sobre a cabeça): cúmulos com miolo claro e
    base acinzentada, periódica nos dois eixos; alfa = densidade"""
    n = ruido(N, [64, 32, 16, 8, 4], [0.45, 0.25, 0.15, 0.10, 0.05], 77)
    g = ruido(N, [128, 64], [0.7, 0.3], 78)                     # onde há grupos de nuvem
    d = np.clip((n * (0.55 + 0.9 * g) - 0.52) / 0.20, 0, 1)
    d = d * d * (3 - 2 * d)
    esp = ruido(N, [16, 8], [0.6, 0.4], 79)                    # espessura: base mais escura
    luz = np.clip(0.80 + 0.25 * (1 - d) * 0.5 + 0.18 * (esp - 0.5) - 0.12 * d, 0.62, 1.0)
    img = np.zeros((N, N, 4), np.float32)
    img[..., 0] = img[..., 1] = img[..., 2] = 255 * luz
    img[..., 2] = np.minimum(255, img[..., 2] * 1.03)
    img[..., 3] = 255 * d
    return img


def para565(img):
    img = np.clip(img, 0, 255)
    B = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.float32)
    h, w = img.shape[:2]
    dd = (B[(np.arange(h) % 4)[:, None], (np.arange(w) % 4)[None, :]] + 0.5) / 16.0
    R = np.clip(img[..., 0] + dd * 8, 0, 255).astype(np.uint16)
    G = np.clip(img[..., 1] + dd * 4, 0, 255).astype(np.uint16)
    Bb = np.clip(img[..., 2] + dd * 8, 0, 255).astype(np.uint16)
    return (((R >> 3) << 11) | ((G >> 2) << 5) | (Bb >> 3)).astype('<u2')


def cinza(v):
    return np.stack([v, v, v], -1)


def asfalto(n=256):
    rs = np.random.RandomState(1234)
    k = n // 256
    v = 214 + 26 * (ruido(n, [64, 16, 4], [0.5, 0.3, 0.2], 1) - 0.5)
    grao = rs.rand(n, n)
    v += (grao - 0.5) * 34
    v[grao < 0.035] -= 42                       # pedrinhas escuras
    v[grao > 0.985] += 30                       # pedrinhas claras (quartzo)
    # remendos com borda de selante
    for _ in range(7):
        x0, y0 = rs.randint(0, n, 2); w, h = rs.randint(26, 80) * k, rs.randint(18, 60) * k
        d = rs.choice([-18, 10])
        ys = (np.arange(y0, y0 + h) % n); xs = (np.arange(x0, x0 + w) % n)
        v[np.ix_(ys, xs)] += d
        for yy in (ys[0], ys[-1]): v[yy, xs] -= 38
        for xx in (xs[0], xs[-1]): v[ys, xx] -= 38
    # rachaduras seladas (linhas escuras sinuosas)
    for c in range(7):
        x, y = rs.rand(2) * n; ang = rs.rand() * 6.28
        for _ in range(150 * k):
            ang += (rs.rand() - 0.5) * 0.5 / k
            x += np.cos(ang); y += np.sin(ang)
            xi, yi = int(x) % n, int(y) % n
            v[yi, xi] -= 42; v[yi, (xi + 1) % n] -= 14
    # manchas de óleo/borracha
    m = ruido(n, [128, 32], [0.7, 0.3], 7)
    v -= np.clip((m - 0.62) * 140, 0, 40)
    return cinza(v)


def concreto(n=128):
    rs = np.random.RandomState(321)
    k = n // 128
    v = 226 + 18 * (ruido(n, [32 * k, 8 * k, 2 * k], [0.5, 0.3, 0.2], 11) - 0.5) + (rs.rand(n, n) - 0.5) * 10
    if k > 1:                                      # poros e bolhas do concreto (só na versão grande)
        poro = rs.rand(n, n)
        v[poro < 0.02] -= 30; v[poro > 0.995] += 14
    v[:, 0:2 * k] = 150                           # junta vertical (placa de 4 m)
    v[:, 63 * k] -= 22                            # emenda de forma no meio
    for y in (42, 85): v[y * k, :] -= 16          # linhas da forma
    for _ in range(14):                           # escorridos de água a partir do topo
        x = rs.randint(2 * k, n); comp = rs.randint(20, 90) * k; larg = rs.randint(1, 4) * k
        for y in range(comp):
            v[y, x:x + larg] -= 18 * (1 - y / comp)
    yy = np.arange(n)[:, None] / k                 # sujeira de pneu/poeira embaixo
    v -= np.clip((yy - 100) * 1.6, 0, 45) * (0.6 + 0.4 * ruido(n, [16 * k, 4 * k], [0.6, 0.4], 12))
    for _ in range(6):                             # marcas pretas de pneu
        x = rs.randint(0, n - 30 * k); y = rs.randint(96, 120) * k
        v[y:y + 5 * k, x:x + rs.randint(10, 30) * k] -= 60
    v[0:2 * k, :] = 245                           # borda de cima
    return cinza(v)


def torcida(quadro=0):
    """quadro 0/1: as MESMAS pessoas; no quadro 1 parte levanta os braços,
    quem estava de braço para cima baixa, bandeiras trocam de lado e alguns
    dão um pulinho (animação por troca de textura)"""
    rs = np.random.RandomState(4242)
    ra = np.random.RandomState(77)
    n = 256
    img = np.zeros((n, n, 3), np.float32)
    CAM = [(24, 184, 72), (255, 224, 32), (32, 96, 232), (255, 255, 255), (240, 40, 40), (255, 144, 16),
           (248, 232, 56), (152, 72, 208), (16, 192, 192), (48, 208, 96), (20, 20, 24), (230, 120, 170)]
    PELE = [(232, 184, 150), (198, 138, 100), (138, 90, 60), (240, 204, 170), (110, 70, 48)]
    CAB = [(42, 30, 22), (16, 16, 16), (106, 74, 42), (200, 160, 96), (58, 58, 58)]
    for fil in range(8):
        y0 = fil * 32
        CAD = [(40, 70, 150), (30, 120, 60), (200, 170, 30), (40, 70, 150)]
        img[y0:y0 + 32] = CAD[fil % 4]            # cadeiras coloridas (de longe: mancha colorida, não cinza)
        img[y0 + 26:y0 + 28] = 60                 # sombra do degrau
        img[y0 + 28:y0 + 32] = 150                # borda do degrau
        for col in range(16):
            if rs.rand() < 0.03:
                continue
            x0 = col * 16 + rs.randint(0, 3); yb = y0 + rs.randint(0, 3)
            cam = np.array(CAM[rs.randint(len(CAM))], np.float32)
            pel = np.array(PELE[rs.randint(len(PELE))], np.float32)
            cab = np.array(CAB[rs.randint(len(CAB))], np.float32) if rs.rand() > 0.2 else cam * 0.8
            pose = rs.randint(8)
            mexe = ra.rand()
            if quadro == 1:
                if pose == 0: pose = 5                       # baixa os braços
                elif pose >= 3 and mexe < 0.45: pose = 0     # levanta
                if mexe > 0.7: yb = max(y0, yb - 2)          # pulinho
            # tronco (camisa com sombra lateral e gola)
            for yy in range(yb + 10, yb + 26):
                for xx in range(x0 + 1, x0 + 15):
                    s = 1.0 - 0.25 * (xx - x0 - 2) / 11.0
                    img[yy % n, xx % n] = cam * s
            img[(yb + 10) % n, (x0 + 6) % n:(x0 + 10) % n] = cam * 0.6
            if rs.rand() < 0.3:                   # listra na camisa (time)
                img[(yb + 16) % n, (x0 + 2):(x0 + 14)] = 250
            # cabeça
            for yy in range(yb + 2, yb + 10):
                for xx in range(x0 + 5, x0 + 11):
                    img[yy % n, xx % n] = pel * (0.85 if xx > x0 + 8 else 1.0)
            img[yb + 1:yb + 4, x0 + 5:x0 + 11] = cab
            img[(yb + 6) % n, x0 + 6] = 30; img[(yb + 6) % n, x0 + 9] = 30   # olhos
            # braços
            if pose == 0:      # para cima
                img[(yb - 4) % n:(yb + 11), x0 + 1:x0 + 3] = pel
                img[(yb - 4) % n:(yb + 11), x0 + 13:x0 + 15] = pel
            elif pose == 1:    # bandeira
                ban = np.array(CAM[rs.randint(len(CAM))], np.float32)
                yt = max(yb - 10, 0)
                img[yt:yb + 12, x0 + 14:x0 + 15] = 40
                if quadro == 0: img[yt:max(yb - 3, 0), x0 + 15:x0 + 16 + 6] = ban
                else: img[yt + 1:max(yb - 2, 0), x0 + 15:x0 + 15 + 7] = ban * 0.92   # tremulou
            elif pose == 2:    # cachecol
                img[yb + 10:yb + 12, x0 + 2:x0 + 14] = np.array(CAM[rs.randint(len(CAM))], np.float32)
            else:
                img[yb + 12:yb + 22, x0:x0 + 2] = pel
                img[yb + 12:yb + 22, x0 + 14:x0 + 16] = pel
    return img


def para4444(rgba):
    rgba = np.clip(rgba, 0, 255).astype(np.uint16)
    R, G, B, A = (rgba[..., k] >> 4 for k in range(4))
    return ((A << 12) | (R << 8) | (G << 4) | B).astype('<u2')


def disco(img, cx, cy, r, cor, alfa=None):
    h, w = img.shape[:2]
    y0, y1 = int(max(0, cy - r - 1)), int(min(h, cy + r + 2))
    x0, x1 = int(max(0, cx - r - 1)), int(min(w, cx + r + 2))
    if y0 >= y1 or x0 >= x1:
        return
    yy, xx = np.mgrid[y0:y1, x0:x1]
    m = (yy - cy) ** 2 + (xx - cx) ** 2 <= r * r
    img[y0:y1, x0:x1][m] = cor


def linha_grossa(img, xa, ya, xb, yb, ra, rb, cor):
    n = int(max(abs(xb - xa), abs(yb - ya)) * 2) + 2
    for k in range(n + 1):
        t = k / n
        disco(img, xa + (xb - xa) * t, ya + (yb - ya) * t, ra + (rb - ra) * t, cor)


def cidade():
    """panorama de São Paulo (512x64, repete): fachadas com janelas, caixas
    d'água, antenas; 2 camadas (fundo mais claro/enevoado). RGBA + luzes."""
    W, H = 512, 64
    img = np.zeros((H, W, 4), np.float32)
    luz = np.zeros((H, W, 3), np.float32)
    r = np.random.RandomState(77)
    PAL = [(228, 218, 198), (238, 236, 230), (176, 178, 182), (124, 148, 172), (182, 128, 100), (214, 194, 164),
           (200, 204, 210), (150, 160, 150)]
    def predio(x0, w, alt, cor, nevoa, fundo):
        cor = np.array(cor, np.float32)
        neb = np.array((196, 206, 218), np.float32)
        topo = H - alt
        vidro = cor[2] > cor[0] + 20
        andar = 2 if not vidro else 3
        for x in range(x0, x0 + w):
            xm = x % W
            lado = (x - x0) >= w * 0.72              # face lateral (sombra)
            for y in range(topo, H):
                c = cor * (0.76 if lado else 1.0)
                jx, jy = (x - x0) % 2, (y - topo) % andar
                janela = jx == 1 and jy == 1 and y > topo + 1 and (x - x0) < w - 1 and (x - x0) > 0
                if vidro:
                    c = c * (0.92 + 0.16 * ((y - topo) / max(alt, 1)))   # reflexo do céu em cima
                    if jy == 0: c = c * 0.72                             # faixa entre andares
                elif janela:
                    c = c * 0.58
                c = c + (neb - c) * nevoa
                img[y, xm, :3] = c
                img[y, xm, 3] = 255
                if (janela or (vidro and jy == 1)) and r.rand() < (0.16 if fundo else 0.30):
                    q = 0.6 + 0.4 * r.rand()
                    luz[y, xm] = np.array((255, 196, 110)) * q * (0.6 if fundo else 1.0)
            img[topo, xm, :3] = np.minimum(img[topo, xm, :3] * 1.12, 255)   # beiral claro
        # topo: caixa d'água, casa de máquinas, antena com luz vermelha
        if r.rand() < 0.45 and w >= 6:
            cx = x0 + r.randint(1, w - 4)
            for x in range(cx, cx + 3):
                for y in range(topo - 2, topo):
                    img[y, x % W, :3] = cor * 0.66 + (np.array((196, 206, 218)) - cor * 0.66) * nevoa
                    img[y, x % W, 3] = 255
        if alt > 26 and r.rand() < 0.35:
            ax = x0 + w // 2; ah = r.randint(4, 10)
            for y in range(max(0, topo - ah), topo):
                img[y, ax % W, :3] = (120, 124, 130); img[y, ax % W, 3] = 255
            luz[max(0, topo - ah), ax % W] = (255, 40, 30)
    # camada de fundo (alta e enevoada) e da frente (mais baixa, nítida)
    for camada, (amin, amax, nev) in enumerate(((22, 58, 0.42), (10, 34, 0.12))):
        x = r.randint(0, 8)
        while x < W:
            w = r.randint(5, 20)
            alt = r.randint(amin, amax)
            if r.rand() < 0.12: alt = min(H - 4, alt + r.randint(8, 16))   # torre de destaque
            predio(x, w, alt, PAL[r.randint(len(PAL))], nev, camada == 0)
            x += w + (r.randint(1, 6) if camada == 0 else r.randint(0, 4))
    # base contínua: casas baixas e copas de árvores (sem buracos embaixo)
    for x in range(W):
        hb = 4 + int(3 * np.sin(x * 0.21) + 2 * np.sin(x * 0.07 + 1)) + r.randint(0, 2)
        for y in range(H - hb, H):
            if r.rand() < 0.55:
                c = np.array((92, 110, 86)) * (0.85 + 0.3 * r.rand())
            else:
                c = np.array((178, 110, 82)) if r.rand() < 0.4 else np.array((210, 204, 192))
            img[y, x, :3] = c; img[y, x, 3] = 255
            if r.rand() < 0.05: luz[y, x] = (255, 210, 140)
    return img, luz


def ipe():
    """ipê 128x128: tronco torto, galhos ramificados e copa em guarda-chuva de
    cachos de flores (tons de cinza: a cor vem do vértice)"""
    N = 128
    img = np.zeros((N, N, 4), np.float32)
    r = np.random.RandomState(5)
    MAD = (96, 72, 56, 255)
    pontas = []
    def galho(x, y, ang, comp, esp, nivel):
        x2, y2 = x + np.cos(ang) * comp, y - np.sin(ang) * comp
        linha_grossa(img, x, y, x2, y2, esp, esp * 0.68, MAD)
        if nivel == 0 or y2 < 30:
            pontas.append((x2, y2)); return
        n = 2 if r.rand() < 0.6 else 3
        for k in range(n):
            da = (k - (n - 1) / 2) * (0.55 + 0.25 * r.rand()) + (r.rand() - 0.5) * 0.25
            galho(x2, y2, ang + da, comp * (0.62 + 0.18 * r.rand()), esp * 0.66, nivel - 1)
    # tronco levemente torto
    linha_grossa(img, 64, 127, 62, 96, 4.2, 3.2, MAD)
    linha_grossa(img, 62, 96, 65, 74, 3.2, 2.6, MAD)
    galho(65, 74, np.pi / 2 + 0.55, 22, 2.4, 3)
    galho(65, 74, np.pi / 2 - 0.50, 23, 2.4, 3)
    galho(65, 74, np.pi / 2 + 0.05, 18, 2.0, 2)
    # copa: cachos em volta das pontas + recheio no "guarda-chuva"
    cachos = []
    for (px, py) in pontas:
        for k in range(r.randint(4, 7)):
            cachos.append((px + r.randn() * 6, py + r.randn() * 3.5 - 2, 2.2 + r.rand() * 2.6))
    for k in range(110):
        a = r.rand() * np.pi; rr = np.sqrt(r.rand())
        cx = 64 + np.cos(a) * 54 * rr; cy = 40 - np.sin(a) * 26 * rr + 6
        cachos.append((cx, cy, 2.0 + r.rand() * 2.4))
    cachos.sort(key=lambda c: -c[1])                         # de baixo para cima
    for (cx, cy, cr) in cachos:
        if not (5 < cx < 123 and 4 < cy < 100):
            continue
        y0, y1 = int(cy - cr - 1), int(cy + cr + 2)
        x0, x1 = int(cx - cr - 1), int(cx + cr + 2)
        for y in range(max(0, y0), min(N, y1)):
            for x in range(max(0, x0), min(N, x1)):
                d = np.hypot(x - cx, y - cy) / cr
                if d > 1 or r.rand() < 0.12:
                    continue
                topo = 1.0 - (y - (cy - cr)) / (2 * cr)          # claro em cima do cacho
                alto = 1.0 - (y / 100.0)                          # copa de cima mais clara
                l = 150 + 70 * topo + 35 * alto + r.randint(-18, 18)
                img[y, x] = (l, l, l, 255)
    # pétalas caídas / flores soltas fora da copa (bordas irregulares)
    for k in range(40):
        x, y = r.randint(8, 120), r.randint(8, 70)
        if img[y, x, 3] == 0 and r.rand() < 0.5:
            img[y, x] = (230, 230, 230, 255)
    return img


def marcas():
    """marca de frenagem (32x128): 3-4 riscos de borracha ao longo de v, que
    escurecem no começo e somem aos poucos"""
    W, H = 32, 128
    r = np.random.RandomState(9)
    u = np.arange(W)[None, :] / W; v = np.arange(H)[:, None] / H
    a = np.zeros((H, W), np.float32)
    for k in range(4):
        c = 0.2 + 0.6 * r.rand(); w = 0.04 + 0.05 * r.rand()
        a += np.exp(-((u - c) / w) ** 2) * (0.5 + 0.5 * r.rand())
    a = np.clip(a, 0, 1) * (0.75 + 0.25 * ruido(128, [32, 8], [0.6, 0.4], 3)[:H, :W])
    perfil = np.clip(v / 0.06, 0, 1) * np.clip((1.0 - v) / 0.55, 0, 1) ** 1.3
    a *= perfil * (np.abs(u - 0.5) < 0.47)
    img = np.zeros((H, W, 4), np.float32)
    img[..., 3] = a * 255
    return img


def roda():
    """roda girando rápido (64x64, RGB): banda de letras borrada, aro em anel"""
    N = 64
    y, x = np.mgrid[0:N, 0:N]
    d = np.hypot(x - 31.5, y - 31.5) / 32.0 * 0.92          # em raios do pneu
    ang = np.arctan2(y - 31.5, x - 31.5)
    v = np.full((N, N), 48.0)
    v[d < 0.92] = 46 + 10 * (d[d < 0.92] - 0.56)              # borracha
    letras = (d > 0.835) & (d < 0.885)
    v[letras] = 98 + 16 * np.sin(ang[letras] * 3)              # letras viram anel
    v[(d > 0.50) & (d <= 0.56)] = 168                          # borda do aro
    m = d <= 0.50
    v[m] = 92 + 18 * np.sin(ang[m] * 5) * (d[m] / 0.5) + 10 * np.sin(d[m] * 60)   # raios borrados
    v[d < 0.17] = 54; v[d < 0.06] = 120                        # cubo e porca
    return cinza(v)


def sombra_carro():
    """sombra com o FORMATO do carro (64x64 alfa): u = largura, v = traseira->frente"""
    N = 64
    a = np.zeros((N, N), np.float32)
    def ret(x0, x1, z0, z1):     # x em m (-1.15..1.15), z em m (-2.8..3.2)
        u0, u1 = int((x0 + 1.15) / 2.3 * N), int((x1 + 1.15) / 2.3 * N)
        v0, v1 = int((z0 + 2.8) / 6.0 * N), int((z1 + 2.8) / 6.0 * N)
        a[max(v0, 0):min(v1, N), max(u0, 0):min(u1, N)] = 1
    ret(-0.30, 0.30, -2.3, 2.7)                 # monocoque e bico
    ret(-0.72, 0.72, -1.3, 0.6)                 # laterais
    for l in (-1, 1):
        ret(l * 0.82 - 0.17, l * 0.82 + 0.17, 1.35, 2.05)     # rodas dianteiras
        ret(l * 0.80 - 0.20, l * 0.80 + 0.20, -1.75, -0.95)   # traseiras
    ret(-0.85, 0.85, 2.55, 2.95)                # asa dianteira
    ret(-0.50, 0.50, -2.50, -2.10)              # asa traseira (sombra mais longe)
    for _ in range(3):                          # borra (borda suave)
        a = (a + np.roll(a, 1, 0) + np.roll(a, -1, 0) + np.roll(a, 1, 1) + np.roll(a, -1, 1)) / 5
    a[0, :] = a[-1, :] = a[:, 0] = a[:, -1] = 0
    img = np.zeros((N, N, 4), np.float32)
    img[..., 3] = np.clip(a * 1.25, 0, 1) * 255
    return img


def nuvem():
    """fumaça/poeira (64x64 alfa): bolo redondo com borda irregular"""
    N = 64
    y, x = np.mgrid[0:N, 0:N]
    d = np.hypot(x - 31.5, y - 31.5) / 31.5
    n = ruido(N, [16, 8, 4], [0.5, 0.3, 0.2], 21)
    a = np.clip(1.0 - d * (1.0 + 0.9 * (n - 0.5)) - 0.05, 0, 1) ** 1.0
    l = 200 + 55 * n
    img = np.zeros((N, N, 4), np.float32)
    img[..., 0] = img[..., 1] = img[..., 2] = l
    img[..., 3] = a * 255
    return img


def fachadas():
    """6 fachadas 128x128 (4x4 módulos de 32 px = 3 m): 0 residencial c/ sacada,
    1 vidro, 2 reboco antigo, 3 tijolinho, 4 galpão, 5 sobrado. Cada janela varia
    (cortina, persiana, ar-condicionado). + máscara de janelas acesas (64x64)."""
    N, M = 128, 32
    r = np.random.RandomState(31)
    out = []
    yy, xx = np.mgrid[0:N, 0:N]
    mx, my = xx % M, yy % M
    jan = (mx >= 8) & (mx < 24) & (my >= 8) & (my < 21)
    moldura = (mx >= 7) & (mx < 25) & (my >= 7) & (my < 22) & ~jan
    peitoril = (mx >= 6) & (mx < 26) & (my >= 22) & (my < 24)
    cel = (yy // M) * 4 + xx // M
    var = r.rand(16)                                   # variação por janela
    for t in range(6):
        v = np.full((N, N), 226.0) + (r.rand(N, N) - 0.5) * 7 + 6 * (ruido(N, [32, 8], [0.6, 0.4], 80 + t) - 0.5)
        vidro = 70 + 40 * (my < 12) + 14 * (mx < 14)                      # reflexo no alto da janela
        cortina = var[cel] < 0.35
        if t == 0:
            v[moldura] = 246; v[jan] = np.where(cortina[jan] & (my[jan] < 15), 196, vidro[jan])
            v[peitoril] = 238
            v[(my >= 26) & (my < 29)] = 168                               # laje da sacada
            v[(my >= 23) & (my < 26) & (mx % 3 == 0)] = 214                # grade
            ar = (var[cel] > 0.75) & (mx >= 18) & (mx < 25) & (my >= 22) & (my < 26)
            v[ar] = 236                                                     # ar-condicionado
        elif t == 1:
            v = 118 + 48 * (1 - yy / N) + 26 * (((xx + 2 * yy) % 41) < 6) + 10 * (var[cel] - 0.5)
            v[(mx % 16) == 0] = 206
            v[(my >= 28)] = 92
        elif t == 2:
            v = 198 + 12 * ruido(N, [32, 8], [0.6, 0.4], 41) - 20 * (ruido(N, [16, 4], [0.5, 0.5], 42) > 0.62)
            for x in r.randint(0, N, 16):
                v[:, x:x + 2] -= np.linspace(0, 24, N)[:, None] * (r.rand() < 0.7)
            j2 = (mx >= 10) & (mx < 22) & (my >= 8) & (my < 21)
            fechada = var[cel] < 0.4
            v[j2] = np.where(fechada[j2], 150 - 28 * (my[j2] % 2), vidro[j2] - 10)    # veneziana
            v[(my == 7) & (mx >= 9) & (mx < 23)] = 240
            v[peitoril] = 232
        elif t == 3:
            fila = yy // 4
            v = 172 + (r.rand(N, N) - 0.5) * 24 + 10 * (r.rand(N // 4, N // 8).repeat(4, 0).repeat(8, 1) - 0.5)
            v[yy % 4 == 3] = 222
            v[((xx + (fila % 2) * 4) % 8) == 0] = 222
            v[moldura] = 240; v[jan] = np.where(cortina[jan], 200, vidro[jan]); v[peitoril] = 236
        elif t == 4:
            v = np.where(xx % 4 < 2, 170.0, 208.0) + (r.rand(N, N) - 0.5) * 6
            v[(my >= 6) & (my < 11) & (yy < 32)] = 104
            v -= np.clip((yy - 104) * 1.6, 0, 34)
            v[(yy % 64) == 63] = 150                                       # emenda das chapas
        else:
            v[moldura] = 244; v[jan] = np.where(cortina[jan], 190, vidro[jan])
            grade = jan & ((mx % 3) == 0)
            v[grade] = 200
            v[peitoril] = 246
            plantas = (var[cel] > 0.7) & (my >= 19) & (my < 22) & (mx >= 9) & (mx < 23)
            v[plantas] = 120                                                # vaso na janela
        out.append(cinza(v))
    luz = np.zeros((64, 64, 3), np.float32)
    for my_ in range(4):
        for mx_ in range(4):
            if r.rand() < 0.42:
                q = 0.65 + 0.35 * r.rand()
                cor = np.array((255, 200, 120)) if r.rand() < 0.7 else np.array((220, 235, 255))
                luz[my_ * 16 + 4:my_ * 16 + 11, mx_ * 16 + 4:mx_ * 16 + 12] = cor * q
    return out, luz


def bairros():
    """anel de bairros (512x32, repete): casas com telhado, prédios baixos,
    caixas d'água e uma linha de árvores na frente"""
    W, H = 512, 32
    img = np.zeros((H, W, 4), np.float32)
    r = np.random.RandomState(57)
    x = 0
    while x < W:                                   # construções (atrás)
        w = r.randint(3, 10); alt = r.randint(6, 16) if r.rand() < 0.8 else r.randint(14, 24)
        parede = np.array([(226, 218, 204), (240, 236, 226), (206, 196, 176), (196, 200, 204)][r.randint(4)], np.float32)
        tel = np.array([(170, 92, 62), (150, 78, 52), (140, 140, 136)][r.randint(3)], np.float32)
        topo = H - alt
        for xx in range(x, x + w):
            for y in range(topo, H):
                c = parede * (0.8 if xx == x + w - 1 else 1.0)
                if (y - topo) % 3 == 1 and (xx - x) % 2 == 1 and alt > 9: c = c * 0.62
                img[y, xx % W, :3] = c; img[y, xx % W, 3] = 255
            if alt < 14:                           # telhado de duas águas
                meio = x + w / 2.0
                hh = int(1 + w / 3)
                for y in range(topo - hh, topo + 1):
                    meia = (y - (topo - hh)) / max(hh, 1) * (w / 2.0 + 0.5)
                    if abs(xx + 0.5 - meio) <= meia:
                        img[y, xx % W, :3] = tel * (0.85 if xx + 0.5 > meio else 1.0); img[y, xx % W, 3] = 255
        if alt >= 14 and r.rand() < 0.5:          # caixa d'água
            cx = x + r.randint(0, max(1, w - 2))
            img[topo - 2:topo, cx % W:(cx + 2) % W or None, :3] = 150; img[topo - 2:topo, cx % W:(cx + 2) % W or None, 3] = 255
        x += w + r.randint(0, 3)
    for k in range(260):                            # linha de árvores (frente)
        cx, cr = r.randint(0, W), 2 + r.rand() * 3.5
        cy = H - 2 - r.rand() * 6
        g = np.array((70, 108, 60)) * (0.8 + 0.4 * r.rand())
        yy, xx = np.mgrid[0:H, 0:W]
        dx = np.minimum(np.abs(xx - cx), W - np.abs(xx - cx))
        m = dx ** 2 + ((yy - cy) * 1.2) ** 2 <= cr * cr
        luz = np.clip(1.15 - (yy - (cy - cr)) / (2 * cr) * 0.4, 0.7, 1.15)
        img[m, :3] = (g * luz[m][:, None]); img[m, 3] = 255
    img[H - 3:, :, :3] = np.where(img[H - 3:, :, 3:4] > 0, img[H - 3:, :, :3], (80, 100, 70)); img[H - 3:, :, 3] = 255
    return img


def borda():
    """borda grama/pista (64x32, repete em u): terra e areia na beirada, tufos de
    grama avançando, sem linha reta. v=0 lado da pista, v=1 lado da grama"""
    W, H = 64, 32
    r = np.random.RandomState(73)
    img = np.zeros((H, W, 4), np.float32)
    n = ruido(64, [16, 8, 4], [0.5, 0.3, 0.2], 74)[:H, :W]
    yy = np.arange(H)[:, None] / H
    terra = np.clip(1 - np.abs(yy - 0.45) / 0.35 + (n - 0.5) * 1.2, 0, 1)
    cor_t = np.array((150, 128, 96)) * (0.85 + 0.3 * n[..., None])
    img[..., :3] = cor_t; img[..., 3] = terra * 210
    for _ in range(90):                              # tufos de grama
        x, y = r.randint(0, W), int(H * (0.35 + 0.6 * r.rand()))
        g = np.array((84, 128, 60)) * (0.8 + 0.4 * r.rand())
        for k in range(r.randint(2, 6)):
            yy2 = y - k
            if 0 <= yy2 < H:
                img[yy2, (x + r.randint(-1, 2)) % W, :3] = g; img[yy2, (x + r.randint(-1, 2)) % W, 3] = 255
    img[:, :, 3] *= np.clip(yy * 6, 0, 1)            # some no lado da pista
    return img


def grelha():
    """grelha de drenagem (32x16): moldura de concreto e barras escuras"""
    W, H = 32, 16
    img = np.zeros((H, W, 4), np.float32)
    img[..., :3] = 170; img[..., 3] = 255
    img[2:14, 2:30, :3] = 40
    for x in range(3, 30, 3):
        img[2:14, x, :3] = 110
    return img


def bairros_luz():
    """luzes dos bairros no entardecer (512x32): janelas e postes; e o trânsito
    (256x8): faróis brancos numa mão, lanternas vermelhas na outra"""
    r = np.random.RandomState(58)
    L = np.zeros((32, 512, 3), np.float32)
    for _ in range(700):
        x, y = r.randint(0, 512), r.randint(8, 30)
        L[y, x] = np.array((255, 200, 120)) * (0.4 + 0.6 * r.rand())
    for x in range(0, 512, 7):                       # postes da avenida
        L[29, x] = (255, 170, 80)
    T = np.zeros((8, 256, 3), np.float32)
    for _ in range(46):
        x = r.randint(0, 255); T[2, x] = (255, 250, 230); T[2, x + 1] = (200, 200, 190)
    for _ in range(46):
        x = r.randint(0, 255); T[5, x] = (255, 40, 30); T[5, x + 1] = (180, 20, 20)
    return L, T


def brita(N=128):
    """brita: pedrinhas de 2-5 px, cada uma com luz em cima e sombra embaixo"""
    r = np.random.RandomState(91)
    v = np.full((N, N), 150.0) + (r.rand(N, N) - 0.5) * 20
    for _ in range(2600 * (N // 128) ** 2):
        cx, cy, rr = r.rand() * N, r.rand() * N, 1.2 + r.rand() * 1.8
        base = 175 + 60 * r.rand()
        y0, y1, x0, x1 = int(cy - rr - 1), int(cy + rr + 2), int(cx - rr - 1), int(cx + rr + 2)
        for y in range(y0, y1):
            for x in range(x0, x1):
                d = ((x - cx) ** 2 + (y - cy) ** 2) / (rr * rr)
                if d <= 1:
                    luz = 1.0 + 0.25 * (cy - y) / rr - 0.1 * d
                    v[y % N, x % N] = base * luz
        v[int(cy + rr + 1) % N, int(cx) % N] *= 0.6                         # sombrinha
    return cinza(v)


def faixas():
    """4 faixas de torcida (128x16 cada, num atlas 128x64), pano com dobras e
    franja embaixo; textos genéricos de torcida"""
    from PIL import Image, ImageDraw, ImageFont
    W, H = 128, 16
    CORES = [((24, 150, 60), (250, 220, 30), "VAI BRASIL!"), ((250, 250, 250), (30, 70, 190), "INTERLAGOS"),
             ((250, 215, 30), (20, 130, 50), "FORCA!"), ((200, 30, 40), (255, 255, 255), "OLE OLE OLE")]
    out = np.zeros((64, W, 4), np.float32)
    for k, (fundo, letra, txt) in enumerate(CORES):
        im = Image.new("RGB", (W * 2, H * 2), fundo)
        d = ImageDraw.Draw(im)
        f = ImageFont.load_default()
        tw = d.textlength(txt, font=f)
        pequeno = Image.new("RGB", (int(tw) + 4, 12), fundo)
        ImageDraw.Draw(pequeno).text((2, 0), txt, fill=letra, font=f)
        esc = min((W * 2 - 16) / pequeno.width, (H * 2 - 8) / pequeno.height)
        g = pequeno.resize((int(pequeno.width * esc), int(pequeno.height * esc)), Image.NEAREST)
        im.paste(g, ((W * 2 - g.width) // 2, (H * 2 - g.height) // 2 - 1))
        a = np.asarray(im.resize((W, H), Image.BILINEAR), np.float32)
        xx = np.arange(W)[None, :]
        a = a * (0.86 + 0.14 * np.sin(xx * 0.35 + k))[..., None]          # dobras do pano
        al = np.full((H, W), 255.0)
        al[-2:, :] = np.where((xx % 3 == 0), 255, 0)                       # franja
        al[:, :1] = al[:, -1:] = 255
        out[k * H:(k + 1) * H, :, :3] = a; out[k * H:(k + 1) * H, :, 3] = al
    return out


def nuvens_mov():
    """camada de nuvens que ANDA no céu (256x64, repete em x): cúmulos baixos
    e esgarçados, brancos em cima e acinzentados embaixo, borda macia"""
    W, H = 256, 64
    r = np.random.RandomState(41)
    def fbm_x(semente):                                   # ruído periódico em x
        out = np.zeros((H, W))
        rr = np.random.RandomState(semente)
        for c, a in ((64, 0.5), (32, 0.25), (16, 0.15), (8, 0.1)):
            gx, gy = W // c, H // c + 2
            L = rr.rand(gy, gx)
            y = np.arange(H) / c; x = np.arange(W) / c
            iy = np.floor(y).astype(int); ix = np.floor(x).astype(int)
            fy = (y - iy)[:, None]; fx = (x - ix)[None, :]
            fy = fy * fy * (3 - 2 * fy); fx = fx * fx * (3 - 2 * fx)
            a00 = L[np.ix_(iy, ix % gx)]; a01 = L[np.ix_(iy, (ix + 1) % gx)]
            a10 = L[np.ix_(iy + 1, ix % gx)]; a11 = L[np.ix_(iy + 1, (ix + 1) % gx)]
            out += a * ((a00 * (1 - fx) + a01 * fx) * (1 - fy) + (a10 * (1 - fx) + a11 * fx) * fy)
        return out
    n = fbm_x(5)
    grupos = fbm_x(9)                                                     # onde há nuvem (nuvens separadas)
    yy = np.arange(H)[:, None] / H
    perfil = np.clip(1 - np.abs(yy - 0.55) / 0.40, 0, 1) ** 0.7
    d = np.clip((n * perfil * np.clip((grupos - 0.33) * 2.6, 0, 1.2) - 0.10) / 0.12, 0, 1)
    acima = np.roll(d, 3, 0); acima[:3] = 0
    luz = np.clip(0.90 + (d - acima) * 0.8 - 0.14 * yy, 0.72, 1.0)       # topo claro, base levemente cinza
    img = np.zeros((H, W, 4), np.float32)
    img[..., 0] = img[..., 1] = img[..., 2] = 255 * luz
    img[..., 2] = np.minimum(255, img[..., 2] * 1.04)
    img[..., 3] = 255 * d * 0.85
    return img


def twid_tab(n):
    ys, xs = np.mgrid[0:n, 0:n]
    a = np.zeros((n, n), np.int64)
    for b in range(10):
        a |= ((ys >> b) & 1) << (2 * b) | ((xs >> b) & 1) << (2 * b + 1)
    return a


def relevo_asfalto(img):
    """mapa de RELEVO do PowerVR (256x256, 16 bits por texel: S = inclinação
    da normal, 255 = plano; R = direção) tirado do próprio asfalto: escuro =
    baixo (rachaduras, juntas de remendo, pedrinhas soltas)"""
    h = img[..., 0].astype(np.float32) / 255.0
    for _ in range(1):
        h = (h + np.roll(h, 1, 0) + np.roll(h, -1, 0) + np.roll(h, 1, 1) + np.roll(h, -1, 1)) / 5
    gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5
    gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5
    K = 9.0                                                   # exagero do relevo
    nx, ny, nz = -gx * K, -gy * K, np.ones_like(h)
    l = np.sqrt(nx * nx + ny * ny + nz * nz)
    nx, ny, nz = nx / l, ny / l, nz / l
    S = np.clip(np.arcsin(np.clip(nz, -1, 1)) / (np.pi / 2) * 255, 0, 255).astype(np.uint16)
    R = ((np.arctan2(ny, nx) % (2 * np.pi)) / (2 * np.pi) * 256).astype(np.uint16) & 255
    px = ((S << 8) | R).astype('<u2')
    out = np.zeros(256 * 256, '<u2')
    out[twid_tab(256).ravel()] = px.ravel()
    return out.tobytes()


def cadeia_pal(img, q):
    from PIL import Image
    niveis = []
    a = img.astype(np.float32); n = a.shape[0]
    while True:
        niveis.append(a)
        if n == 1: break
        a = a.reshape(n // 2, 2, n // 2, 2, 3).mean((1, 3)); n //= 2
        if n <= 128: a = (np.roll(a, 1, 1) + 2 * a + np.roll(a, -1, 1)) / 4
    total = 3 + sum(l.shape[0] ** 2 for l in niveis)
    buf = np.zeros((total + 31) & ~31, np.uint8)
    off = total
    for l in niveis:
        m = l.shape[0]; off -= m * m
        idx = np.asarray(Image.fromarray(np.clip(l, 0, 255).astype(np.uint8)).quantize(palette=q, dither=Image.Dither.NONE), np.uint8)
        buf[off + twid_tab(m).ravel()] = idx.ravel()
    return buf.tobytes()


def textura_vq_mip(img):
    """VQ (compressão do PowerVR) COM MIPMAPS: codebook de 256 blocos 2x2 e
    um índice por bloco em cada nível; o nível 1x1 ocupa 1 índice no início"""
    a = img.astype(np.float32); n = a.shape[0]
    niveis = []
    while True:
        niveis.append(a)
        if n == 1: break
        a = a.reshape(n // 2, 2, n // 2, 2, 3).mean((1, 3)); n //= 2
    blocos = []
    for l in niveis:
        m = l.shape[0]
        if m == 1: b = np.repeat(l.reshape(1, 3), 4, 0).reshape(1, 12)
        else: b = l.reshape(m // 2, 2, m // 2, 2, 3).transpose(0, 2, 1, 3, 4).reshape(-1, 12)
        blocos.append(b)
    X = np.concatenate(blocos)
    rsq = np.random.RandomState(3)
    C = X[rsq.choice(len(X), 256, replace=False)].copy()
    amostra = X[rsq.choice(len(X), min(len(X), 40000), replace=False)]
    for _ in range(12):
        d = (amostra * amostra).sum(1)[:, None] - 2 * amostra @ C.T + (C * C).sum(1)[None, :]
        lab = d.argmin(1)
        for k in range(256):
            m = lab == k
            if m.any(): C[k] = amostra[m].mean(0)
            else: C[k] = amostra[rsq.randint(len(amostra))]
    def idx_de(b):
        d = (b * b).sum(1)[:, None] - 2 * b @ C.T + (C * C).sum(1)[None, :]
        return d.argmin(1).astype(np.uint8)
    Cb = C.reshape(256, 2, 2, 3)
    cb = np.zeros((256, 4), np.uint16)
    for k, (dy, dx) in enumerate([(0, 0), (1, 0), (0, 1), (1, 1)]):
        c = np.clip(np.round(Cb[:, dy, dx]), 0, 255).astype(np.uint16)
        cb[:, k] = ((c[:, 0] >> 3) << 11) | ((c[:, 1] >> 2) << 5) | (c[:, 2] >> 3)
    partes = []
    for l, b in zip(reversed(niveis), reversed(blocos)):      # do 1x1 ao maior
        m = l.shape[0]
        ids = idx_de(b)
        if m == 1: partes.append(ids)
        else:
            q = m // 2
            out = np.zeros(q * q, np.uint8)
            out[twid_tab(q).ravel()] = ids
            partes.append(out)
    dados = cb.astype('<u2').tobytes() + b"".join(p.tobytes() for p in partes)
    return dados + bytes((-len(dados)) % 32)


def torcida_paleta(img, img2=None):
    """torcida em PALETA de 8 bits (1/2 da memória) com a cadeia de mipmaps
    (níveis menores borrados na horizontal, como no jogo) — layout do PowerVR"""
    from PIL import Image
    niveis = []
    a = img.astype(np.float32)
    n = a.shape[0]
    while True:
        niveis.append(a)
        if n == 1:
            break
        a = a.reshape(n // 2, 2, n // 2, 2, 3).mean((1, 3))
        n //= 2
        if n <= 128:
            a = (np.roll(a, 1, 1) + 2 * a + np.roll(a, -1, 1)) / 4
    grande = Image.fromarray(np.clip(niveis[0], 0, 255).astype(np.uint8))
    q = grande.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pl = q.getpalette()[:768]
    pl = pl + [0] * (768 - len(pl))
    pal = np.array(pl, np.uint16).reshape(256, 3)
    pal565 = (((pal[:, 0] >> 3) << 11) | ((pal[:, 1] >> 2) << 5) | (pal[:, 2] >> 3)).astype('<u2')
    if img2 is not None:
        return pal565.tobytes(), cadeia_pal(img, q), cadeia_pal(img2, q)
    total = 3 + sum(l.shape[0] ** 2 for l in niveis)
    buf = np.zeros((total + 31) & ~31, np.uint8)
    off = total
    for l in niveis:                                            # do maior para o menor
        m = l.shape[0]
        off -= m * m
        ql = Image.fromarray(np.clip(l, 0, 255).astype(np.uint8)).quantize(palette=q, dither=Image.Dither.NONE)
        idx = np.asarray(ql, np.uint8)
        buf[off + twid_tab(m).ravel()] = idx.ravel()
    return pal565.tobytes(), buf.tobytes()


def grama(N=256):
    """grama 256 (cinza: a cor vem do vértice): touceiras, folhas finas em
    várias direções, falhas de terra"""
    r = np.random.RandomState(17)
    v = 196 + 26 * (ruido(N, [64, 16, 4], [0.45, 0.35, 0.2], 18) - 0.5)
    for _ in range(9000):                                  # folhas: riscos curtos
        x, y = r.rand() * N, r.rand() * N
        a = r.rand() * np.pi; L = 2 + r.rand() * 4
        d = (r.rand() - 0.45) * 60
        for t in np.linspace(0, 1, int(L * 2)):
            xi, yi = int(x + np.cos(a) * L * t) % N, int(y - np.sin(a) * L * t) % N
            v[yi, xi] += d * (1 - 0.5 * t)
    falha = ruido(N, [32, 8], [0.7, 0.3], 19)
    v = np.where(falha > 0.72, v * 0.86 + 18, v)           # falhas (terra aparecendo)
    return cinza(np.clip(v, 60, 255))


def relevo_de(img, K=9.0, n=None):
    """mapa de relevo do PowerVR a partir da luminância (escuro = baixo)"""
    h = img[..., 0].astype(np.float32) / 255.0
    n = h.shape[0]
    h = (h + np.roll(h, 1, 0) + np.roll(h, -1, 0) + np.roll(h, 1, 1) + np.roll(h, -1, 1)) / 5
    gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5
    gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5
    nx, ny, nz = -gx * K, -gy * K, np.ones_like(h)
    l = np.sqrt(nx * nx + ny * ny + nz * nz)
    S = np.clip(np.arcsin(np.clip(nz / l, -1, 1)) / (np.pi / 2) * 255, 0, 255).astype(np.uint16)
    R = ((np.arctan2(ny / l, nx / l) % (2 * np.pi)) / (2 * np.pi) * 256).astype(np.uint16) & 255
    out = np.zeros(n * n, '<u2')
    out[twid_tab(n).ravel()] = ((S << 8) | R).astype('<u2').ravel()
    return out.tobytes()


def zebra_relevo():
    """zebra 64x64 (u: atravessa a zebra, v: ao longo): 4 lombadas por
    pedaço e a borda chanfrada"""
    N = 64
    yy, xx = np.mgrid[0:N, 0:N] / N
    h = 0.5 + 0.5 * np.cos(yy * 2 * np.pi * 4)              # lombadas ao longo
    h = h * np.clip(np.minimum(xx, 1 - xx) / 0.12, 0, 1)    # chanfro nas bordas
    return relevo_de(cinza(h * 255), K=14.0)


def main():
    previa = sys.argv[1] if len(sys.argv) > 1 else None
    partes, ofs, nomes = [], 0, []
    def junta(nome, dados):
        nonlocal ofs
        nomes.append((nome, ofs)); partes.append(dados); ofs += len(dados)
    def mostra(nome, img):
        if previa:
            from PIL import Image
            m = 'RGBA' if img.shape[2] == 4 else 'RGB'
            Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), m).save(os.path.join(previa, f"tex_{nome}.png"))
    for nome, f in (("asfalto", asfalto), ("concreto", concreto), ("torcida", torcida)):
        img = f(); junta(nome, para565(img).tobytes()); mostra(nome, img)
        if nome == "torcida": img_tor = img
    a512 = asfalto(512)
    junta("asfalto_vq", textura_vq_mip(a512)); mostra("asfalto512", a512)
    img_asf = a512.reshape(256, 2, 256, 2, 3).mean((1, 3))
    junta("relevo_asfalto", relevo_asfalto(img_asf))
    pal, idx, idx2 = torcida_paleta(img_tor, torcida(1))
    junta("torcida_pal", pal); junta("torcida_idx", idx); junta("torcida_idx2", idx2)
    mostra("torcida_q1", torcida(1))
    r = np.random.RandomState(5)
    det = 128 + 34 * (ruido(64, [8, 4, 2, 1], [0.15, 0.25, 0.3, 0.3], 61) - 0.5) * 2 + (r.rand(64, 64) - 0.5) * 30
    det = det - det.mean() + 128
    junta("detalhe", para565(cinza(det)).tobytes()); mostra("detalhe", cinza(det))
    cid, luz = cidade()
    junta("cidade", para4444(cid).tobytes()); mostra("cidade", cid)
    junta("cidade_luz", para565(luz).tobytes()); mostra("cidade_luz", luz)
    for nome, f, fmt in (("ipe", ipe, 4), ("marcas", marcas, 4), ("roda", roda, 5), ("sombra_carro", sombra_carro, 4), ("nuvem", nuvem, 4)):
        img = f()
        junta(nome, (para4444(img) if fmt == 4 else para565(img)).tobytes()); mostra(nome, img)
    b = bairros()
    junta("bairros", para4444(b).tobytes()); mostra("bairros", b)
    for nome, f in (("borda", borda), ("grelha", grelha)):
        im = f(); junta(nome, para4444(im).tobytes()); mostra(nome, im)
    bl, tr = bairros_luz()
    junta("bairros_luz", para565(bl).tobytes()); mostra("bairros_luz", bl)
    junta("transito", para565(tr).tobytes()); mostra("transito", tr)
    bt = brita(); junta("brita", para565(bt).tobytes()); mostra("brita", bt)
    # VQ + mipmap em 256: grama, brita e concreto (o dobro da resolução, ~24 KB cada)
    g256 = grama(256); junta("grama_vq", textura_vq_mip(g256)); mostra("grama256", g256)
    b256 = brita(256); junta("brita_vq", textura_vq_mip(b256)); mostra("brita256", b256)
    c256 = concreto(256); junta("concreto_vq", textura_vq_mip(c256)); mostra("concreto256", c256)
    junta("relevo_concreto", relevo_de(c256.reshape(128, 2, 128, 2, 3).mean((1, 3)), K=7.0))
    junta("relevo_zebra", zebra_relevo())
    junta("fim_vq", bytes(32))
    fx = faixas(); junta("faixas", para4444(fx).tobytes()); mostra("faixas", fx)
    nm = nuvens_mov(); junta("nuvens_mov", para4444(nm).tobytes()); mostra("nuvens_mov", nm)
    fs, lj = fachadas()
    com_foto = 0
    if os.environ.get("FOTOS", "0") == "1":       # fachadas das FOTOS do usuário (FOTOS=0 desliga)
        try:
            import fotos
            for t in range(len(fs)):
                ff = fotos.fachada(t)
                if ff is not None:
                    fs[t] = ff; com_foto = 1
        except Exception as e:
            print("fachadas de foto:", e)
    for t, img in enumerate(fs):
        junta(f"fachada{t}", para565(img).tobytes()); mostra(f"fachada{t}", img)
    junta("janela_luz", para565(lj).tobytes()); mostra("janela_luz", lj)
    sys.path.insert(0, AQUI); import volante      # arte do cockpit (volante.py)
    import cockpit_arte                           # cockpit v2 (cockpit_arte.py), nas duas pinturas
    tdados, timgs = cockpit_arte.tiles_cockpit()   # texturas repetidas 128x128 do cockpit 3D
    junta("ck_tiles", b"".join(tdados))
    for k, ti in enumerate(timgs): mostra(f"ck_tile{k}", np.dstack([ti, ti, ti]))
    fdados, fimg, fa = cockpit_arte.face_1555()   # face do volante do cockpit 3D (sem compressão)
    junta("volante_face", fdados + bytes((-len(fdados)) % 32)); mostra("volante_face", np.dstack([fimg, fa * 255.0]))
    for pint, suf in (("normal", ""), ("sf3", "_sf3")):
        (vc, va), (cc, ca) = cockpit_arte.montar(pint)
        vimg, v1 = cockpit_arte.fecha(vc, va)
        cimg, c1 = cockpit_arte.fecha(cc, ca)
        junta("volante" + suf, volante.vq_1555(vimg, v1)[0]); mostra("volante" + suf, np.dstack([vimg, v1 * 255.0]))
        junta("cockpit" + suf, volante.vq_1555(cimg, c1)[0]); mostra("cockpit" + suf, np.dstack([cimg, c1 * 255.0]))
    tn = teto_nuvens(); junta("teto_nuvens", para4444(tn).tobytes()); mostra("teto_nuvens", tn)
    if os.path.isfile(os.path.join(AQUI, "modelo_obj", "Car.obj")):   # modelo .obj de terceiros (opcional, fora do git)
        import conv_obj                           # pinturas do carro .obj, VQ com mipmap
        for k, d in enumerate(conv_obj.obj_texturas(textura_vq_mip)):
            junta(f"obj_tex{k}", d)
    open(os.path.join(AQUI, "texturas.bin"), "wb").write(b"".join(partes))
    with open(os.path.join(AQUI, "texturas.h"), "w", encoding="utf-8") as f:
        f.write("/* gerado por gerar_texturas.py: posição de cada textura em texturas.bin */\n")
        for nome, o in nomes:
            f.write(f"#define TEXB_{nome.upper()} {o}\n")
        f.write(f"#define FACH_FOTO {com_foto}\n")
    print("texturas.bin:", ofs, "bytes")


if __name__ == "__main__":
    main()
