# Atalho para compilar e rodar projetos Dreamcast (DreamSDK + Flycast).
#
# Uso (no PowerShell, dentro de "Projetos Dreamcast"):
#   .\dc.ps1 build 01-hello     -> compila projetos\01-hello
#   .\dc.ps1 run   01-hello     -> compila e abre o .elf no Flycast
#   .\dc.ps1 clean 01-hello     -> apaga os arquivos gerados
#   .\dc.ps1 shell 01-hello     -> abre o shell do DreamSDK na pasta do projeto
#   .\dc.ps1 cdi   03-cubo      -> gera projetos\03-cubo\cubo.cdi (selfboot, p/ GDEMU)
#   .\dc.ps1 runcdi 03-cubo     -> gera o .cdi e o abre no Flycast (teste do boot)
#   .\dc.ps1 cdi 03-cubo -Titulo "CUBO 3D"   -> define o título gravado no IP.BIN
#
# Se o PowerShell bloquear scripts, rode uma vez:
#   Set-ExecutionPolicy -Scope CurrentUser RemoteSigned

param(
    [Parameter(Mandatory)][ValidateSet('build', 'run', 'clean', 'shell', 'cdi', 'runcdi')] [string]$Acao,
    [Parameter(Mandatory)] [string]$Projeto,
    [string]$Titulo
)

$ErrorActionPreference = 'Stop'
$raiz    = $PSScriptRoot
$pasta   = Join-Path $raiz "projetos\$Projeto"
$bash    = 'C:\DreamSDK\usr\bin\bash.exe'
$flycast = Join-Path $raiz 'ferramentas\flycast\flycast.exe'

if (-not (Test-Path $pasta)) { throw "Projeto não encontrado: $pasta" }
if (-not (Test-Path $bash))  { throw "DreamSDK não encontrado em C:\DreamSDK" }

# Converte C:\x\y para /c/x/y (formato do MSYS2).
function ConvertTo-MsysPath([string]$p) {
    '/' + $p.Substring(0,1).ToLower() + ($p.Substring(2) -replace '\\', '/')
}
$pastaMsys = ConvertTo-MsysPath $pasta

function Invoke-DreamSDK([string]$cmd) {
    # O perfil do DreamSDK (/etc/profile.d/000-dreamsdk.sh) carrega o KOS
    # e executa $_EXTERNAL_COMMAND dentro de $_WORKING_DIRECTORY.
    $env:_WORKING_DIRECTORY = $pastaMsys
    $env:_EXTERNAL_COMMAND  = $cmd
    $exitFile = [IO.Path]::GetTempFileName()
    $env:_EXITCODE = ConvertTo-MsysPath $exitFile
    try {
        # O bash interativo avisa "no job control" no stderr; o PowerShell 5.1
        # trataria isso como erro fatal, então relaxamos só nesta chamada.
        $ErrorActionPreference = 'Continue'
        & $bash --login -i 2>$null
        $ErrorActionPreference = 'Stop'
        $code = [int](Get-Content $exitFile -Raw).Trim()
    } finally {
        Remove-Item $exitFile -ErrorAction SilentlyContinue
        Remove-Item Env:_WORKING_DIRECTORY, Env:_EXTERNAL_COMMAND, Env:_EXITCODE -ErrorAction SilentlyContinue
    }
    if ($code -ne 0) { throw "Falhou (código $code)" }
}

switch ($Acao) {
    'build' { Invoke-DreamSDK 'make 2>&1' }
    'clean' { Invoke-DreamSDK 'make clean 2>&1' }
    'run' {
        Invoke-DreamSDK 'make 2>&1'
        $elf = Get-ChildItem $pasta -Filter *.elf | Where-Object { $_.Name -notlike '*-estudo.elf' } | Select-Object -First 1
        if (-not $elf) { throw "Nenhum .elf gerado em $pasta" }
        Start-Process $flycast -ArgumentList "`"$($elf.FullName)`"" -WorkingDirectory (Split-Path $flycast)
    }
    { $_ -in 'cdi', 'runcdi' } {
        # recompila do zero: os .o podem ter vindo de um build de teste (DEMO/OPC)
        Invoke-DreamSDK 'rm -f *.o; make 2>&1'
        $elf = Get-ChildItem $pasta -Filter *.elf | Where-Object { $_.Name -notlike '*-estudo.elf' } | Select-Object -First 1
        if (-not $elf) { throw "Nenhum .elf gerado em $pasta" }
        if (-not $Titulo) { $Titulo = $Projeto.ToUpper() }
        $script = ConvertTo-MsysPath (Join-Path $raiz 'ferramentas\mkcdi.sh')
        Invoke-DreamSDK "bash `"$script`" `"$($elf.Name)`" `"$Titulo`" 2>&1"
        if ($Acao -eq 'runcdi') {
            $cdi = Join-Path $pasta ($elf.BaseName + '.cdi')
            Start-Process $flycast -ArgumentList "`"$cdi`"" -WorkingDirectory (Split-Path $flycast)
        }
    }
    'shell' {
        $env:_WORKING_DIRECTORY = $pastaMsys
        Start-Process 'C:\DreamSDK\DreamSDK Shell.lnk'
    }
}
