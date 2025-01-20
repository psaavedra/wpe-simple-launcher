project('wpe-simple-launcher', 'c',
  version: '1.0',
  default_options: ['c_std=gnu11'])

# Dependencies
glib_dep = dependency('glib-2.0')
webkit2_dep = dependency('wpe-webkit-2.0')

# Source files
sources = files('main.c')

# Build executable
executable('wpe-simple-launcher',
  sources,
  dependencies: [glib_dep, webkit2_dep])

