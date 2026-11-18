param(
    [Parameter(Mandatory = $true)][string]$InputPath,
    [Parameter(Mandatory = $true)][string]$OutputPath
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class NativeMszip
{
    private const uint CompressAlgorithmMszip = 2;

    [DllImport("cabinet.dll", SetLastError = true)]
    private static extern bool CreateCompressor(
        uint algorithm,
        IntPtr allocationRoutines,
        out IntPtr compressorHandle);

    [DllImport("cabinet.dll", SetLastError = true)]
    private static extern bool Compress(
        IntPtr compressorHandle,
        byte[] uncompressedData,
        UIntPtr uncompressedDataSize,
        byte[] compressedBuffer,
        UIntPtr compressedBufferSize,
        out UIntPtr compressedDataSize);

    [DllImport("cabinet.dll", SetLastError = true)]
    private static extern bool CloseCompressor(IntPtr compressorHandle);

    public static byte[] CompressMszip(byte[] input)
    {
        if (input == null || input.Length == 0)
            throw new InvalidOperationException("Input buffer is empty.");

        IntPtr handle;
        if (!CreateCompressor(CompressAlgorithmMszip, IntPtr.Zero, out handle))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "CreateCompressor failed");

        try
        {
            UIntPtr required = UIntPtr.Zero;
            if (!Compress(handle, input, (UIntPtr)input.Length, null, UIntPtr.Zero, out required))
            {
                int err = Marshal.GetLastWin32Error();
                if (err != 122) // ERROR_INSUFFICIENT_BUFFER
                    throw new System.ComponentModel.Win32Exception(err, "Compress size query failed");
            }

            byte[] output = new byte[(int)required.ToUInt64()];
            UIntPtr written = UIntPtr.Zero;
            if (!Compress(handle, input, (UIntPtr)input.Length, output, (UIntPtr)output.Length, out written))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Compress failed");

            byte[] exact = new byte[(int)written.ToUInt64()];
            Buffer.BlockCopy(output, 0, exact, 0, exact.Length);
            return exact;
        }
        finally
        {
            CloseCompressor(handle);
        }
    }
}
'@

$raw = [System.IO.File]::ReadAllBytes($InputPath)
$compressed = [NativeMszip]::CompressMszip($raw)
$outDir = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($OutputPath))
if (-not [string]::IsNullOrEmpty($outDir)) {
    [System.IO.Directory]::CreateDirectory($outDir) | Out-Null
}
[System.IO.File]::WriteAllBytes($OutputPath, $compressed)
Write-Host ("MSZIP compressed {0}: {1} B -> {2} B" -f [System.IO.Path]::GetFileName($InputPath), $raw.Length, $compressed.Length)
