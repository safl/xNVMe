# SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
#
# SPDX-License-Identifier: BSD-3-Clause

dlfcn_dep = cc.find_library(
  'dl',
  has_headers: ['dlfcn.h'],
  required: conf_data.get('XNVME_BE_SPDK_ENABLED'),
)
math_dep = cc.find_library(
  'm',
  has_headers: ['math.h'],
  required: conf_data.get('XNVME_BE_SPDK_ENABLED'),
)
numa_dep = cc.find_library(
  'numa',
  has_headers: ['numaif.h'],
  required: is_linux and conf_data.get('XNVME_BE_SPDK_ENABLED'),
)
uuid_dep = cc.find_library(
  'uuid',
  dirs: is_freebsd ? [ '/usr/local/lib' ] : [],
  header_include_directories: is_freebsd ? [ include_directories('/usr/local/include') ] : [],
  has_headers: ['uuid/uuid.h'],
  required: conf_data.get('XNVME_BE_SPDK_ENABLED'),
)
ssl_dep = dependency(
  'openssl',
  version: '>=1.1.1',
  required: conf_data.get('XNVME_BE_SPDK_ENABLED'),
)
execinfo_dep = cc.find_library(
  'execinfo',
  has_headers: ['execinfo.h'],
  required: is_freebsd and conf_data.get('XNVME_BE_SPDK_ENABLED'),
)
elf_dep = cc.find_library(
  'elf',
  has_headers: ['sys/elf_common.h'],
  required: is_freebsd and conf_data.get('XNVME_BE_SPDK_ENABLED'),
)
spdk_system_library_dependencies = [dlfcn_dep, math_dep, numa_dep, uuid_dep, ssl_dep, execinfo_dep, elf_dep,]

spdk_libnames = [
  'spdk_nvme',
  'spdk_env_dpdk',
  'spdk_sock',
  'spdk_sock_posix',
  'spdk_rpc',
  'spdk_trace',
  'spdk_jsonrpc',
  'spdk_json',
  'spdk_util',
  'spdk_log'
]

if host_machine.system() != 'freebsd'
   spdk_libnames += 'spdk_vfio_user'
endif

dpdk_libnames = [
  'rte_eal',
  'rte_telemetry',
  'rte_bus_pci',
  'rte_pci',
  'rte_ring',
  'rte_mempool',
  'rte_kvargs'
]

isal_libnames = [
  'isal',
]

spdk_static_library_paths = []
foreach libname : spdk_libnames + dpdk_libnames + isal_libnames
  lib_dep = cc.find_library(
    libname,
    dirs: [
      spdk_repository_path / 'build' / 'lib',
      spdk_repository_path / 'dpdk' / 'build' / 'lib',
      spdk_repository_path / 'isa-l' / '.libs',
    ],
    static: true
  )

  # Create a bunch of paths
  paths = [
    spdk_repository_path / 'build' / 'lib' / 'lib' + libname + '.a',
    spdk_repository_path / 'dpdk' / 'build' / 'lib' / 'lib' + libname + '.a',
    spdk_repository_path / 'isa-l' / '.libs' / 'lib' + libname + '.a',
  ]
  foreach path : paths
    if lib_dep.found() and fs.exists(path)
      spdk_static_library_paths += path
    endif
  endforeach
endforeach

spdk_include_paths = include_directories(
  spdk_repository_path / 'dpdk' / 'build' / 'include',
  spdk_repository_path / 'build' / 'include'
)
spdk_link_args = ['-Wl,--whole-archive'] + spdk_static_library_paths + ['-Wl,--no-whole-archive']

# Construct link_args based on the above
spdk_dep = declare_dependency(
  dependencies: spdk_system_library_dependencies,
  link_args: spdk_link_args,
  include_directories: spdk_include_paths,
)
