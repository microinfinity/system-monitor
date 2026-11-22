project('system-monitor', ['cpp'],
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=2',
    'optimization=2',
  ]
)

cpp = meson.get_compiler('cpp')

# 1. Core Graphics & Native OS Backend Dependencies (Nisaba)
nisaba_dep = dependency('nisaba', required : true)
backend_os_lib = cpp.find_library('nisaba_backend_os', required : true)

# 2. System, Windowing & GPU Dependencies
gl_dep = dependency('gl', required : false)
glew_dep = dependency('glew', required : false)
threads_dep = dependency('threads', required : true)
wayland_client_dep = dependency('wayland-client', required : true)
dl_dep = cpp.find_library('dl', required : false)
m_dep = cpp.find_library('m', required : false)

sysmon_inc = include_directories('include', 'theme')

sysmon_cpp_args = [
  '-DNISABA_HAS_BACKEND_OS=1',
  '-D_USE_MATH_DEFINES',
  '-DSYSTEM_MONITOR_VERSION="' + meson.project_version() + '"',
]

sysmon_deps = [
  nisaba_dep,
  backend_os_lib,
  wayland_client_dep,
  threads_dep,
]

if gl_dep.found()
  sysmon_deps += gl_dep
endif

if glew_dep.found()
  sysmon_deps += glew_dep
  sysmon_cpp_args += '-DNISABA_GLEW=1'
endif

if dl_dep.found()
  sysmon_deps += dl_dep
endif

if m_dep.found()
  sysmon_deps += m_dep
endif

# 3. Sovereign System Monitor Sources (100% Native C++20 on Nisaba Engine)
sysmon_sources = files(
  'src/main.cpp',
  'src/csd_frame.cpp',
  'src/embedded_font.cpp',
  'src/system_telemetry.cpp',
  'src/system_monitor_app.cpp',
)

executable('system-monitor',
  sysmon_sources,
  include_directories : sysmon_inc,
  cpp_args : sysmon_cpp_args,
  dependencies : sysmon_deps,
  install : true,
)

install_data('system-monitor.desktop',
  install_dir : get_option('datadir') / 'applications',
)

install_data('theme/system-monitor.svg',
  install_dir : get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

summary({
  'Version'        : meson.project_version(),
  'Nisaba'         : nisaba_dep.found(),
  'Backend OS'     : backend_os_lib.found(),
  'Wayland Client' : wayland_client_dep.found(),
  'OpenGL'         : gl_dep.found(),
  'GLEW'           : glew_dep.found(),
}, section : 'System Monitor (Nisaba Engine)', bool_yn : true)
