lib_tests = [
	'igt_assert',
	'igt_can_fail',
	'igt_can_fail_simple',
	'igt_exit_handler',
	'igt_fork',
	'igt_fork_helper',
	'igt_hdmi_inject',
	'igt_list_only',
	'igt_invalid_subtest_name',
	'igt_no_exit',
	'igt_segfault',
	'igt_simulation',
	'igt_stats',
	'igt_subtest_group',
]

lib_fail_tests = [
	'igt_no_subtest',
	'igt_simple_test_subtests',
	'igt_timeout',
]

foreach lib_test : lib_tests
	exec = executable(lib_test, lib_test + '.c', install : false,
			dependencies : igt_deps)
	test('lib: ' + lib_test, exec)
endforeach

foreach lib_test : lib_fail_tests
	exec = executable(lib_test, lib_test + '.c', install : false,
			dependencies : igt_deps)
	test('lib: ' + lib_test, exec, should_fail : true)
endforeach
