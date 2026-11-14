# Types editor commands into UnrealEd's command box (the Edit inside its ComboBox), one after the other.
param([string[]]$Commands, [int]$PauseMs = 1500)
$ErrorActionPreference = "Stop"
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public class EdCmd {
  public delegate bool P(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(P p, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, P f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  static string Cls(IntPtr h){ var c=new StringBuilder(128); GetClassName(h,c,128); return c.ToString(); }
  static string Txt(IntPtr h){ var c=new StringBuilder(256); GetWindowText(h,c,256); return c.ToString(); }
  public static IntPtr EditorWindow(uint pid){ IntPtr found=IntPtr.Zero; EnumWindows((h,l)=>{uint p; GetWindowThreadProcessId(h,out p); if(p==pid && IsWindowVisible(h) && Txt(h).StartsWith("Unreal Editor")) found=h; return true;}, IntPtr.Zero); return found; }
  public static List<IntPtr> Dialogs(uint pid){ var r=new List<IntPtr>(); EnumWindows((h,l)=>{uint p; GetWindowThreadProcessId(h,out p); if(p==pid && IsWindowVisible(h)){ string t=Txt(h); if(t.Length>0 && !t.StartsWith("Unreal Editor") && t!="Generic") r.Add(h);} return true;}, IntPtr.Zero); return r; }
  // The command box: the first Edit whose parent is a ComboBox
  public static IntPtr Box(IntPtr main){ IntPtr found=IntPtr.Zero; EnumChildWindows(main,(h,l)=>{ if(found==IntPtr.Zero && Cls(h)=="Edit" && Cls(GetParent(h))=="ComboBox") found=h; return true;}, IntPtr.Zero); return found; }
  public static void Run(IntPtr edit, string cmd){ SendMessage(edit,0x000C,IntPtr.Zero,cmd); PostMessage(edit,0x0100,(IntPtr)0x0D,(IntPtr)0x001C0001); PostMessage(edit,0x0102,(IntPtr)0x0D,(IntPtr)0x001C0001); PostMessage(edit,0x0101,(IntPtr)0x0D,(IntPtr)0xC01C0001); }
  public static string Describe(IntPtr h){ var parts=new List<string>(); parts.Add(Txt(h)); EnumChildWindows(h,(c,l)=>{ string t=Txt(c); if(t.Length>0) parts.Add(t); return true;}, IntPtr.Zero); return string.Join(" | ", parts); }
  public static void Close(IntPtr h){ PostMessage(h,0x0010,IntPtr.Zero,IntPtr.Zero); }
}
"@
$p = Get-Process MirrorsEdge -ErrorAction Stop | Select-Object -First 1
$main = [IntPtr]::Zero
for ($i = 0; $i -lt 240 -and $main -eq [IntPtr]::Zero; $i++) { $main = [EdCmd]::EditorWindow([uint32]$p.Id); if ($main -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 } }
if ($main -eq [IntPtr]::Zero) { throw "no editor window" }
foreach ($d in [EdCmd]::Dialogs([uint32]$p.Id)) { [EdCmd]::Close($d) }
Start-Sleep -Milliseconds 500
$box = [EdCmd]::Box($main)
if ($box -eq [IntPtr]::Zero) { throw "no command box" }
foreach ($c in $Commands) {
    "> $c"
    [EdCmd]::Run($box, $c)
    Start-Sleep -Milliseconds $PauseMs
    foreach ($d in [EdCmd]::Dialogs([uint32]$p.Id)) { "  dialog: " + [EdCmd]::Describe($d); [EdCmd]::Close($d) }
}
