project('rgt-main-integration', 'cpp', default_options: ['cpp_std=c++20'])

cpp = meson.get_compiler('cpp')

devkit_dep = dependency('rgt-devkit',
  fallback: ['rgt-devkit', 'devkit_dep'],
  required: true,
)

gtest_dep = dependency('gtest', static: false)
gtest_main_dep = dependency('gtest_main', required: true)

poco_foundation = cpp.find_library('PocoFoundation', required: true)
poco_json = cpp.find_library('PocoJSON', required: true)
poco_net = cpp.find_library('PocoNet', required: true)
poco_data = cpp.find_library('PocoData', required: true)
poco_data_postgresql = cpp.find_library('PocoDataPostgreSQL', required: true)

gpx_dir = join_paths(meson.current_source_dir(), 'gpx-files')

rgt_main_integration_tests = executable(
  'rgt-main-integration-tests',
  files('race_test.cpp'),
  dependencies: [
    gtest_dep,
    gtest_main_dep,
    poco_foundation,
    poco_json,
    poco_net,
    poco_data,
    poco_data_postgresql,
    devkit_dep,
  ],
  include_directories: include_directories('.'),
  install: false,
  link_args: [
    '-Wl,-rpath,@executable_path/../lib',
    '-Wl,-rpath,/opt/homebrew/lib',
    '-Wl,-rpath,/usr/local/lib',
  ],
  cpp_args: [
    '-DPROJECT_NAME="rgt-main"',
    '-DGPX_FILES_DIR="@0@"'.format(gpx_dir),
  ],
)

test(
  'rgt-main-integration-tests',
  rgt_main_integration_tests,
  timeout: 600,
  is_parallel: false,
  workdir: meson.project_source_root(),
)
