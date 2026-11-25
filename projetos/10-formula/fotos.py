# Fotos do usuário -> material para o céu e as fachadas (só a FORMA das nuvens e
# o desenho das fachadas; cores e estilo continuam os do jogo).
#   fotos_texturas/ceu/*.jpeg      -> camada de nuvens (limpo e fechado)
#   fotos_texturas/predios/*.jpeg  -> fachadas 128x128 sem emenda
import os, glob
import numpy as np
from PIL import Image, ImageFilter

AQUI = os.path.dirname(os.path.abspath(__file__))
PASTA = os.path.join(AQUI, "fotos_texturas")

# recortes (x0, y0, x1, y1 em frações), escolhidos à mão: só céu, sem postes,
# prédios, fios baixos nem a moldura do vidro do carro
CEU_LIMPO = {3: (0, 0, 1, .60), 4: (0, 0, 1, .60), 7: (0, 0, 1, .58), 12: (0, 0, .85, .88), 13: (0, 0, 1, .68),
             17: (0, 0, 1, .84), 19: (0, 0, 1, .74), 20: (0, 0, 1, .78), 22: (0, 0, 1, 1), 23: (0, 0, 1, .60),
             24: (0, 0, .9, .88), 25: (0, 0, 1, .52)}
CEU_FECHADO = {0: (0, 0, 1, .85), 1: (0, 0, 1, .55), 6: (0, 0, 1, .90), 9: (0, 0, 1, .55), 5: (.32, 0, 1, .58),
               14: (.32, 0, 1, .58), 10: (0, 0, 1, .50)}
# fachadas: foto e os 4 cantos de um trecho da fachada (sup-esq, sup-dir,
# inf-dir, inf-esq, em frações): a perspectiva é corrigida para ficar de frente
FACHADAS = {0: (8, ((.36, .37), (.98, .37), (.98, .98), (.36, .98))),     # residencial, sacadas verdes
            1: (0, ((.55, .13), (.97, .10), (.97, .75), (.55, .76))),     # vidro
            2: (3, ((.30, .22), (.95, .22), (.95, .74), (.30, .74))),     # reboco
            3: (2, ((.19, .36), (.75, .41), (.75, .92), (.17, .86))),     # painéis tijolo + sacadas
            5: (1, ((.00, .10), (.42, .09), (.45, .55), (.00, .56)))}     # residencial, janelas largas

def lista(sub):
    return sorted(glob.glob(os.path.join(PASTA, sub, "*.jp*g")) + glob.glob(os.path.join(PASTA, sub, "*.png")))


def recorte(arq, box, larg=None):
    im = Image.open(arq).convert("RGB")
    w, h = im.size
    im = im.crop((int(box[0] * w), int(box[1] * h), int(box[2] * w), int(box[3] * h)))
    if larg:
        im = im.resize((larg, max(8, int(im.size[1] * larg / im.size[0]))), Image.LANCZOS)
    return im


def nuvem_de_foto(im):
    """(alfa, luz) da nuvem: céu azul = saturado; nuvem = cinza/branco"""
    im = im.filter(ImageFilter.MedianFilter(5))                     # some com fios e antenas finas
    a = np.asarray(im, np.float32) / 255.0
    R, G, B = a[..., 0], a[..., 1], a[..., 2]
    V = a.max(-1)
    S = (V - a.min(-1)) / (V + 1e-3)
    azul = np.clip((B - R) / (B + 1e-3), 0, 1)
    alfa = np.clip((0.42 - azul) / 0.22, 0, 1) * np.clip((V - 0.25) / 0.2, 0, 1)
    K = float(os.environ.get("CONTRASTE", "1.0"))                     # 1 = atual; <1 = mais suave
    luz = np.clip((V - 0.50) / 0.45, 0, 1) ** 1.3
    luz = 0.62 + (luz - 0.62) * K
    return alfa, luz


def faixa_nuvens(fechado, W, Hh):
    """panorama de nuvens (W x Hh) emendando os recortes; a transição entre
    duas fotos é ONDULADA (varia com a altura), para não virar uma linha reta"""
    fs = lista("ceu")
    sel = CEU_FECHADO if fechado else CEU_LIMPO
    itens = [(fs[i], b) for i, b in sel.items() if i < len(fs)]
    if not itens:
        return None
    n = len(itens)
    passo = W / n
    larg = int(passo * 2.2)
    rs = np.random.RandomState(11)
    yy = np.arange(Hh)[:, None]
    A = np.zeros((Hh, W)); L = np.zeros((Hh, W)); P = np.zeros((Hh, W)) + 1e-6
    for k, (arq, box) in enumerate(itens):
        im = recorte(arq, box).resize((larg, Hh), Image.LANCZOS)
        al, lu = nuvem_de_foto(im)
        x = np.linspace(-1, 1, larg)[None, :]
        onda = 0.22 * np.sin(yy / Hh * (5 + 3 * rs.rand()) + rs.rand() * 6) + 0.10 * np.sin(yy / Hh * 17 + rs.rand() * 6)
        t = np.clip(1.0 - np.abs(x + onda) * 1.05, 0, 1)
        peso = (t * t * (3 - 2 * t)) ** 3 + 1e-4                       # domina no meio, some nas pontas
        x0 = int(k * passo - (larg - passo) / 2)
        cols = (np.arange(larg) + x0) % W
        np.add.at(A.T, cols, (al * peso).T); np.add.at(L.T, cols, (lu * peso).T); np.add.at(P.T, cols, peso.T)
    return A / P, L / P


def faixa_rgb(fechado, W, Hh):
    """panorama com as FOTOS COMO ESTÃO (sem estilizar): recortes emendados
    com transição ondulada e estreita (para não sobrepor duas fotos)"""
    fs = lista("ceu")
    sel = CEU_FECHADO if fechado else CEU_LIMPO
    itens = [(fs[i], b) for i, b in sel.items() if i < len(fs)]
    n = len(itens)
    passo = W / n
    larg = int(passo * 1.6)
    rs = np.random.RandomState(11)
    yy = np.arange(Hh)[:, None]
    C = np.zeros((Hh, W, 3)); P = np.zeros((Hh, W)) + 1e-6
    for k, (arq, box) in enumerate(itens):
        im = recorte(arq, box).resize((larg, Hh), Image.LANCZOS).filter(ImageFilter.MedianFilter(3))
        a = np.asarray(im, np.float32)
        x = np.linspace(-1, 1, larg)[None, :]
        onda = 0.15 * np.sin(yy / Hh * (5 + 3 * rs.rand()) + rs.rand() * 6) + 0.07 * np.sin(yy / Hh * 17 + rs.rand() * 6)
        t = np.clip(1.0 - np.abs(x + onda) * 1.05, 0, 1)
        peso = (t * t * (3 - 2 * t)) ** 6 + 1e-6
        x0 = int(k * passo - (larg - passo) / 2)
        cols = (np.arange(larg) + x0) % W
        for c in range(3):
            np.add.at(C[..., c].T, cols, (a[..., c] * peso).T)
        np.add.at(P.T, cols, peso.T)
    return C / P[..., None]


def estilizar_nuvens(alfa, luz, niveis=4, corte=0.18):
    """sombreamento em faixas (como os céus pintados da época), bordas macias;
    o topo de cada nuvem fica mais claro e a base mais escura (volume)"""
    a = np.clip((alfa - corte) / 0.45, 0, 1)
    a = a * a * (3 - 2 * a)
    acima = np.roll(a, 6, 0); acima[:6] = 0
    K = float(os.environ.get("CONTRASTE", "1.0"))
    volume = np.clip(0.50 + (a - acima) * 4.0 * K, 0, 1)              # borda de cima acesa, base escura
    l = 0.6 * luz + 0.4 * volume
    q = l * (niveis - 1)
    base = np.floor(q)
    f = np.clip(((q - base) - 0.35) / 0.3, 0, 1)
    return a, (base + f) / (niveis - 1)


def sem_emenda(im, borda=0.14):
    """repete sem costura: a faixa da borda vira uma transição para o lado oposto"""
    a = np.asarray(im, np.float32)
    for eixo in (1, 0):
        n = a.shape[eixo]; b = int(n * borda)
        t = np.linspace(0, 1, b)
        t = t * t * (3 - 2 * t)
        if eixo == 1:
            meio = a[:, :n - b].copy()
            meio[:, :b] = a[:, :b] * t[None, :, None] + a[:, n - b:] * (1 - t[None, :, None])
        else:
            meio = a[:n - b].copy()
            meio[:b] = a[:b] * t[:, None, None] + a[n - b:] * (1 - t[:, None, None])
        a = meio
    return a


def fachada(tipo, N=128):
    if tipo not in FACHADAS:
        return None
    fs = lista("predios")
    i, q = FACHADAS[tipo]
    if i >= len(fs):
        return None
    im = Image.open(fs[i]).convert("RGB")
    w, h = im.size
    tl, tr, br, bl = [(x * w, y * h) for x, y in q]
    M = int(N * 1.2)
    im = im.transform((M, M), Image.QUAD, data=(*tl, *bl, *br, *tr), resample=Image.BICUBIC)
    im = im.filter(ImageFilter.MedianFilter(3))
    a = sem_emenda(im)
    im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).resize((N, N), Image.LANCZOS)
    a = np.asarray(im, np.float32)
    g = a.mean(-1, keepdims=True)
    a = g + (a - g) * 0.8                                                # um pouco menos saturada
    a = (a - 128) * 1.10 + 136                                           # mais contraste, levemente clara
    return np.clip(a, 0, 255)
