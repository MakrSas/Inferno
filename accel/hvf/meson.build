hvf_ss = ss.source_set()
hvf_ss.add(files(
  'hvf-all.c',
  'hvf-accel-ops.c',
  'hvf-ios-compat.c',
))

specific_ss.add_all(when: 'CONFIG_HVF', if_true: hvf_ss)
