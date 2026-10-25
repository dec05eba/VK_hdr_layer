vkroots_dep = dependency('vkroots')
xcb_dep = dependency('xcb')
x11_xcb_dep = dependency('x11-xcb')
xcb_shape_dep = dependency('xcb-shape')
xcb_randr_dep = dependency('xcb-randr')

hdr_wsi_layer = shared_library('VkLayer_hdr_wsi', 'VkLayer_hdr_wsi.cpp',
  dependencies     : [ vkroots_dep, xcb_dep, x11_xcb_dep, xcb_shape_dep, xcb_randr_dep ],
  install          : true )

out_lib_dir = join_paths(prefix, lib_dir)

configure_file(
    input         : 'VkLayer_hdr_wsi.json.in',
    output        : 'VkLayer_hdr_wsi.' + build_machine.cpu_family() + '.json',
    configuration : {'family' : build_machine.cpu_family(), 'lib_dir' : out_lib_dir },
    install       : true,
    install_dir   : join_paths(data_dir, 'vulkan', 'implicit_layer.d'),
)