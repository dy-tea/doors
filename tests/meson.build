test_includes = include_directories('../include', '../src/ipc/cmd')

test_programs = {
	'str': 'test_str.c',
	'bezier': 'test_bezier.c',
	'spring': 'test_spring.c',
	'config-table': 'test_config_table.c',
	'config-validate': 'test_config_validate.c',
	'ipc-dry-run': 'test_ipc_dry_run.c',
	'subscribe': 'test_subscribe.c',
	'config-reset': 'test_config_reset.c',
}

foreach name, source : test_programs
	prog = executable(
		'test-' + name,
		source,
		include_directories: test_includes,
		link_with: doors_core,
		dependencies: libs,
	)
	test(name, prog)
endforeach
