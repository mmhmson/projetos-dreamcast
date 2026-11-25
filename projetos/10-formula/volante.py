"""VOLANTE DO COCKPIT pintado no PC (como os jogos da época faziam com a
arte 2D do cockpit): volante de carbono, manoplas de borracha, botões,
seletores, visor, luvas do piloto, tecido do macacão (antebraços) e a
carenagem dos retrovisores.

Tudo é desenhado com supersampling 2x e iluminado por "relevo" (a máscara de
cada peça é borrada e vira uma altura: dela saem as normais -> sombra, brilho
especular e bisel), depois comprimido em VQ (2x2, 256 blocos) com alfa de
1 bit (ARGB1555) para a lista de recorte (punch-through) do PowerVR.

Textura 512x512:
  linhas   0..255  volante + luvas  (coordenadas locais x -200..200, y -104..96; 1,28 texel/pixel)
  linhas 256..383  tecido do antebraço (cinza; o jogo tinge com a cor da equipe)
  linhas 384..511, colunas 0..255  carenagem do retrovisor (miolo transparente)

As partes que mudam (luzes de troca, dígitos do visor, punhos na cor da
equipe) continuam sendo desenhadas pelo jogo por cima.
"""
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as nd

S = 1
K = 1024 / 452.0            # pixels do desenho por pixel da tela (x -226..226, y -118..108)
W, H = 1024, 512            # desenho 1024x512 -> textura 512x512 (x reduzido 2:1, y inteiro)
L = np.array([-0.45, -0.75, 0.55]); L /= np.linalg.norm(L)       # luz de cima/esquerda (y para baixo)
HV = L + np.array([0, 0, 1.0]); HV /= np.linalg.norm(HV)


def P(x, y):
    return ((x + 226) * K * S, (y + 118) * K * S)


def mascara(desenho):
    img = Image.new('L', (W, H), 0)
    desenho(ImageDraw.Draw(img))
    return np.asarray(img, np.float32) / 255.0


def m_poli(pts):
    return mascara(lambda d: d.polygon([P(*p) for p in pts], fill=255))


def m_elipse(cx, cy, rx, ry=None):
    ry = rx if ry is None else ry
    a, b = P(cx - rx, cy - ry), P(cx + rx, cy + ry)
    return mascara(lambda d: d.ellipse([a, b], fill=255))


def m_rret(x0, y0, x1, y1, r):
    x0, x1 = min(x0, x1), max(x0, x1)
    y0, y1 = min(y0, y1), max(y0, y1)
    a, b = P(x0, y0), P(x1, y1)
    return mascara(lambda d: d.rounded_rectangle([a, b], radius=r * K * S, fill=255))


def normais(alt, forca):
    gy, gx = np.gradient(alt)
    n = np.dstack([-gx * forca, -gy * forca, np.ones_like(alt)])
    return n / np.linalg.norm(n, axis=2, keepdims=True)


def luz(m, raio, forca=40.0, amb=0.50, dif=0.62, spec=0.0, exp=24, alt_extra=None):
    """sombra + brilho de uma peça a partir da máscara (bisel arredondado)"""
    alt = nd.gaussian_filter(m, raio * K * S)
    if alt_extra is not None:
        alt = alt + alt_extra
    n = normais(alt, forca)
    nl = np.clip(n @ L, 0, 1)
    sh = amb + dif * nl
    sp = spec * np.clip(n @ HV, 0, 1) ** exp
    return sh, sp


class Tela:
    def __init__(self, w, h):
        self.rgb = np.zeros((h, w, 3), np.float32)
        self.a = np.zeros((h, w), np.float32)

    def pinta(self, m, cor, sh=1.0, sp=0.0):
        cor = np.asarray(cor, np.float32)
        if cor.ndim == 1:
            cor = cor[None, None, :]
        sh = sh if np.ndim(sh) == 0 else sh[..., None]
        sp = sp if np.ndim(sp) == 0 else sp[..., None]
        c = cor * sh + 255.0 * sp
        mm = m[..., None]
        self.rgb = self.rgb * (1 - mm) + c * mm
        self.a = np.maximum(self.a, m)

    def escurece(self, m, k):
        self.rgb *= (1 - m * k)[..., None]


def rgb(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], np.float32)


def grade():
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float32)
    return xs / (K * S) - 226, ys / (K * S) - 118          # coordenadas locais (pixels da tela)


def ruido(shape, sigma, sem):
    r = np.random.RandomState(sem).rand(*shape).astype(np.float32)
    r = nd.gaussian_filter(r, sigma)
    return (r - r.mean()) / (r.std() + 1e-6)


FONTE_TTF = "C:/Windows/Fonts/arialbd.ttf"


def m_texto(txt, cx, cy, tam, ang=0.0):
    """máscara de um texto (etiquetas do volante), centrado em (cx, cy)"""
    from PIL import ImageFont
    try:
        f = ImageFont.truetype(FONTE_TTF, int(tam * K * S))
    except Exception:
        f = ImageFont.load_default()
    img = Image.new('L', (W, H), 0)
    d = ImageDraw.Draw(img)
    d.text(P(cx, cy), txt, font=f, fill=255, anchor='mm')
    if ang:
        img = img.rotate(-ang, center=P(cx, cy), resample=Image.BICUBIC)
    return np.asarray(img, np.float32) / 255.0


def sombra(t, m, dx=1.2, dy=2.0, raio=2.0, k=0.55):
    """sombra de contato: a peça escurece o que está embaixo, deslocada da luz"""
    sh = nd.shift(nd.gaussian_filter(m, raio * K * S), (dy * K * S, dx * K * S), order=1)
    t.escurece(np.clip(sh, 0, 1), k)


def volante():
    t = Tela(W, H)
    X, Y = grade()
    # ---- borboletas de câmbio (alumínio escovado) com "+" e "-" ----
    for l in (-1, 1):
        m = m_poli([(l * 92, -96), (l * 150, -84), (l * 146, -40), (l * 96, -60)])
        sh, sp = luz(m, 2.0, spec=0.55, exp=30)
        esc = 1 + 0.08 * ruido((H, W), (0.5, 8), 3)
        t.pinta(m, rgb(0x6E7178) * esc[..., None], sh, sp)
        mt = m_texto("+" if l > 0 else "-", l * 128, -78, 13)
        t.pinta(mt * 0.85, rgb(0x1A1A1E))
        # parafuso da borboleta
        mb = m_elipse(l * 104, -84, 2.2)
        t.pinta(mb, rgb(0x2A2B30), *luz(mb, 1.0, spec=0.7))
    # ---- corpo de carbono ----
    CORPO = [(-150, -24), (-126, -70), (-62, -84), (62, -84), (126, -70), (150, -24), (150, 58), (90, 70), (-90, 70), (-150, 58)]
    corpo = m_poli(CORPO)
    sombra(t, corpo, 2.0, 3.0, 3.0, 0.6)
    c = 2.6                                             # trama 2x2 (sarja) do carbono, mais fina
    i, j = np.floor(X / c), np.floor(Y / c)
    hor = ((i + j) % 2) < 1
    fx, fy = (X / c) % 1, (Y / c) % 1
    fio = np.where(hor, np.sin(np.pi * fy), np.sin(np.pi * fx)) ** 0.7
    trama = 21 + 17 * fio * np.where(hor, 1.0, 0.72)
    car = np.dstack([trama * 0.95, trama * 0.97, trama * 1.10])
    sh, sp = luz(corpo, 4.0, forca=60, amb=0.75, dif=0.45, spec=0.9, exp=40)
    verniz = 0.10 * np.clip((-Y - 10) / 80, 0, 1)
    t.pinta(corpo, car, sh, sp + verniz)
    ref = m_poli([(-110, -66), (110, -66), (100, -60), (-100, -60)]) * corpo
    t.pinta(ref * 0.22, rgb(0xFFFFFF))
    # placa central (painel de alumínio anodizado escuro, rebaixado)
    placa = m_rret(-92, -60, 92, 36, 10) * corpo
    sh, sp = luz(placa, 2.0, forca=70, amb=0.8, dif=0.3, spec=0.4, exp=30, alt_extra=-0.0)
    t.escurece(np.clip(nd.gaussian_filter(placa, 1.5 * K * S) - placa * 0.7, 0, 1), 0.6)   # vinco em volta
    t.pinta(placa, rgb(0x24262C) * (1 + 0.04 * ruido((H, W), (0.4, 5), 31))[..., None], sh, sp)
    # parafusos da placa
    for bx, by in ((-84, -52), (84, -52), (-84, 28), (84, 28), (-36, 30), (36, 30)):
        mb = m_elipse(bx, by, 2.4)
        sombra(t, mb, 0.6, 0.8, 0.8, 0.5)
        t.pinta(mb, rgb(0x8A8E96), *luz(mb, 1.0, forca=90, spec=0.9, exp=20))
        t.pinta(m_rret(bx - 1.6, by - 0.35, bx + 1.6, by + 0.35, 0.2), rgb(0x2A2C30))
    # ---- manoplas de borracha (cilindro + sulcos + textura em losango) ----
    for l in (-1, 1):
        x0, x1 = (146, 190) if l > 0 else (-190, -146)
        m = m_rret(x0, -36, x1, 66, 12)
        sombra(t, m, 1.5, 2.0, 2.5, 0.5)
        cx = (x0 + x1) / 2
        cil = np.clip(1 - ((X - cx) / 22) ** 2, 0, 1) ** 0.5
        sulcos = np.zeros_like(X)
        for k in range(5):
            sulcos += np.exp(-((Y - (0 + k * 13)) / 1.6) ** 2)
        los = (np.abs(((X + Y) / 2.2) % 1 - 0.5) < 0.12) | (np.abs(((X - Y) / 2.2) % 1 - 0.5) < 0.12)
        sh, sp = luz(m, 3.0, forca=50, amb=0.45, dif=0.5, spec=0.25, exp=12, alt_extra=-0.25 * sulcos * m)
        borr = rgb(0x3A3431) * ((1 + 0.10 * ruido((H, W), 0.7, 11)) * (1 - 0.18 * los))[..., None]
        t.pinta(m, borr, sh * (0.55 + 0.55 * cil), sp)
    # ---- luzes de troca: moldura + lentes (os LEDs acesos são desenhados no jogo) ----
    m = m_rret(-80, -82, 80, -62, 5)
    sombra(t, m, 1.0, 1.5, 1.5, 0.5)
    sh, sp = luz(m, 2.0, spec=0.8, exp=40)
    t.pinta(m, rgb(0x101013), sh, sp)
    for k in range(9):
        cor = (0x1A4A22, 0x4A1A16, 0x1A244A)[k // 3]
        m = m_elipse(-64 + k * 16, -72, 5.6)
        t.pinta(m, rgb(0x050506))
        m = m_elipse(-64 + k * 16, -72, 4.8)
        sh, sp = luz(m, 2.2, forca=80, spec=1.0, exp=30)
        anel = np.clip(np.hypot(X - (-64 + k * 16), Y + 72) / 4.8, 0, 1) ** 4    # lente facetada
        t.pinta(m, rgb(cor) * (1 + 0.6 * anel)[..., None], sh, sp)
    # ---- visor LCD com moldura e retroiluminação ----
    m = m_rret(-60, -60, 60, -12, 5)
    sombra(t, m, 1.2, 1.8, 2.0, 0.6)
    sh, sp = luz(m, 2.5, spec=0.7, exp=40)
    t.pinta(m, rgb(0x0B0B0E), sh, sp)
    m = m_rret(-54, -55, 54, -17, 2)
    grad = np.clip((Y + 55) / 38, 0, 1)
    cen = np.exp(-((X / 70) ** 2 + ((Y + 36) / 28) ** 2))                  # retroiluminação mais forte no meio
    tela = rgb(0x86A07A)[None, None, :] * (0.96 - 0.12 * grad + 0.10 * cen)[..., None]
    t.pinta(m, tela)
    t.escurece(m * np.exp(-((Y + 55) / 3.0) ** 2), 0.40)
    t.escurece(m * np.exp(-((X + 54) / 3.0) ** 2), 0.25)
    brilho = m * np.clip(1 - np.abs((X - 0.9 * (Y + 36)) - 18) / 7, 0, 1) * 0.16
    t.pinta(brilho, rgb(0xFFFFFF))
    t.pinta(m_texto("GEAR", -40, -51, 4.5) * 0.55, rgb(0x2C3A28))
    t.pinta(m_texto("KM/H", 40, -51, 4.5) * 0.55, rgb(0x2C3A28))
    # ---- botões (domos) com etiquetas ----
    BOT = [(0xD82020, -104, -36, "N"), (0xF0C010, -72, -48, "PIT"), (0x2050E0, 72, -48, "RAD"),
           (0x20A040, 104, -36, "OK"), (0xE8E8E8, -40, 4, "BEB")]
    for cor, bx, by, et in BOT:
        m = m_elipse(bx, by, 9.8)
        sombra(t, m, 1.0, 1.6, 1.6, 0.55)
        sh, sp = luz(m, 1.5, spec=0.5)
        t.pinta(m, rgb(0x0C0C0E), sh, sp)
        m = m_elipse(bx, by, 7.0)
        sh, sp = luz(m, 3.5, forca=90, amb=0.45, dif=0.75, spec=1.0, exp=28)
        t.pinta(m, rgb(cor), sh, sp)
        t.pinta(m_texto(et, bx, by + 15.5, 6.0) * 0.92, rgb(0xE8E8E8))
    # ---- seletores giratórios serrilhados com escala ----
    for l, nome in ((-1, "MIX"), (1, "DIF")):
        x, y = l * 62.0, 22.0
        for k in range(6):
            a = np.radians(-150 + k * 24 + 30) - np.pi / 2
            m = m_elipse(x - l * np.cos(a) * 19.5, y + np.sin(a) * 19.5, 1.3)
            t.pinta(m, rgb(0xD0D0D0))
            t.pinta(m_texto(str(k + 1), x - l * np.cos(a) * 26, y + np.sin(a) * 26, 5.2) * 0.85, rgb(0xD8D8D8))
        t.pinta(m_texto(nome, x, y + 21, 5.5) * 0.9, rgb(0xF0C010))
        m = m_elipse(x, y, 15.5)
        sombra(t, m, 1.5, 2.2, 2.2, 0.6)
        sh, sp = luz(m, 1.5, spec=0.5)
        t.pinta(m, rgb(0x0C0C0E), sh, sp)
        m = m_elipse(x, y, 12.5)
        ang = np.arctan2(Y - y, X - x)
        serr = 0.5 + 0.5 * np.sin(ang * 32)
        anel = np.clip((np.hypot(X - x, Y - y) - 9.5) / 3, 0, 1)
        sh, sp = luz(m, 3.0, forca=70, spec=0.7, exp=30)
        t.pinta(m, rgb(0x55575F) * (1 - 0.40 * anel * serr)[..., None], sh, sp)
        mt = m_elipse(x, y, 8.5)                           # tampa usinada (anéis concêntricos)
        rr_ = np.hypot(X - x, Y - y)
        t.pinta(mt, rgb(0x6C6F78) * (0.9 + 0.12 * np.sin(rr_ * 4.0))[..., None], *luz(mt, 2.0, forca=60, spec=0.6))
        m = m_rret(x - 1.4, y - 11, x + 1.4, y - 3, 0.6)
        t.pinta(m, rgb(0xF2F2F2))
    # botão pequeno
    m = m_elipse(40, 4, 6.2); sombra(t, m, 0.8, 1.2, 1.2, 0.5); t.pinta(m, rgb(0x0C0C0E))
    m = m_elipse(40, 4, 4.2); sh, sp = luz(m, 2.5, forca=80, spec=0.9); t.pinta(m, rgb(0x8C949C), sh, sp)
    t.pinta(m_texto("LIM", 40, 15, 4.8) * 0.85, rgb(0xE0E0E0))
    # ---- cubo de engate rápido (metal) + anel amarelo de soltar ----
    m = m_elipse(0, 50, 18)
    sombra(t, m, 1.5, 2.0, 2.5, 0.55)
    sh, sp = luz(m, 3.0, forca=50, spec=0.8, exp=20)
    t.pinta(m, rgb(0x2E3036), sh, sp)
    m = np.clip(m_elipse(0, 50, 14) - m_elipse(0, 50, 11), 0, 1)
    t.pinta(m, rgb(0xE8B020), *luz(m, 1.2, forca=60, spec=0.8))
    m = m_elipse(0, 50, 9.5)
    esc = 0.85 + 0.25 * np.cos(np.arctan2(Y - 50, X) * 2 + 0.6)
    sh, sp = luz(m, 2.0, forca=60, spec=0.9, exp=16)
    t.pinta(m, rgb(0x9CA0A6) * esc[..., None], sh, sp)
    for k in range(6):
        a = k * np.pi / 3
        m = m_elipse(6.0 * np.cos(a), 50 + 6.0 * np.sin(a), 1.3)
        t.pinta(m, rgb(0x5A5E66), *luz(m, 1.0, spec=0.6))
    # adesivo do número (placa branca com "7")
    m = m_rret(-12, -8, 12, 10, 3)
    t.pinta(m, rgb(0xEDEDED), *luz(m, 1.0, amb=0.9, dif=0.15, spec=0.3))
    t.pinta(m_texto("7", 0, 1, 15) * 0.95, rgb(0x0E5C8C))
    # ---- luvas do piloto ----
    # Como se segura um volante de F1: o DORSO da mão fica do lado de fora da
    # manopla (o pulso vem de baixo/fora), os dedos dão a volta por TRÁS e só
    # as pontas aparecem na borda de dentro; o POLEGAR fica deitado na frente,
    # apontando para o centro do volante.
    tec = 1 + 0.05 * ruido((H, W), 0.8, 21) + 0.04 * ruido((H, W), 4, 22)
    luva = rgb(0xE8E6E0)[None, None, :] * tec[..., None]
    camurca = rgb(0x8A8C92)[None, None, :] * (1 + 0.08 * ruido((H, W), 0.6, 23))[..., None]
    def capsula(x0, y0, x1, y1, r0, r1, n=24):
        dx, dy = x1 - x0, y1 - y0
        dl = np.hypot(dx, dy); nx, ny = -dy / dl, dx / dl
        pts = []
        for q in range(n + 1):                       # lado A
            tq = q / n; r = r0 + (r1 - r0) * tq
            pts.append((x0 + dx * tq + nx * r, y0 + dy * tq + ny * r))
        for q in range(n + 1):                       # ponta arredondada (x1)
            a = np.pi * q / n
            pts.append((x1 + (nx * np.cos(a) + dx / dl * np.sin(a)) * r1, y1 + (ny * np.cos(a) + dy / dl * np.sin(a)) * r1))
        for q in range(n, -1, -1):                   # lado B
            tq = q / n; r = r0 + (r1 - r0) * tq
            pts.append((x0 + dx * tq - nx * r, y0 + dy * tq - ny * r))
        for q in range(n + 1):                       # base arredondada (x0)
            a = np.pi * q / n
            pts.append((x0 - (nx * np.cos(a) + dx / dl * np.sin(a)) * r0, y0 - (ny * np.cos(a) + dy / dl * np.sin(a)) * r0))
        return m_poli(pts)
    for l in (-1, 1):
        x = l * 168.0
        # DEDOS: dão a volta por trás da manopla e voltam pela frente, saindo
        # da borda de DENTRO e apontando para fora (para a palma). Cada dedo é
        # um cilindro curvo (2 falanges), com vinco na junta e ponta de camurça.
        for k in range(4):
            yc = -3 + k * 12.3 + (1.2 if k in (1, 2) else 0)       # médio/anelar um pouco mais baixos
            comp = (15.0, 17.0, 16.0, 13.0)[k]                        # comprimento visível de cada dedo
            esp = (5.4, 5.6, 5.4, 4.8)[k]                             # meia-espessura
            xb = x - l * 21.5                                         # sai de trás da manopla (borda de dentro)
            xm = xb + l * comp * 0.55
            xt = xb + l * comp
            seg1 = capsula(xb, yc - 0.8, xm, yc, esp, esp * 0.96, 16)
            seg2 = capsula(xm, yc, xt, yc + 0.8, esp * 0.96, esp * 0.86, 16)
            m = np.clip(seg1 + seg2, 0, 1)
            sombra(t, m, 0.8, 1.6, 1.4, 0.55)
            cil = np.clip(1 - ((Y - yc) / (esp + 0.5)) ** 2, 0, 1) ** 0.5
            sh, sp = luz(m, 1.8, forca=35, amb=0.48, dif=0.55, spec=0.22, exp=12)
            tip = np.clip(1 - np.hypot((X - (xt - l * 1.5)) / 4.5, (Y - yc - 0.6) / (esp * 0.9)), 0, 1) > 0
            corf = luva * (1 - tip[..., None]) + camurca * tip[..., None]
            t.pinta(m, corf, sh * (0.55 + 0.5 * cil), sp * cil)
            t.escurece(m * np.exp(-((X - xm) / 0.8) ** 2) * (np.abs(Y - yc) < esp * 0.8), 0.30)   # junta
            t.escurece(m * np.exp(-((Y - (yc + esp * 0.85)) / 0.8) ** 2), 0.35)                   # vão para o dedo de baixo
        # dorso da mão, do lado de FORA da manopla (cobre a metade de fora dela)
        dorso = m_poli([(x + l * 3, -24), (x + l * 28, -29), (x + l * 49, -8), (x + l * 52, 28),
                        (x + l * 42, 54), (x + l * 10, 54), (x + l * 1, 28)])
        dorso = np.clip((nd.gaussian_filter(dorso, 3.0 * S) - 0.3) * 2.5, 0, 1)
        sombra(t, dorso, 2.5, 3.5, 3.5, 0.6)
        cil = np.clip(1 - ((X - (x + l * 24)) / 28) ** 2, 0, 1) ** 0.5          # dorso arredondado
        sh, sp = luz(dorso, 6.0, forca=45, amb=0.50, dif=0.55, spec=0.15, exp=10)
        t.pinta(dorso, luva, sh * (0.7 + 0.35 * cil), sp)
        # tendões / vincos do dorso (leve) e painel acolchoado costurado
        for k in range(4):
            vin = dorso * np.exp(-((X - (x + l * (18 + k * 7.5)) - 0.25 * l * (Y - 10)) / 0.9) ** 2) * (Y > -8) * (Y < 34)
            t.escurece(vin, 0.10)
        pa = m_rret(x + l * 18, 24, x + l * 44, 42, 4)
        t.pinta(pa, luva * 0.97, *luz(pa, 2.5, forca=50, amb=0.55, dif=0.5, spec=0.15))
        t.escurece(np.clip(nd.gaussian_filter(pa, 0.6 * S) - pa, 0, 1) * 1.5, 0.25)
        # NÓS DOS DEDOS: protetor vertical ao longo da manopla, com os 4 nós
        m = m_rret(x + l * 3, -20, x + l * 13, 40, 5)
        sombra(t, m, 0.8, 1.2, 1.5, 0.45)
        nos = np.zeros_like(X)
        for k in range(4):
            nos += np.exp(-(((X - (x + l * 6)) / 5) ** 2 + ((Y - (-11 + k * 15)) / 5.5) ** 2))
        sh, sp = luz(m, 2.2, forca=60, spec=0.3, alt_extra=0.5 * nos * m)
        t.pinta(m, rgb(0x6A6D74), sh, sp)
        furos = m * (((X / 3.0) % 1 - 0.5) ** 2 + ((Y / 3.0) % 1 - 0.5) ** 2 < 0.05) * (nos < 0.5)
        t.escurece(furos.astype(np.float32), 0.6)
        # POLEGAR deitado na frente da manopla, apontando para o centro (alto)
        m = capsula(x + l * 8, -14, x - l * 26, -27, 7.2, 5.6)
        t.escurece(nd.gaussian_filter(m, 2 * S) * 0.8, 0.45)
        sh, sp = luz(m, 2.6, forca=45, amb=0.5, dif=0.6, spec=0.3, exp=12)
        ponta = np.clip(1 - np.hypot(X - (x - l * 24), Y + 26.5) / 7.5, 0, 1) > 0
        corp = luva * (1 - ponta[..., None]) + camurca * ponta[..., None]
        t.pinta(m, corp, sh, sp)
        t.escurece(m * np.exp(-((X - (x - l * 6)) / 1.0) ** 2), 0.22)            # dobra da junta
        # faixa de velcro no pulso (embaixo do dorso; o punho na cor da equipe vem por baixo)
        m = m_rret(x + l * 6, 46, x + l * 46, 55, 2.5)
        sombra(t, m, 0.6, 1.2, 1.2, 0.4)
        t.pinta(m, rgb(0x3C3E44) * (1 + 0.10 * ruido((H, W), 0.5, 25))[..., None], *luz(m, 1.5, forca=60, spec=0.2))
        t.pinta(m * (np.abs(Y - 46.8) < 0.6), rgb(0xBFC2C8))
        tab = m_rret(x + l * 34, 45, x + l * 46, 56, 2)
        t.pinta(tab, rgb(0x2E3036), *luz(tab, 1.5, forca=60, spec=0.3))
    return t


def tecido():
    """antebraço: tecido do macacão em cinza (o jogo tinge com a cor da equipe);
    u = do punho ao cotovelo, v = de cima (iluminado) para baixo"""
    w, h = 512, 256
    u, v = np.meshgrid(np.linspace(0, 1, w), np.linspace(0, 1, h))
    cil = np.sin(np.pi * np.clip(v, 0.02, 0.98)) ** 0.6
    base = (0.50 + 0.55 * cil * (1.08 - 0.45 * v))
    base *= 1 - 0.38 * np.clip((u - 0.55) / 0.45, 0, 1) ** 1.5          # sombra entrando no cockpit
    dobras = (np.sin(u * 46 + v * 9) * np.exp(-((u - 0.12) / 0.12) ** 2)
              + 0.8 * np.sin(u * 26 - v * 8) * np.exp(-((u - 0.62) / 0.10) ** 2)   # dobra do cotovelo
              + 0.3 * np.sin(u * 13 + v * 3))
    r = np.random.RandomState(7).rand(h, w)
    fibra = nd.gaussian_filter(r, (0.8, 3)) - 0.5
    trama = 0.5 + 0.5 * np.sin(np.arange(w)[None, :] * 2.1) * np.sin(np.arange(h)[:, None] * 1.05)
    g = base * (1 + 0.12 * dobras) * (1 + 0.20 * fibra) * (0.97 + 0.05 * trama)
    # painéis do macacão: costuras longitudinais (vinco escuro + pespontos)
    for vc in (0.20, 0.80):
        vinco = np.exp(-((v - vc) / 0.012) ** 2)
        g *= 1 - 0.22 * vinco
        g *= 1 - 0.06 * np.exp(-((v - (vc + 0.035)) / 0.008) ** 2)
    # punho elástico (canelado) perto da luva
    can = (u < 0.06) * (0.5 + 0.5 * np.sin(v * 60))
    g *= 1 - 0.12 * can
    g = np.clip(g, 0, 1.05) * 242
    return np.dstack([g, g, g]).astype(np.float32), np.ones((h, w), np.float32)


def lado_cockpit():
    """borda lateral do cockpit vista de dentro (canto de baixo da tela):
    friso de carbono em cima e enchimento de camurça preta em capitonê"""
    S = 2
    w, h = 256 * S, 256 * S
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    xn, yn = xs / w, ys / h
    topo = 0.10 + 0.72 * xn ** 1.35                    # borda sobe para o canto da tela (x=0)
    forma = np.clip((yn - topo) * h / 1.5, 0, 1)
    t = Tela(w, h)
    # enchimento: capitonê (losangos acolchoados) com pespontos
    c = 18.0 * S
    a1, a2 = (xs + ys) / c, (xs - ys) / c
    alm = (np.sin(np.pi * (a1 % 1)) * np.sin(np.pi * (a2 % 1))) ** 0.5
    n = normais(nd.gaussian_filter(alm * forma, 2 * S) * 8.0, 6.0)
    sh = 0.45 + 0.85 * np.clip(n @ L, 0, 1)
    cam = 62 * (1 + 0.12 * ruido((h, w), 0.6, 41))
    t.pinta(forma, np.dstack([cam, cam, cam * 1.05]), sh * (0.75 + 0.25 * (1 - yn)))
    pesp = ((np.abs((a1 % 1) - 0.5) > 0.47) | (np.abs((a2 % 1) - 0.5) > 0.47)) * ((np.floor((xs + 2 * ys) / (2.5 * S)) % 2) == 0)
    t.escurece((pesp * forma).astype(np.float32), 0.45)
    # friso de carbono na borda de cima (com bisel brilhante)
    friso = np.clip((yn - topo) * h / 1.5, 0, 1) * np.clip((topo + 0.12 - yn) * h / 1.5, 0, 1)
    cc = 3.0 * S
    hor = ((np.floor(xs / cc) + np.floor(ys / cc)) % 2) < 1
    fio = np.where(hor, np.sin(np.pi * ((ys / cc) % 1)), np.sin(np.pi * ((xs / cc) % 1))) ** 0.7
    tr = 24 + 18 * fio
    alt = nd.gaussian_filter(friso, 3 * S)
    nn = normais(alt, 40)
    shf = 0.6 + 0.6 * np.clip(nn @ L, 0, 1)
    spf = 0.9 * np.clip(nn @ HV, 0, 1) ** 30
    t.pinta(friso, np.dstack([tr * 0.95, tr * 0.97, tr * 1.1]), shf, spf)
    rgbx = t.rgb.reshape(h // S, S, w // S, S, 3).mean((1, 3))
    a = t.a.reshape(h // S, S, w // S, S).mean((1, 3))
    return rgbx, a


def retrovisor():
    """carenagem do retrovisor (carbono com bisel), miolo transparente para o vidro"""
    S = 2
    w, h = 256 * S, 128 * S
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    def rr(x0, y0, x1, y1, r):
        img = Image.new('L', (w, h), 0)
        ImageDraw.Draw(img).rounded_rectangle([x0 * S, y0 * S, x1 * S, y1 * S], radius=r * S, fill=255)
        return np.asarray(img, np.float32) / 255
    fora = rr(2, 4, 253, 124, 34)
    dentro = rr(16, 15, 240, 105, 14)
    t = Tela(w, h)
    c = 6.0
    i, j = np.floor(xs / c), np.floor(ys / c)
    hor = ((i + j) % 2) < 1
    fio = np.where(hor, np.sin(np.pi * ((ys / c) % 1)), np.sin(np.pi * ((xs / c) % 1))) ** 0.7
    tr = 24 + 16 * fio
    car = np.dstack([tr * 0.95, tr * 0.97, tr * 1.1])
    alt = nd.gaussian_filter(fora, 10 * S) - 0.6 * nd.gaussian_filter(dentro, 3 * S)
    n = normais(alt, 60)
    sh = 0.6 + 0.6 * np.clip(n @ L, 0, 1)
    sp = 0.8 * np.clip(n @ HV, 0, 1) ** 40
    t.pinta(fora, car, sh, sp)
    # borda interna escura (sombra sobre o vidro) e miolo transparente
    borda = np.clip(rr(12, 11, 244, 109, 16) - dentro, 0, 1)
    t.pinta(borda, rgb(0x08080A))
    t.a = np.clip(t.a - dentro, 0, 1)
    rgbx = t.rgb.reshape(h // S, S, w // S, S, 3).mean((1, 3))
    a = t.a.reshape(h // S, S, w // S, S).mean((1, 3))
    return rgbx, a


def reduz(t):
    rgbx = t.rgb.reshape(H, W // 2, 2, 3).mean(2)
    a = t.a.reshape(H, W // 2, 2).mean(2)
    # cor "pré-multiplicada" desfeita (bordas sem halo escuro)
    rgbx = np.where(a[..., None] > 0.01, rgbx / np.maximum(a[..., None], 0.01), 0)
    return rgbx, a


def _fecha(img, al):
    """alfa de 1 bit; a cor das partes transparentes copia a vizinha (filtro bilinear sem halo)"""
    a1 = al > 0.5
    if (~a1).any() and a1.any():
        _, (iy, ix) = nd.distance_transform_edt(~a1, return_indices=True)
        img = img[iy, ix]
    return np.clip(img, 0, 255), a1


def montar():
    """duas imagens 512x512: (1) volante + luvas; (2) tecido, retrovisor e lateral do cockpit"""
    v_rgb, v_a = reduz(volante())
    A = _fecha(v_rgb, v_a)
    img = np.zeros((512, 512, 3), np.float32)
    al = np.zeros((512, 512), np.float32)
    t_rgb, t_a = tecido()
    img[0:256], al[0:256] = t_rgb, t_a
    r_rgb, r_a = retrovisor()
    r_rgb = np.where(r_a[..., None] > 0.01, r_rgb / np.maximum(r_a[..., None], 0.01), 0)
    img[256:384, 0:256], al[256:384, 0:256] = r_rgb, r_a
    c_rgb, c_a = lado_cockpit()
    c_rgb = np.where(c_a[..., None] > 0.01, c_rgb / np.maximum(c_a[..., None], 0.01), 0)
    img[256:512, 256:512], al[256:512, 256:512] = c_rgb, c_a
    B = _fecha(img, al)
    return A, B


def vq_1555(img, a1, iters=14):
    """VQ do PowerVR (sem mipmap): 256 blocos 2x2 ARGB1555 + 1 índice por bloco (twiddled)"""
    n = img.shape[0]
    rgba = np.dstack([img, a1.astype(np.float32) * 255])
    q = n // 2
    blocos = rgba.reshape(q, 2, q, 2, 4).transpose(0, 2, 3, 1, 4).reshape(-1, 16)   # ordem (x,y): (0,0),(0,1),(1,0),(1,1)
    pes = np.tile(np.array([1, 1, 1, 3], np.float32), 4)           # alfa pesa mais (borda certa)
    X = blocos * pes
    rs = np.random.RandomState(4)
    C = X[rs.choice(len(X), 256, replace=False)].copy()
    amostra = X[rs.choice(len(X), min(len(X), 60000), replace=False)]
    for _ in range(iters):
        d = (amostra * amostra).sum(1)[:, None] - 2 * amostra @ C.T + (C * C).sum(1)[None, :]
        lab = d.argmin(1)
        for k in range(256):
            m = lab == k
            C[k] = amostra[m].mean(0) if m.any() else amostra[rs.randint(len(amostra))]
    ids = np.empty(len(X), np.uint8)
    for i0 in range(0, len(X), 8192):
        b = X[i0:i0 + 8192]
        d = (b * b).sum(1)[:, None] - 2 * b @ C.T + (C * C).sum(1)[None, :]
        ids[i0:i0 + 8192] = d.argmin(1)
    Cr = (C / pes).reshape(256, 4, 4)
    cb = np.zeros((256, 4), np.uint16)
    for k in range(4):
        c = np.clip(np.round(Cr[:, k]), 0, 255).astype(np.uint16)
        cb[:, k] = ((c[:, 3] > 127).astype(np.uint16) << 15) | ((c[:, 0] >> 3) << 10) | ((c[:, 1] >> 3) << 5) | (c[:, 2] >> 3)
    # índices twiddled
    ys, xs = np.mgrid[0:q, 0:q]
    tw = np.zeros((q, q), np.int64)
    for b in range(10):
        tw |= ((ys >> b) & 1) << (2 * b) | ((xs >> b) & 1) << (2 * b + 1)
    out = np.zeros(q * q, np.uint8)
    out[tw.ravel()] = ids
    dados = cb.astype('<u2').tobytes() + out.tobytes()
    # prévia decodificada (para conferir a qualidade da compressão)
    dec = Cr[ids].reshape(q, q, 2, 2, 4).transpose(0, 3, 1, 2, 4).reshape(n, n, 4)
    return dados + bytes((-len(dados)) % 32), dec


if __name__ == "__main__":
    import sys, os
    pasta = sys.argv[1] if len(sys.argv) > 1 else "."
    for nome, (img, a1) in zip(("volante", "cockpit"), montar()):
        dados, dec = vq_1555(img, a1)
        d = np.clip(dec, 0, 255).astype(np.uint8)
        d[..., 3] = np.where(d[..., 3] > 127, 255, 0)
        Image.fromarray(d, 'RGBA').save(os.path.join(pasta, f"{nome}_vq.png"))
        print(nome, len(dados), "bytes")
