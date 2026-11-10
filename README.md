# Investigation Sort

[Читать на русском](README_RU.md)

Investigation Sort is a Monster Hunter: World Iceborne mod that brings the Resource Center's sorting menu to **Post a New Quest → Investigations**.

Press **Tab** on the keyboard or click the **right stick (RS/R3)** on a controller to open the menu. The selected sort order applies to all pages of registered investigations for the selected rank. Canceling keeps the current order. The reset option restores the game's default order.

This mod was developed with the help of artificial intelligence (AI).

## Installation

The mod targets Steam build `15539686` on Windows x64 and requires Stracker’s Loader.

Copy the `nativePC` folder from the install archive into the game directory. To uninstall, close the game and remove `nativePC/plugins/InvestigationSort.dll`.

## Building from source

Run the build script from the repository root in PowerShell using Zig 0.13.0.

```powershell
./build.ps1 -ZigPath 'C:/path/to/zig.exe'
```

The script writes `build/InvestigationSort.dll`. The repository includes the [MinHook source and license](src/vendor/).
