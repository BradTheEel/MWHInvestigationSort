param(
    [string]$ZigPath = '',
    [string]$Launcher = '',
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
if (-not $ZigPath) {
    $zigCommand = Get-Command zig -ErrorAction SilentlyContinue
    if (-not $zigCommand) { throw 'Pass -ZigPath to Zig 0.13.0.' }
    $ZigPath = $zigCommand.Source
}
if ((& $ZigPath version) -ne '0.13.0') { throw 'This build requires Zig 0.13.0.' }

$build = Join-Path $PSScriptRoot 'build'
$src = Join-Path $PSScriptRoot 'src'
$vendor = Join-Path $src 'vendor'
New-Item -ItemType Directory -Path $build -Force | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $build 'zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $build 'zig-local'

$flags = @('cc', '-target', 'x86_64-windows-gnu', '-O2', '-Wall', '-Wextra', '-Werror')
$dll = Join-Path $build 'InvestigationSort.dll'
& $ZigPath @flags -shared -fms-extensions -I (Join-Path $vendor 'include') `
    (Join-Path $src 'investigation_sort.c') (Join-Path $src 'sort.c') `
    (Join-Path $vendor 'src/hook.c') (Join-Path $vendor 'src/trampoline.c') `
    (Join-Path $vendor 'src/buffer.c') (Join-Path $vendor 'src/hde/hde64.c') `
    -lkernel32 -o $dll
if ($LASTEXITCODE) { throw "DLL build failed with exit code $LASTEXITCODE." }
Write-Output $dll

if ($Test) {
    if (-not $Launcher -or -not (Test-Path -LiteralPath $Launcher -PathType Leaf)) {
        throw 'Pass -Launcher to the owned-test-launcher-v1 launcher.exe.'
    }
    $testExe = Join-Path $build 'sort_test.exe'
    & $ZigPath @flags -UNDEBUG (Join-Path $PSScriptRoot 'tests/sort_test.c') `
        (Join-Path $src 'sort.c') -o $testExe
    if ($LASTEXITCODE) { throw "Fixture build failed with exit code $LASTEXITCODE." }
    & $Launcher $testExe
    if ($LASTEXITCODE) { throw "Fixture exited with code $LASTEXITCODE." }
    $menuExe = Join-Path $build 'menu_test.exe'
    & $ZigPath @flags -UNDEBUG -fms-extensions -I (Join-Path $vendor 'include') `
        (Join-Path $PSScriptRoot 'tests/menu_test.c') (Join-Path $src 'sort.c') `
        (Join-Path $vendor 'src/hook.c') (Join-Path $vendor 'src/trampoline.c') `
        (Join-Path $vendor 'src/buffer.c') (Join-Path $vendor 'src/hde/hde64.c') `
        -lkernel32 -o $menuExe
    if ($LASTEXITCODE) { throw "Menu fixture build failed with exit code $LASTEXITCODE." }
    & $Launcher $menuExe
    if ($LASTEXITCODE) { throw "Menu fixture exited with code $LASTEXITCODE." }
}
