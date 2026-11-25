"""
converter_motor.py — MODO ESTUDO: transforma uma gravação de motor (.wav)
em estudo/motor_gravado.h para o som.c tocar no lugar do sintetizador.

Uso:  python converter_motor.py motor.wav [--ini 2.0] [--dur 1.2] [--rpm 16000]

  --ini  segundo onde começa o trecho (escolha um trecho de rotação ESTÁVEL,
         p.ex. um carro passando em alta ou um onboard em reta de pé no fundo)
  --dur  duração do trecho em segundos (0,5 a 2 s; vira um loop)
  --rpm  rotação do motor nesse trecho. Se omitido, o script estima a nota
         de explosão por autocorrelação.
  --cil  cilindros do motor gravado (6 = V6 turbo atual, 10 = V10 de 2000);
         nota de explosão = rpm x cil / 120 Hz

O arquivo gerado é SÓ para estudo local: não distribuir.
"""
import argparse, os, sys
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly

TAXA = 22050

ap = argparse.ArgumentParser()
ap.add_argument("wav")
ap.add_argument("--ini", type=float, default=None)
ap.add_argument("--dur", type=float, default=1.2)
ap.add_argument("--rpm", type=float, default=None)
ap.add_argument("--cil", type=int, default=6)
a = ap.parse_args()

taxa, x = wavfile.read(a.wav)
x = x.astype(np.float64)
if x.ndim > 1:
    x = x.mean(axis=1)
if np.issubdtype(x.dtype, np.integer) or np.abs(x).max() > 2:
    x /= np.abs(x).max() + 1e-9
# reamostra para 22050 Hz
from math import gcd
g = gcd(int(taxa), TAXA)
x = resample_poly(x, TAXA // g, int(taxa) // g)
total = len(x) / TAXA
print(f"gravação: {total:.1f} s")

n = int(a.dur * TAXA)
if a.ini is None:
    # escolhe sozinho o trecho mais "estável e forte" (energia alta, pouca variação)
    janela, melhor, ini = n, -1, 0
    for i in range(0, max(1, len(x) - janela), TAXA // 10):
        seg = x[i:i + janela]
        partes = np.array_split(seg, 6)
        e = np.array([np.sqrt((p ** 2).mean()) for p in partes])
        nota = e.mean() / (e.std() + 1e-3)
        if nota > melhor:
            melhor, ini = nota, i
    print(f"trecho escolhido automaticamente: {ini / TAXA:.2f} s")
else:
    ini = int(a.ini * TAXA)
seg = x[ini:ini + n].copy()
if len(seg) < TAXA // 4:
    sys.exit("trecho curto demais")
seg -= seg.mean()

# loop sem estalo: funde o fim com o começo (60 ms)
cf = int(0.06 * TAXA)
rampa = np.linspace(0, 1, cf)
seg[:cf] = seg[:cf] * rampa + seg[-cf:] * (1 - rampa)
seg = seg[:-cf]

# volume parecido com o do sintetizador
seg *= 0.30 / (np.sqrt((seg ** 2).mean()) + 1e-9)
seg = np.clip(seg, -0.98, 0.98)

if a.rpm:
    hz = a.rpm * a.cil / 120.0
else:
    # nota de explosão em alta: V6 ~450..650 Hz, V10 ~1200..1600 Hz
    ac = np.correlate(seg[:8192], seg[:8192], "full")[8191:]
    lo, hi = int(TAXA / 1700), int(TAXA / 350)
    # a "meia ordem" do escape cria um período 2x maior; prefere o MENOR
    # atraso que já tenha quase a correlação máxima (a nota de explosão)
    faixa = ac[lo:hi]
    picos = [i for i in range(1, len(faixa) - 1) if faixa[i] >= faixa[i - 1] and faixa[i] >= faixa[i + 1]]
    bons = [i for i in picos if faixa[i] >= 0.8 * faixa.max()]
    lag = lo + (bons[0] if bons else int(np.argmax(faixa)))
    hz = TAXA / lag
    print(f"nota estimada: {hz:.0f} Hz (~{hz * 120 / a.cil:.0f} rpm com {a.cil} cilindros). Se soar errado, use --rpm.")

pcm = (seg * 32767).astype(np.int16)
saida = os.path.join(os.path.dirname(os.path.abspath(__file__)), "motor_gravado.h")
with open(saida, "w", encoding="utf-8") as f:
    f.write("/* GERADO por converter_motor.py — gravação SÓ PARA ESTUDO, não distribuir. */\n")
    f.write(f"#define MG_N {len(pcm)}\n#define MG_HZ {hz:.2f}f\n")
    f.write(f"static const int16_t MG[MG_N] = {{\n")
    for i in range(0, len(pcm), 16):
        f.write(",".join(str(v) for v in pcm[i:i + 16]) + ",\n")
    f.write("};\n")
print(f"ok: {saida} ({len(pcm) / TAXA:.2f} s, nota {hz:.0f} Hz)")
