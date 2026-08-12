<#
    Datamosh diagnostics collector — Windows.

    Answers, without you needing to know what matters: are the plugin files
    where Resolume looks, are they loadable, did Resolume look, did Resolume
    load them, and did the parameters register.

    From PowerShell:
        powershell -ExecutionPolicy Bypass -File .\collect-datamosh-diagnostics.ps1 |
            Tee-Object $env:USERPROFILE\Desktop\datamosh-diag.txt

    From cmd.exe:
        powershell -ExecutionPolicy Bypass -File .\collect-datamosh-diagnostics.ps1 > %USERPROFILE%\Desktop\datamosh-diag.txt 2>&1

    Then paste the file. Add -RedactUser to replace your username with <user>.

    STRICTLY READ-ONLY. It never runs Unblock-File, never writes inside Resolume
    or its plugin folders, and only ever issues HTTP GETs. It cannot fix
    anything — that is deliberate, so it is safe to run at any point and its
    output describes the machine as it actually is.

    Every section is independent: a missing tool or folder prints a SKIPPED line
    and the run continues. The script always exits 0.

    NOT YET RUN ON WINDOWS. It was written and structurally checked on Linux,
    where no PowerShell interpreter was available. Every section is wrapped in
    Safely{}, so a section that throws reports itself and the rest still runs —
    but a parse error would stop the whole file before any of that helps. If it
    fails to parse, say so and the macOS collector's section list is the spec
    for what to gather by hand.
#>

[CmdletBinding()]
param(
    [switch]$RedactUser
)

$ScriptVersion = '1.0.0'
$ErrorActionPreference = 'Continue'

# ------------------------------------------------------------------ plumbing

$script:Output = New-Object System.Collections.Generic.List[string]

function Emit    { param([string]$Text = '') $script:Output.Add($Text) }
function Section { param($N, $Title) Emit ''; Emit "===== [$N] $Title =====" }
function SectionEnd { param($N) Emit "===== [$N] END =====" }
function Skip    { param($N, $Why) Emit "[SECTION ${N}: SKIPPED - $Why]" }
function Note    { param($Text) Emit "  NOTE: $Text" }
function Finding { param($Text) Emit "  >>> $Text" }

# Wraps a section so one failure cannot abort the run.
function Safely {
    param($N, [scriptblock]$Body)
    try { & $Body } catch { Skip $N "collector error: $($_.Exception.Message)" }
}

# ------------------------------------------------------------------ [0] header

Section 0 'HEADER'
Safely 0 {
    Emit "  collector version:  $ScriptVersion"
    Emit "  plugin under test:  ffgl-datamosh (Datamosh / Mosh Transplant)"
    Emit "  UTC:                $((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss')) UTC"
    Emit "  local:              $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss K')"
    Emit "  redact-user:        $($RedactUser.IsPresent)"
    Emit "  OS:                 $([System.Environment]::OSVersion.VersionString)"
    Emit "  arch:               $env:PROCESSOR_ARCHITECTURE"
    Emit "  PowerShell:         $($PSVersionTable.PSVersion)"
}
SectionEnd 0

# ------------------------------------------------------- [1] Resolume install

Section 1 'RESOLUME INSTALLATION'
Safely 1 {
    $roots = @("$env:ProgramFiles\Resolume*", "${env:ProgramFiles(x86)}\Resolume*") |
             ForEach-Object { Get-Item $_ -ErrorAction SilentlyContinue }
    if (-not $roots) {
        Skip 1 'no Resolume installation found under Program Files'
    } else {
        foreach ($root in $roots) {
            Emit "  INSTALL: $($root.FullName)"
            Get-ChildItem $root.FullName -Filter *.exe -ErrorAction SilentlyContinue |
                ForEach-Object {
                    Emit "    $($_.Name)  ProductVersion=$($_.VersionInfo.ProductVersion)  FileVersion=$($_.VersionInfo.FileVersion)"
                }
            # The bundled MCP server appears in 7.26+. Its presence dates the install.
            Get-ChildItem (Join-Path $root.FullName 'mcp') -Filter *.mcpb -ErrorAction SilentlyContinue |
                ForEach-Object { Emit "    MCP server present: $($_.FullName)  (Resolume 7.26+)" }
        }
    }
    $proc = Get-Process -Name 'Resolume*' -ErrorAction SilentlyContinue
    if ($proc) { Emit "  RUNNING: $(($proc | ForEach-Object ProcessName) -join ', ')" }
    else       { Emit '  RUNNING: no Resolume process found' }
}
SectionEnd 1

# ------------------------------------------------- [2] plugin folder discovery

$script:Candidates = @()

Section 2 'PLUGIN FOLDER DISCOVERY'
Safely 2 {
    Note 'Two official Resolume sources disagree on this path, so nothing is assumed.'
    $bases = Get-Item "$env:USERPROFILE\Documents\Resolume*" -ErrorAction SilentlyContinue
    if (-not $bases) {
        Skip 2 'no Documents\Resolume* folder exists'
    } else {
        foreach ($base in $bases) {
            $dir = Join-Path $base.FullName 'Extra Effects'
            if (Test-Path $dir) {
                $n = (Get-ChildItem $dir -ErrorAction SilentlyContinue | Measure-Object).Count
                Emit ('  CANDIDATE: {0,-58} EXISTS   ({1} entries)' -f $dir, $n)
                $script:Candidates += $dir
            } else {
                Emit ('  CANDIDATE: {0,-58} ABSENT' -f $dir)
            }
        }
        foreach ($dir in $script:Candidates) {
            Emit ''
            Emit "  Contents of $dir"
            Get-ChildItem $dir -Force -ErrorAction SilentlyContinue |
                ForEach-Object { Emit ('    {0,-34} {1,12} {2}' -f $_.Name, $_.Length, $_.LastWriteTime) }
        }
    }
    Note 'Which of these Resolume actually scans is answered by Preferences > Video'
    Note "(which lists the FFGL directories) and by section 6's 'Scanning directory'"
    Note 'lines - not by this script.'
    Note 'If more than one candidate holds a Datamosh binary you cannot tell which'
    Note 'one Resolume loaded. Empty all but one before testing.'
}
SectionEnd 2

# ------------------------------------------------ [3] plugin files + loadability

Section 3 'PLUGIN FILES AND LOADABILITY'
Safely 3 {
    $anyDll = $false
    foreach ($dir in $script:Candidates) {
        foreach ($name in @('Datamosh.dll', 'DatamoshTransplant.dll')) {
            $dll = Join-Path $dir $name
            if (-not (Test-Path $dll)) { continue }
            $anyDll = $true
            $item = Get-Item $dll
            Emit "  ---- $dll"
            Emit "    size:     $($item.Length) bytes"
            Emit "    modified: $($item.LastWriteTime)"
            Emit "    sha256:   $((Get-FileHash $dll -Algorithm SHA256).Hash)"

            # Mark-of-the-Web. Windows adds this to anything downloaded, and it
            # can stop the load with no visible error anywhere.
            $streams = Get-Item $dll -Stream * -ErrorAction SilentlyContinue
            $zone = $streams | Where-Object { $_.Stream -eq 'Zone.Identifier' }
            if ($zone) {
                Emit '    MARK-OF-THE-WEB: PRESENT'
                Finding "Zone.Identifier present - Windows may refuse to load this. Fix: Get-ChildItem '$dir' -Recurse | Unblock-File"
                Get-Content $dll -Stream Zone.Identifier -ErrorAction SilentlyContinue |
                    ForEach-Object { Emit "      $_" }
            } else {
                Emit '    MARK-OF-THE-WEB: absent'
            }

            # PE machine type, read straight out of the header. Resolume has
            # been 64-bit only since v6, and "Error: 193" in the log means
            # exactly this and nothing else.
            try {
                $bytes = [System.IO.File]::ReadAllBytes($dll)
                $peOff = [BitConverter]::ToInt32($bytes, 0x3C)
                $machine = [BitConverter]::ToUInt16($bytes, $peOff + 4)
                $arch = switch ($machine) {
                    0x8664  { 'x64 (0x8664)' }
                    0x14c   { 'x86 (0x14c)' }
                    0xAA64  { 'ARM64 (0xAA64)' }
                    default { ('UNKNOWN (0x{0:X})' -f $machine) }
                }
                Emit "    PE machine:  $arch"
                if ($machine -ne 0x8664) {
                    Finding 'NOT x64. Resolume is 64-bit only; this cannot load. A log line reading "Error: 193" means precisely this.'
                }

                # Weak but useful: catches the historical build that shipped
                # with no entry point at all. Not an export-table read.
                $text = [System.Text.Encoding]::ASCII.GetString($bytes)
                foreach ($sym in @('plugMain', 'SetLogCallback')) {
                    if ($text.Contains($sym)) { Emit "    '$sym' present: yes  (WEAK probe - byte scan, not an export-table read)" }
                    else {
                        Emit "    '$sym' present: NO   (WEAK probe)"
                        if ($sym -eq 'plugMain') { Finding 'plugMain not found even as a string. The host has nothing to call.' }
                    }
                }
            } catch {
                Emit "    PE parse failed: $($_.Exception.Message)"
            }

            $sig = Get-AuthenticodeSignature $dll -ErrorAction SilentlyContinue
            if ($sig) { Emit "    signature:   $($sig.Status)  (NotSigned is expected and fine)" }

            # A missing VC++ runtime shows up as a named DLL here rather than as
            # a mystery failure in the log.
            $dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
            if ($dumpbin) {
                Emit '    dependents (dumpbin):'
                & $dumpbin.Source /dependents $dll 2>$null |
                    Select-String -Pattern '\.dll' | Select-Object -First 20 |
                    ForEach-Object { Emit "      $($_.Line.Trim())" }
            } else {
                Note 'dumpbin.exe not on PATH (it ships with Visual Studio) - export and dependency checks are the weak byte-scan above.'
            }
        }
    }
    if (-not $anyDll) { Emit '  (no Datamosh DLLs found in any candidate folder)' }
}
SectionEnd 3

# --------------------------------------------------------- [5] GPU and driver

Section 5 'GPU AND DRIVER'
Safely 5 {
    Note "The shader-compile risk is entirely a property of this machine's GLSL compiler."
    Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue |
        ForEach-Object {
            Emit "  $($_.Name)"
            Emit "    driver $($_.DriverVersion)  dated $($_.DriverDate)  VideoProcessor=$($_.VideoProcessor)"
        }
}
SectionEnd 5

# ------------------------------------------------------------ [6] Resolume log

Section 6 'RESOLUME LOG'
Safely 6 {
    Note "Resolume's v7 directory list names it 'Resolume Arena log.txt' (substitute"
    Note 'Avenue); the 7.11 revision gives only the folder. So: every file is listed.'
    Note 'Both LOCALAPPDATA and APPDATA are checked - both are official, from'
    Note 'different revisions of the docs, and checking only one silently misses it.'

    $logDirs = @("$env:LOCALAPPDATA\Resolume*", "$env:APPDATA\Resolume*") |
               ForEach-Object { Get-Item $_ -ErrorAction SilentlyContinue } |
               Where-Object { $_.PSIsContainer }

    if (-not $logDirs) {
        Skip 6 'no Resolume log directory found under LOCALAPPDATA or APPDATA'
    } else {
        $files = @()
        foreach ($d in $logDirs) {
            Emit "  LOG DIR: $($d.FullName)"
            Get-ChildItem $d.FullName -File -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                ForEach-Object {
                    Emit ('    {0,-40} {1,12} {2}' -f $_.Name, $_.Length, $_.LastWriteTime)
                    $files += $_
                }
        }

        $newest = $files | Sort-Object LastWriteTime -Descending | Select-Object -First 2
        $pattern = 'datamosh|DMSH|DMMX|transplant|scanning directory|ffgl|plugin main function|library could not be loaded|failed loading plugin|shader|ERROR'
        $matched = @()
        Emit ''
        Emit '  Matches (case-insensitive substrings), newest file first, capped at 200 lines:'
        foreach ($f in $newest) {
            $hits = Select-String -Path $f.FullName -Pattern $pattern -ErrorAction SilentlyContinue
            $matched += $hits
            $hits | Select-Object -Last 200 |
                ForEach-Object { Emit "    $($f.Name):$($_.LineNumber): $($_.Line.Trim())" }
        }

        Emit ''
        Emit '  LOG SUMMARY'
        $scan = @($matched | Where-Object { $_.Line -match '(?i)scanning directory' })
        Emit "    'Scanning directory' lines:              $($scan.Count)"
        if ($scan | Where-Object { $_.Line -match '(?i)extra effects' }) {
            Emit '    ...one naming an Extra Effects folder:   yes'
        } else {
            Emit '    ...one naming an Extra Effects folder:   no'
            Finding 'Resolume never scanned an Extra Effects folder. The plugin was NEVER FOUND - a path problem, not a loading problem. Check Preferences > Video.'
        }
        Emit "    'datamosh' lines:                        $(@($matched | Where-Object { $_.Line -match '(?i)datamosh' }).Count)"

        if ($matched | Where-Object { $_.Line -match '(?i)library could not be loaded|failed loading plugin' }) {
            Finding 'The file was FOUND but the OS refused to load it - Mark-of-the-Web, wrong architecture, or a missing VC++ runtime. See section 3.'
        }
        if ($matched | Where-Object { $_.Line -match '(?i)plugin main function' }) {
            Finding "The library loaded and the plugin's own init returned FAILURE. That is a bug in the plugin, and is the signature of the phantom-parameter class of failure."
        }
        if ($matched | Where-Object { $_.Line -match '(?i)Error: 193' }) {
            Finding 'Error 193 = "not a valid Win32 application" = wrong architecture. Confirm PE machine is x64 in section 3.'
        }
    }
}
SectionEnd 6

# ------------------------------------------------------------- [7] REST API

Section 7 'RESOLUME REST API (GET only)'
Safely 7 {
    Note 'Needs Preferences > Webserver enabled. Introduced in Resolume 7.8.'
    $base = 'http://localhost:8080/api/v1'
    $product = $null
    try { $product = Invoke-RestMethod -Uri "$base/product" -TimeoutSec 3 -ErrorAction Stop } catch { }

    if (-not $product) {
        Skip 7 'nothing answering on localhost:8080 - webserver disabled, or a different port'
    } else {
        Emit "  GET $base/product"
        Emit "    $($product | ConvertTo-Json -Compress)"
        try {
            $effects = Invoke-RestMethod -Uri "$base/effects" -TimeoutSec 3 -ErrorAction Stop
            $hits = @($effects.video | Where-Object { $_.name -match '(?i)datamosh' -or $_.idstring -eq 'DMSH' })
            Emit "  GET $base/effects  (filtered to Datamosh)"
            if ($hits.Count -gt 0) {
                $hits | ForEach-Object { Emit "    idstring=$($_.idstring)  name=$($_.name)" }
                Emit '    >>> Datamosh IS registered with the host.'
            } else {
                Finding 'Datamosh is NOT in the host effect list. Matched on name as well as idstring - it is not documented that third-party FFGL IDs are surfaced verbatim.'
            }
        } catch {
            Emit "  GET $base/effects failed: $($_.Exception.Message)"
        }
        Note 'The mixer will NOT appear there. FFGL mixers are blend modes: GET'
        Note "$base/composition/layers/1 and read video.mixer instead."
    }
}
SectionEnd 7

Emit ''
Emit '===== END OF REPORT ====='

# ------------------------------------------------------------------- flush

$text = $script:Output -join [Environment]::NewLine
if ($RedactUser) {
    $text = $text -replace [regex]::Escape($env:USERNAME), '<user>'
    $text = $text -replace [regex]::Escape($env:USERPROFILE), '<home>'
}
Write-Output $text

exit 0
