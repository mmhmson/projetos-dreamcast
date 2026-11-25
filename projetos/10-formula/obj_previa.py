"""Prévia de um .obj (renderizador simples por software, com textura):
uso: py -I obj_previa.py modelo.obj textura.png saida.png"""
import sys
import numpy as np
from PIL import Image


def ler_obj(caminho):
    V, T, N, F = [], [], [], []
    for ln in open(caminho, encoding='utf-8', errors='ignore'):
        p = ln.split()
        if not p: continue
        if p[0] == 'v': V.append([float(x) for x in p[1:4]])
        elif p[0] == 'vt': T.append([float(x) for x in p[1:3]])
        elif p[0] == 'vn': N.append([float(x) for x in p[1:4]])
        elif p[0] == 'f':
            f = []
            for c in p[1:]:
                k = c.split('/')
                f.append((int(k[0]) - 1, int(k[1]) - 1 if len(k) > 1 and k[1] else -1))
            F.append(f)
    return np.array(V), np.array(T) if T else np.zeros((1, 2)), F


def render(V, T, F, tex, yaw, pitch, W=640, H=420, escala=150):
    cy, sy, cp, sp = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    P = (V - V.mean(0)) @ Ry.T @ Rx.T
    img = np.full((H, W, 3), 60, np.float32)
    zb = np.full((H, W), -1e9, np.float32)
    tw, th = tex.size
    tx = np.asarray(tex.convert('RGB'), np.float32)
    L = np.array([-0.4, 0.7, 0.6]); L /= np.linalg.norm(L)
    for f in F:
        for k in range(1, len(f) - 1):
            idx = [f[0], f[k], f[k + 1]]
            p = np.array([P[i[0]] for i in idx])
            uv = np.array([T[i[1]] if i[1] >= 0 else (0, 0) for i in idx])
            n = np.cross(p[1] - p[0], p[2] - p[0]); nl = np.linalg.norm(n)
            if nl < 1e-9: continue
            n /= nl
            sh = 0.45 + 0.55 * abs(n @ L)
            xs = W / 2 + p[:, 0] * escala; ys = H / 2 - p[:, 1] * escala
            x0, x1 = int(max(0, xs.min())), int(min(W - 1, xs.max() + 1))
            y0, y1 = int(max(0, ys.min())), int(min(H - 1, ys.max() + 1))
            if x1 < x0 or y1 < y0: continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0])
            if abs(d) < 1e-9: continue
            w1 = ((gx - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (gy - ys[0])) / d
            w2 = ((xs[1] - xs[0]) * (gy - ys[0]) - (gx - xs[0]) * (ys[1] - ys[0])) / d
            w0 = 1 - w1 - w2
            m = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
            z = w0 * p[0, 2] + w1 * p[1, 2] + w2 * p[2, 2]
            sub = zb[y0:y1 + 1, x0:x1 + 1]
            m &= z > sub
            if not m.any(): continue
            u = w0 * uv[0, 0] + w1 * uv[1, 0] + w2 * uv[2, 0]
            v = w0 * uv[0, 1] + w1 * uv[1, 1] + w2 * uv[2, 1]
            ui = np.clip((u % 1.0) * tw, 0, tw - 1).astype(int); vi = np.clip((1 - v % 1.0) * th, 0, th - 1).astype(int)
            cor = tx[vi, ui] * sh
            sub[m] = z[m]
            img[y0:y1 + 1, x0:x1 + 1][m] = cor[m]
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


if __name__ == "__main__":
    V, T, F = ler_obj(sys.argv[1])
    print("vértices", len(V), "faces", len(F), "min", V.min(0).round(2), "max", V.max(0).round(2))
    tex = Image.open(sys.argv[2])
    vistas = [(0.6, 0.35), (2.5, 0.30), (np.pi / 2, 0.0), (0.0, 1.45)]
    m = Image.new('RGB', (1280, 840))
    for k, (yw, pt) in enumerate(vistas):
        m.paste(render(V, T, F, tex, yw, pt), ((k % 2) * 640, (k // 2) * 420))
    m.save(sys.argv[3])
