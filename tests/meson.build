test_includes = include_directories('..', '../include', '../src/ipc/cmd')

strip_plugin = shared_module(
	'strip',
	'../examples' / 'layout_plugin' / 'strip.c',
	name_prefix: '',
	include_directories: include_directories('..', '../include'),
	c_args: ['-DWLR_USE_UNSTABLE'],
	dependencies: [
		dependency('wayland-server'),
		dependency('glib-2.0'),
		dependency('wlroots-0.21', version: ['>=0.21.0', '<0.22.0']),
	],
)

test_programs = {
	'actions': 'test_actions.c',
	'str': 'test_str.c',
	'bezier': 'test_bezier.c',
	'spring': 'test_spring.c',
	'layout-registry': 'test_layout_registry.c',
	'layout-config': 'test_layout_config.c',
	'config-table': 'test_config_table.c',
	'config-validate': 'test_config_validate.c',
	'ipc-dry-run': 'test_ipc_dry_run.c',
	'layer-rule': 'test_layer_rule.c',
	'permission': 'test_permission.c',
	'subscribe': 'test_subscribe.c',
	'config-reset': 'test_config_reset.c',
	'diagnostics': 'test_diagnostics.c',
}

foreach name, source : test_programs
	prog = executable(
		'test-' + name,
		source,
		include_directories: test_includes,
		link_with: doors_core,
		dependencies: libs + [wl_protos_dep],
	)
	test(name, prog)
endforeach

test_plugin_loader = executable(
	'test-plugin-loader',
	'test_plugin_loader.c',
	include_directories: test_includes,
	link_whole: doors_core,
	export_dynamic: true,
	dependencies: libs + [wl_protos_dep],
	c_args: ['-DSTRIP_PLUGIN_PATH="' + strip_plugin.full_path() + '"'],
)
test('plugin-loader', test_plugin_loader, depends: strip_plugin)
