[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9][0-9A-Za-z.+~-]*$')]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$KernelModule,

    [string]$KernelRelease = '4.15.0-60-generic',
    [string]$Architecture = 'i386',
    [string]$OutputDirectory = 'dist-pcct',
    [string]$Image = 'ghcr.io/iotsharp/pcct-build-x86:latest'
)

$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerShell 7 or later is required.'
}

$projectDirectory = Split-Path -Parent $PSScriptRoot
$resolvedKernelModule = (Resolve-Path -LiteralPath $KernelModule).Path
if ([IO.Path]::IsPathRooted($OutputDirectory)) {
    $resolvedOutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
}
else {
    $resolvedOutputDirectory = [IO.Path]::GetFullPath(
        (Join-Path (Get-Location) $OutputDirectory)
    )
}
$containerName = 'usbdisplay-pcct-' + [guid]::NewGuid().ToString('N').Substring(0, 12)
$containerId = $null
$sourcePath = Join-Path $projectDirectory '.'

# 远端容器命令必须作为单个参数传递，避免 PowerShell 提前解释 shell 变量。
$buildScript = @'
set -eu
cd /src
rm -rf build dist
make userspace USBDISPLAY_VERSION="$PACKAGE_VERSION"
make check
KERNEL_MODULE=/usbdisplay.ko \
KERNEL_RELEASE="$PACKAGE_KERNEL" \
DEB_ARCH="$PACKAGE_ARCH" \
    sh scripts/package-deb.sh "$PACKAGE_VERSION" dist
'@

try {
    & docker image inspect $Image | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "PCCT image is unavailable: $Image"
    }

    $containerId = (& docker create --name $containerName `
        --env "PACKAGE_VERSION=$Version" `
        --env "PACKAGE_KERNEL=$KernelRelease" `
        --env "PACKAGE_ARCH=$Architecture" `
        --entrypoint sh $Image -lc $buildScript).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($containerId)) {
        throw 'Failed to create the PCCT build container.'
    }

    & docker cp $sourcePath "${containerId}:/src"
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to copy USBDisplayStack sources into the PCCT container.'
    }
    & docker cp $resolvedKernelModule "${containerId}:/usbdisplay.ko"
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to copy the kernel module into the PCCT container.'
    }

    & docker start --attach $containerId
    if ($LASTEXITCODE -ne 0) {
        throw 'PCCT compilation or Debian packaging failed.'
    }

    [IO.Directory]::CreateDirectory($resolvedOutputDirectory) | Out-Null
    & docker cp "${containerId}:/src/dist/." $resolvedOutputDirectory
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to copy Debian artifacts out of the PCCT container.'
    }

    Get-ChildItem -LiteralPath $resolvedOutputDirectory -File |
        Sort-Object Name |
        Select-Object Name, Length, LastWriteTime
}
finally {
    if (-not [string]::IsNullOrWhiteSpace($containerId)) {
        & docker rm $containerId | Out-Null
    }
}
