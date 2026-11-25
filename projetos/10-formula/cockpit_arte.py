"""ARTE DO COCKPIT (versão 2, inspirada em fotos reais de F1 1998-2000 e em
capturas de simuladores; sem nomes nem logos).

Tudo é pintado em coordenadas da TELA (640x480) numa "janela" com escala
própria, iluminado por relevo (máscara borrada -> normais -> sombra/brilho)
e depois reduzido para a região da textura. Duas texturas 512x512 VQ
(ARGB1555, lista de recorte):

  VOLANTE (gira com o esterço): volante + luvas, janela da tela
      x 170..470, y 344..504 (centro do volante em 320,424) -> 512x512

  COCKPIT (fixa):
      cols   0..255 rows   0..255  lateral ESQUERDA da carroceria (x 0..235, y 330..480)
      cols 256..511 rows   0..127  retrovisor esquerdo + haste    (x 10..150, y 280..360)
      cols 256..511 rows 128..255  manga do macacão (u = punho->cotovelo, v = fora->dentro)
      cols   0..511 rows 256..511  vão do cockpit + borda da frente (x 150..490, y 320..480)

Gerado nas duas pinturas (original e "SF3").
"""
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage as nd

L = np.array([-0.45, -0.75, 0.55]); L /= np.linalg.norm(L)
HV = L + np.array([0, 0, 1.0]); HV /= np.linalg.norm(HV)
FONTE = "C:/Windows/Fonts/arialbd.ttf"

PINTURAS = {
    "normal": dict(princ=0x0E5C8C, sec=0xF2C21B, luva=0x1E4F86, luva_det=0xE6E8EC, punho=0xF2C21B),
    "sf3":    dict(princ=0xECEEF0, sec=0x1A2550, luva=0x2256C8, luva_det=0xF2F2F2, punho=0x1A2550),
}

# ---------------- janela de desenho ----------------
OX = OY = 0.0
S = 2.0
W = H = 0
X = Y = None


def janela(ox, oy, w, h, s):
    """define a área da tela que vai ser pintada e a escala (pixels por pixel da tela)"""
    global OX, OY, S, W, H, X, Y
    OX, OY, S = ox, oy, s
    W, H = int(round(w * s)), int(round(h * s))
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    X, Y = OX + xs / S, OY + ys / S


def rgb(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], np.float32)


def mascara(f):
    img = Image.new('L', (W, H), 0)
    f(ImageDraw.Draw(img))
    return np.asarray(img, np.float32) / 255.0


def _p(x, y):
    return ((x - OX) * S, (y - OY) * S)


def poli(pts):
    return mascara(lambda d: d.polygon([_p(x, y) for x, y in pts], fill=255))


def elipse(cx, cy, rx, ry=None):
    ry = rx if ry is None else ry
    a, b = _p(cx - rx, cy - ry), _p(cx + rx, cy + ry)
    return mascara(lambda d: d.ellipse([a, b], fill=255))


def rret(x0, y0, x1, y1, r):
    x0, x1 = min(x0, x1), max(x0, x1)
    y0, y1 = min(y0, y1), max(y0, y1)
    a, b = _p(x0, y0), _p(x1, y1)
    return mascara(lambda d: d.rounded_rectangle([a, b], radius=r * S, fill=255))


def suave(m, r=1.0):
    return np.clip((nd.gaussian_filter(m, r * S) - 0.5) * 3 + 0.5, 0, 1)


def capsula(x0, y0, x1, y1, r0, r1):
    pts = []
    dx, dy = x1 - x0, y1 - y0
    dl = np.hypot(dx, dy); nx, ny = -dy / dl, dx / dl
    n = 20
    for q in range(n + 1):
        t = q / n; r = r0 + (r1 - r0) * t
        pts.append((x0 + dx * t + nx * r, y0 + dy * t + ny * r))
    for q in range(n + 1):
        a = np.pi * q / n
        pts.append((x1 + (nx * np.cos(a) + dx / dl * np.sin(a)) * r1, y1 + (ny * np.cos(a) + dy / dl * np.sin(a)) * r1))
    for q in range(n, -1, -1):
        t = q / n; r = r0 + (r1 - r0) * t
        pts.append((x0 + dx * t - nx * r, y0 + dy * t - ny * r))
    for q in range(n + 1):
        a = np.pi * q / n
        pts.append((x0 - (nx * np.cos(a) + dx / dl * np.sin(a)) * r0, y0 - (ny * np.cos(a) + dy / dl * np.sin(a)) * r0))
    return poli(pts)


def luz(m, raio, forca=40.0, amb=0.5, dif=0.6, spec=0.0, exp=24, extra=None):
    alt = nd.gaussian_filter(m, raio * S)
    if extra is not None:
        alt = alt + extra
    gy, gx = np.gradient(alt)
    k = forca * 2.0 / S                         # mesma "força" do relevo em qualquer escala
    n = np.dstack([-gx * k, -gy * k, np.ones_like(alt)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return amb + dif * np.clip(n @ L, 0, 1), spec * np.clip(n @ HV, 0, 1) ** exp


def ruido(sig, sem):
    r = np.random.RandomState(sem).rand(H, W).astype(np.float32)
    r = nd.gaussian_filter(r, np.array(sig) * S / 2.0)
    return (r - r.mean()) / (r.std() + 1e-6)


class Tela:
    def __init__(self):
        self.rgb = np.zeros((H, W, 3), np.float32)
        self.a = np.zeros((H, W), np.float32)

    def pinta(self, m, cor, sh=1.0, sp=0.0):
        cor = np.asarray(cor, np.float32)
        if cor.ndim == 1:
            cor = cor[None, None, :]
        sh = sh if np.ndim(sh) == 0 else sh[..., None]
        sp = sp if np.ndim(sp) == 0 else sp[..., None]
        c = cor * sh + 255.0 * sp
        mm = m[..., None]
        self.rgb = self.rgb * (1 - mm) + c * mm
        self.a = self.a + m * (1 - self.a)

    def escurece(self, m, k):
        self.rgb *= (1 - m * k)[..., None]

    def saida(self, w, h):
        """cor (sem pré-multiplicar) e alfa, reduzidos para w x h"""
        a = np.clip(self.a, 0, 1)
        c = np.where(a[..., None] > 0.002, self.rgb / np.maximum(a[..., None], 0.002), 0)
        ci = Image.fromarray(np.clip(c, 0, 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)
        ai = Image.fromarray((a * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)
        return np.asarray(ci, np.float32), np.asarray(ai, np.float32) / 255.0


def sombra(t, m, dx=1.0, dy=2.0, r=2.0, k=0.5):
    sh = nd.shift(nd.gaussian_filter(m, r * S), (dy * S, dx * S), order=1)
    t.escurece(np.clip(sh, 0, 1), k)


def texto(t, txt, cx, cy, tam, cor, alfa=1.0):
    f = ImageFont.truetype(FONTE, max(4, int(tam * S)))
    img = Image.new('L', (W, H), 0)
    ImageDraw.Draw(img).text(_p(cx, cy), txt, font=f, fill=255, anchor='mm')
    t.pinta(np.asarray(img, np.float32) / 255.0 * alfa, rgb(cor))


def carbono(c=1.6):
    i, j = np.floor(X / c), np.floor(Y / c)
    hor = ((i + j) % 2) < 1
    fio = np.where(hor, np.sin(np.pi * ((Y / c) % 1)), np.sin(np.pi * ((X / c) % 1))) ** 0.7
    tr = 20 + 17 * fio * np.where(hor, 1.0, 0.72)
    return np.dstack([tr * 0.95, tr * 0.97, tr * 1.10])


# ======================================================================
# VOLANTE + LUVAS (centro em 320,424)
# ======================================================================
def volante(pt, luvas=True, saida=(512, 512)):
    janela(170, 344, 300, 160, 3.2)
    t = Tela()
    cxv = 320.0
    # borboletas de câmbio (alumínio) atrás
    for l in (-1, 1):
        b = suave(poli([(cxv + l * 68, 362), (cxv + l * 114, 352), (cxv + l * 122, 386), (cxv + l * 80, 394)]), 0.8)
        sombra(t, b, 1.0, 1.5, 1.5, 0.4)
        t.pinta(b, rgb(0x707580) * (1 + 0.08 * ruido((0.5, 8), 4))[..., None], *luz(b, 1.6, spec=0.7))
        texto(t, "+" if l > 0 else "-", cxv + l * 104, 362, 8, 0x1A1A1E, 0.8)
    # contorno: placa + manoplas, com os aros vazados
    furos = [suave(elipse(cxv + l * 80, 404, 15, 23), 1.0) for l in (-1, 1)]
    corpo = suave(rret(cxv - 124, 352, cxv + 124, 504, 26), 1.0)
    for f in furos:
        corpo = np.clip(corpo - f, 0, 1)
    sombra(t, corpo, 2.0, 3.0, 4.0, 0.6)
    # manoplas de camurça (com pesponto e textura de camurça)
    for l in (-1, 1):
        x0, x1 = sorted((cxv + l * 92, cxv + l * 126))
        mp = suave(rret(x0, 352, x1, 504, 14), 1.0)
        cil = np.clip(1 - ((X - (cxv + l * 109)) / 18) ** 2, 0, 1) ** 0.5
        cam = rgb(0x2E2C2C) * (1 + 0.14 * ruido(0.4, 11 + l) + 0.05 * ruido(3.0, 21 + l))[..., None]
        sh, sp = luz(mp, 4.0, forca=50, amb=0.5, dif=0.5, spec=0.10, exp=8)
        t.pinta(mp, cam, sh * (0.55 + 0.6 * cil), sp)
        pesp = mp * (np.abs(X - (cxv + l * 96)) < 0.35) * ((np.floor(Y / 1.6) % 2) == 0)
        t.escurece(pesp.astype(np.float32), 0.35)
    # placa de carbono central (com bisel e verniz)
    placa = suave(rret(cxv - 94, 356, cxv + 94, 504, 16), 1.0)
    for f in furos:
        placa = np.clip(placa - f, 0, 1)
    sh, sp = luz(placa, 4.0, forca=60, amb=0.75, dif=0.4, spec=0.85, exp=40)
    t.pinta(placa, carbono(1.3), sh, sp + 0.08 * np.clip((396 - Y) / 40, 0, 1))
    for l in (-1, 1):
        anel = np.clip(suave(elipse(cxv + l * 80, 404, 18, 26), 1.0) - furos[(l + 1) // 2], 0, 1)
        t.escurece(anel, 0.5)
    # faixa dos LEDs (o jogo acende por cima): lentes apagadas
    fx = suave(rret(cxv - 58, 366, cxv + 58, 380, 4), 0.6)
    sombra(t, fx, 0.6, 1.0, 1.0, 0.5)
    t.pinta(fx, rgb(0x0C0C0E), *luz(fx, 1.5, spec=0.7))
    cores_led = [0x1A4A22] * 3 + [0x4A1A16] * 4 + [0x1A244A] * 3
    for k in range(10):
        led = elipse(cxv - 50 + k * 11.1, 373, 3.8)
        t.pinta(led, rgb(cores_led[k]), *luz(led, 1.2, forca=80, spec=0.9))
    # visor (os números são desenhados pelo jogo)
    vis = suave(rret(cxv - 39, 385, cxv + 39, 421, 4), 0.6)
    sombra(t, vis, 1.0, 1.5, 1.5, 0.5)
    t.pinta(vis, rgb(0x0A0A0C), *luz(vis, 2.0, spec=0.7))
    tela = rret(cxv - 34, 389, cxv + 34, 417, 2)
    g = np.clip((Y - 389) / 28, 0, 1)
    cen = np.exp(-(((X - cxv) / 40) ** 2 + ((Y - 403) / 18) ** 2))
    t.pinta(tela, np.dstack([116 - 14 * g + 10 * cen, 142 - 14 * g + 10 * cen, 104 - 14 * g + 8 * cen]))
    t.escurece(tela * np.exp(-((Y - 389) / 1.5) ** 2), 0.4)
    t.pinta(tela * np.clip(1 - np.abs((X - cxv) - 0.9 * (Y - 403) - 10) / 5, 0, 1) * 0.14, rgb(0xFFFFFF))
    # botões (tampa colorida) com etiquetas brancas pequenas
    BOT = [(-62, 392, 0x2BB0A8, "N"), (-62, 418, 0xE03020, "R"), (62, 392, 0xF0A020, "RAD"), (62, 418, 0xE8E8E8, "PIT"),
           (-46, 438, 0x2AA040, "OK"), (46, 438, 0x3A70FF, "BEB"), (0, 432, 0xD8D8D8, "LIM")]
    for bx, by, cor, et in BOT:
        anel = elipse(cxv + bx, by, 7.4 if bx else 5.6)
        sombra(t, anel, 0.8, 1.2, 1.2, 0.5)
        t.pinta(anel, rgb(0x101012), *luz(anel, 1.0, spec=0.5))
        dome = elipse(cxv + bx, by, 5.4 if bx else 4.0)
        t.pinta(dome, rgb(cor), *luz(dome, 2.6, forca=90, amb=0.45, dif=0.75, spec=1.0, exp=28))
        if bx == 0:
            texto(t, et, cxv, by + 9, 4.2, 0xEDEDED, 0.95)
        else:
            texto(t, et, cxv + bx + (11 if bx < 0 else -11) * (1 if len(et) == 1 else 1.45), by, 4.8, 0xEDEDED, 0.95)
    # seletores com anel colorido e números
    SEL = [(-34, 462, 0x2BB0A8, "MIX"), (0, 472, 0xE8C020, "DIF"), (34, 462, 0xE03020, "FR")]
    for sx, sy, cor, et in SEL:
        anel = elipse(cxv + sx, sy, 12)
        sombra(t, anel, 1.0, 1.6, 1.6, 0.5)
        ang = np.arctan2(Y - sy, X - (cxv + sx))
        t.pinta(anel, rgb(cor) * (0.92 + 0.08 * (np.cos(ang - 0.6) > 0.15))[..., None], *luz(anel, 1.5, spec=0.4))
        for k in range(6):
            a = np.radians(-200 + k * 44)
            texto(t, str(k + 1), cxv + sx + np.cos(a) * 9.3, sy + np.sin(a) * 9.3, 3.4, 0x111111, 0.9)
        cen = elipse(cxv + sx, sy, 6.4)
        t.pinta(cen, rgb(0x1A1B1E), *luz(cen, 2.0, forca=70, spec=0.6))
        alav = capsula(cxv + sx, sy, cxv + sx + 4, sy - 8, 2.0, 1.6)
        t.pinta(alav, rgb(0xE8E8E8) * 0.6 + rgb(cor) * 0.4, *luz(alav, 1.0, spec=0.7))
        texto(t, et, cxv + sx, sy + 16.5, 3.8, 0xE0E0E0, 0.85)
    for bx, by in ((-86, 362), (86, 362), (-86, 496), (86, 496), (-24, 362), (24, 362)):
        p = elipse(cxv + bx, by, 2.0)
        t.pinta(p, rgb(0x9A9EA6), *luz(p, 0.8, forca=90, spec=0.8))
        t.pinta(rret(cxv + bx - 1.3, by - 0.3, cxv + bx + 1.3, by + 0.3, 0.1), rgb(0x303236))
    # ---------------- LUVAS segurando o topo das manoplas ----------------
    luva, det = pt["luva"], pt["luva_det"]
    for l in ((-1, 1) if luvas else ()):
        gx = cxv + l * 109
        mao = suave(poli([(gx + l * 31, 380), (gx + l * 25, 360), (gx + l * 4, 351), (gx - l * 14, 357), (gx - l * 21, 376),
                          (gx - l * 19, 420), (gx - l * 6, 442), (gx + l * 18, 444), (gx + l * 31, 422)]), 3.0)
        sombra(t, mao, l * 1.5, 3.0, 3.5, 0.6)
        tecl = (1 + 0.06 * ruido(0.4, 41) + 0.04 * ruido(2.5, 42))
        sh, sp = luz(mao, 9.0, forca=55, amb=0.36, dif=0.82, spec=0.22, exp=10)
        t.pinta(mao, rgb(luva) * tecl[..., None], sh, sp)
        # painel acolchoado claro no dorso, com costura e vincos
        pain = np.zeros_like(mao)
        for (ya, yb) in ((362, 382), (385, 404)):              # dois gomos acolchoados
            gomo = suave(poli([(gx + l * 20, ya + 2), (gx + l * 13, ya), (gx - l * 3, ya + 1), (gx - l * 6, (ya + yb) / 2),
                               (gx - l * 3, yb), (gx + l * 14, yb + 1)]), 1.6)
            sh2, sp2 = luz(gomo, 3.5, forca=60, amb=0.42, dif=0.72, spec=0.28)
            t.pinta(gomo, (rgb(det) * 0.85 + rgb(luva) * 0.15) * tecl[..., None], sh2, sp2)
            pain = np.maximum(pain, gomo)
        cost = np.clip(nd.gaussian_filter(pain, 0.6 * S) - pain, 0, 1) * 2
        t.escurece(cost * ((np.floor((X + Y) / 1.4) % 2) == 0), 0.4)
        for k in range(3):                                          # vincos do dorso
            t.escurece(pain * np.exp(-((X - (gx + l * (2 + k * 6)) - 0.35 * l * (Y - 385)) / 0.6) ** 2) * (Y > 368) * (Y < 402), 0.12)
        # dedos dando a volta na manopla, aparecendo do lado de dentro
        for k in range(4):
            yk = 375 + k * 13.6
            ded = capsula(gx - l * 1, yk, gx - l * 25, yk + 1.5, 6.7, 5.9) * (1 - pain)
            sombra(t, ded, -l * 0.8, 1.8, 1.6, 0.55)
            cil = np.clip(1 - ((Y - yk - 0.8) / 6.7) ** 2, 0, 1) ** 0.5
            sh, sp = luz(ded, 2.4, forca=45, amb=0.4, dif=0.7, spec=0.25)
            t.pinta(ded, rgb(luva) * 1.04 * tecl[..., None], sh * (0.6 + 0.5 * cil), sp)
            ponta = ded * (np.hypot((X - (gx - l * 26)) / 5.2, (Y - yk - 0.8) / 5.6) < 1)
            t.pinta(ponta, rgb(0x8E9096) * (1 + 0.1 * ruido(0.4, 44))[..., None], sh * (0.6 + 0.5 * cil))
            t.escurece(ded * np.exp(-((X - (gx - l * 12)) / 0.8) ** 2), 0.3)
        # polegar por cima, apontando para o centro do volante
        pol = capsula(gx - l * 10, 365, gx - l * 37, 361, 7.0, 5.6)
        sombra(t, pol, 0.8, 2.0, 2.0, 0.55)
        sh, sp = luz(pol, 3.0, forca=45, amb=0.42, dif=0.72, spec=0.3)
        t.pinta(pol, rgb(luva) * tecl[..., None], sh, sp)
        ponta = pol * (np.hypot(X - (gx - l * 39), Y - 361) < 5.2)
        t.pinta(ponta, rgb(0x8E9096), sh)
        t.escurece(pol * np.exp(-((X - (gx - l * 24)) / 0.8) ** 2), 0.3)
        # punho da luva (cor de detalhe) com velcro
        pul = suave(poli([(gx - 23, 433), (gx + 25, 433), (gx + 27, 450), (gx - 25, 450)]), 1.0)
        t.pinta(pul, rgb(pt["punho"]), *luz(pul, 2.0, forca=50, spec=0.2))
        vel = suave(rret(gx - 11, 437, gx + 13, 446, 2), 0.6)
        t.pinta(vel, rgb(0x2A2C30) * (1 + 0.1 * ruido(0.4, 45))[..., None], *luz(vel, 1.0, spec=0.2))
    return t.saida(*saida)


# ======================================================================
# LATERAL DA CARROCERIA (esquerda; a direita é espelhada)
# ======================================================================
def lateral(pt):
    janela(0, 330, 235, 150, 1024 / 235.0 / 2)     # ~2,2 px por pixel
    t = Tela()
    pts = [(0, 392), (70, 378), (140, 360), (196, 344), (214, 340), (204, 380), (190, 430), (180, 480), (0, 480)]
    m = suave(poli(pts), 1.5)
    sh, sp = luz(m, 9.0, forca=90, amb=0.55, dif=0.55, spec=0.9, exp=30)
    base = rgb(pt["princ"])
    escuro = 1.05 - 0.45 * np.clip((Y - 350) / 130, 0, 1)
    t.pinta(m, base[None, None, :] * escuro[..., None], sh, sp)
    # reflexo do céu ao longo da borda de cima (verniz)
    ref = np.clip(m - nd.shift(m, (6 * S, 0), order=0), 0, 1)
    t.pinta(np.clip(nd.gaussian_filter(ref, 1.2 * S) * 0.6, 0, 1), rgb(0xCFE6F5))
    # reflexo comprido e suave (janela do céu) no meio da lateral
    faixa = m * np.exp(-((Y - (395 - 0.18 * X)) / 6.0) ** 2) * 0.18
    t.pinta(faixa, rgb(0xFFFFFF))
    # filete na cor secundária acompanhando a borda
    fil = np.clip(np.clip(m - nd.shift(m, (11 * S, 0), order=0), 0, 1) - np.clip(m - nd.shift(m, (8 * S, 0), order=0), 0, 1), 0, 1)
    t.pinta(fil * 0.95, rgb(pt["sec"]), sh * 0.95 + 0.05)
    # parafusos de fixação da carenagem
    for bx, by in ((40, 410), (110, 392), (160, 378)):
        p = elipse(bx, by, 1.6)
        t.pinta(p, rgb(0x7A7E86), *luz(p, 0.6, forca=90, spec=0.8))
    # acolchoado preto na borda de dentro (camurça) com pesponto
    pad = suave(poli([(214, 340), (226, 343), (214, 400), (200, 480), (184, 480), (196, 400), (204, 380)]), 1.0)
    t.pinta(pad, rgb(0x232427) * (1 + 0.12 * ruido(0.5, 9))[..., None], *luz(pad, 3.0, forca=70, spec=0.12))
    return t.saida(256, 256)


# ======================================================================
# RETROVISOR (esquerdo; o direito é espelhado) - miolo transparente p/ o vidro
# ======================================================================
VIDRO = (30, 296, 106, 321)          # retângulo do vidro na tela (o jogo põe a imagem aqui)


def retrovisor(pt):
    janela(10, 280, 140, 80, 256 / 140.0 * 1.6)
    t = Tela()
    cx = 68.0
    haste = capsula(cx + 36, 326, 146, 356, 3.0, 4.0)
    sombra(t, haste, 1.0, 1.5, 1.5, 0.4)
    t.pinta(haste, rgb(0x2E3138), *luz(haste, 1.2, spec=0.7))
    # carenagem em gota (mais larga e alta do lado de fora), aerodinâmica
    pts = []
    for a in np.linspace(0, 2 * np.pi, 80, endpoint=False):
        rx = 58 - 4 * np.cos(a)                     # mais larga/alta do lado de fora (esquerda)
        ry = 26 - 3 * np.cos(a)
        pts.append((cx + rx * np.cos(a), 309 + ry * np.sin(a)))
    caixa = suave(poli(pts), 1.0)
    sombra(t, caixa, 1.5, 2.0, 2.5, 0.45)
    sh, sp = luz(caixa, 7.0, forca=80, amb=0.55, dif=0.5, spec=0.95, exp=30)
    t.pinta(caixa, rgb(pt["princ"]) * 1.02, sh, sp)
    t.pinta(np.clip(caixa - nd.shift(caixa, (3 * S, 0), order=0), 0, 1) * 0.5, rgb(0xDCEBF6))   # brilho na borda de cima
    junc = caixa * np.exp(-((np.hypot((X - cx) / 54.0, (Y - 309) / 22.5) - 1.0) / 0.012) ** 2)
    t.escurece(junc, 0.35)                                                      # junção da tampa
    p = elipse(cx + 50, 309, 1.6)
    t.pinta(p, rgb(0x7A7E86), *luz(p, 0.6, forca=90, spec=0.8))
    # moldura interna escura (sombra sobre o vidro) e o furo do vidro
    x0, y0, x1, y1 = VIDRO
    moldura = suave(rret(x0 - 4, y0 - 4, x1 + 4, y1 + 4, 9), 0.6)
    t.pinta(moldura, rgb(0x0E0F12), *luz(moldura, 1.5, forca=60, spec=0.4))
    furo = suave(rret(x0, y0, x1, y1, 7), 0.5)
    t.a = np.clip(t.a - furo, 0, 1)
    return t.saida(256, 128)


# ======================================================================
# MANGA DO MACACÃO (u = punho -> cotovelo, v = lado de fora -> de dentro)
# ======================================================================
def manga(pt):
    w, h = 256, 128
    u, v = np.meshgrid(np.linspace(0, 1, w), np.linspace(0, 1, h))
    cil = np.sin(np.pi * np.clip(v, 0.02, 0.98)) ** 0.8
    luzl = 0.30 + 0.80 * cil * (1.10 - 0.40 * v)                          # luz vem de fora/cima; bordas bem escuras
    luzl *= 1 - 0.35 * np.clip((u - 0.6) / 0.4, 0, 1)                      # escurece entrando no cockpit
    dobras = (np.sin(u * 40 + v * 7) * np.exp(-((u - 0.18) / 0.14) ** 2)
              + 0.8 * np.sin(u * 22 - v * 6) * np.exp(-((u - 0.65) / 0.12) ** 2) + 0.25 * np.sin(u * 11 + v * 2))
    r = np.random.RandomState(7).rand(h, w)
    fibra = nd.gaussian_filter(r, (0.8, 2.5)) - 0.5
    tecido = rgb(pt["princ"]) * 0.80 + np.array([200.0, 200.0, 200.0]) * 0.20      # fosco: mais "lavado" que o verniz
    base = tecido[None, None, :] * (luzl * (1 + 0.14 * dobras) * (1 + 0.18 * fibra))[..., None]
    # faixa na cor secundária do lado de fora
    fx = np.exp(-((v - 0.22) / 0.06) ** 2)[..., None]
    sec = rgb(pt["sec"])[None, None, :] * (luzl * (1 + 0.1 * dobras))[..., None]
    img = base * (1 - fx) + sec * fx
    # costuras dos painéis
    for vc in (0.12, 0.33, 0.82):
        img *= (1 - 0.18 * np.exp(-((v - vc) / 0.01) ** 2))[..., None]
    # punho canelado perto da luva
    can = ((u < 0.07) * (0.5 + 0.5 * np.sin(v * 70)))[..., None]
    img = img * (1 - 0.15 * can)
    return np.clip(img, 0, 255).astype(np.float32), np.ones((h, w), np.float32)


# ======================================================================
# VÃO DO COCKPIT + BORDA DA FRENTE (fica atrás de tudo)
# ======================================================================
def vao(pt):
    janela(150, 320, 340, 160, 1.6)
    t = Tela()
    m = poli([(150, 338), (490, 338), (490, 480), (150, 480)])
    grad = np.clip((Y - 338) / 142, 0, 1)
    tom = np.dstack([18 + 12 * (1 - grad), 18 + 12 * (1 - grad), 22 + 14 * (1 - grad)])
    t.pinta(m, tom * (1 + 0.08 * ruido(1.5, 3))[..., None])
    # assoalho de carbono visível no fundo, joelhos/pernas do macacão (sombra)
    for l in (-1, 1):
        joelho = suave(elipse(320 + l * 52, 470, 40, 26), 3.0)
        t.pinta(joelho * 0.85, rgb(pt["princ"]) * 0.35, *luz(joelho, 8.0, forca=40, amb=0.4, dif=0.4))
    # coluna de direção
    col = poli([(306, 342), (334, 342), (340, 430), (300, 430)])
    t.pinta(col, rgb(0x2A2C30), *luz(col, 2.0, spec=0.3))
    # borda da frente do cockpit: lábio acolchoado + painel na cor da carroceria por cima
    painel = suave(poli([(150, 320), (490, 320), (490, 336), (410, 330), (320, 327), (230, 330), (150, 336)]), 1.0)
    t.pinta(painel, rgb(pt["princ"]) * 0.9, *luz(painel, 5.0, forca=60, spec=0.7, exp=30))
    labio = suave(poli([(150, 334), (230, 328), (320, 325), (410, 328), (490, 334), (490, 346), (410, 339), (320, 336), (230, 339), (150, 346)]), 1.0)
    t.pinta(labio, rgb(0x1C1D20) * (1 + 0.1 * ruido(0.5, 5))[..., None], *luz(labio, 2.5, forca=60, spec=0.3))
    return t.saida(512, 256)


def montar(nome):
    pt = PINTURAS[nome]
    vol = volante(pt)
    img = np.zeros((512, 512, 3), np.float32)
    al = np.zeros((512, 512), np.float32)
    c, a = lateral(pt);   img[0:256, 0:256], al[0:256, 0:256] = c, a
    c, a = retrovisor(pt); img[0:128, 256:512], al[0:128, 256:512] = c, a
    c, a = manga(pt);     img[128:256, 256:512], al[128:256, 256:512] = c, a
    c, a = vao(pt);       img[256:512, 0:512], al[256:512, 0:512] = c, a
    return vol, (img, al)


def face_1555():
    """face do volante (sem luvas) para o cockpit 3D: 256x128 ARGB1555 sem compressão, linear"""
    c, a = volante(PINTURAS["normal"], luvas=False, saida=(256, 128))
    c, a1 = fecha(c, a)
    c = np.clip(c, 0, 255).astype(np.uint16)
    v = (a1.astype(np.uint16) << 15) | ((c[..., 0] >> 3) << 10) | ((c[..., 1] >> 3) << 5) | (c[..., 2] >> 3)
    return v.astype('<u2').tobytes(), c, a1


def fecha(img, al):
    a1 = al > 0.5
    if (~a1).any() and a1.any():
        _, (iy, ix) = nd.distance_transform_edt(~a1, return_indices=True)
        img = img[iy, ix]
    return np.clip(img, 0, 255), a1


if __name__ == "__main__":
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import volante as vq
    pasta = sys.argv[1] if len(sys.argv) > 1 else "."
    nome = sys.argv[2] if len(sys.argv) > 2 else "normal"
    (vc, va), (cc, ca) = montar(nome)
    for n, (c, a) in (("cv_volante", (vc, va)), ("cv_cockpit", (cc, ca))):
        img, a1 = fecha(c, a)
        dados, dec = vq.vq_1555(img, a1)
        d = np.clip(dec, 0, 255).astype(np.uint8)
        d[..., 3] = np.where(d[..., 3] > 127, 255, 0)
        Image.fromarray(d, 'RGBA').save(os.path.join(pasta, f"{n}_{nome}.png"))
        print(n, len(dados))


# ======================================================================
# TEXTURAS REPETIDAS DO COCKPIT 3D (128x128, cinza: a cor vem do vértice)
# relevo por mapa de alturas -> luz; emendam sem costura (periódicas)
# ======================================================================
def _ruido_per(n, sig, sem):
    r = np.random.RandomState(sem).rand(n, n).astype(np.float32)
    r = nd.gaussian_filter(r, sig, mode='wrap')
    return (r - r.mean()) / (r.std() + 1e-6)


def _relevo(alt, forca, amb=0.62, dif=0.48):
    gy = (np.roll(alt, -1, 0) - np.roll(alt, 1, 0)) * 0.5
    gx = (np.roll(alt, -1, 1) - np.roll(alt, 1, 1)) * 0.5
    n = np.dstack([-gx * forca, -gy * forca, np.ones_like(alt)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return amb + dif * np.clip(n @ L, 0, 1)


def tiles_cockpit():
    N = 128
    y, x = np.mgrid[0:N, 0:N].astype(np.float32)
    out = []
    # 0 TECIDO: sarja (fios diagonais) + fibra + rugas + costura de painel com pesponto
    c = 4.0
    diag = ((x + y) / c) % 1.0
    fio = np.sin(np.pi * diag) ** 0.8
    trama = np.where(((np.floor(x / c) + np.floor(y / (2 * c))) % 2) < 1, fio, 1 - 0.6 * fio)
    rugas = np.sin(2 * np.pi * (y / N * 2 + 0.08 * np.sin(2 * np.pi * x / N) + 0.35 * _ruido_per(N, 14, 4) / 3)) * 0.5 + 0.5
    alt = 0.35 * trama + 0.08 * _ruido_per(N, 0.7, 1) + 0.9 * rugas + 0.5 * _ruido_per(N, 9, 2)
    costura = np.exp(-((x - 2) / 1.2) ** 2) + np.exp(-((x - (N - 2)) / 1.2) ** 2)
    alt -= 1.2 * costura
    v = 222 * _relevo(alt, 3.0) * (1 + 0.05 * _ruido_per(N, 0.6, 3))
    pesp = (np.abs(x - 7) < 1.0) & ((y % 6) < 3)
    v = np.where(pesp, v * 0.78, v)
    out.append(v)
    # 1 CAMURÇA: fibras com direção, manchas suaves (o "pelo" muda de tom)
    fib = nd.gaussian_filter(np.random.RandomState(5).rand(N, N).astype(np.float32), (0.5, 2.5), mode='wrap')
    fib = (fib - fib.mean()) / fib.std()
    manchas = _ruido_per(N, 10, 6)
    v = 176 + 20 * fib + 14 * manchas + 6 * _ruido_per(N, 0.4, 7)
    out.append(v)
    # 2 LUVA: painéis costurados, poros do couro, furos de ventilação, vincos
    poros = _ruido_per(N, 0.6, 8)
    vincos = np.sin(2 * np.pi * (x / N * 4 + y / N * 1.5 + 0.15 * _ruido_per(N, 8, 9))) * 0.5 + 0.5
    alt = 0.10 * poros + 0.9 * vincos ** 4
    for pc in (0, 64):
        alt -= 1.6 * np.exp(-((y - pc) / 1.3) ** 2) + 1.6 * np.exp(-((y - (pc + N)) / 1.3) ** 2)   # costura do painel
    alt -= 0.5 * np.exp(-((x - 64) / 1.0) ** 2) * (y > 64)
    v = 230 * _relevo(alt, 2.5, amb=0.66, dif=0.42)
    pesp = ((np.abs((y % 64) - 4) < 0.9) & ((x % 6) < 3))
    v = np.where(pesp, v * 0.80, v)
    furos = ((((x % 8) - 4) ** 2 + ((y % 8) - 4) ** 2) < 2.0) & (y > 90) & (y < 114) & (x > 20) & (x < 108)
    v = np.where(furos, v * 0.45, v)
    out.append(v)
    # 3 CARBONO: sarja 2x2, cada fio com brilho do verniz
    c = 8.0
    i, j = np.floor(x / c), np.floor(y / c)
    hor = ((i + j) % 2) < 1
    fy, fx = (y / c) % 1, (x / c) % 1
    perfil = np.where(hor, np.sin(np.pi * fy), np.sin(np.pi * fx))
    filam = np.where(hor, np.sin(np.pi * ((y / 1.3) % 1)), np.sin(np.pi * ((x / 1.3) % 1)))
    v = 28 + 34 * perfil ** 0.7 * np.where(hor, 1.0, 0.72) + 8 * filam + 26 * perfil ** 8
    out.append(v)
    # 4 PINTURA: "casca de laranja" do verniz, micro-riscos e pontinhos de sujeira/borracha
    casca = _ruido_per(N, 1.6, 21)
    v = 236 + 5 * casca
    v = v * _relevo(casca * 0.6, 1.2, amb=0.96, dif=0.06) / 0.99
    r = np.random.RandomState(22)
    for _ in range(14):                                       # riscos finos (polimento)
        x0, y0, ang, comp = r.rand() * N, r.rand() * N, r.rand() * np.pi, 8 + r.rand() * 22
        dx, dy = np.cos(ang), np.sin(ang)
        px, py = (x - x0 + N / 2) % N - N / 2, (y - y0 + N / 2) % N - N / 2
        t = np.clip(px * dx + py * dy, 0, comp)
        d = np.hypot(px - dx * t, py - dy * t)
        v = v + 7 * np.exp(-(d / 0.45) ** 2)
    pts = (np.random.RandomState(23).rand(N, N) > 0.9985).astype(np.float32)   # pontinhos de borracha (poucos, suaves)
    mancha = np.clip(nd.gaussian_filter(pts, 0.8, mode='wrap') * 9.0, 0, 1)
    v = v * (1 - 0.22 * mancha)
    out.append(v)
    res = []
    for v in out:
        g = np.clip(v, 0, 255).astype(np.uint16)
        res.append((((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3)).astype('<u2').tobytes())
    return res, out
