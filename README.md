# Vulkan X11 HDR WSI Layer

A Vulkan layer that adds HDR10 output to X11 applications. It implements:
- [VK_EXT_swapchain_colorspace](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_swapchain_colorspace.html) with the `VK_COLOR_SPACE_HDR10_ST2084_EXT` colorspace
- [VK_EXT_hdr_metadata](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_hdr_metadata.html)

The layer only works with an X server that supports HDR through the protocol described below. A stock Xorg server doesn't, and the layer then doesn't expose any HDR formats.

# How it works

- The HDR10 colorspace is offered while every monitor the window is on has the `HDR` and `HDR capable` RandR output properties set to 1.
- When an application creates an HDR10 swapchain, the layer sets the `hdr_output_metadata` window property to a `struct hdr_output_metadata` (from `drm_mode.h`). It is built from the values passed to `vkSetHdrMetadataEXT`. The X server shows the window as HDR on monitors with HDR enabled, fullscreen or windowed. The property is deleted when the swapchain is not HDR10.
- Vulkan drivers only expose 10-bit formats for depth 30 windows. When the desktop runs at depth 24, the layer renders 10-bit swapchains into a depth 30 child window, which needs an X server that offers a 30-bit visual on depth 24 screens. 8-bit swapchains keep using the application's window.
- When HDR is enabled or disabled on the window's monitors while the application runs, or the window moves to another monitor, presents return `VK_SUBOPTIMAL_KHR`. The application then recreates its swapchain with the formats that are now available.

# Requirements

- An X server with:
  - the `HDR`, `HDR capable` and `SDR white level` RandR output properties
  - support for the `hdr_output_metadata` window property
  - a 30-bit visual on depth 24 screens
- A Vulkan driver that supports 10-bit swapchains on X11, such as Mesa (RADV)
- A monitor that supports HDR10

# Building

Dependencies:
- [Meson](https://mesonbuild.com/Getting-meson.html) and [Ninja](https://github.com/ninja-build/ninja/wiki)
- Vulkan headers and loader
- libxcb (including xcb-randr and xcb-shape) and libX11-xcb

```bash
meson setup build
ninja -C build
sudo meson install -C build
```

# Usage

1. Enable HDR on the monitor:
   ```bash
   xrandr --output DP-1 --set HDR 1
   ```
   Run `xrandr --prop` to check that the monitor reports `HDR capable: 1`. The `SDR white level` property sets how bright the SDR desktop is shown in HDR mode, in nits.
2. Start the application with the layer enabled:
   ```bash
   ENABLE_HDR_WSI=1 vkcube
   ```
3. The application can run fullscreen or in a window. HDR in a window needs an X server that composites HDR windows, such as [this Xorg fork](https://github.com/dec05eba/xserver) with per-monitor framebuffers enabled (the default). Other X servers only show HDR for fullscreen windows, and HDR content in a window looks washed out.

The layer prints messages prefixed with `[HDR Layer]` to stderr. `Enabling format: ... colorspace: 1000104008` means that HDR10 is offered to the application.

Don't use the layer with applications that set the `hdr_output_metadata` property themselves, such as an mpv build with X11 HDR support.

## Steam

Set the launch options in the game's Properties → General:
- Native Linux Vulkan games: `ENABLE_HDR_WSI=1 %command%`
- Proton games: `ENABLE_HDR_WSI=1 DXVK_HDR=1 %command%`. `DXVK_HDR=1` makes DXVK and vkd3d-proton report HDR support to the game.

Then enable HDR in the game's settings.

Direct3D 12 games run through vkd3d-proton, and upstream vkd3d-proton only creates HDR10 swapchains with `VK_FORMAT_A2B10G10R10_UNORM_PACK32`. Vulkan drivers don't offer that format for depth 30 X11 windows, so these games need a vkd3d-proton build that falls back to `VK_FORMAT_A2R10G10B10_UNORM_PACK32`, installed into the Proton build the game uses.
