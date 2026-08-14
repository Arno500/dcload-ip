# Inject key presses into flycast without needing the foreground focus.
#
# WScript.Shell SendKeys was measured to do nothing here: it delivers to
# whatever window the DESKTOP considers foreground, and AppActivate cannot
# reliably raise a window from a non-interactive shell. PostMessage addresses
# the window handle directly, and flycast has RawInput = no, so SDL reads the
# ordinary WM_KEYDOWN/WM_KEYUP path that PostMessage feeds.
#
# The lParam is not decoration: SDL derives its scancode from bits 16-23, so a
# bare 0 lParam is discarded as an unknown key.
param(
  [int]$Count = 1,
  [int]$DelayMs = 1500,
  [int]$HoldMs = 80,
  [string]$Vk = "0x0D",       # VK_RETURN
  [string]$Scan = "0x1C"      # scancode for Return
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class W32 {
  [DllImport("user32.dll", SetLastError=true)]
  public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@

$WM_KEYDOWN = 0x0100
$WM_KEYUP   = 0x0101

$vkCode   = [Convert]::ToInt32($Vk, 16)
$scanCode = [Convert]::ToInt32($Scan, 16)

# repeat count 1 | scancode<<16
$lpDown = [IntPtr](1 -bor ($scanCode -shl 16))
# same, plus bit30 (previous state down) and bit31 (transition = release)
$lpUp   = [IntPtr](1 -bor ($scanCode -shl 16) -bor 0x40000000 -bor -2147483648)

$p = Get-Process flycast -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output "NO_FLYCAST"; exit 1 }
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "NO_WINDOW_HANDLE"; exit 1 }
Write-Output ("hwnd=" + $h + " title=" + $p.MainWindowTitle)

for ($i = 0; $i -lt $Count; $i++) {
  [void][W32]::PostMessage($h, $WM_KEYDOWN, [IntPtr]$vkCode, $lpDown)
  Start-Sleep -Milliseconds $HoldMs
  [void][W32]::PostMessage($h, $WM_KEYUP, [IntPtr]$vkCode, $lpUp)
  Start-Sleep -Milliseconds $DelayMs
}
Write-Output ("sent=" + $Count)
