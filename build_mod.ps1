#requires -Version 5.1
<#
  build_mod.ps1 - Compile et installe le proxy SDL2 pour Chicago 1930 (Steam, 32 bits).
  Placer ce script a cote de hooks.c, puis :
    powershell -ExecutionPolicy Bypass -File .\build_mod.ps1 -GameDir "C:\...\Chicago 1930"
  (si -GameDir est omis, le dossier du script est utilise)
#>
[CmdletBinding()]
param(
    [string]$GameDir = $PSScriptRoot,
    [switch]$KeepToolchain
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$Hooked = @('SDL_GetDesktopDisplayMode','SDL_CreateWindow','SDL_CreateRenderer',
            'SDL_SetRenderTarget','SDL_RenderPresent','SDL_PollEvent','SDL_Delay')

function Get-PE32Exports([string]$Path) {
    $d = [IO.File]::ReadAllBytes($Path)
    $pe = [BitConverter]::ToInt32($d, 0x3C)
    if ([BitConverter]::ToUInt32($d, $pe) -ne 0x00004550) { throw "$Path n'est pas un PE valide" }
    if ([BitConverter]::ToUInt16($d, $pe + 4) -ne 0x14C)  { throw "$Path n'est pas une DLL 32 bits (x86)" }
    $nsec  = [BitConverter]::ToUInt16($d, $pe + 6)
    $optsz = [BitConverter]::ToUInt16($d, $pe + 20)
    $opt   = $pe + 24
    if ([BitConverter]::ToUInt16($d, $opt) -ne 0x10B) { throw "$Path n'est pas au format PE32" }
    $expRva = [BitConverter]::ToUInt32($d, $opt + 96)
    $secs   = $opt + $optsz
    function Convert-Rva([uint32]$rva) {
        for ($i = 0; $i -lt $nsec; $i++) {
            $s  = $secs + 40 * $i
            $vs = [BitConverter]::ToUInt32($d, $s + 8)
            $va = [BitConverter]::ToUInt32($d, $s + 12)
            $rs = [BitConverter]::ToUInt32($d, $s + 16)
            $ro = [BitConverter]::ToUInt32($d, $s + 20)
            if ($rva -ge $va -and $rva -lt ($va + [Math]::Max($vs, $rs))) { return [int]($rva - $va + $ro) }
        }
        throw "RVA introuvable : $rva"
    }
    if ($expRva -eq 0) { throw "$Path n'exporte rien" }
    $e = Convert-Rva $expRva
    $n = [BitConverter]::ToUInt32($d, $e + 24)
    $namesOff = Convert-Rva ([BitConverter]::ToUInt32($d, $e + 32))
    $names = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $n; $i++) {
        $o = Convert-Rva ([BitConverter]::ToUInt32($d, $namesOff + 4 * $i))
        $end = $o; while ($d[$end] -ne 0) { $end++ }
        $names.Add([Text.Encoding]::ASCII.GetString($d, $o, $end - $o))
    }
    return $names
}

# ---------- 1. Verifications ----------
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path
$Src = Join-Path $PSScriptRoot 'hooks.c'
if (-not (Test-Path $Src)) { throw "hooks.c introuvable a cote du script ($PSScriptRoot)" }
if (-not (Test-Path (Join-Path $GameDir 'Chicago1930.exe'))) {
    throw "Chicago1930.exe introuvable dans '$GameDir'. Utilisez -GameDir avec le dossier du jeu."
}
$Dll  = Join-Path $GameDir 'SDL2.dll'
$Real = Join-Path $GameDir 'SDL2_real.dll'
$alreadyRenamed = Test-Path $Real
$realSource = if ($alreadyRenamed) { $Real } elseif (Test-Path $Dll) { $Dll } else { throw "Aucune SDL2.dll dans '$GameDir'" }
Write-Host "[1/6] DLL d'origine : $realSource"

# ---------- 2. Generation du .def (equivalent de gen_def.py, sans Python) ----------
$work = Join-Path $env:TEMP 'chicago_mod_build'
New-Item -ItemType Directory -Force -Path $work | Out-Null
$names = Get-PE32Exports $realSource
$missing = $Hooked | Where-Object { $names -notcontains $_ }
if ($missing) { throw "Exports absents de la DLL d'origine : $($missing -join ', ')" }
$def = New-Object System.Collections.Generic.List[string]
$def.Add('LIBRARY SDL2'); $def.Add('EXPORTS')
foreach ($n in $names) {
    if ($Hooked -contains $n) { $def.Add("    $n") } else { $def.Add("    $n=SDL2_real.$n") }
}
$DefPath = Join-Path $work 'SDL2.def'
Set-Content -Path $DefPath -Value $def -Encoding ASCII
Write-Host "[2/6] SDL2.def genere ($($names.Count) exports, dont $($Hooked.Count) interceptes)"

# ---------- 3. Compilateur temporaire (w64devkit i686, officiel skeeto/w64devkit) ----------
$tc = Join-Path $work 'toolchain'
$gcc = Get-ChildItem -Path $tc -Recurse -Filter gcc.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $gcc) {
    Write-Host "[3/6] Telechargement de GCC 32 bits (w64devkit)..."
    $rel = Invoke-RestMethod 'https://api.github.com/repos/skeeto/w64devkit/releases/latest' -Headers @{ 'User-Agent' = 'build_mod' }
    $asset = $rel.assets | Where-Object { $_.name -match '^w64devkit-i686-.*\.(7z\.exe|zip)$' } | Select-Object -First 1
    if (-not $asset) { throw "Archive i686 introuvable dans la derniere release ($($rel.html_url)). Telechargez-la a la main." }
    $dl = Join-Path $work $asset.name
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $dl -UseBasicParsing
    New-Item -ItemType Directory -Force -Path $tc | Out-Null
    if ($asset.name -like '*.zip') {
        Expand-Archive -Path $dl -DestinationPath $tc -Force
    } else {
        $p = Start-Process -FilePath $dl -ArgumentList @('-y', "-o`"$tc`"") -Wait -PassThru -WindowStyle Hidden
        if ($p.ExitCode -ne 0) { throw "Echec de l'extraction (code $($p.ExitCode))" }
    }
    $gcc = Get-ChildItem -Path $tc -Recurse -Filter gcc.exe | Select-Object -First 1
    if (-not $gcc) { throw "gcc.exe introuvable apres extraction" }
} else { Write-Host "[3/6] Compilateur deja present dans $tc" }
$env:PATH = "$($gcc.DirectoryName);$env:PATH"
Write-Host "      $(& $gcc.FullName --version | Select-Object -First 1)"

# ---------- 4. Compilation ----------
Write-Host "[4/6] Compilation..."
$Out = Join-Path $work 'SDL2.dll'
if (Test-Path $Out) { Remove-Item $Out -Force }
& $gcc.FullName -O2 -shared -o $Out $Src $DefPath -lwinmm -static-libgcc
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Out)) { throw "La compilation a echoue (voir messages ci-dessus)" }
$outNames = Get-PE32Exports $Out      # verifie aussi que le resultat est bien un PE32 x86
if ($outNames.Count -lt $names.Count) { throw "Le proxy exporte $($outNames.Count) fonctions au lieu de $($names.Count)" }
Write-Host "      OK : PE32 x86, $($outNames.Count) exports"

# ---------- 5. Installation ----------
Write-Host "[5/6] Installation dans $GameDir"
if (-not $alreadyRenamed) { Rename-Item -LiteralPath $Dll -NewName 'SDL2_real.dll' }
Copy-Item -LiteralPath $Out -Destination $Dll -Force
$ini = @(
    '[mod]',
    'width=1280',
    'height=720',
    'fps=100',
    'zoom=1',
    'maxzoom_x100=300',
    'vsync=0',
    'log=1'
)
Set-Content -Path (Join-Path $GameDir 'chicago_mod.ini') -Value $ini -Encoding ASCII

# ---------- 6. Nettoyage ----------
if (-not $KeepToolchain) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
Write-Host "[6/6] Termine. Lancez le jeu, puis lisez chicago_mod.log dans le dossier du jeu."
Write-Host "      Desinstaller : supprimer SDL2.dll et renommer SDL2_real.dll en SDL2.dll."
