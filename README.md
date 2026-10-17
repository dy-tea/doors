<div align="center">

# doors

_Tiling Wayland compositor_

[Highlights](#highlights) · [Screenshots](#screenshots) · [Building](#building) · [Configuration](#configuration) · [Packages](#packages)

<br>
<img alt="Tiled layout" src="https://dawn.wine/api/v1/repos/CreeperFace/doors/media/repository/tiled_layout.png">
</div>
<br>

## Highlights

- Custom gles2 and vulkan blur rendering
- Customizable animations
- Written in blazingly fast, memory safe C programming language
- Support for extra protocols such as ext-background-effect-v1, xdg-system-bell-v1, xx-hotkey-v1 and xx-session-management-v1 (and more)
- Block out from screenshare
- HDR support

## Screenshots


| ![Resized window with presel](https://dawn.wine/api/v1/repos/CreeperFace/doors/media/repository/resized_windows_with_presel.png) | ![Scroller Layout](https://dawn.wine/api/v1/repos/StarTwi/doors/media/repository/scroller_layout.png) |
| :---: | :---: |
| *Resized windows with presel* | *Scroller Layout* |
| ![Floating window with blur and rounding](https://dawn.wine/api/v1/repos/CreeperFace/doors/media/repository/floating_window_blur_rounding.png) | ![Master stack layout](https://dawn.wine/api/v1/repos/CreeperFace/doors/media/repository/master_stack_layout.png) |
| *Floating window with blur and rounding* | *Master stack layout* |

_Above screenshots are a laptop running doors with noctalia 5.1.0._

## Building

### Dependencies

Ensure you have the following dependencies installed (individual names provided by your distro's package manager may differ):

|  |  |
|---|---|
| [wlroots-git (0.21)](https://gitlab.freedesktop.org/wlroots/wlroots) (latest git) | wayland |
| wayland-protocols | xkbcommon |
| libinput | pixman-1 |
| xcb | xcb-icccm |
| glesv2 | egl |
| cairo | pangocairo |
| shaderc | vulkan |
| libpcre2-8 | |

### Compile

Build with meson:
```sh
meson setup build
ninja -C build
```

## Configuration

You can use the example config under [examples](examples).
These should be placed under `$XDG_CONFIG_HOME/doors/` (or `$HOME/.config/doors/`).
Alternatively, you can pass the directory you have put them in with the -c arg like:
```sh
doors -c ./examples/
```

The **doorsrc** is a bash file that is run at startup, and can be used to configure settings and launch applications (like an **.xinitrc**).

Use `doorsctl env set <name> <value>` in `doorsrc` to update the compositor's environment. Applications launched later by Doors, including hotkey commands, inherit these values.

The **doorshkrc** is a config file for your hotkeys, and is reloaded on every save. It uses a config format similar to [sxhkd](https://github.com/baskerville/sxhkd) and can execute compositor binds by calling the `doorsctl` command.

## Packages

| Platform | Package |
|---|---|
| **Arch Linux** | [`doors-git`](https://aur.archlinux.org/packages/doors-git) on the AUR |
| **Nix** | A flake maintained by [foxtrottt](https://github.com/duckysocks22) |
