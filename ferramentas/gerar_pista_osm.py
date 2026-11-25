#!/usr/bin/env python3
"""
gerar_pista_osm.py — transforma o traçado REAL de um circuito (OpenStreetMap)
numa pista do ASFALTO (lista de segmentos com curva e altura).

Uso: py -I gerar_pista_osm.py <circuito> <saida.h> [--previa png]
     circuito: interlagos | silverstone | abudhabi   (configurações abaixo)

Dados:
  - Traçado: © colaboradores do OpenStreetMap, licença ODbL
    (https://www.openstreetmap.org/copyright), baixado da Overpass API:
      way["highway"="raceway"](sul,oeste,norte,leste); out tags geom;
    -> dados_osm/<circuito>_raw.json
  - Relevo: tiles "Terrarium" (AWS Open Data, elevation-tiles-prod; fontes
    SRTM/NASA e outras, domínio público/abertas). Cada pixel do PNG codifica a
    altitude: h = R*256 + G + B/256 - 32768. Sem API e sem limite de pedidos
    (a api.open-elevation.com, usada antes, devolvia HTTP 429). Tiles em cache
    em dados_osm/tiles/.
  - Ambiente (prédios, água, matas): Overpass, dados_osm/<circuito>_ambiente.json

Passo a passo:
  1. ENCADEAR: o circuito vem picado em vários "ways". Um autódromo tem
     variantes (traçados antigos, pistas de kart, pit lane), então damos a
     SEQUÊNCIA de trechos do traçado de GP e o script confere se cada um
     começa onde o anterior termina.
  2. METROS: lat/lon -> metros (projeção equiretangular; erro desprezível).
  3. REAMOSTRAR a cada METROS_SEG metros = 1 segmento do jogo.
  4. CURVA = quanto a DIREÇÃO muda por segmento, suavizada, vezes um ganho.
  5. ALTURA: relevo SRTM, mediana (tira saltos) + suavização larga.
  6. EXTRAS: reta dos boxes (onde o pit lane corre colado na pista, e de que
     lado), zonas de frenagem (para as placas 300/200/100) e marcos
     (o hotel sobre a pista em Abu Dhabi).
  7. AMBIENTE REAL: para cada segmento, há água / mata à esquerda ou direita?
     E os PRÉDIOS de verdade ao redor (posição, tamanho, altura do OSM),
     convertidos para "segmento + distância lateral" da pista.
  8. Gera um .h com tudo + contorno para o MINIMAPA + nomes das curvas.
"""

import json
import math
import os
import sys
import time
import urllib.request

METROS_SEG = 1.0                  # 1 segmento do jogo = 1 m
UNID_POR_M = 200.0 / METROS_SEG   # SEG_LEN (200 unidades) por segmento
CURVA_MAX = 7.5
PONTOS_MAPA = 256
CORRECOES = {"Pinherinho": "Pinheirinho"}   # erro de digitação no OSM

PASTA = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dados_osm")

CIRCUITOS = {
    # sentido anti-horário; largada no começo do primeiro trecho
    "interlagos": dict(
        prefixo="INTERLAGOS", exagero=1.6,
        trechos=[138567241, 131439569, 189535473, 807691487, 138566995, 138567001, 138566996,
                 807691486, 138567000, 138566997, 138566994, 138567002, 190164735, 138566998,
                 138566993, 190164734, 138566999, 138567236, 189535472, 190164736, 138567242,
                 190164737, 138567239, 138567234, 190164733]),
    # traçado "Arena" (desde 2011), sentido horário, largada na reta Hamilton
    "silverstone": dict(
        prefixo="SILVERSTONE", exagero=2.0,
        trechos=[55224167, 169854842, 169800226, 169800223, 169800225, 169848882, 169800224,
                 169800222, 169618242, 169618240, 169618241, 169618245, 169609611, 169730588,
                 3571477, 169730585, 169730587, 169733768, 169730586, 430075118, 169733766,
                 169733769, 169733770, 169848880, 169848884, 169848881, 55224168]),
    # traçado 2021+, anti-horário; largada na reta dos boxes (ao lado da
    # "Main Grandstand" do OSM, em 24.4703, 54.6056)
    "abudhabi": dict(
        prefixo="ABUDHABI", exagero=1.0,
        trechos=[168477013, 1469184731, 1469184732, 188337196, 1255169703, 1255169704],
        largada=(24.4692, 54.6058),
        ignorar_nomes={"Yas Marina Circuit", "YAS Marina Circuit"},
        # o pit lane do OSM aqui mistura entrada, saída e túnel (oscila 15-98 m da
        # pista): definido à mão, do lado DIREITO (conferido: 15-19 m da pista)
        boxes_manual=[(4960, 5282 + 220, 1)],
        marcos={"HOTEL": (24.46742, 54.60571)},
        setores=True),
}


def eh_pit(tags):
    nome = tags.get("name", "").lower()
    return (tags.get("raceway") == "pit_lane" or "pit lane" in nome) and \
        tags.get("sport", "motor").startswith("motor") and not nome.startswith("stowe")


def encadear(dados, ids, ignorar=()):
    por_id = {e["id"]: e for e in dados["elements"]}
    chave = lambda p: (round(p["lat"], 6), round(p["lon"], 6))
    pontos, nomes = [], []
    for k, i in enumerate(ids):
        w = por_id[i]
        if k and chave(w["geometry"][0]) != chave(por_id[ids[k - 1]]["geometry"][-1]):
            raise SystemExit(f"trecho {i} não começa onde o {ids[k-1]} termina")
        nome = w.get("tags", {}).get("name")
        nome = CORRECOES.get(nome, nome)
        if nome in ignorar:
            nome = None
        if nome and (not nomes or nomes[-1][1] != nome):
            nomes.append((len(pontos), nome))
        pontos += [(p["lat"], p["lon"]) for p in w["geometry"][:-1]]
    if chave(por_id[ids[-1]]["geometry"][-1]) != chave(por_id[ids[0]]["geometry"][0]):
        raise SystemExit("o traçado não fecha a volta")
    print(f"encadeados {len(ids)} trechos, {len(pontos)} pontos")
    return pontos, nomes


class Projecao:
    def __init__(self, pontos):
        self.lat0 = sum(p[0] for p in pontos) / len(pontos)
        self.lon0 = sum(p[1] for p in pontos) / len(pontos)
        self.k = math.cos(math.radians(self.lat0))

    def xy(self, lat, lon):
        return ((lon - self.lon0) * 111320.0 * self.k, (lat - self.lat0) * 110540.0)

    def latlon(self, x, y):
        return (self.lat0 + y / 110540.0, self.lon0 + x / (111320.0 * self.k))


def reamostrar(xy, passo):
    n = len(xy)
    acum = [0.0]
    for i in range(n):
        acum.append(acum[-1] + math.dist(xy[i], xy[(i + 1) % n]))
    total = acum[-1]
    m = int(round(total / passo))
    saida, j = [], 0
    for k in range(m):
        d = k * total / m
        while acum[j + 1] < d:
            j += 1
        a, b = xy[j], xy[(j + 1) % n]
        t = (d - acum[j]) / max(1e-9, acum[j + 1] - acum[j])
        saida.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
    return saida, total, acum


def suavizar_circular(v, raio):
    n = len(v)
    pesos = [math.exp(-0.5 * (k / (raio / 2.0)) ** 2) for k in range(-raio, raio + 1)]
    soma = sum(pesos)
    return [sum(pesos[k + raio] * v[(i + k) % n] for k in range(-raio, raio + 1)) / soma for i in range(n)]


def relevo_terrarium(pontos_latlon, cache, z=15):
    """Altitude (m) de cada ponto, lida dos tiles PNG Terrarium (interpolação bilinear)."""
    if os.path.exists(cache):
        alt = json.load(open(cache, encoding="utf-8"))
        if len(alt) == len(pontos_latlon):
            return alt
    from PIL import Image
    pasta = os.path.join(PASTA, "tiles")
    tiles = {}

    def tile(tx, ty):
        if (tx, ty) not in tiles:
            arq = os.path.join(pasta, f"{z}_{tx}_{ty}.png")
            if not os.path.exists(arq):
                os.makedirs(pasta, exist_ok=True)
                url = f"https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{tx}/{ty}.png"
                open(arq, "wb").write(urllib.request.urlopen(
                    urllib.request.Request(url, headers={"User-Agent": "dc-estudo/1.0"}), timeout=60).read())
            tiles[(tx, ty)] = Image.open(arq).convert("RGB").load()
        return tiles[(tx, ty)]

    def h(px, py):                                      # altitude de um pixel GLOBAL
        t = tile(int(px) // 256, int(py) // 256)
        r, g, b = t[int(px) % 256, int(py) % 256]
        return r * 256 + g + b / 256 - 32768

    alturas = []
    n = 2 ** z * 256
    for lat, lon in pontos_latlon:
        x = (lon + 180) / 360 * n
        y = (1 - math.log(math.tan(math.radians(lat)) + 1 / math.cos(math.radians(lat))) / math.pi) / 2 * n
        x0, y0 = math.floor(x - 0.5), math.floor(y - 0.5)
        fx, fy = x - 0.5 - x0, y - 0.5 - y0
        alturas.append(h(x0, y0) * (1 - fx) * (1 - fy) + h(x0 + 1, y0) * fx * (1 - fy) +
                       h(x0, y0 + 1) * (1 - fx) * fy + h(x0 + 1, y0 + 1) * fx * fy)
    print(f"  relevo: {len(tiles)} tiles Terrarium")
    json.dump(alturas, open(cache, "w", encoding="utf-8"))
    return alturas


# ---------------------------------------------------------------------------
# AMBIENTE: polígonos do OSM (água, mata, prédios)
# ---------------------------------------------------------------------------
def poligonos(dados, proj, filtro):
    """Lista de (pontos_xy, bbox, tags) para ways e relações (anéis externos)."""
    out = []
    for e in dados["elements"]:
        t = e.get("tags", {})
        if not filtro(t):
            continue
        aneis = []
        if e["type"] == "way" and "geometry" in e:
            aneis = [e["geometry"]]
        elif e["type"] == "relation":
            aneis = [m["geometry"] for m in e.get("members", []) if m.get("role") == "outer" and "geometry" in m]
        for g in aneis:
            xy = [proj.xy(p["lat"], p["lon"]) for p in g]
            if len(xy) < 3:
                continue
            xs = [q[0] for q in xy]; ys = [q[1] for q in xy]
            out.append((xy, (min(xs), min(ys), max(xs), max(ys)), t))
    return out


def dentro(p, pol):
    xy, (x0, y0, x1, y1), _ = pol
    if not (x0 <= p[0] <= x1 and y0 <= p[1] <= y1):
        return False
    c, n = False, len(xy)
    for i in range(n):                                  # "ray casting"
        a, b = xy[i], xy[(i + 1) % n]
        if (a[1] > p[1]) != (b[1] > p[1]) and p[0] < a[0] + (p[1] - a[1]) * (b[0] - a[0]) / (b[1] - a[1]):
            c = not c
    return c


def carregar_costa(arq, proj):
    """Segmentos da linha de costa (natural=coastline). Convenção do OSM:
    a TERRA fica à esquerda do sentido do desenho, a ÁGUA à direita."""
    if not os.path.exists(arq):
        return []
    segs = []
    for e in json.load(open(arq, encoding="utf-8"))["elements"]:
        if e.get("tags", {}).get("natural") == "coastline" and "geometry" in e:
            g = [proj.xy(p["lat"], p["lon"]) for p in e["geometry"]]
            segs += list(zip(g, g[1:]))
    return segs


def no_mar(p, costa, raio=400):
    """O ponto está na água? Olha o trecho de costa mais próximo (até 400 m)."""
    melhor, lado = raio * raio, False
    for a, b in costa:
        dx, dy = b[0] - a[0], b[1] - a[1]
        l2 = dx * dx + dy * dy or 1
        t = max(0, min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2))
        cx, cy = a[0] + dx * t, a[1] + dy * t
        d2 = (p[0] - cx) ** 2 + (p[1] - cy) ** 2
        if d2 < melhor:
            melhor = d2
            lado = dx * (p[1] - a[1]) - dy * (p[0] - a[0]) < 0      # à direita = água
    return lado


def altura_predio(t):
    try:
        return float(t["height"].split()[0])
    except (KeyError, ValueError):
        pass
    try:
        return float(t["building:levels"]) * 3.2 + 1.5
    except (KeyError, ValueError):
        pass
    return {"house": 6, "residential": 6, "detached": 6, "garage": 3, "shed": 3, "roof": 6,
            "industrial": 9, "warehouse": 9, "hangar": 11, "grandstand": 12, "commercial": 12,
            "apartments": 20, "hotel": 30, "office": 25}.get(t.get("building"), 7)


def mais_proximo(pts, p):
    return min(range(len(pts)), key=lambda i: (pts[i][0] - p[0]) ** 2 + (pts[i][1] - p[1]) ** 2)


def sem_acento(s):
    tr = str.maketrans("ãáâàéêíóôõúçÃÁÂÀÉÊÍÓÔÕÚÇ", "aaaaeeiooouc" + "AAAAEEIOOOUC")
    return s.translate(tr).upper()


def main():
    nome_circ, saida = sys.argv[1], sys.argv[2]
    previa = sys.argv[sys.argv.index("--previa") + 1] if "--previa" in sys.argv else None
    cfg = CIRCUITOS[nome_circ]
    P = cfg["prefixo"]
    dados = json.load(open(os.path.join(PASTA, nome_circ + "_raw.json"), encoding="utf-8"))
    latlon, nomes = encadear(dados, cfg["trechos"], cfg.get("ignorar_nomes", ()))
    proj = Projecao(latlon)
    xy = [proj.xy(*p) for p in latlon]
    pts, total, acum = reamostrar(xy, METROS_SEG)
    n = len(pts)
    curvas_nomeadas = [(int(acum[i] / total * n), nm) for i, nm in nomes]

    # largada em outro ponto? gira a lista (o segmento 0 é a linha de chegada)
    if "largada" in cfg:
        r = mais_proximo(pts, proj.xy(*cfg["largada"]))
        pts = pts[r:] + pts[:r]
        curvas_nomeadas = sorted(((s - r) % n, nm) for s, nm in curvas_nomeadas)
    print(f"comprimento: {total:.0f} m -> {n} segmentos de {METROS_SEG} m")

    # ---- curvatura ----
    rumo = [math.atan2(pts[(i + 1) % n][1] - pts[i][1], pts[(i + 1) % n][0] - pts[i][0]) for i in range(n)]
    dtheta = [((rumo[(i + 1) % n] - rumo[i]) + math.pi) % (2 * math.pi) - math.pi for i in range(n)]
    giro = math.degrees(sum(dtheta))
    print(f"giro total: {giro:.0f} graus ({'anti-horario' if giro > 0 else 'horario'})")
    dtheta = suavizar_circular(dtheta, int(28 / METROS_SEG))
    maxd = max(abs(d) for d in dtheta)
    # no jogo, curva > 0 = DIREITA; na matemática, ângulo + = ESQUERDA: daí o "-".
    # LINEAR (curva proporcional a 1/raio), como na geometria real: assim a
    # física do jogo (força centrífuga ~ v²·curva) dá velocidades de curva
    # proporcionais a raiz(raio), como num carro de verdade.
    curva = [-CURVA_MAX * d / maxd for d in dtheta]
    curva = [0.0 if abs(c) < 0.35 else c for c in curva]

    # ---- relevo ----
    passo_rel = int(24 / METROS_SEG)
    amostras_ll = [proj.latlon(*pts[i]) for i in range(0, n, passo_rel)]
    alt = relevo_terrarium(amostras_ll, os.path.join(PASTA, nome_circ + "_relevo.json"))
    m = len(alt)
    alt = [sorted(alt[(i + k) % m] for k in range(-3, 4))[3] for i in range(m)]
    alt_seg = []
    for i in range(n):
        a = i / passo_rel
        j = int(a) % m
        t = a - int(a)
        alt_seg.append(alt[j] * (1 - t) + alt[(j + 1) % m] * t)
    # 90 m: tira o ruído de 2-3 m do dado sem apagar cristas e mergulhos
    alt_seg = suavizar_circular(alt_seg, int(90 / METROS_SEG))
    y = [(h - alt_seg[0]) * UNID_POR_M * cfg["exagero"] for h in alt_seg]
    print(f"relevo: {min(alt_seg):.0f}..{max(alt_seg):.0f} m (desnivel {max(alt_seg)-min(alt_seg):.0f} m)")

    # ---- reta dos boxes: segmentos com o pit lane a menos de 35 m, ao lado ----
    dmax = cfg.get("dist_pit", 35)
    pit = []
    for e in dados["elements"]:
        if eh_pit(e.get("tags", {})):
            g = [proj.xy(p["lat"], p["lon"]) for p in e["geometry"]]
            for a, b in zip(g, g[1:]):                        # densifica a cada 5 m
                k = max(1, int(math.dist(a, b) / 5))
                pit += [(a[0] + (b[0] - a[0]) * t / k, a[1] + (b[1] - a[1]) * t / k) for t in range(k)]
    lados = []
    for i in range(n):
        p = pts[i]
        perto = [q for q in pit if abs(q[0] - p[0]) < dmax and abs(q[1] - p[1]) < dmax]
        if len(perto) < 2:
            lados.append(0)
            continue
        q = min(perto, key=lambda q: (q[0] - p[0]) ** 2 + (q[1] - p[1]) ** 2)
        if not (cfg.get("dist_min", 8) < math.dist(q, p) < dmax):                    # cruzamento/entrada: ignora
            lados.append(0)
            continue
        dx, dy = pts[(i + 1) % n][0] - p[0], pts[(i + 1) % n][1] - p[1]
        cruz = dx * (q[1] - p[1]) - dy * (q[0] - p[0])        # > 0: pit à ESQUERDA
        lados.append(-1 if cruz > 0 else 1)
    # trechos contínuos do mesmo lado (tolerando falhas de até 20 m)
    runs, i = [], 0
    while i < n:
        if lados[i]:
            j, falha = i, 0
            while j + 1 < n and falha <= 20:
                j += 1
                falha = 0 if lados[j] == lados[i] else falha + 1
            runs.append([i, j - falha, lados[i]])
            i = j + 1
        else:
            i += 1
    if len(runs) > 1 and runs[0][0] == 0 and runs[-1][1] >= n - 21 and runs[0][2] == runs[-1][2]:
        runs[-1][1] = runs[0][1] + n                          # passa pela largada: junta
        runs.pop(0)
    # só pit lanes de verdade (>= 400 m); entradas e saídas curtas ficam de fora
    boxes = [tuple(r) for r in runs if (r[1] - r[0]) * METROS_SEG >= cfg.get("min_boxes", 400)]
    if "boxes_manual" in cfg:
        boxes = cfg["boxes_manual"]
    print("boxes:", boxes)

    # ---- zonas de frenagem: curva forte depois de >= 200 m quase retos ----
    frenagens, reto = [], 0
    for i in range(n + 400):                                  # +400: a volta é um laço
        c = abs(curva[i % n])
        if c < 2.5:
            reto += 1
            continue
        # começou uma curva: ela é FORTE (>= 4 nos próximos 80 m)? veio de reta longa?
        if reto * METROS_SEG >= 200 and max(abs(curva[(i + k) % n]) for k in range(80)) >= 3.5:
            if i % n not in frenagens:
                frenagens.append(i % n)
        reto = 0
    frenagens.sort()
    print("frenagens:", frenagens)

    # ---- marcos (hotel) e setores ----
    hotel = (-1, -1)
    for nm, ll in cfg.get("marcos", {}).items():
        c = mais_proximo(pts, proj.xy(*ll))
        curvas_nomeadas.append((max(0, c - 120), nm))
        hotel = (c - 70, c + 70)
        print(f"marco {nm}: segmento {c}")
    if cfg.get("setores"):
        curvas_nomeadas += [(0, "SETOR 1"), (n // 3, "SETOR 2"), (2 * n // 3, "SETOR 3")]
    curvas_nomeadas.sort()

    # ---- AMBIENTE: água e mata de cada lado (amostras a 40 e 90 m da pista) ----
    amb_arq = os.path.join(PASTA, nome_circ + "_ambiente.json")
    ambiente = [0] * n
    predios = []
    if os.path.exists(amb_arq):
        amb = json.load(open(amb_arq, encoding="utf-8"))
        agua = poligonos(amb, proj, lambda t: t.get("natural") == "water" or t.get("landuse") == "reservoir")
        mata = poligonos(amb, proj, lambda t: t.get("natural") == "wood" or t.get("landuse") == "forest")
        costa = carregar_costa(os.path.join(PASTA, nome_circ + "_costa.json"), proj)
        for i in range(n):
            p, q = pts[i], pts[(i + 1) % n]
            l = math.dist(p, q) or 1
            nx, ny = -(q[1] - p[1]) / l, (q[0] - p[0]) / l          # normal para a ESQUERDA
            bits = 0
            for lado, (b_agua, b_mata) in ((1, (1, 4)), (-1, (2, 8))):
                for d in (40, 90):
                    s_ = (p[0] + nx * d * lado, p[1] + ny * d * lado)
                    if any(dentro(s_, w) for w in agua) or (costa and no_mar(s_, costa)):
                        bits |= b_agua
                    if any(dentro(s_, w) for w in mata):
                        bits |= b_mata
            ambiente[i] = bits
        # filtro de maioria (+-15 m): sem "piscar" de um segmento para o outro
        amb2 = []
        for i in range(n):
            b = 0
            for bit in (1, 2, 4, 8):
                if sum(1 for k in range(-15, 16) if ambiente[(i + k) % n] & bit) > 15:
                    b |= bit
            amb2.append(b)
        ambiente = amb2
        print("ambiente: água em", sum(1 for b in ambiente if b & 3), "segmentos, mata em", sum(1 for b in ambiente if b & 12))

        # ---- PRÉDIOS REAIS: posição relativa à pista ----
        cand = []
        for xy, bb, t in poligonos(amb, proj, lambda t: "building" in t):
            area = 0.5 * abs(sum(xy[k][0] * xy[(k + 1) % len(xy)][1] - xy[(k + 1) % len(xy)][0] * xy[k][1] for k in range(len(xy))))
            if area < 60:
                continue
            c = (sum(q[0] for q in xy) / len(xy), sum(q[1] for q in xy) / len(xy))
            i = mais_proximo(pts, c)
            p, q = pts[i], pts[(i + 1) % n]
            l = math.dist(p, q) or 1
            tx, ty = (q[0] - p[0]) / l, (q[1] - p[1]) / l
            nx, ny = -ty, tx
            ao_longo = [(v[0] - p[0]) * tx + (v[1] - p[1]) * ty for v in xy]
            lateral = [-((v[0] - p[0]) * nx + (v[1] - p[1]) * ny) for v in xy]    # + = DIREITA (jogo)
            lat_c = (max(lateral) + min(lateral)) / 2
            larg, prof = max(lateral) - min(lateral), max(ao_longo) - min(ao_longo)
            perto = abs(lat_c) - larg / 2
            if perto < 14 or perto > 320 or larg > 160 or prof > 160:
                continue
            seg_c = (i + int(round((max(ao_longo) + min(ao_longo)) / 2 / METROS_SEG))) % n
            if any((a - 60) <= seg_c + k * n <= (b + 60) and abs(lat_c) < 45 for a, b, _ in boxes for k in (0, 1)):
                continue                                        # já tem boxes/arquibancada nossos
            alt_m = altura_predio(t)
            tipo = 1 if t.get("building") == "grandstand" else (2 if t.get("building") == "hangar" else 0)
            importancia = area * min(alt_m, 60) / (1 + perto / 60)
            cand.append((importancia, seg_c, lat_c, larg, prof, alt_m, tipo))
        cand.sort(reverse=True)
        predios = sorted(c_[1:] for c_ in cand[:700])
        print(f"prédios: {len(cand)} candidatos, {len(predios)} usados")

    # ---- minimapa ----
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    lado = max(max(xs) - min(xs), max(ys) - min(ys))
    mapa = [((pts[k * n // PONTOS_MAPA][0] - min(xs)) / lado, 1.0 - (pts[k * n // PONTOS_MAPA][1] - min(ys)) / lado)
            for k in range(PONTOS_MAPA)]
    mw = (max(xs) - min(xs)) / lado; mh = (max(ys) - min(ys)) / lado

    # ---- .h ----
    with open(saida, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"/* GERADO por ferramentas/gerar_pista_osm.py {nome_circ} — não editar à mão.\n")
        f.write(" * Traçado: (c) colaboradores do OpenStreetMap, ODbL. Relevo: SRTM (NASA).\n")
        f.write(f" * {total:.0f} m reais, {n} segmentos de {METROS_SEG} m. */\n\n#include \"pista_osm.h\"\n\n")
        f.write(f"static const float {P}_PISTA[{n}][2] = {{\n")
        f.writelines(f"    {{{curva[i]:.3f}f, {y[i]:.1f}f}},\n" for i in range(n))
        f.write(f"}};\n\nstatic const float {P}_MAPA[{PONTOS_MAPA}][2] = {{\n")
        f.writelines(f"    {{{p[0]:.4f}f, {p[1]:.4f}f}},\n" for p in mapa)
        f.write(f"}};\n\nstatic const osm_curva_t {P}_CURVAS[] = {{\n")
        f.writelines(f"    {{{s}, \"{sem_acento(nm)}\"}},\n" for s, nm in curvas_nomeadas)
        f.write(f"}};\n\nstatic const osm_trecho_t {P}_BOXES[] = {{\n")
        f.writelines(f"    {{{a}, {b}, {l}}},\n" for a, b, l in boxes)
        if not boxes:
            f.write("    {0, 0, 0},\n")
        f.write(f"}};\n\nstatic const int {P}_FRENAGENS[] = {{ {', '.join(map(str, frenagens or [-1]))} }};\n\n")
        f.write("/* rumo real de cada segmento, em décimos de grau (0 = leste, 900 = norte, anti-horário) */\n")
        f.write(f"static const short {P}_RUMO[{n}] = {{")
        for i in range(n):
            f.write(("\n    " if i % 24 == 0 else "") + f"{int(round(math.degrees(rumo[i]) * 10)) % 3600},")
        f.write("\n};\n\n")
        f.write("/* ambiente por segmento: 1 água esq, 2 água dir, 4 mata esq, 8 mata dir */\n")
        f.write(f"static const unsigned char {P}_AMBIENTE[{n}] = {{")
        for i in range(n):
            f.write(("\n    " if i % 32 == 0 else "") + f"{ambiente[i]},")
        f.write("\n};\n\n/* prédios reais: segmento, lateral (m, + = direita), largura, profundidade, altura, tipo */\n")
        f.write(f"static const osm_predio_t {P}_PREDIOS[] = {{\n")
        f.writelines(f"    {{{a}, {b:.1f}f, {c:.1f}f, {d:.1f}f, {e:.1f}f, {g}}},\n" for a, b, c, d, e, g in predios)
        if not predios:
            f.write("    {0, 0, 0, 0, 0, 0},\n")
        f.write("};\n\n")
        f.write(f"static const pista_osm_t PISTA_{P} = {{\n")
        f.write(f"    {n}, {P}_PISTA, {PONTOS_MAPA}, {P}_MAPA, {mw:.4f}f, {mh:.4f}f,\n")
        f.write(f"    {len(curvas_nomeadas)}, {P}_CURVAS, {len(boxes)}, {P}_BOXES,\n")
        f.write(f"    {hotel[0]}, {hotel[1]}, {len(frenagens)}, {P}_FRENAGENS,\n")
        f.write(f"    {P}_AMBIENTE, {len(predios)}, {P}_PREDIOS, {P}_RUMO\n}};\n")
    print("gerado", saida, "| curvas:", ", ".join(f"{sem_acento(nm)}@{s}" for s, nm in curvas_nomeadas))

    if previa:
        from PIL import Image, ImageDraw
        img = Image.new("RGB", (900, 600), (20, 24, 30))
        d = ImageDraw.Draw(img)
        sx = lambda v: 30 + (v[0] - min(xs)) / lado * 540
        sy = lambda v: 570 - (v[1] - min(ys)) / lado * 540
        for k in range(n):
            c = curva[k]
            cor = (255, int(255 - min(255, abs(c) * 34)), 60) if c > 0 else (int(255 - min(255, abs(c) * 34)), 200, 255)
            d.line((sx(pts[k]), sy(pts[k]), sx(pts[(k + 1) % n]), sy(pts[(k + 1) % n])), fill=cor, width=4)
        for a, b, l in boxes:
            for k in range(a, b, 4):
                k %= n
                d.ellipse((sx(pts[k]) - 2, sy(pts[k]) - 2, sx(pts[k]) + 2, sy(pts[k]) + 2), fill=(255, 0, 255))
        for k in frenagens:
            d.ellipse((sx(pts[k]) - 5, sy(pts[k]) - 5, sx(pts[k]) + 5, sy(pts[k]) + 5), outline=(255, 60, 60), width=2)
        d.ellipse((sx(pts[0]) - 6, sy(pts[0]) - 6, sx(pts[0]) + 6, sy(pts[0]) + 6), fill=(255, 255, 255))
        for s, nm in curvas_nomeadas:
            d.text((sx(pts[s]) + 6, sy(pts[s])), sem_acento(nm), fill=(255, 255, 255))
        for k in range(n - 1):
            d.line((600 + k * 290 / n, 300 - y[k] / 60, 600 + (k + 1) * 290 / n, 300 - y[k + 1] / 60), fill=(120, 255, 120))
            d.line((600 + k * 290 / n, 480 - curva[k] * 8, 600 + (k + 1) * 290 / n, 480 - curva[k + 1] * 8), fill=(255, 160, 60))
        d.text((600, 160), "altura", fill=(120, 255, 120)); d.text((600, 400), "curva (+ = direita)", fill=(255, 160, 60))
        d.text((30, 10), f"{nome_circ}: {total:.0f} m | branco = largada, magenta = boxes, vermelho = frenagem", fill=(255, 255, 255))
        img.save(previa)


if __name__ == "__main__":
    main()
