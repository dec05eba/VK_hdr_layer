#define VK_USE_PLATFORM_XCB_KHR
#define VK_USE_PLATFORM_XLIB_KHR
#include "vkroots.h"

#include <X11/Xlib-xcb.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcbext.h>
#include <drm/drm_mode.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>
#include <optional>
#include <functional>
#include <ranges>

using namespace std::literals;

namespace HdrLayer
{

static constexpr std::array<VkSurfaceFormatKHR, 2> s_HdrSurfaceFormats = {{
    { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },
    { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT },
}};

struct X11Output {
    int x;
    int y;
    int width;
    int height;
    bool hdr;
};

struct HdrSurfaceData {
    VkInstance instance;
    bool supportsPassthrough = false;

    xcb_connection_t *xcbConnection;
    xcb_window_t xcbWindow;
    xcb_window_t xcbRoot;
    xcb_atom_t hdrMetadataAtom;
    xcb_atom_t hdrAtom;
    xcb_atom_t hdrCapableAtom;

    xcb_window_t hdrWindow;
    xcb_colormap_t hdrColormap;
    VkSurfaceKHR hdrWindowSurface;
    uint16_t hdrWindowWidth;
    uint16_t hdrWindowHeight;

    int windowX;
    int windowY;
    int windowWidth;
    int windowHeight;
    unsigned int windowPositionRequest;
    unsigned int windowGeometryRequest;

    xcb_connection_t *randrEventConnection;
    uint8_t randrFirstEvent;
    std::vector<X11Output> outputs;
    bool outputsDirty;
    bool hdrAvailable;
};
VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrSurface, VkSurfaceKHR);

static constexpr uint8_t HDR_WINDOW_DEPTH = 30;

static bool isTenBitFormat(VkFormat format)
{
    return format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || format == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
}

static void discardX11Errors(xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    xcb_discard_reply(connection, cookie.sequence);
}

static bool checkX11Request(xcb_connection_t *connection, xcb_void_cookie_t cookie)
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    free(error);
    return error == nullptr;
}

static xcb_visualid_t findHdrVisual(xcb_connection_t *connection, xcb_window_t root)
{
    for (auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection)); screen.rem; xcb_screen_next(&screen)) {
        if (screen.data->root != root) {
            continue;
        }
        for (auto depth = xcb_screen_allowed_depths_iterator(screen.data); depth.rem; xcb_depth_next(&depth)) {
            if (depth.data->depth != HDR_WINDOW_DEPTH) {
                continue;
            }
            for (auto visual = xcb_depth_visuals_iterator(depth.data); visual.rem; xcb_visualtype_next(&visual)) {
                if (visual.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR && visual.data->bits_per_rgb_value == 10) {
                    return visual.data->visual_id;
                }
            }
        }
    }
    return XCB_NONE;
}

static void resizeX11HdrWindow(HdrSurfaceData &surface, uint16_t width, uint16_t height)
{
    width = std::max<uint16_t>(width, 1);
    height = std::max<uint16_t>(height, 1);
    if (width == surface.hdrWindowWidth && height == surface.hdrWindowHeight) {
        return;
    }

    const uint32_t values[] = { width, height };
    discardX11Errors(surface.xcbConnection,
                     xcb_configure_window_checked(surface.xcbConnection, surface.hdrWindow,
                                                  XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values));
    surface.hdrWindowWidth = width;
    surface.hdrWindowHeight = height;
}

static void updateX11WindowGeometry(HdrSurfaceData &surface, const xcb_translate_coordinates_reply_t *position,
                                    const xcb_get_geometry_reply_t *geometry)
{
    if (position) {
        surface.windowX = position->dst_x;
        surface.windowY = position->dst_y;
    }
    if (geometry) {
        surface.windowWidth = geometry->width;
        surface.windowHeight = geometry->height;
        if (surface.hdrWindow) {
            resizeX11HdrWindow(surface, geometry->width, geometry->height);
        }
    }
}

static void syncX11WindowGeometry(HdrSurfaceData &surface)
{
    xcb_connection_t *connection = surface.xcbConnection;
    const auto positionCookie = xcb_translate_coordinates(connection, surface.xcbWindow, surface.xcbRoot, 0, 0);
    const auto geometryCookie = xcb_get_geometry(connection, surface.xcbWindow);
    xcb_translate_coordinates_reply_t *position = xcb_translate_coordinates_reply(connection, positionCookie, nullptr);
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection, geometryCookie, nullptr);
    updateX11WindowGeometry(surface, position, geometry);
    free(position);
    free(geometry);
    xcb_flush(connection);
}

static void setX11HdrWindowMapped(const HdrSurfaceData &surface, bool mapped)
{
    xcb_connection_t *connection = surface.xcbConnection;
    discardX11Errors(connection, mapped ? xcb_map_window_checked(connection, surface.hdrWindow)
                                        : xcb_unmap_window_checked(connection, surface.hdrWindow));
    xcb_flush(connection);
}

static void pollX11WindowGeometry(HdrSurfaceData &surface)
{
    xcb_connection_t *connection = surface.xcbConnection;
    if (surface.windowGeometryRequest) {
        xcb_translate_coordinates_reply_t *position = nullptr;
        xcb_get_geometry_reply_t *geometry = nullptr;
        xcb_generic_error_t *error = nullptr;
        if (!xcb_poll_for_reply(connection, surface.windowGeometryRequest, reinterpret_cast<void **>(&geometry), &error)) {
            return;
        }
        free(error);
        error = nullptr;
        if (!xcb_poll_for_reply(connection, surface.windowPositionRequest, reinterpret_cast<void **>(&position), &error)) {
            xcb_discard_reply(connection, surface.windowPositionRequest);
        }
        free(error);
        updateX11WindowGeometry(surface, position, geometry);
        free(position);
        free(geometry);
    }

    surface.windowPositionRequest = xcb_translate_coordinates(connection, surface.xcbWindow, surface.xcbRoot, 0, 0).sequence;
    surface.windowGeometryRequest = xcb_get_geometry(connection, surface.xcbWindow).sequence;
    xcb_flush(connection);
}

static bool getX11OutputFlag(xcb_connection_t *connection, xcb_randr_output_t output, xcb_atom_t property)
{
    xcb_randr_get_output_property_reply_t *reply = xcb_randr_get_output_property_reply(
        connection, xcb_randr_get_output_property(connection, output, property, XCB_ATOM_INTEGER, 0, 1, false, false), nullptr);
    const bool enabled = reply && reply->format == 32 && reply->num_items == 1
        && *reinterpret_cast<const int32_t *>(xcb_randr_get_output_property_data(reply)) == 1;
    free(reply);
    return enabled;
}

static void readX11Outputs(HdrSurfaceData &surface)
{
    xcb_connection_t *connection = surface.xcbConnection;
    surface.outputs.clear();
    surface.outputsDirty = false;

    xcb_randr_get_screen_resources_current_reply_t *resources = xcb_randr_get_screen_resources_current_reply(
        connection, xcb_randr_get_screen_resources_current(connection, surface.xcbRoot), nullptr);
    if (!resources) {
        return;
    }

    const xcb_randr_output_t *outputs = xcb_randr_get_screen_resources_current_outputs(resources);
    const int numOutputs = xcb_randr_get_screen_resources_current_outputs_length(resources);
    for (int i = 0; i < numOutputs; i++) {
        xcb_randr_get_output_info_reply_t *info = xcb_randr_get_output_info_reply(
            connection, xcb_randr_get_output_info(connection, outputs[i], resources->config_timestamp), nullptr);
        xcb_randr_get_crtc_info_reply_t *crtc = info && info->crtc
            ? xcb_randr_get_crtc_info_reply(connection, xcb_randr_get_crtc_info(connection, info->crtc, resources->config_timestamp), nullptr)
            : nullptr;
        if (crtc) {
            surface.outputs.push_back(X11Output{
                .x = crtc->x,
                .y = crtc->y,
                .width = crtc->width,
                .height = crtc->height,
                .hdr = getX11OutputFlag(connection, outputs[i], surface.hdrAtom)
                    && getX11OutputFlag(connection, outputs[i], surface.hdrCapableAtom),
            });
        }
        free(crtc);
        free(info);
    }
    free(resources);
}

static void openX11RandrEventConnection(HdrSurfaceData &surface)
{
    xcb_connection_t *connection = xcb_connect(nullptr, nullptr);
    const xcb_query_extension_reply_t *randr = xcb_connection_has_error(connection)
        ? nullptr : xcb_get_extension_data(connection, &xcb_randr_id);
    if (!randr || !randr->present) {
        xcb_disconnect(connection);
        return;
    }

    xcb_randr_select_input(connection, surface.xcbRoot,
                           XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE | XCB_RANDR_NOTIFY_MASK_CRTC_CHANGE
                           | XCB_RANDR_NOTIFY_MASK_OUTPUT_CHANGE | XCB_RANDR_NOTIFY_MASK_OUTPUT_PROPERTY);
    xcb_flush(connection);
    surface.randrEventConnection = connection;
    surface.randrFirstEvent = randr->first_event;
}

static void pollX11RandrEvents(HdrSurfaceData &surface)
{
    if (!surface.randrEventConnection) {
        return;
    }
    while (xcb_generic_event_t *event = xcb_poll_for_event(surface.randrEventConnection)) {
        const uint8_t type = event->response_type & ~0x80;
        if (type == surface.randrFirstEvent + XCB_RANDR_SCREEN_CHANGE_NOTIFY || type == surface.randrFirstEvent + XCB_RANDR_NOTIFY) {
            surface.outputsDirty = true;
        }
        free(event);
    }
}

static void updateX11HdrAvailability(HdrSurfaceData &surface)
{
    if (!surface.hdrAtom || !surface.hdrCapableAtom) {
        return;
    }

    pollX11RandrEvents(surface);
    if (surface.outputsDirty) {
        readX11Outputs(surface);
    }

    bool onOutput = false;
    surface.hdrAvailable = true;
    for (const X11Output &output : surface.outputs) {
        const bool overlaps = surface.windowX < output.x + output.width && output.x < surface.windowX + surface.windowWidth
            && surface.windowY < output.y + output.height && output.y < surface.windowY + surface.windowHeight;
        if (overlaps) {
            onOutput = true;
            surface.hdrAvailable &= output.hdr;
        }
    }
    surface.hdrAvailable &= onOutput;
}

static void destroyX11HdrWindow(const HdrSurfaceData &surface)
{
    xcb_connection_t *connection = surface.xcbConnection;
    discardX11Errors(connection, xcb_destroy_window_checked(connection, surface.hdrWindow));
    discardX11Errors(connection, xcb_free_colormap_checked(connection, surface.hdrColormap));
    xcb_flush(connection);
}

static void setX11HdrMetadata(const HdrSurfaceData &surface, const hdr_output_metadata *metadata)
{
    const xcb_window_t window = surface.hdrWindow ? surface.hdrWindow : surface.xcbWindow;
    const auto cookie = metadata
        ? xcb_change_property_checked(surface.xcbConnection, XCB_PROP_MODE_REPLACE, window,
                                      surface.hdrMetadataAtom, XCB_ATOM_CARDINAL, 8, sizeof(*metadata), metadata)
        : xcb_delete_property_checked(surface.xcbConnection, window, surface.hdrMetadataAtom);
    discardX11Errors(surface.xcbConnection, cookie);
    xcb_flush(surface.xcbConnection);
}

static void destroyX11Surface(const HdrSurfaceData &surface)
{
    if (surface.windowGeometryRequest) {
        xcb_discard_reply(surface.xcbConnection, surface.windowPositionRequest);
        xcb_discard_reply(surface.xcbConnection, surface.windowGeometryRequest);
    }
    if (surface.hdrWindow) {
        destroyX11HdrWindow(surface);
    } else {
        setX11HdrMetadata(surface, nullptr);
    }
    if (surface.randrEventConnection) {
        xcb_disconnect(surface.randrEventConnection);
    }
}

static VkResult getDriverSurfaceFormats(const vkroots::VkInstanceDispatch *pDispatch, VkPhysicalDevice physicalDevice,
                                        VkSurfaceKHR surface, std::vector<VkSurfaceFormatKHR> &formats)
{
    uint32_t count = 0;
    VkResult result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, nullptr);
    if (result != VK_SUCCESS) {
        return result;
    }
    formats.resize(count);
    result = pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &count, formats.data());
    formats.resize(count);
    return result;
}

static std::vector<VkSurfaceFormatKHR> getHdrFormats(HdrSurfaceData &surface, const std::vector<VkSurfaceFormatKHR> &formats)
{
    surface.supportsPassthrough = std::ranges::any_of(formats, [](const VkSurfaceFormatKHR fmt) {
        return fmt.colorSpace == VK_COLOR_SPACE_PASS_THROUGH_EXT;
    });

    std::vector<VkSurfaceFormatKHR> hdrFormats = {};
    if (!surface.hdrAvailable) {
        return hdrFormats;
    }

    for (const VkSurfaceFormatKHR &hdrFormat : s_HdrSurfaceFormats) {
        const bool alreadySupported = std::ranges::any_of(formats, [&hdrFormat](const VkSurfaceFormatKHR fmt) {
            return fmt.format == hdrFormat.format && fmt.colorSpace == hdrFormat.colorSpace;
        });
        const bool hasFormat = std::ranges::any_of(formats, [&hdrFormat](const VkSurfaceFormatKHR fmt) {
            return fmt.format == hdrFormat.format;
        });
        if (!alreadySupported && hasFormat) {
            fprintf(stderr, "[HDR Layer] Enabling format: %u colorspace: %u\n", hdrFormat.format, hdrFormat.colorSpace);
            hdrFormats.push_back(hdrFormat);
        }
    }
    return hdrFormats;
}

static VkResult getHdrSurfaceExtraFormats(const vkroots::VkInstanceDispatch *pDispatch, VkPhysicalDevice physicalDevice,
                                          VkSurfaceKHR surface, HdrSurfaceData &hdrSurface,
                                          std::vector<VkSurfaceFormatKHR> &extraFormats)
{
    std::vector<VkSurfaceFormatKHR> formats;
    VkResult result = getDriverSurfaceFormats(pDispatch, physicalDevice, surface, formats);
    if (result < 0) {
        return result;
    }

    syncX11WindowGeometry(hdrSurface);
    updateX11HdrAvailability(hdrSurface);

    if (hdrSurface.hdrWindowSurface) {
        std::vector<VkSurfaceFormatKHR> hdrWindowFormats;
        result = getDriverSurfaceFormats(pDispatch, physicalDevice, hdrSurface.hdrWindowSurface, hdrWindowFormats);
        if (result < 0) {
            return result;
        }
        for (const auto &hdrWindowFormat : hdrWindowFormats) {
            const bool duplicate = std::ranges::any_of(formats, [&hdrWindowFormat](const VkSurfaceFormatKHR fmt) {
                return fmt.format == hdrWindowFormat.format && fmt.colorSpace == hdrWindowFormat.colorSpace;
            });
            if (!duplicate) {
                extraFormats.push_back(hdrWindowFormat);
            }
        }
        formats.insert(formats.end(), extraFormats.begin(), extraFormats.end());
    }

    const std::vector<VkSurfaceFormatKHR> hdrFormats = getHdrFormats(hdrSurface, formats);
    extraFormats.insert(extraFormats.end(), hdrFormats.begin(), hdrFormats.end());
    return VK_SUCCESS;
}

struct HdrSwapchainData {
    VkSurfaceKHR surface;
    VkSurfaceKHR driverSurface;
    bool usesHdrWindow = false;
    bool hdrAvailable = false;
    bool hdr10 = false;

    VkHdrMetadataEXT metadata;
    bool metadataDirty;
};
VKROOTS_DEFINE_SYNCHRONIZED_MAP_TYPE(HdrSwapchain, VkSwapchainKHR);

class VkInstanceOverrides
{
public:
    static VkResult CreateXcbSurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
    {
        VkResult res = pDispatch->CreateXcbSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
        if (res != VK_SUCCESS) {
            return res;
        }

        createX11HdrSurface(instance, *pSurface, pCreateInfo->connection, pCreateInfo->window,
                            [&](xcb_window_t window, VkSurfaceKHR *surface) {
            VkXcbSurfaceCreateInfoKHR info = *pCreateInfo;
            info.window = window;
            return pDispatch->CreateXcbSurfaceKHR(instance, &info, pAllocator, surface);
        });
        return VK_SUCCESS;
    }

    static VkResult CreateXlibSurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSurfaceKHR *pSurface)
    {
        VkResult res = pDispatch->CreateXlibSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
        if (res != VK_SUCCESS) {
            return res;
        }

        createX11HdrSurface(instance, *pSurface, XGetXCBConnection(pCreateInfo->dpy), pCreateInfo->window,
                            [&](xcb_window_t window, VkSurfaceKHR *surface) {
            VkXlibSurfaceCreateInfoKHR info = *pCreateInfo;
            info.window = window;
            return pDispatch->CreateXlibSurfaceKHR(instance, &info, pAllocator, surface);
        });
        return VK_SUCCESS;
    }

    static VkResult GetPhysicalDeviceSurfaceFormatsKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        VkSurfaceKHR surface,
        uint32_t *pSurfaceFormatCount,
        VkSurfaceFormatKHR *pSurfaceFormats)
    {
        auto hdrSurface = HdrSurface::get(surface);
        if (!hdrSurface)
            return pDispatch->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);

        std::vector<VkSurfaceFormatKHR> extraFormats;
        const VkResult result = getHdrSurfaceExtraFormats(pDispatch, physicalDevice, surface, *hdrSurface.get(), extraFormats);
        if (result < 0) {
            return result;
        }

        return vkroots::helpers::append(
                   pDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
                   extraFormats,
                   pSurfaceFormatCount,
                   pSurfaceFormats,
                   physicalDevice,
                   surface);
    }

    static VkResult GetPhysicalDeviceSurfaceFormats2KHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        const VkPhysicalDeviceSurfaceInfo2KHR *pSurfaceInfo,
        uint32_t *pSurfaceFormatCount,
        VkSurfaceFormat2KHR *pSurfaceFormats)
    {
        auto hdrSurface = HdrSurface::get(pSurfaceInfo->surface);
        if (!hdrSurface) {
            return pDispatch->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
        }

        std::vector<VkSurfaceFormatKHR> extraSurfaceFormats;
        const VkResult result = getHdrSurfaceExtraFormats(pDispatch, physicalDevice, pSurfaceInfo->surface, *hdrSurface.get(), extraSurfaceFormats);
        if (result < 0) {
            return result;
        }

        std::vector<VkSurfaceFormat2KHR> extraFormats = {};
        for (const auto &format : extraSurfaceFormats) {
            extraFormats.push_back(VkSurfaceFormat2KHR{
                .sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,
                .pNext = nullptr,
                .surfaceFormat = format,
            });
        }

        return vkroots::helpers::append(
                   pDispatch->GetPhysicalDeviceSurfaceFormats2KHR,
                   extraFormats,
                   pSurfaceFormatCount,
                   pSurfaceFormats,
                   physicalDevice,
                   pSurfaceInfo);
    }

    static void DestroySurfaceKHR(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkInstance instance,
        VkSurfaceKHR surface,
        const VkAllocationCallbacks *pAllocator)
    {
        if (auto state = HdrSurface::get(surface)) {
            if (state->hdrWindow) {
                pDispatch->DestroySurfaceKHR(instance, state->hdrWindowSurface, pAllocator);
            }
            destroyX11Surface(*state.get());
        }
        HdrSurface::remove(surface);
        pDispatch->DestroySurfaceKHR(instance, surface, pAllocator);
    }

    static VkResult
    EnumerateDeviceExtensionProperties(
        const vkroots::VkInstanceDispatch *pDispatch,
        VkPhysicalDevice physicalDevice,
        const char *pLayerName,
        uint32_t *pPropertyCount,
        VkExtensionProperties *pProperties)
    {
        static constexpr std::array<VkExtensionProperties, 1> s_LayerExposedExts = {{
                {
                    VK_EXT_HDR_METADATA_EXTENSION_NAME,
                    VK_EXT_HDR_METADATA_SPEC_VERSION
                },
            }
        };

        if (pLayerName) {
            if (pLayerName == "VK_LAYER_hdr_wsi"sv) {
                return vkroots::helpers::array(s_LayerExposedExts, pPropertyCount, pProperties);
            } else {
                return pDispatch->EnumerateDeviceExtensionProperties(physicalDevice, pLayerName, pPropertyCount, pProperties);
            }
        }

        return vkroots::helpers::append(
                   pDispatch->EnumerateDeviceExtensionProperties,
                   s_LayerExposedExts,
                   pPropertyCount,
                   pProperties,
                   physicalDevice,
                   pLayerName);
    }

private:
    using CreateDriverSurface = std::function<VkResult(xcb_window_t window, VkSurfaceKHR *surface)>;

    static xcb_atom_t internX11Atom(xcb_connection_t *connection, std::string_view name, bool onlyIfExists)
    {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
            connection, xcb_intern_atom(connection, onlyIfExists, name.size(), name.data()), nullptr);
        xcb_atom_t atom = XCB_ATOM_NONE;
        if (reply) {
            atom = reply->atom;
        }
        free(reply);
        return atom;
    }

    static void createX11HdrSurface(VkInstance instance, VkSurfaceKHR surface, xcb_connection_t *connection, xcb_window_t window,
                                    const CreateDriverSurface &createDriverSurface)
    {
        const xcb_atom_t hdrMetadataAtom = internX11Atom(connection, "hdr_output_metadata"sv, false);
        if (hdrMetadataAtom == XCB_ATOM_NONE) {
            fprintf(stderr, "[HDR Layer] failed to intern the hdr_output_metadata atom\n");
            return;
        }

        HdrSurfaceData data{
            .instance = instance,
            .xcbConnection = connection,
            .xcbWindow = window,
            .hdrMetadataAtom = hdrMetadataAtom,
            .hdrAtom = internX11Atom(connection, "HDR"sv, true),
            .hdrCapableAtom = internX11Atom(connection, "HDR capable"sv, true),
            .outputsDirty = true,
        };

        createX11HdrWindow(data, createDriverSurface);
        if (data.hdrAtom && data.hdrCapableAtom) {
            openX11RandrEventConnection(data);
        } else {
            fprintf(stderr, "[HDR Layer] X server doesn't have the HDR output properties\n");
        }
        HdrSurface::create(surface, std::move(data));

        fprintf(stderr, "[HDR Layer] Created X11 HDR surface\n");
    }

    static void createX11HdrWindow(HdrSurfaceData &surface, const CreateDriverSurface &createDriverSurface)
    {
        xcb_connection_t *connection = surface.xcbConnection;
        xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, surface.xcbWindow), nullptr);
        if (!geometry) {
            return;
        }
        const uint8_t depth = geometry->depth;
        const xcb_window_t root = geometry->root;
        surface.xcbRoot = root;
        const uint16_t width = std::max<uint16_t>(geometry->width, 1);
        const uint16_t height = std::max<uint16_t>(geometry->height, 1);
        free(geometry);

        if (depth == HDR_WINDOW_DEPTH) {
            return;
        }

        const xcb_visualid_t visual = findHdrVisual(connection, root);
        if (visual == XCB_NONE) {
            fprintf(stderr, "[HDR Layer] X server doesn't support 10-bit windows\n");
            return;
        }

        surface.hdrColormap = xcb_generate_id(connection);
        surface.hdrWindow = xcb_generate_id(connection);
        surface.hdrWindowWidth = width;
        surface.hdrWindowHeight = height;

        const uint32_t values[] = { XCB_BACK_PIXMAP_NONE, 0, surface.hdrColormap };
        const bool created =
            checkX11Request(connection, xcb_create_colormap_checked(connection, XCB_COLORMAP_ALLOC_NONE, surface.hdrColormap, root, visual)) &&
            checkX11Request(connection, xcb_create_window_checked(connection, HDR_WINDOW_DEPTH, surface.hdrWindow, surface.xcbWindow,
                                                                  0, 0, width, height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, visual,
                                                                  XCB_CW_BACK_PIXMAP | XCB_CW_BORDER_PIXEL | XCB_CW_COLORMAP, values)) &&
            checkX11Request(connection, xcb_shape_rectangles_checked(connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                                                                     XCB_CLIP_ORDERING_UNSORTED, surface.hdrWindow, 0, 0, 0, nullptr));

        if (!created || createDriverSurface(surface.hdrWindow, &surface.hdrWindowSurface) != VK_SUCCESS) {
            fprintf(stderr, "[HDR Layer] Failed to create a 10-bit window\n");
            destroyX11HdrWindow(surface);
            surface.hdrWindow = XCB_NONE;
            surface.hdrColormap = XCB_NONE;
            surface.hdrWindowSurface = VK_NULL_HANDLE;
        }
    }
};

class VkDeviceOverrides
{
public:
    static void DestroySwapchainKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        VkSwapchainKHR swapchain,
        const VkAllocationCallbacks *pAllocator)
    {
        HdrSwapchain::remove(swapchain);
        pDispatch->DestroySwapchainKHR(device, swapchain, pAllocator);
    }

    static VkResult CreateSwapchainKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        const VkSwapchainCreateInfoKHR *pCreateInfo,
        const VkAllocationCallbacks *pAllocator,
        VkSwapchainKHR *pSwapchain)
    {
        VkSurfaceKHR oldDriverSurface = VK_NULL_HANDLE;
        if (auto oldSwapchain = HdrSwapchain::get(pCreateInfo->oldSwapchain)) {
            oldDriverSurface = oldSwapchain->driverSurface;
        }

        auto hdrSurface = HdrSurface::get(pCreateInfo->surface);
        if (!hdrSurface)
            return pDispatch->CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

        VkSwapchainCreateInfoKHR swapchainInfo = *pCreateInfo;

        // If this is a custom surface, force the colorspace to something the driver won't touch
        if (hdrSurface->supportsPassthrough) {
            swapchainInfo.imageColorSpace = VK_COLOR_SPACE_PASS_THROUGH_EXT;
        } else {
            swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        }

        const bool useHdrWindow = hdrSurface->hdrWindowSurface && isTenBitFormat(pCreateInfo->imageFormat);
        if (useHdrWindow) {
            swapchainInfo.surface = hdrSurface->hdrWindowSurface;
        }
        if (oldDriverSurface && oldDriverSurface != swapchainInfo.surface) {
            swapchainInfo.oldSwapchain = VK_NULL_HANDLE;
        }

        fprintf(stderr, "[HDR Layer] Creating swapchain for window: %u - format: %s - colorspace: %s%s\n",
                hdrSurface->xcbWindow,
                vkroots::helpers::enumString(pCreateInfo->imageFormat),
                vkroots::helpers::enumString(pCreateInfo->imageColorSpace),
                useHdrWindow ? " - 10-bit window" : "");

        // Check for VkFormat support and return VK_ERROR_INITIALIZATION_FAILED
        // if that VkFormat is unsupported for the underlying surface.
        {
            std::vector<VkSurfaceFormatKHR> supportedSurfaceFormats;
            vkroots::helpers::enumerate(
                pDispatch->pPhysicalDeviceDispatch->pInstanceDispatch->GetPhysicalDeviceSurfaceFormatsKHR,
                supportedSurfaceFormats,
                pDispatch->PhysicalDevice,
                swapchainInfo.surface);

            bool supportedSwapchainFormat = std::ranges::find_if(supportedSurfaceFormats, [=](VkSurfaceFormatKHR value) {
                return value.format == swapchainInfo.imageFormat;
            }) != supportedSurfaceFormats.end();

            if (!supportedSwapchainFormat) {
                fprintf(stderr, "[HDR Layer] Refusing to make swapchain (unsupported VkFormat) for window: %u - format: %s - colorspace: %s\n",
                        hdrSurface->xcbWindow,
                        vkroots::helpers::enumString(pCreateInfo->imageFormat),
                        vkroots::helpers::enumString(pCreateInfo->imageColorSpace));

                return VK_ERROR_INITIALIZATION_FAILED;
            }
        }

        if (useHdrWindow) {
            syncX11WindowGeometry(*hdrSurface.get());
        }

        VkResult result = pDispatch->CreateSwapchainKHR(device, &swapchainInfo, pAllocator, pSwapchain);
        if (result != VK_SUCCESS) {
            return result;
        }
        if (hdrSurface->hdrWindow) {
            setX11HdrWindowMapped(*hdrSurface.get(), useHdrWindow);
        }
        const bool hdrAvailable = hdrSurface->hdrAvailable;
        hdrSurface.clear();

        const bool hdr10 = pCreateInfo->imageColorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT;
        if (!hdr10 && pCreateInfo->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
            && pCreateInfo->imageColorSpace != VK_COLOR_SPACE_PASS_THROUGH_EXT) {
            fprintf(stderr, "[HDR Layer] Unknown colorspace %d, assuming SDR\n", pCreateInfo->imageColorSpace);
        }

        HdrSwapchain::create(*pSwapchain, HdrSwapchainData{
            .surface = pCreateInfo->surface,
            .driverSurface = swapchainInfo.surface,
            .usesHdrWindow = useHdrWindow,
            .hdrAvailable = hdrAvailable,
            .hdr10 = hdr10,
            .metadataDirty = true,
        });
        return VK_SUCCESS;
    }

    static void
    SetHdrMetadataEXT(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkDevice device,
        uint32_t swapchainCount,
        const VkSwapchainKHR *pSwapchains,
        const VkHdrMetadataEXT *pMetadata)
    {
        for (uint32_t i = 0; i < swapchainCount; i++) {
            auto hdrSwapchain = HdrSwapchain::get(pSwapchains[i]);
            if (!hdrSwapchain) {
                fprintf(stderr, "[HDR Layer] SetHdrMetadataEXT: Swapchain %u does not support HDR.\n", i);
                continue;
            }

            auto hdrSurface = HdrSurface::get(hdrSwapchain->surface);
            if (!hdrSurface) {
                fprintf(stderr, "[HDR Layer] SetHdrMetadataEXT: Surface for swapchain %u was already destroyed. (App use after free).\n", i);
                abort();
            }

            const VkHdrMetadataEXT &metadata = pMetadata[i];

            fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: mastering luminance min %f nits, max %f nits\n", metadata.minLuminance, metadata.maxLuminance);
            fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: maxContentLightLevel %f nits\n", metadata.maxContentLightLevel);
            fprintf(stderr, "[HDR Layer] VkHdrMetadataEXT: maxFrameAverageLightLevel %f nits\n", metadata.maxFrameAverageLightLevel);

            hdrSwapchain->metadata = metadata;
            hdrSwapchain->metadataDirty = true;
        }
    }

    struct hdr_luminance_metadata {
        uint32_t min_luminance;
        uint32_t max_luminance;
        uint32_t max_fall;
        uint32_t max_cll;
    };
    /* Some games set luminance values above the maximum that pq can represent */
    static uint32_t clampLuminance(float nits, double unit)
    {
        constexpr double PQ_MAX_LUMINANCE = 10000.0;
        return std::round(std::fmin(std::fmax(nits, 0.0), PQ_MAX_LUMINANCE) * unit);
    }

    static std::optional<hdr_luminance_metadata> validateHdrMetadata(const VkHdrMetadataEXT &metadata)
    {
        constexpr int MIN_LUM_FACTOR = 10000;
        hdr_luminance_metadata ret {
            .min_luminance = clampLuminance(metadata.minLuminance, MIN_LUM_FACTOR),
            .max_luminance = clampLuminance(metadata.maxLuminance, 1.0),
            .max_fall = clampLuminance(metadata.maxFrameAverageLightLevel, 1.0),
            .max_cll = clampLuminance(metadata.maxContentLightLevel, 1.0),
        };
        if (ret.max_cll != 0) {
            const bool invalid = ret.max_cll * MIN_LUM_FACTOR < ret.min_luminance
                || (ret.max_luminance != 0 && ret.max_cll > ret.max_luminance);
            if (invalid) {
                return std::nullopt;
            }
        }
        if (ret.max_fall != 0) {
            const bool invalid = ret.max_fall * MIN_LUM_FACTOR < ret.min_luminance
                || (ret.max_luminance != 0 && ret.max_fall > ret.max_luminance)
                || (ret.max_cll != 0 && ret.max_fall > ret.max_cll);
            if (invalid) {
                return std::nullopt;
            }
        }

        if (ret.max_luminance != 0 && ret.max_luminance * MIN_LUM_FACTOR <= ret.min_luminance) {
            return std::nullopt;
        }
        return ret;
    }

    static uint16_t toUint16(double value)
    {
        return std::clamp(std::round(value), 0.0, 65535.0);
    }

    static hdr_output_metadata toHdrOutputMetadata(const VkHdrMetadataEXT &metadata)
    {
        constexpr double primaryUnit = 50'000.0;
        constexpr uint8_t HDMI_EOTF_SMPTE_ST2084 = 2;
        constexpr uint8_t HDMI_STATIC_METADATA_TYPE1 = 0;

        hdr_output_metadata ret;
        memset(&ret, 0, sizeof(ret));
        ret.metadata_type = HDMI_STATIC_METADATA_TYPE1;

        hdr_metadata_infoframe &infoframe = ret.hdmi_metadata_type1;
        infoframe.eotf = HDMI_EOTF_SMPTE_ST2084;
        infoframe.metadata_type = HDMI_STATIC_METADATA_TYPE1;

        const VkXYColorEXT primaries[] = {metadata.displayPrimaryRed, metadata.displayPrimaryGreen, metadata.displayPrimaryBlue};
        for (size_t i = 0; i < std::size(primaries); i++) {
            infoframe.display_primaries[i].x = toUint16(primaries[i].x * primaryUnit);
            infoframe.display_primaries[i].y = toUint16(primaries[i].y * primaryUnit);
        }
        infoframe.white_point.x = toUint16(metadata.whitePoint.x * primaryUnit);
        infoframe.white_point.y = toUint16(metadata.whitePoint.y * primaryUnit);

        if (const auto luminance = validateHdrMetadata(metadata)) {
            infoframe.max_display_mastering_luminance = toUint16(luminance->max_luminance);
            infoframe.min_display_mastering_luminance = toUint16(luminance->min_luminance);
            infoframe.max_cll = toUint16(luminance->max_cll);
            infoframe.max_fall = toUint16(luminance->max_fall);
        }
        return ret;
    }

    static void updateX11HdrMetadata(const HdrSurfaceData &surface, const HdrSwapchainData &swapchain)
    {
        if (!swapchain.hdr10) {
            setX11HdrMetadata(surface, nullptr);
            return;
        }

        const hdr_output_metadata metadata = toHdrOutputMetadata(swapchain.metadata);
        setX11HdrMetadata(surface, &metadata);
    }

    static VkResult QueuePresentKHR(
        const vkroots::VkDeviceDispatch *pDispatch,
        VkQueue queue,
        const VkPresentInfoKHR *pPresentInfo)
    {
        std::vector<uint32_t> suboptimalSwapchains;
        for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
            auto hdrSwapchain = HdrSwapchain::get(pPresentInfo->pSwapchains[i]);
            if (!hdrSwapchain) {
                continue;
            }

            auto hdrSurface = HdrSurface::get(hdrSwapchain->surface);
            if (!hdrSurface) {
                continue;
            }

            pollX11WindowGeometry(*hdrSurface.get());
            updateX11HdrAvailability(*hdrSurface.get());
            if (hdrSurface->hdrAvailable != hdrSwapchain->hdrAvailable) {
                suboptimalSwapchains.push_back(i);
            }

            if (hdrSwapchain->metadataDirty) {
                updateX11HdrMetadata(*hdrSurface.get(), *hdrSwapchain.get());
                hdrSwapchain->metadataDirty = false;
            }
        }

        VkResult result = pDispatch->QueuePresentKHR(queue, pPresentInfo);
        for (const uint32_t i : suboptimalSwapchains) {
            if (pPresentInfo->pResults && pPresentInfo->pResults[i] == VK_SUCCESS) {
                pPresentInfo->pResults[i] = VK_SUBOPTIMAL_KHR;
            }
        }
        if (!suboptimalSwapchains.empty() && result == VK_SUCCESS) {
            result = VK_SUBOPTIMAL_KHR;
        }
        return result;
    }
};
}

VKROOTS_DEFINE_LAYER_INTERFACES(HdrLayer::VkInstanceOverrides,
                                vkroots::NoOverrides,
                                HdrLayer::VkDeviceOverrides);

VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(HdrLayer::HdrSurface);
VKROOTS_IMPLEMENT_SYNCHRONIZED_MAP_TYPE(HdrLayer::HdrSwapchain);
