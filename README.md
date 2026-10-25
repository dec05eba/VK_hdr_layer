# Vulkan X11 HDR WSI Layer

A Vulkan layer that adds HDR10 output to X11 applications. It implements:
- [VK_EXT_swapchain_colorspace](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_swapchain_colorspace.html) with the `VK_COLOR_SPACE_HDR10_ST2084_EXT` colorspace
- [VK_EXT_hdr_metadata](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VK_EXT_hdr_metadata.html)

The layer only works with an X server that supports HDR through the protocol described below. A stock Xorg server doesn't, and the layer then doesn't expose any HDR formats.

# How it works

- The HDR10 colorspace is offered while every monitor the window is on has the `HDR` and `HDR capable` RandR output properties set to 1.
- When an application creates an HDR10 swapchain, the layer sets the `hdr_output_metadata` window property to a `struct hdr_output_metadata` (from `drm_mode.h`). It is built from the values passed to `vkSetHdrMetadataEXT`. The X server applies it to the monitor while the window covers that monitor fullscreen. The property is deleted when the swapchain is not HDR10.
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
2. Start the application with `ENABLE_HDR_WSI=1`.
3. Run the application fullscreen. The X server treats a window that doesn't cover the monitor as SDR content, so HDR content in it looks washed out.

For Proton/Wine games, set the launch options to `ENABLE_HDR_WSI=1 DXVK_HDR=1 %command%` and enable HDR in the game's settings.

Don't use the layer with applications that set the `hdr_output_metadata` property themselves, such as an mpv build with X11 HDR support.
