# LightWM (fork)

A small tiling window manager for Windows 10/11, written in C. This fork rebuilds the tiling,
workspaces and configuration of [nir9/lightwm](https://github.com/nir9/lightwm).

## What it does

- **Exact tiling.** Windows fill the work area with no leftover pixels and the exact gap you configure.
  Layouts: `master-stack`, `grid`, `columns`. The invisible resize borders Windows 10/11 put around
  windows are compensated, so edges line up.
- **Only real windows are tiled.** Resizable, un-owned, visible, not cloaked, not on another desktop,
  not a shell surface or tray flyout. Everything else floats untouched.
- **Native Task View desktops as workspaces.** `alt+1..9` switches, `alt+shift+1..9` sends the focused
  window. Missing desktops are created on demand. Focusing a window that lives on another desktop
  switches to it (`follow-focus`).
- **Built-in altdrag.** Hold `mod`, left-drag anywhere on a window to move it (drop it on another tile
  to swap), right-drag to resize (the 3x3 region you grab picks the edges; dragging the master/stack
  divider changes the master width).
- **KDL config** with variables, custom keybinds and `spawn` commands.

No DLL injection: the manager follows windows with `SetWinEventHook`, so it also sees 32-bit apps.

## Build

```
nmake                              (MSVC x64 Native Tools prompt)  -> release\lightwm.exe
mingw32-make -f Makefile.mingw     (MinGW-w64)                     -> build\lightwm.exe
```

## Workspaces need one extra file

Windows has no public API to switch virtual desktops or move windows between them. Put
[VirtualDesktopAccessor.dll](https://github.com/Ciantic/VirtualDesktopAccessor) (v2.x, 64-bit) next to
`lightwm.exe`. It wraps the undocumented interfaces and tracks them across Windows builds. Without it
everything except `goto`/`send`/`workspace-*` works, and LightWM tells you at startup.

## Config

`config.kdl` next to the exe wins, otherwise `%APPDATA%\lightwm\config.kdl`. A commented default is
written on first run. Reload with `$mod+shift+r`.

```kdl
vars { mod "alt"; term "wt.exe" }
general { gap 6; master-width 50; layout "master-stack"; float "Calculator" "mpv.exe" }
workspaces { follow-focus true; goto "$mod+{1-9}"; send "$mod+shift+{1-9}" }
altdrag { enabled true; mod "$mod"; move "left"; resize "right" }
binds {
    $mod+Return { spawn "$term" }
    $mod+shift+q { close-window }
}
```

Actions: `focus-next focus-prev focus-left/right/up/down move-left/right/up/down close-window
toggle-floating monocle fullscreen (borderless, covers the taskbar) retile toggle-tiling workspace-next workspace-prev reload-config quit`,
plus `spawn "command"`, `goto N`, `send N`. Underscores work too (`focus_next`). Key names: letters,
digits, `f1`-`f24`, `return space tab escape backspace delete insert home end pageup pagedown left right up
down comma period slash backslash semicolon apostrophe minus equal bracketleft bracketright grave
numpad0-9 print volumeup volumedown mute playpause nexttrack prevtrack`. Modifiers: `alt ctrl shift win`.
`spawn` expands `%ENV%` variables and opens URLs/documents too.

## Limits

- Windows running as administrator can't be moved by a non-elevated LightWM. Run LightWM elevated if you need them.
- Apps with a minimum size or size-snapping terminals can leave a gap or overlap their cell; no window manager can fix that.
- Keybinds fire two ways that back each other up: a low-level keyboard hook (sees keys first, so it beats other programs' hotkeys, and is re-installed every 15 s so Windows can't silently drop it) and `RegisterHotKey` (still works while an administrator window has focus; LightWM itself must be elevated for the hook to see those windows too). Binding `alt+f` takes Alt+F (the File menu mnemonic) away from every app. Combos the system handles before hooks see them (`win+l`, `ctrl+alt+del`) can't be bound.
- `fullscreen` works on whatever window has focus, tiled or not.

## Tests

`mingw32-make -f Makefile.mingw test` runs the layout (14,400 randomized cases: no overlap, exact gaps,
flush edges) and KDL parser tests. These cover the portable modules only.

License: MIT. AltDrag (GPLv3) inspired the behavior; none of its code is used.
