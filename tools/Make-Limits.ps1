<#
Make-Limits.ps1
Reads a ListDinos survey (ListDinos.txt) and writes limits.txt: one Limit entry per wild class
that is over its cap, ready to paste into a map's "Limit" block.

Caps: Dragonfly 125, sea and resource creatures 300, everything else (land and air) 250.
Only wild counts are used. Event Ghost_ classes are skipped unless -IncludeGhosts is given.

Usage (from the folder holding ListDinos.txt):
    .\Make-Limits.ps1
    .\Make-Limits.ps1 -InputFile .\Aberration.txt -OutputFile .\limits.txt -IncludeGhosts
#>
param(
    [string]$InputFile  = (Join-Path $PSScriptRoot 'ListDinos.txt'),
    [string]$OutputFile = (Join-Path $PSScriptRoot 'limits.txt'),
    [int]$LandAirCap    = 250,
    [int]$SeaCap        = 300,
    [int]$DragonflyCap  = 125,
    [switch]$IncludeGhosts
)

# Class-name patterns (regex, case-insensitive) that get the sea/resource cap. Edit as needed.
$SeaPatterns = @(
    'Coel', 'Salmon', 'Trilobite', 'Piranha', 'Manta', 'Megalodon', 'Dolphin', 'Leech', 'Eel_',
    'Angler', 'Euryp', 'Ammonite', 'Cnidaria', 'Xiph', 'Helicoprion', 'Archelon', 'Leedsichthys',
    'Basilosaurus', 'Plesiosaur', 'Mosa', 'Liop', 'Tusoteuthis', 'Dunkle', 'Pteroteuthis',
    'Obelisksquid', 'Cryptoclidus', 'Bungartius', 'Ambulocetus',
    'Achatina', 'DungBeetle', 'Jugbug', 'LanternGoat'
)
$SeaRegex = ($SeaPatterns | ForEach-Object { "($_)" }) -join '|'

if (-not (Test-Path -LiteralPath $InputFile)) {
    Write-Error "Input file not found: $InputFile"
    exit 1
}

$lineRegex = '^\s*There are (\d+) Tamed and (\d+) Wild of BlueprintGeneratedClass (\S+) in the world'
$entries = @()

foreach ($line in Get-Content -LiteralPath $InputFile) {
    if ($line -notmatch $lineRegex) { continue }
    $wild  = [int]$Matches[2]
    $path  = $Matches[3]
    $class = $path.Substring($path.LastIndexOf('.') + 1)   # e.g. Dilo_Character_BP_C

    if (-not $IncludeGhosts -and $class -like 'Ghost_*') { continue }

    if ($class -match 'Dragonfly')     { $cap = $DragonflyCap; $kind = 'dragonfly' }
    elseif ($class -match $SeaRegex)   { $cap = $SeaCap;       $kind = 'sea/resource' }
    else                               { $cap = $LandAirCap;   $kind = 'land/air' }

    if ($wild -gt $cap) {
        $entries += [pscustomobject]@{ Class = $class; Wild = $wild; Cap = $cap; Kind = $kind }
    }
}

# The same short class name can appear twice (e.g. a vanilla and a mod path); keep the larger count.
$entries = $entries | Group-Object Class | ForEach-Object {
    $_.Group | Sort-Object Wild -Descending | Select-Object -First 1
} | Sort-Object Wild -Descending

if (-not $entries) {
    Set-Content -LiteralPath $OutputFile -Value '' -Encoding ASCII
    Write-Host "No class is over its cap. Wrote an empty $OutputFile."
    exit 0
}

# One entry per line; no comma after the last one (add one if pasting above existing entries).
$lines = for ($i = 0; $i -lt $entries.Count; $i++) {
    $comma = if ($i -lt $entries.Count - 1) { ',' } else { '' }
    '"{0}": {1}{2}' -f $entries[$i].Class, $entries[$i].Cap, $comma
}
Set-Content -LiteralPath $OutputFile -Value $lines -Encoding ASCII

Write-Host "$($entries.Count) classes over their cap -> $OutputFile"
$entries | Format-Table Class, Wild, Cap, Kind -AutoSize
