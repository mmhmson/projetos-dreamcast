"""PRÉVIA do cockpit novo (inspirado nas referências reais e de simuladores,
sem nomes nem logos): pinta a camada 2D em coordenadas da tela 640x480
(desenho 2x) e monta por cima de uma captura limpa do jogo.

uso: py -I previa_cockpit.py captura_limpa.png saida.png
"""
import sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy import ndimage as nd

S = 2
W, H = 640 * S, 480 * S
L = np.array([-0.45, -0.75, 0.55]); L /= np.linalg.norm(L)
HV = L + np.array([0, 0, 1.0]); HV /= np.linalg.norm(HV)
YS, XS = np.mgrid[0:H, 0:W].astype(np.float32)
X, Y = XS / S, YS / S                       # coordenadas da tela (0..640, 0..480)
FONTE = "C:/Windows/Fonts/arialbd.ttf"

# cores do jogador (pintura original): azul-petróleo e amarelo
AZUL = 0x0E5C8C
AMARELO = 0xF2C21B


def rgb(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], np.float32)


def mascara(f):
    img = Image.new('L', (W, H), 0)
    f(ImageDraw.Draw(img))
    return np.asarray(img, np.float32) / 255.0


def poli(pts):
    return mascara(lambda d: d.polygon([(x * S, y * S) for x, y in pts], fill=255))


def elipse(cx, cy, rx, ry=None):
    ry = rx if ry is None else ry
    return mascara(lambda d: d.ellipse([(cx - rx) * S, (cy - ry) * S, (cx + rx) * S, (cy + ry) * S], fill=255))


def rret(x0, y0, x1, y1, r):
    return mascara(lambda d: d.rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=r * S, fill=255))


def suave(m, r=1.0):
    """arredonda os cantos de uma máscara"""
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
    n = np.dstack([-gx * forca, -gy * forca, np.ones_like(alt)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return amb + dif * np.clip(n @ L, 0, 1), spec * np.clip(n @ HV, 0, 1) ** exp


def ruido(sig, sem):
    r = np.random.RandomState(sem).rand(H, W).astype(np.float32)
    r = nd.gaussian_filter(r, sig)
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


def sombra(t, m, dx=1.0, dy=2.0, r=2.0, k=0.5):
    sh = nd.shift(nd.gaussian_filter(m, r * S), (dy * S, dx * S), order=1)
    t.escurece(np.clip(sh, 0, 1), k)


def texto(t, txt, cx, cy, tam, cor, alfa=1.0):
    f = ImageFont.truetype(FONTE, int(tam * S))
    img = Image.new('L', (W, H), 0)
    ImageDraw.Draw(img).text((cx * S, cy * S), txt, font=f, fill=255, anchor='mm')
    t.pinta(np.asarray(img, np.float32) / 255.0 * alfa, rgb(cor))


def carbono(c=1.6):
    i, j = np.floor(X / c), np.floor(Y / c)
    hor = ((i + j) % 2) < 1
    fio = np.where(hor, np.sin(np.pi * ((Y / c) % 1)), np.sin(np.pi * ((X / c) % 1))) ** 0.7
    tr = 20 + 17 * fio * np.where(hor, 1.0, 0.72)
    return np.dstack([tr * 0.95, tr * 0.97, tr * 1.10])


def cockpit():
    t = Tela()
    # ---------------- vão do cockpit (interior escuro) ----------------
    vao = poli([(150, 340), (490, 340), (470, 480), (170, 480)])
    grad = np.clip((Y - 340) / 140, 0, 1)
    interior = np.dstack([18 + 10 * (1 - grad), 18 + 10 * (1 - grad), 22 + 12 * (1 - grad)])
    t.pinta(vao, interior * (1 + 0.08 * ruido(1.5, 3))[..., None])
    # coluna de direção (some atrás do volante)
    col = poli([(305, 345), (335, 345), (340, 420), (300, 420)])
    t.pinta(col, rgb(0x2A2C30), *luz(col, 2.0, spec=0.3))
    # ---------------- borda da frente do cockpit (lábio acolchoado) ----------------
    labio = poli([(170, 338), (230, 328), (320, 324), (410, 328), (470, 338), (470, 348), (410, 338), (320, 334), (230, 338), (170, 348)])
    labio = suave(labio, 1.0)
    t.pinta(labio, rgb(0x1C1D20), *luz(labio, 2.5, forca=60, spec=0.35))
    # ---------------- laterais da carroceria (cor da equipe, verniz) ----------------
    for l in (-1, 1):
        def X_(x): return 320 + l * (x - 320)
        pts = [(X_(0), 392), (X_(70), 378), (X_(140), 360), (X_(196), 344), (X_(214), 340),
               (X_(204), 380), (X_(190), 430), (X_(180), 480), (X_(0), 480)]
        m = suave(poli(pts), 1.5)
        sombra(t, m, -l * 2.0, 2.0, 4.0, 0.35)
        # curvatura: a borda de cima é arredondada (reflexo do céu); embaixo mais escura
        prof = np.clip((Y - (392 - (X_(X) if False else 0))) / 120, 0, 1)
        sh, sp = luz(m, 9.0, forca=90, amb=0.55, dif=0.55, spec=0.9, exp=30)
        topo = 392 - (392 - 340) * np.clip((l * (X - 320) * -1 + 320) / 214, 0, 1) if False else None
        corpo = rgb(AZUL)[None, None, :] * (1.05 - 0.45 * np.clip((Y - 350) / 130, 0, 1))[..., None]
        t.pinta(m, corpo, sh, sp)
        # reflexo do céu ao longo da borda de cima (faixa clara do verniz)
        borda = np.clip(nd.gaussian_filter(m, 3 * S) - 0.25, 0, 1) * (1 - np.clip(nd.gaussian_filter(m, 3 * S) - 0.75, 0, 1) * 4)
        ref = np.clip(m - nd.shift(m, (6 * S, 0), order=0), 0, 1)
        ref = nd.gaussian_filter(ref, 1.2 * S)
        t.pinta(np.clip(ref * 0.65, 0, 1), rgb(0xCFE6F5))
        # filete amarelo da pintura acompanhando a borda
        fil = np.clip(nd.shift(m, (-10 * S, 0), order=0) - nd.shift(m, (-13 * S, 0), order=0), 0, 1)
        fil = np.clip(m - nd.shift(m, (11 * S, 0), order=0), 0, 1) - np.clip(m - nd.shift(m, (8 * S, 0), order=0), 0, 1)
        fil = np.clip(fil, 0, 1)
        t.pinta(fil * 0.95, rgb(AMARELO), sh * 0.95 + 0.05)
        # acolchoado preto na borda de dentro (lado do vão)
        pad = suave(poli([(X_(214), 340), (X_(226), 343), (X_(214), 400), (X_(200), 480), (X_(184), 480), (X_(196), 400), (X_(204), 380)]), 1.0)
        t.pinta(pad, rgb(0x232427) * (1 + 0.10 * ruido(0.7, 9))[..., None], *luz(pad, 3.0, forca=70, spec=0.15))
    # ---------------- retrovisores em hastes, no alto ao lado dos pneus ----------------
    for l in (-1, 1):
        cx = 320 + l * 252
        haste = capsula(cx - l * 22, 322, 320 + l * 176, 354, 2.6, 3.4)
        sombra(t, haste, 1.0, 1.5, 1.5, 0.4)
        t.pinta(haste, rgb(0x2E3138), *luz(haste, 1.2, spec=0.6))
        # carenagem em gota (mais larga do lado de fora)
        pts = []
        for a in np.linspace(0, 2 * np.pi, 60, endpoint=False):
            rx = 46 + 6 * np.cos(a) * l
            pts.append((cx + rx * np.cos(a), 308 + 20 * np.sin(a)))
        caixa = suave(poli(pts), 1.0)
        sombra(t, caixa, 1.5, 2.0, 2.5, 0.45)
        sh, sp = luz(caixa, 7.0, forca=80, amb=0.55, dif=0.5, spec=0.9, exp=30)
        t.pinta(caixa, rgb(AZUL) * 1.05, sh, sp)
        vidro = suave(rret(cx - 38, 296, cx + 38, 321, 7), 0.8)
        g = np.clip((Y - 296) / 25, 0, 1)
        t.pinta(vidro, np.dstack([150 + 40 * (1 - g), 165 + 40 * (1 - g), 180 + 40 * (1 - g)]))   # (o jogo põe a imagem do retrovisor aqui)
        t.escurece(vidro * np.exp(-((Y - 296) / 2.0) ** 2), 0.5)
    # ---------------- VOLANTE (estilo fim dos anos 90) ----------------
    cxv, cyv = 320.0, 424.0
    # borboletas atrás
    for l in (-1, 1):
        b = suave(poli([(cxv + l * 70, 360), (cxv + l * 112, 354), (cxv + l * 118, 384), (cxv + l * 80, 392)]), 0.8)
        t.pinta(b, rgb(0x6C7078) * (1 + 0.06 * ruido((0.5, 6), 4))[..., None], *luz(b, 1.5, spec=0.6))
    # contorno: placa + manoplas, com vãos (aros vazados)
    corpo = suave(rret(cxv - 122, 354, cxv + 122, 500, 26), 1.0)
    for l in (-1, 1):
        furo = suave(elipse(cxv + l * 80, 408, 15, 22), 1.0)
        corpo = np.clip(corpo - furo, 0, 1)
    sombra(t, corpo, 2.0, 3.0, 4.0, 0.6)
    # manoplas de camurça (lados)
    for l in (-1, 1):
        mp = suave(rret(cxv + l * 122 - (0 if l > 0 else -0) - 30 * (l > 0) - 0, 362, cxv + l * 122 + 0, 500, 14) if False else rret(min(cxv + l * 92, cxv + l * 124), 356, max(cxv + l * 92, cxv + l * 124), 500, 14), 1.0)
        cil = np.clip(1 - ((X - (cxv + l * 108)) / 17) ** 2, 0, 1) ** 0.5
        cam = rgb(0x2E2C2C) * (1 + 0.12 * ruido(0.6, 11 + l))[..., None]
        sh, sp = luz(mp, 4.0, forca=50, amb=0.5, dif=0.5, spec=0.12, exp=8)
        t.pinta(mp, cam, sh * (0.55 + 0.6 * cil), sp)
    # placa de carbono central
    placa = suave(rret(cxv - 92, 358, cxv + 92, 500, 16), 1.0)
    for l in (-1, 1):
        placa = np.clip(placa - suave(elipse(cxv + l * 80, 408, 15, 22), 1.0), 0, 1)
    sh, sp = luz(placa, 4.0, forca=60, amb=0.75, dif=0.4, spec=0.85, exp=40)
    t.pinta(placa, carbono(), sh, sp + 0.08 * np.clip((400 - Y) / 40, 0, 1))
    # bordas dos furos (aro vazado): sombra interna
    for l in (-1, 1):
        anel = np.clip(suave(elipse(cxv + l * 80, 408, 18, 25), 1.0) - suave(elipse(cxv + l * 80, 408, 15, 22), 1.0), 0, 1)
        t.escurece(anel, 0.5)
    # faixa de LEDs no alto (alguns acesos: verde/vermelho/azul)
    fx = suave(rret(cxv - 58, 368, cxv + 58, 380, 4), 0.6)
    t.pinta(fx, rgb(0x0C0C0E), *luz(fx, 1.5, spec=0.7))
    cores_led = [0x30E050] * 3 + [0xFF3020] * 4 + [0x3A70FF] * 3
    for k in range(10):
        led = elipse(cxv - 50 + k * 11.1, 374, 3.6)
        acesa = k < 6
        c = rgb(cores_led[k]) * (1.0 if acesa else 0.25)
        t.pinta(led, c, *luz(led, 1.2, forca=80, spec=0.9))
        if acesa:
            t.pinta(np.clip(nd.gaussian_filter(led, 2.5 * S) * 0.6, 0, 1), rgb(cores_led[k]) * 1.2)
    # visor
    vis = suave(rret(cxv - 38, 386, cxv + 38, 420, 4), 0.6)
    sombra(t, vis, 1.0, 1.5, 1.5, 0.5)
    t.pinta(vis, rgb(0x0A0A0C), *luz(vis, 2.0, spec=0.7))
    tela = rret(cxv - 34, 389, cxv + 34, 417, 2)
    g = np.clip((Y - 389) / 28, 0, 1)
    t.pinta(tela, np.dstack([120 - 14 * g, 146 - 14 * g, 108 - 14 * g]))
    texto(t, "4", cxv - 12, 404, 22, 0x16221A)
    texto(t, "218", cxv + 16, 398, 10, 0x16221A)
    texto(t, "P05", cxv + 16, 410, 8, 0x16221A)
    t.pinta(tela * np.clip(1 - np.abs((X - cxv) - 0.9 * (Y - 403) - 10) / 5, 0, 1) * 0.15, rgb(0xFFFFFF))
    # botões com tampa colorida e etiquetas brancas pequenas
    BOT = [(-62, 392, 0x2BB0A8, "N"), (-62, 418, 0xE03020, "R"), (62, 392, 0xF0A020, "RAD"), (62, 418, 0xE8E8E8, "PIT"),
           (-46, 438, 0x2AA040, "OK"), (46, 438, 0x3A70FF, "BEB")]
    for bx, by, cor, et in BOT:
        anel = elipse(cxv + bx, by, 7.4)
        sombra(t, anel, 0.8, 1.2, 1.2, 0.5)
        t.pinta(anel, rgb(0x101012), *luz(anel, 1.0, spec=0.5))
        dome = elipse(cxv + bx, by, 5.4)
        t.pinta(dome, rgb(cor), *luz(dome, 2.6, forca=90, amb=0.45, dif=0.75, spec=1.0, exp=28))
        texto(t, et, cxv + bx + (12 if bx < 0 else -12) * (1 if len(et) == 1 else 1.4), by, 5.0, 0xEDEDED, 0.95)
    # seletores com anéis coloridos e números (como nas referências)
    SEL = [(-34, 462, 0x2BB0A8), (0, 470, 0xE8C020), (34, 462, 0xE03020)]
    for sx, sy, cor in SEL:
        anel = elipse(cxv + sx, sy, 12)
        sombra(t, anel, 1.0, 1.6, 1.6, 0.5)
        ang = np.arctan2(Y - sy, X - (cxv + sx))
        faixa = np.clip(np.cos(ang - 0.6), 0, 1) > 0.15
        t.pinta(anel, rgb(cor) * np.where(faixa, 1.0, 0.92)[..., None], *luz(anel, 1.5, spec=0.4))
        for k in range(6):
            a = np.radians(-200 + k * 44)
            texto(t, str(k + 1), cxv + sx + np.cos(a) * 9.3, sy + np.sin(a) * 9.3, 3.6, 0x111111, 0.9)
        cen = elipse(cxv + sx, sy, 6.4)
        t.pinta(cen, rgb(0x1A1B1E), *luz(cen, 2.0, forca=70, spec=0.6))
        alav = capsula(cxv + sx, sy, cxv + sx + 4, sy - 8, 2.0, 1.6)
        t.pinta(alav, rgb(0xE8E8E8) * 0.6 + rgb(cor) * 0.4, *luz(alav, 1.0, spec=0.7))
    # parafusos nos cantos da placa
    for bx, by in ((-84, 372), (84, 372), (-84, 492), (84, 492)):
        p = elipse(cxv + bx, by, 2.0)
        t.pinta(p, rgb(0x9A9EA6), *luz(p, 0.8, forca=90, spec=0.8))
    # ---------------- BRAÇOS (sobem de baixo, quase verticais) ----------------
    for l in (-1, 1):
        wx, wy = cxv + l * 110, 440             # pulso (atrás da luva)
        ex, ey = cxv + l * 160, 540             # cotovelo (abaixo da tela)
        br = capsula(ex, ey, wx, wy, 44, 30)
        sombra(t, br, -l * 1.5, -1.0, 3.0, 0.3)
        # dobras do macacão + cilindro (claro no lado da luz)
        dirx, diry = (wx - ex), (wy - ey)
        dl = np.hypot(dirx, diry); ux, uy = dirx / dl, diry / dl
        lat = ((X - ex) * -uy + (Y - ey) * ux) / 40
        ao_longo = ((X - ex) * ux + (Y - ey) * uy) / dl
        cil = np.clip(1 - lat ** 2, 0, 1) ** 0.5
        dobras = np.sin(ao_longo * 26 + lat * 3) * np.exp(-((ao_longo - 0.75) / 0.18) ** 2) + 0.5 * np.sin(ao_longo * 15 - lat * 4)
        sh, sp = luz(br, 6.0, forca=40, amb=0.45, dif=0.55, spec=0.15, extra=0.05 * dobras * br)
        tec = rgb(AZUL)[None, None, :] * ((0.6 + 0.55 * cil) * (1 + 0.06 * dobras) * (1 + 0.05 * ruido(0.6, 31 + l)))[..., None]
        t.pinta(br, tec, sh, sp)
        # faixa amarela do macacão do lado de fora
        fx = br * np.exp(-((lat * l - 0.55) / 0.10) ** 2)
        t.pinta(fx * 0.9, rgb(AMARELO), sh)
        # punho elástico (canelado) perto da luva
        pun = br * (ao_longo > 0.80)
        t.pinta(pun, rgb(0x0A2E4A) * (0.9 + 0.2 * (0.5 + 0.5 * np.sin(lat * 30)))[..., None], sh)
    # ---------------- LUVAS segurando o topo das manoplas ----------------
    LUVA = 0x1E4F86
    for l in (-1, 1):
        gx = cxv + l * 108
        # dorso da mão (vista de trás, um pouco de lado), cobrindo o topo da manopla
        mao = suave(poli([(gx + l * 30, 380), (gx + l * 24, 362), (gx + l * 4, 354), (gx - l * 14, 360), (gx - l * 20, 378),
                          (gx - l * 18, 420), (gx - l * 6, 440), (gx + l * 18, 442), (gx + l * 30, 420)]), 3.0)
        sombra(t, mao, l * 1.5, 3.0, 3.5, 0.55)
        sh, sp = luz(mao, 9.0, forca=55, amb=0.38, dif=0.80, spec=0.25, exp=10)
        t.pinta(mao, rgb(LUVA) * (1 + 0.05 * ruido(0.6, 41))[..., None], sh, sp)
        # painel acolchoado claro no dorso, com costura
        pain = suave(poli([(gx + l * 22, 378), (gx + l * 16, 364), (gx - l * 2, 362), (gx - l * 8, 380), (gx - l * 4, 404), (gx + l * 16, 410)]), 2.0)
        t.pinta(pain, rgb(0xE6E8EC), *luz(pain, 4.0, forca=55, amb=0.45, dif=0.7, spec=0.25))
        t.escurece(np.clip(nd.gaussian_filter(pain, 0.7 * S) - pain, 0, 1) * 1.5, 0.35)
        # dedos: dão a volta na manopla e aparecem do lado de dentro, um sobre o outro
        for k in range(4):
            yk = 376 + k * 13.5
            ded = capsula(gx - l * 2, yk, gx - l * 25, yk + 1.5, 6.6, 5.8) * (1 - pain)
            sombra(t, ded, -l * 0.8, 1.8, 1.6, 0.5)
            cil = np.clip(1 - ((Y - yk - 0.8) / 6.6) ** 2, 0, 1) ** 0.5
            sh, sp = luz(ded, 2.4, forca=45, amb=0.4, dif=0.7, spec=0.25)
            t.pinta(ded, rgb(LUVA) * 1.04, sh * (0.6 + 0.5 * cil), sp)
            ponta = ded * (np.hypot((X - (gx - l * 26)) / 5.0, (Y - yk - 0.8) / 5.5) < 1)
            t.pinta(ponta, rgb(0x8E9096), sh * (0.6 + 0.5 * cil))
            t.escurece(ded * np.exp(-((X - (gx - l * 12)) / 0.8) ** 2), 0.3)
        # polegar por cima, apontando para o centro do volante
        pol = capsula(gx - l * 10, 366, gx - l * 36, 362, 7.0, 5.6)
        sombra(t, pol, 0.8, 2.0, 2.0, 0.5)
        sh, sp = luz(pol, 3.0, forca=45, amb=0.42, dif=0.72, spec=0.3)
        t.pinta(pol, rgb(LUVA) * (1 + 0.05 * ruido(0.6, 43))[..., None], sh, sp)
        ponta = pol * (np.hypot(X - (gx - l * 38), Y - 362) < 5.2)
        t.pinta(ponta, rgb(0x8E9096), sh)
        # punho da luva (faixa amarela com velcro escuro)
        pul = suave(poli([(gx - 22, 432), (gx + 24, 432), (gx + 26, 448), (gx - 24, 448)]), 1.0)
        t.pinta(pul, rgb(AMARELO) * 0.95, *luz(pul, 2.0, forca=50, spec=0.2))
        vel = suave(rret(gx - 10, 436, gx + 12, 445, 2), 0.6)
        t.pinta(vel, rgb(0x2A2C30), *luz(vel, 1.0, spec=0.2))
    return t


def main():
    cap, saida = sys.argv[1], sys.argv[2]
    im = Image.open(cap).convert('RGB')
    w, h = im.size
    jogo = im.crop((30, 31, 1040, 790)).resize((W, H), Image.LANCZOS)
    t = cockpit()
    base = np.asarray(jogo, np.float32)
    a = t.a[..., None]
    rgbx = np.where(a > 0.001, t.rgb / np.maximum(a, 0.001), 0)
    out = base * (1 - a) + np.clip(rgbx, 0, 255) * a
    Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(saida)


if __name__ == "__main__":
    main()
