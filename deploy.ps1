# DP_DBStorage: собрать, упаковать PBO, разложить мод по серверу.
#
#   powershell -File deploy.ps1 -Game "C:\DayZServer"
#   powershell -File deploy.ps1 -Game "C:\DayZServer" -ModDir "@test" -GraftSource "E:\source\KR_GRAFTED"
#
# Результат: <Game>\<ModDir>\addons\DP_DBStorage_Server.pbo
#            <Game>\<ModDir>\grafted\DP_DBStorage.grafted.dll
#            <Game>\<ModDir>\dpdb.exe   (утилита администратора)
# Ядро graft ставится один раз: graft install <Game>; мод добавить в -serverMod=.
param(
    [Parameter(Mandatory)] [string] $Game,
    [string] $ModDir      = "@dbstorage",
    [string] $Preset      = "release",
    [string] $GraftSource = "",          # локальный клон KR_GRAFTED; пусто — скачать тег
    [string] $MakePbo     = "C:/Program Files (x86)/Mikero/DePboTools/bin/makepbo.exe"
)
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$name = "DP_DBStorage_Server"

if ($GraftSource) { $env:GRAFT_SOURCE_DIR = $GraftSource }
& cmd /c "`"$root\build.cmd`" $Preset"
if ($LASTEXITCODE) { throw "сборка упала" }
$out = Join-Path $root "out\$Preset"

$dist = Join-Path $root "dist\$ModDir"
New-Item -ItemType Directory -Force "$dist\addons", "$dist\grafted" | Out-Null
& $MakePbo -P -D (Join-Path $root "mod\$name") "$dist\addons\$name.pbo"
if ($LASTEXITCODE) { throw "MakePbo упал" }
Copy-Item -Force (Join-Path $out "DP_DBStorage.grafted.dll") "$dist\grafted\"
Copy-Item -Force (Join-Path $out "dpdb.exe") $dist

Copy-Item -Recurse -Force $dist $Game
Write-Host "готово: $Game\$ModDir (запускать с -serverMod=$ModDir)"
