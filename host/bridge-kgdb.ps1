# Generic VMware-Workstation-on-Windows bridge from a guest serial port (exposed
# as a Windows named pipe) to a TCP listener that GDB connects to.
#
# This is the Windows-host side of the `kgdb` debug method when the hypervisor
# is VMware Workstation. Configure a serial port in the VM as:
#   Use named pipe   = \\.\pipe\<your-pipe-name>
#   This end is the server
#   The other end is an application
#   Connect at power on
#
# Then run this script on the Windows host whenever you want to debug:
#   .\bridge-kgdb.ps1 -PipeName kgdb-server -Port 5520
#
# The lab does not bundle target-specific defaults here on purpose; pipe names
# and ports are user choices that should live in your own wrapper, in your VM
# settings, or in your shell history. Match TARGET_DEBUG_ENDPOINT_KGDB[<target>]
# in lab.local.env to the -ListenAddress:-Port you pass here.

param(
    [Parameter(Mandatory = $true)]
    [string]$PipeName,

    [Parameter(Mandatory = $true)]
    [int]$Port,

    [string]$ListenAddress = "127.0.0.1",

    [int]$PipeConnectTimeoutMs = 30000
)

$ErrorActionPreference = "Stop"

if ($PipeName -match '^\\\\[^\\]+\\pipe\\(.+)$') {
    $PipeName = $Matches[1]
}

$Address = [System.Net.IPAddress]::Parse($ListenAddress)
$Listener = [System.Net.Sockets.TcpListener]::new($Address, $Port)

function Close-Quietly($Resource) {
    if ($null -ne $Resource) {
        try {
            $Resource.Dispose()
        } catch {
        }
    }
}

Write-Host "Starting KGDB bridge"
Write-Host "  TCP:  ${ListenAddress}:${Port}"
Write-Host "  Pipe: \\.\pipe\$PipeName"
Write-Host "Press Ctrl+C to stop."

$Listener.Start()

try {
    while ($true) {
        Write-Host "Waiting for GDB TCP connection..."
        $TcpClient = $Listener.AcceptTcpClient()
        $PipeClient = $null
        $NetworkStream = $null

        try {
            $Remote = $TcpClient.Client.RemoteEndPoint
            Write-Host "GDB connected from $Remote; connecting to \\.\pipe\$PipeName..."

            $PipeClient = [System.IO.Pipes.NamedPipeClientStream]::new(
                ".",
                $PipeName,
                [System.IO.Pipes.PipeDirection]::InOut,
                [System.IO.Pipes.PipeOptions]::Asynchronous
            )
            $PipeClient.Connect($PipeConnectTimeoutMs)
            Write-Host "Named pipe connected. Forwarding bytes until one side closes."

            $NetworkStream = $TcpClient.GetStream()
            $TcpToPipe = $NetworkStream.CopyToAsync($PipeClient)
            $PipeToTcp = $PipeClient.CopyToAsync($NetworkStream)
            $PumpTasks = [System.Threading.Tasks.Task[]]@($TcpToPipe, $PipeToTcp)

            [void][System.Threading.Tasks.Task]::WaitAny($PumpTasks)
            Write-Host "Bridge connection closed."
        } catch {
            Write-Host "Bridge connection failed: $($_.Exception.Message)"
        } finally {
            Close-Quietly $NetworkStream
            Close-Quietly $PipeClient
            Close-Quietly $TcpClient
        }
    }
} finally {
    $Listener.Stop()
}
