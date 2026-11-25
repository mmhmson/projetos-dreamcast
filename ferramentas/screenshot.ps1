# Captura SÓ uma janela do Flycast (não a tela inteira) e salva num PNG.
# Uso: .\ferramentas\screenshot.ps1 saida.png [-Titulo "Serial"]
#   -Titulo: parte do título da janela (padrão: "Flycast -", a janela do jogo).
#            Use "Serial" para a janela "Flycast Serial Output".
#   -Maximizar: maximiza a janela antes (útil para ver mais linhas do serial).
param(
    [Parameter(Mandatory)][string]$Saida,
    [string]$Titulo = 'Flycast -',
    [switch]$Maximizar
)

Add-Type -AssemblyName System.Drawing
if (-not ('JanelaFC3' -as [type])) {
    Add-Type @"
using System; using System.Text; using System.Collections.Generic; using System.Runtime.InteropServices;
public class JanelaFC3 {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Ri, B; }
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  public static IntPtr Achar(uint[] pids, string parte) {
    IntPtr achada = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr p) {
      if (!IsWindowVisible(h)) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (Array.IndexOf(pids, pid) < 0) return true;
      var sb = new StringBuilder(256); GetWindowText(h, sb, 256);
      if (sb.ToString().Contains(parte)) { achada = h; return false; }
      return true;
    }, IntPtr.Zero);
    return achada;
  }
}
"@
}
$pids = [uint32[]]@(Get-Process flycast -ErrorAction Stop | ForEach-Object { $_.Id })
$h = [JanelaFC3]::Achar($pids, $Titulo)
if ($h -eq [IntPtr]::Zero) { throw "Janela com '$Titulo' no título não encontrada" }
if ($Maximizar) {
    [JanelaFC3]::ShowWindow($h, 3) | Out-Null       # 3 = SW_MAXIMIZE
    Start-Sleep -Milliseconds 700
}
elseif ([JanelaFC3]::IsIconic($h)) {                # minimizada: restaura antes
    [JanelaFC3]::ShowWindow($h, 9) | Out-Null       # 9 = SW_RESTORE
    Start-Sleep -Milliseconds 500
}
$r = New-Object JanelaFC3+R
[JanelaFC3]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.Ri - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
[JanelaFC3]::PrintWindow($h, $dc, 2) | Out-Null   # 2 = PW_RENDERFULLCONTENT
$g.ReleaseHdc($dc)
$bmp.Save($Saida)
