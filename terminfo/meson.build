tic = find_program('tic', native: true)

tic_generated = custom_target(
  'terminfo',
  output: 'f',
  input: 'foot.info',
  command: [tic, '-x', '-o', '@OUTDIR@', '-e', 'foot,foot-direct', '@INPUT@'],
  install: false,
  install_dir: join_paths(get_option('datadir'), 'terminfo'))

foreach terminfo : ['foot', 'foot-direct']
  custom_target(
    'terminfo-@0@'.format(terminfo),
    output: terminfo,
    input: tic_generated,
    command: ['cp', join_paths('@INPUT@', terminfo), '@OUTPUT@'],
    install: true,
    install_dir: join_paths(get_option('datadir'), 'terminfo', 'f'))
endforeach
