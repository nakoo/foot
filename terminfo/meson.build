tic = find_program('tic', native: true)

if host_machine.system() != 'freebsd'
  custom_target(
    'terminfo',
    output: 'f',
    input: 'foot.info',
    command: [tic, '-x', '-o', '@OUTDIR@', '-e', 'foot,foot-direct', '@INPUT@'],
    install: true,
    install_dir: join_paths(get_option('datadir'), 'terminfo'))
endif
