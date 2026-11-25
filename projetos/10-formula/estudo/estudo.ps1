# MODO ESTUDO do som do motor: converte uma gravação real (.wav), compila
# formula-estudo.elf (separado do jogo normal) e abre no Flycast.
#
# Uso (PowerShell, nesta pasta):
#   .\estudo.ps1 motor.wav                  -> escolhe o trecho e estima a rotação sozinho
#   .\estudo.ps1 motor.wav -Ini 3.5 -Dur 1 -Rpm 11500          (V6 turbo)
#   .\estudo.ps1 motor.wav -Rpm 17000 -Cil 10                  (V10 de 2000)
#
# A gravação fica SÓ aqui (estudo\motor_gravado.h) e só no formula-estudo.elf:
# o formula.elf / formula.cdi normal continua 100% sintetizado. Não distribuir.
param(
    [Parameter(Mandatory)] [string]$Wav,
    [double]$Ini = -1,
    [double]$Dur = 1.2,
    [double]$Rpm = 0,
    [int]$Cil = 6
)
$ErrorActionPreference = 'Stop'
$aqui = $PSScriptRoot
$proj = Split-Path $aqui
$raiz = Split-Path (Split-Path $proj)

$arg = @("$aqui\converter_motor.py", (Resolve-Path $Wav).Path, '--dur', $Dur)
if ($Ini -ge 0) { $arg += @('--ini', $Ini) }
if ($Rpm -gt 0) { $arg += @('--rpm', $Rpm) }
$arg += @('--cil', $Cil)
& python @arg
if ($LASTEXITCODE) { throw 'conversão falhou' }

Get-Process flycast -ErrorAction SilentlyContinue | Stop-Process -Force
$ErrorActionPreference = 'Continue'      # o bash do DreamSDK escreve avisos no stderr
$env:_WORKING_DIRECTORY = '/' + $proj.Substring(0, 1).ToLower() + ($proj.Substring(2) -replace '\\', '/')
$env:_EXTERNAL_COMMAND = 'rm -f som_estudo.o formula-estudo.elf; make ESTUDO=1 2>&1 | grep -iE "error|warning" | head'
& 'C:\DreamSDK\usr\bin\bash.exe' --login -i 2>$null | Select-Object -Last 10
$elf = Join-Path $proj 'formula-estudo.elf'
if (-not (Test-Path $elf)) { throw 'compilação falhou' }
$fc = Join-Path $raiz 'ferramentas\flycast\flycast.exe'
Start-Process $fc -ArgumentList "`"$elf`"" -WorkingDirectory (Split-Path $fc)
Write-Host "Rodando formula-estudo.elf (som gravado). O jogo normal continua em formula.cdi."
