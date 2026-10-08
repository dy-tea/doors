project(
	'doors',
	'c',
	default_options: ['warning_level=3'],
)

add_project_arguments('-DWLR_USE_UNSTABLE', language: 'c')
if get_option('debug')
	add_project_arguments('-DDEBUG', language: 'c')
endif

wayland_protos = dependency('wayland-protocols')

wlroots_version = ['>=0.21.0', '<0.22.0']
subproject(
	'wlroots',
	default_options: ['examples=false'],
	required: false,
	version: wlroots_version,
)
wlroots = dependency('wlroots-0.21', version: wlroots_version, fallback: 'wlroots')

libs = [
	wlroots,
	dependency('wayland-server'),
	dependency('xkbcommon'),
	dependency('libinput'),
	dependency('pixman-1'),
	dependency('xcb'),
	dependency('xcb-icccm'),
	dependency('glesv2'),
	dependency('egl'),
	dependency('cairo'),
	dependency('pangocairo'),
	dependency('vulkan'),
	dependency('shaderc'),
	dependency('libpcre2-8'),
	wayland_protos,
]

libsystemd = dependency('libsystemd', required: false)
if libsystemd.found()
  libs += libsystemd
  add_project_arguments('-DHAVE_SYSTEMD=1', language: 'c')
endif

if host_machine.system() == 'freebsd'
  libs += declare_dependency(link_args: ['/usr/local/lib/libinotify.so', '-lpthread'])
endif

wl_protocol_dir = wayland_protos.get_variable('pkgdatadir')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
	wayland_scanner_dep.get_variable('wayland_scanner'),
	native: true,
)

protocols = [
	wl_protocol_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml',
	wl_protocol_dir / 'unstable' / 'text-input' / 'text-input-unstable-v3.xml',
	'protocols' / 'wlr-layer-shell-unstable-v1.xml',
	'protocols' / 'wlr-output-power-management-unstable-v1.xml',
	'protocols' / 'wlr-screencopy-unstable-v1.xml',
	'protocols' / 'ext-image-capture-source-v1.xml',
	'protocols' / 'ext-image-copy-capture-v1.xml',
	'protocols' / 'xx-hotkey-v1.xml',
	'protocols' / 'xx-session-management-v1.xml',
]

wl_protos_src = []

foreach xml : protocols
	wl_protos_src += custom_target(
		xml.underscorify() + '_c',
		input: xml,
		output: '@BASENAME@-protocol.c',
		command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
	)
	wl_protos_src += custom_target(
		xml.underscorify() + '_server_h',
		input: xml,
		output: '@BASENAME@-protocol.h',
		command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'],
	)
	wl_protos_src += custom_target(
		xml.underscorify() + '_client_h',
		input: xml,
		output: '@BASENAME@-client-protocol.h',
		command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
	)
endforeach

shader_headers = []
subdir('src' / 'shaders')

inc = include_directories(['include'])

executable(
	'doors',
	[
		'src' / 'animation.c',
		'src' / 'backend.c',
		'src' / 'bezier.c',
		'src' / 'borders.c',
		'src' / 'client.c',
		'src' / 'config.c',
		'src' / 'effects' / 'effects.c',
		'src' / 'effects' / 'gles2.c',
		'src' / 'effects' / 'vk.c',
		'src' / 'fs.c',
		'src' / 'input' / 'cursor.c',
		'src' / 'input' / 'gesture.c',
		'src' / 'input' / 'input.c',
		'src' / 'input' / 'input_method.c',
		'src' / 'input' / 'keyboard.c',
		'src' / 'input' / 'pointer.c',
		'src' / 'input' / 'seat.c',
		'src' / 'input' / 'tablet.c',
		'src' / 'input' / 'touch.c',
		'src' / 'ipc' / 'args.c',
		'src' / 'ipc' / 'helpers.c',
		'src' / 'ipc' / 'ipc.c',
		'src' / 'ipc' / 'json.c',
		'src' / 'ipc' / 'subscribe.c',
		'src' / 'ipc' / 'cmd' / 'actions.c',
		'src' / 'ipc' / 'cmd' / 'config.c',
		'src' / 'ipc' / 'cmd' / 'desktop.c',
		'src' / 'ipc' / 'cmd' / 'dispatch.c',
		'src' / 'ipc' / 'cmd' / 'env.c',
		'src' / 'ipc' / 'cmd' / 'globalshortcuts.c',
		'src' / 'ipc' / 'cmd' / 'hotkey.c',
		'src' / 'ipc' / 'cmd' / 'input.c',
		'src' / 'ipc' / 'cmd' / 'node.c',
		'src' / 'ipc' / 'cmd' / 'output.c',
		'src' / 'ipc' / 'cmd' / 'query.c',
		'src' / 'ipc' / 'cmd' / 'quit.c',
		'src' / 'ipc' / 'cmd' / 'rule.c',
		'src' / 'ipc' / 'cmd' / 'scratchpad.c',
		'src' / 'ipc' / 'cmd' / 'scroller.c',
		'src' / 'ipc' / 'cmd' / 'seat.c',
		'src' / 'ipc' / 'cmd' / 'sessions.c',
		'src' / 'ipc' / 'cmd' / 'wm.c',
		'src' / 'launcher.c',
		'src' / 'layout' / 'floating.c',
		'src' / 'layout' / 'layout.c',
		'src' / 'layout' / 'master_stack.c',
		'src' / 'layout' / 'scroller.c',
		'src' / 'layout' / 'scroller_view.c',
		'src' / 'layout' / 'tree.c',
		'src' / 'log.c',
		'src' / 'main.c',
		'src' / 'output' / 'config.c',
		'src' / 'output' / 'output.c',
		'src' / 'protocol' / 'bell.c',
		'src' / 'protocol' / 'capture.c',
		'src' / 'protocol' / 'copy_capture.c',
		'src' / 'protocol' / 'cursor_shape.c',
		'src' / 'protocol' / 'dialog.c',
		'src' / 'protocol' / 'drm_lease.c',
		'src' / 'protocol' / 'foreign_capture.c',
		'src' / 'protocol' / 'global_shortcuts.c',
		'src' / 'protocol' / 'idle.c',
		'src' / 'protocol' / 'idle_power.c',
		'src' / 'protocol' / 'layer.c',
		'src' / 'protocol' / 'lock.c',
		'src' / 'protocol' / 'output_mgmt.c',
		'src' / 'protocol' / 'pointer_constraint.c',
		'src' / 'protocol' / 'pointer_warp.c',
		'src' / 'protocol' / 'popup.c',
		'src' / 'protocol' / 'screencopy.c',
		'src' / 'protocol' / 'security_ctx.c',
		'src' / 'protocol' / 'session_mgmt.c',
		'src' / 'protocol' / 'shortcuts_inhibit.c',
		'src' / 'protocol' / 'tearing.c',
		'src' / 'protocol' / 'toplevel_tag.c',
		'src' / 'protocol' / 'virtual_keyboard.c',
		'src' / 'protocol' / 'virtual_pointer.c',
		'src' / 'protocol' / 'workspace.c',
		'src' / 'protocol' / 'xdg_decoration.c',
		'src' / 'protocol' / 'xdg_shell.c',
		'src' / 'protocol' / 'xdg_toplevel.c',
		'src' / 'protocol' / 'xwayland.c',
		'src' / 'realtime.c',
		'src' / 'render_unfocused.c',
		'src' / 'rule.c',
		'src' / 'scratchpad.c',
		'src' / 'server.c',
		'src' / 'settings.c',
		'src' / 'spring.c',
		'src' / 'surface.c',
		'src' / 'tabs.c',
		'src' / 'text.c',
		'src' / 'tiling_drag.c',
		'src' / 'transaction.c',
		'src' / 'tree.c',
		'src' / 'tree_focus.c',
		'src' / 'view.c',
		wl_protos_src,
		shader_headers,
	],
	include_directories: inc,
	dependencies: libs,
	install: true,
	install_dir: get_option('bindir'),
)

executable(
	'doorsctl',
	'doorsctl' / 'doorsctl.c',
	install: true,
	install_dir: get_option('bindir'),
)

install_data('doors.desktop', install_dir: '/usr/share/wayland-sessions')
