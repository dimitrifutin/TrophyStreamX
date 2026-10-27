$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$PluginDir = Join-Path $ProjectRoot 'plugin'
$BuildDir = Join-Path $PluginDir 'build'

Write-Host 'TrophyStream — compilation du plugin natif OBS' -ForegroundColor Cyan
Write-Host 'Pré-requis : Visual Studio 2022 (C++), CMake et un SDK/dossier de dépendances OBS qui expose libobsConfig.cmake.'

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'CMake introuvable. Installe CMake puis relance ce script.' }
if (-not $env:OBS_CMAKE_PREFIX_PATH) {
  Write-Host 'Définis OBS_CMAKE_PREFIX_PATH vers le dossier contenant libobsConfig.cmake avant la compilation.' -ForegroundColor Yellow
  Write-Host 'Exemple : $env:OBS_CMAKE_PREFIX_PATH = "C:\chemin\vers\obs-deps"'
  throw 'Dépendances de développement OBS non configurées.'
}
cmake -S $PluginDir -B $BuildDir -A x64 -DCMAKE_PREFIX_PATH="$env:OBS_CMAKE_PREFIX_PATH" -DCMAKE_INSTALL_PREFIX="$ProjectRoot\release"
cmake --build $BuildDir --config Release --parallel
cmake --install $BuildDir --config Release
Write-Host "Plugin compilé. Vérifie release\trophy-stream\bin\64bit\trophy-stream.dll" -ForegroundColor Green
