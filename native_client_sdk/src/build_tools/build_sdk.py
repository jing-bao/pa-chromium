#!/usr/bin/env python
# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Entry point for both build and try bots.

This script is invoked from XXX, usually without arguments
to package an SDK. It automatically determines whether
this SDK is for mac, win, linux.

The script inspects the following environment variables:

BUILDBOT_BUILDERNAME to determine whether the script is run locally
and whether it should upload an SDK to file storage (GSTORE)
"""

# pylint: disable=W0621

# std python includes
import copy
import datetime
import glob
import optparse
import os
import re
import sys

if sys.version_info < (2, 6, 0):
  sys.stderr.write("python 2.6 or later is required run this script\n")
  sys.exit(1)

# local includes
import buildbot_common
import build_projects
import build_updater
import build_version
import generate_make
import generate_notice
import manifest_util
import parse_dsc
import verify_filelist

from build_paths import SCRIPT_DIR, SDK_SRC_DIR, SRC_DIR, NACL_DIR, OUT_DIR
from build_paths import PPAPI_DIR, NACLPORTS_DIR, GSTORE

# Add SDK make tools scripts to the python path.
sys.path.append(os.path.join(SDK_SRC_DIR, 'tools'))
sys.path.append(os.path.join(NACL_DIR, 'build'))

import getos
import oshelpers

CYGTAR = os.path.join(NACL_DIR, 'build', 'cygtar.py')

NACLPORTS_URL = 'https://naclports.googlecode.com/svn/trunk/src'
NACLPORTS_REV = 774

GYPBUILD_DIR = 'gypbuild'

options = None


def GetGlibcToolchain(platform, arch):
  tcdir = os.path.join(NACL_DIR, 'toolchain', '.tars')
  tcname = 'toolchain_%s_%s.tar.bz2' % (platform, arch)
  return os.path.join(tcdir, tcname)


def GetNewlibToolchain(platform, arch):
  tcdir = os.path.join(NACL_DIR, 'toolchain', '.tars')
  tcname = 'naclsdk_%s_%s.tgz' % (platform, arch)
  return os.path.join(tcdir, tcname)


def GetPNaClToolchain(os_platform, arch):
  tcdir = os.path.join(NACL_DIR, 'toolchain', '.tars')
  tcname = 'naclsdk_pnacl_%s_%s.tgz' % (os_platform, arch)
  return os.path.join(tcdir, tcname)


def GetArchName(arch, xarch=None):
  if xarch:
    return arch + '-' + str(xarch)
  return arch


def GetToolchainNaClInclude(tcname, tcpath, arch):
  if arch == 'x86':
    if tcname == 'pnacl':
      return os.path.join(tcpath, 'newlib', 'sdk', 'include')
    return os.path.join(tcpath, 'x86_64-nacl', 'include')
  elif arch == 'arm':
    return os.path.join(tcpath, 'arm-nacl', 'include')
  else:
    buildbot_common.ErrorExit('Unknown architecture: %s' % arch)


def GetToolchainNaClLib(tcname, tcpath, arch, xarch):
  if arch == 'x86':
    if tcname == 'pnacl':
      return os.path.join(tcpath, 'newlib', 'sdk', 'lib')
    if str(xarch) == '32':
      return os.path.join(tcpath, 'x86_64-nacl', 'lib32')
    if str(xarch) == '64':
      return os.path.join(tcpath, 'x86_64-nacl', 'lib')
    if str(xarch) == 'arm':
      return os.path.join(tcpath, 'arm-nacl', 'lib')
  buildbot_common.ErrorExit('Unknown architecture: %s' % arch)


def GetPNaClNativeLib(tcpath, arch):
  if arch not in ['arm', 'x86-32', 'x86-64']:
    buildbot_common.ErrorExit('Unknown architecture %s.' % arch)
  return os.path.join(tcpath, 'lib-' + arch)


def GetSconsArgs(tcpath, outdir, arch, xarch=None):
  """Return list of scons build arguments to generate user libraries.

  Only used for pnacl builds.
  """
  if sys.platform in ['cygwin', 'win32']:
    scons = 'scons.bat'
  else:
    scons = './scons'
  mode = '--mode=opt-host,nacl'
  arch_name = GetArchName(arch, xarch)
  plat = 'platform=' + arch_name
  binarg = 'bindir=' + os.path.join(outdir, 'tools')
  lib = 'libdir=' + GetToolchainNaClLib('pnacl', tcpath, arch, xarch)
  args = [scons, mode, plat, binarg, lib, '-j10', 'install_lib', 'bitcode=1']

  print "Building pnacl (%s): %s" % (arch, ' '.join(args))
  return args


def BuildStepDownloadToolchains():
  buildbot_common.BuildStep('Running download_toolchains.py')
  download_script = os.path.join('build', 'download_toolchains.py')
  buildbot_common.Run([sys.executable, download_script,
                      '--no-arm-trusted', '--arm-untrusted', '--keep'],
                      cwd=NACL_DIR)


def BuildStepCleanPepperDirs(pepperdir, pepperdir_old):
  buildbot_common.BuildStep('Clean Pepper Dirs')
  buildbot_common.RemoveDir(pepperdir_old)
  buildbot_common.RemoveDir(pepperdir)
  buildbot_common.MakeDir(pepperdir)


def BuildStepMakePepperDirs(pepperdir, subdirs):
  for subdir in subdirs:
    buildbot_common.MakeDir(os.path.join(pepperdir, subdir))


def BuildStepCopyTextFiles(pepperdir, pepper_ver, revision):
  buildbot_common.BuildStep('Add Text Files')
  files = ['AUTHORS', 'COPYING', 'LICENSE']
  files = [os.path.join(SDK_SRC_DIR, filename) for filename in files]
  oshelpers.Copy(['-v'] + files + [pepperdir])

  # Replace a few placeholders in README
  readme_text = open(os.path.join(SDK_SRC_DIR, 'README')).read()
  readme_text = readme_text.replace('${VERSION}', pepper_ver)
  readme_text = readme_text.replace('${REVISION}', revision)

  # Year/Month/Day Hour:Minute:Second
  time_format = '%Y/%m/%d %H:%M:%S'
  readme_text = readme_text.replace('${DATE}',
      datetime.datetime.now().strftime(time_format))

  open(os.path.join(pepperdir, 'README'), 'w').write(readme_text)


def BuildStepUntarToolchains(pepperdir, platform, arch, toolchains):
  buildbot_common.BuildStep('Untar Toolchains')
  tcname = platform + '_' + arch
  tmpdir = os.path.join(OUT_DIR, 'tc_temp')
  buildbot_common.RemoveDir(tmpdir)
  buildbot_common.MakeDir(tmpdir)

  if 'newlib' in toolchains:
    # Untar the newlib toolchains
    tarfile = GetNewlibToolchain(platform, arch)
    buildbot_common.Run([sys.executable, CYGTAR, '-C', tmpdir, '-xf', tarfile],
                        cwd=NACL_DIR)

    # Then rename/move it to the pepper toolchain directory
    srcdir = os.path.join(tmpdir, 'sdk', 'nacl-sdk')
    newlibdir = os.path.join(pepperdir, 'toolchain', tcname + '_newlib')
    buildbot_common.Move(srcdir, newlibdir)

  if 'arm' in toolchains:
    # Copy the existing arm toolchain from native_client tree
    arm_toolchain = os.path.join(NACL_DIR, 'toolchain',
                                 platform + '_arm_newlib')
    arm_toolchain_sdk = os.path.join(pepperdir, 'toolchain',
                                     os.path.basename(arm_toolchain))
    buildbot_common.CopyDir(arm_toolchain, arm_toolchain_sdk)

  if 'glibc' in toolchains:
    # Untar the glibc toolchains
    tarfile = GetGlibcToolchain(platform, arch)
    buildbot_common.Run([sys.executable, CYGTAR, '-C', tmpdir, '-xf', tarfile],
                        cwd=NACL_DIR)

    # Then rename/move it to the pepper toolchain directory
    srcdir = os.path.join(tmpdir, 'toolchain', tcname)
    glibcdir = os.path.join(pepperdir, 'toolchain', tcname + '_glibc')
    buildbot_common.Move(srcdir, glibcdir)

  # Untar the pnacl toolchains
  if 'pnacl' in toolchains:
    tmpdir = os.path.join(tmpdir, 'pnacl')
    buildbot_common.RemoveDir(tmpdir)
    buildbot_common.MakeDir(tmpdir)
    tarfile = GetPNaClToolchain(platform, arch)
    buildbot_common.Run([sys.executable, CYGTAR, '-C', tmpdir, '-xf', tarfile],
                        cwd=NACL_DIR)

    # Then rename/move it to the pepper toolchain directory
    pnacldir = os.path.join(pepperdir, 'toolchain', tcname + '_pnacl')
    buildbot_common.Move(tmpdir, pnacldir)

  buildbot_common.RemoveDir(tmpdir)

  if options.gyp and sys.platform not in ['cygwin', 'win32']:
    # If the gyp options is specified we install a toolchain
    # wrapper so that gyp can switch toolchains via a commandline
    # option.
    bindir = os.path.join(pepperdir, 'toolchain', tcname, 'bin')
    wrapper = os.path.join(SDK_SRC_DIR, 'tools', 'compiler-wrapper.py')
    buildbot_common.MakeDir(bindir)
    buildbot_common.CopyFile(wrapper, bindir)

    # Module 'os' has no 'symlink' member (on Windows).
    # pylint: disable=E1101

    os.symlink('compiler-wrapper.py', os.path.join(bindir, 'i686-nacl-g++'))
    os.symlink('compiler-wrapper.py', os.path.join(bindir, 'i686-nacl-gcc'))
    os.symlink('compiler-wrapper.py', os.path.join(bindir, 'i686-nacl-ar'))


# List of toolchain headers to install.
# Source is relative to native_client tree, destination is relative
# to the toolchain header directory.
NACL_HEADER_MAP = {
  'newlib': [
      ('src/include/nacl/nacl_exception.h', 'nacl/'),
      ('src/include/nacl/nacl_minidump.h', 'nacl/'),
      ('src/untrusted/irt/irt.h', ''),
      ('src/untrusted/irt/irt_ppapi.h', ''),
      ('src/untrusted/nacl/nacl_dyncode.h', 'nacl/'),
      ('src/untrusted/nacl/nacl_startup.h', 'nacl/'),
      ('src/untrusted/nacl/nacl_thread.h', 'nacl/'),
      ('src/untrusted/nacl/pnacl.h', ''),
      ('src/untrusted/pthread/pthread.h', ''),
      ('src/untrusted/pthread/semaphore.h', ''),
      ('src/untrusted/valgrind/dynamic_annotations.h', 'nacl/'),
  ],
  'glibc': [
      ('src/include/nacl/nacl_exception.h', 'nacl/'),
      ('src/include/nacl/nacl_minidump.h', 'nacl/'),
      ('src/untrusted/irt/irt.h', ''),
      ('src/untrusted/irt/irt_ppapi.h', ''),
      ('src/untrusted/nacl/nacl_dyncode.h', 'nacl/'),
      ('src/untrusted/nacl/nacl_startup.h', 'nacl/'),
      ('src/untrusted/nacl/nacl_thread.h', 'nacl/'),
      ('src/untrusted/nacl/pnacl.h', ''),
      ('src/untrusted/valgrind/dynamic_annotations.h', 'nacl/'),
  ],
  'host': []
}

# Source relative to 'ppapi' foler.  Destiniation relative
# to SDK include folder.
PPAPI_HEADER_MAP = [
  # Copy the KHR headers
  ('lib/gl/include/KHR/khrplatform.h',     'KHR/'),

  # Copy the GLES2 headers
  ('lib/gl/include/GLES2/gl2.h',           'GLES2/'),
  ('lib/gl/include/GLES2/gl2ext.h',        'GLES2/'),
  ('lib/gl/include/GLES2/gl2platform.h',   'GLES2/'),

  # Copy the EGL headers
  ('lib/gl/include/EGL/egl.h',             'EGL/'),
  ('lib/gl/include/EGL/eglext.h',          'EGL/'),
  ('lib/gl/include/EGL/eglplatform.h',     'EGL/'),

  # Copy in the gles2 headers
  ('lib/gl/gles2/gl2ext_ppapi.h',          'ppapi/gles2/'),
  # Create a duplicate copy of this header
  # TODO(sbc), remove this copy once we find a way to build gl2ext_ppapi.c.
  ('lib/gl/gles2/gl2ext_ppapi.h',          'ppapi/lib/gl/gles2/'),

  # Copy in the C++ headers
  ('utility/graphics/paint_aggregator.h',  'ppapi/utility/graphics/'),
  ('utility/graphics/paint_manager.h',     'ppapi/utility/graphics/'),
  ('utility/threading/lock.h',             'ppapi/utility/threading/'),
  ('utility/threading/simple_thread.h',    'ppapi/utility/threading/'),
  ('utility/websocket/websocket_api.h',    'ppapi/utility/websocket/'),
  ('utility/completion_callback_factory.h','ppapi/utility/'),
  ('utility/completion_callback_factory_thread_traits.h', 'ppapi/utility/'),

  # Copy in c, c/dev and c/extensions/dev headers
  # TODO(sbc): remove the use of wildcards here so that we can more
  # tightly control what ends up in the SDK.
  ('c/*.h',                  'ppapi/c/'),
  ('c/dev/*.h',              'ppapi/c/dev/'),
  ('c/extensions/dev/*.h',   'ppapi/c/extensions/dev/'),

  # Copy in cpp, cpp/dev, cpp/extensions/, cpp/extensions/dev
  ('cpp/*.h',                'ppapi/cpp/'),
  ('cpp/extensions/*.h',     'ppapi/cpp/extensions/'),
  ('cpp/dev/*.h',            'ppapi/cpp/dev/'),
  ('cpp/extensions/dev/*.h', 'ppapi/cpp/extensions/dev/'),

  # Copy certain private headers (specifically these are the ones
  # that are used by nacl-mounts)
  ('cpp/private/ext_crx_file_system_private.h', 'ppapi/cpp/private/'),
  ('cpp/private/file_io_private.h', 'ppapi/cpp/private/'),
  ('cpp/private/net_address_private.h', 'ppapi/cpp/private/'),
  ('cpp/private/tcp_server_socket_private.h', 'ppapi/cpp/private/'),
  ('cpp/private/host_resolver_private.h', 'ppapi/cpp/private/'),
  ('cpp/private/pass_file_handle.h', 'ppapi/cpp/private/'),
  ('cpp/private/tcp_socket_private.h', 'ppapi/cpp/private/'),
  ('cpp/private/udp_socket_private.h', 'ppapi/cpp/private/'),
  ('cpp/private/x509_certificate_private.h', 'ppapi/cpp/private/'),

  ('c/private/pp_file_handle.h', 'ppapi/c/private/'),
  ('c/private/ppb_ext_crx_file_system_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_file_io_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_file_ref_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_host_resolver_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_tcp_server_socket_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_net_address_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_tcp_socket_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_udp_socket_private.h', 'ppapi/c/private/'),
  ('c/private/ppb_x509_certificate_private.h', 'ppapi/c/private/'),
]


def InstallCommonHeaders(inc_path):
  InstallFiles(PPAPI_DIR, inc_path, PPAPI_HEADER_MAP)


def InstallFiles(src_root, dest_root, file_list):
  """Copy a set of files from src_root to dest_root according
  to the given mapping.  This allows files to be copied from
  to a location in the destination tree that is different to the
  location in the source tree.

  If the destination mapping ends with a '/' then the destination
  basename is inherited from the the source file.

  Wildcards can be used in the source list but it is not recommended
  as this can end up adding things to the SDK unintentionally.
  """
  for file_spec in file_list:
    # The list of files to install can be a simple list of
    # strings or a list of pairs, where each pair corresponds
    # to a mapping from source to destination names.
    if type(file_spec) == str:
      src_file = dest_file = file_spec
    else:
      src_file, dest_file = file_spec

    src_file = os.path.join(src_root, src_file)

    # Expand sources files using glob.
    sources = glob.glob(src_file)
    if not sources:
      sources = [src_file]

    if len(sources) > 1 and not dest_file.endswith('/'):
      buildbot_common.ErrorExit("Target file must end in '/' when "
                                "using globbing to install multiple files")

    for source in sources:
      if dest_file.endswith('/'):
        dest = os.path.join(dest_file, os.path.basename(source))
      else:
        dest = dest_file
      dest = os.path.join(dest_root, dest)
      if not os.path.isdir(os.path.dirname(dest)):
        buildbot_common.MakeDir(os.path.dirname(dest))
      buildbot_common.CopyFile(source, dest)


def InstallNaClHeaders(tc_dst_inc, tc_name):
  """Copies NaCl headers to expected locations in the toolchain."""
  if tc_name == 'arm':
    # arm toolchain header should be the same as the x86 newlib
    # ones
    tc_name = 'newlib'

  InstallFiles(NACL_DIR, tc_dst_inc, NACL_HEADER_MAP[tc_name])


def MakeNinjaRelPath(path):
  return os.path.join(os.path.relpath(OUT_DIR, SRC_DIR), path)


TOOLCHAIN_LIBS = {
  'newlib' : [
    'crti.o',
    'crtn.o',
    'libminidump_generator.a',
    'libnacl.a',
    'libnacl_dyncode.a',
    'libnacl_exception.a',
    'libnacl_list_mappings.a',
    'libnosys.a',
    'libppapi.a',
    'libppapi_stub.a',
    'libpthread.a',
  ],
  'glibc': [
    'libminidump_generator.a',
    'libminidump_generator.so',
    'libnacl.a',
    'libnacl_dyncode.a',
    'libnacl_dyncode.so',
    'libnacl_exception.a',
    'libnacl_exception.so',
    'libnacl_list_mappings.a',
    'libnacl_list_mappings.so',
    'libppapi.a',
    'libppapi.so',
    'libppapi_stub.a',
  ]
}


def GypNinjaInstall(pepperdir, platform, toolchains):
  build_dir = GYPBUILD_DIR
  ninja_out_dir = os.path.join(OUT_DIR, build_dir, 'Release')
  tools_files = [
    ['sel_ldr', 'sel_ldr_x86_32'],
    ['ncval_new', 'ncval'],
    ['irt_core_newlib_x32.nexe', 'irt_core_x86_32.nexe'],
    ['irt_core_newlib_x64.nexe', 'irt_core_x86_64.nexe'],
  ]
  if sys.platform not in ['cygwin', 'win32']:
    minidump_files = [
      ['dump_syms', 'dump_syms'],
      ['minidump_dump', 'minidump_dump'],
      ['minidump_stackwalk', 'minidump_stackwalk']
    ]
    tools_files.extend(minidump_files)

  # TODO(binji): dump_syms doesn't currently build on Windows. See
  # http://crbug.com/245456
  if platform != 'win':
    tools_files.append(['dump_syms', 'dump_syms'])

  if platform != 'mac':
    # Mac doesn't build 64-bit binaries.
    tools_files.append(['sel_ldr64', 'sel_ldr_x86_64'])

  if platform == 'linux':
    tools_files.append(['nacl_helper_bootstrap',
                        'nacl_helper_bootstrap_x86_32'])
    tools_files.append(['nacl_helper_bootstrap64',
                        'nacl_helper_bootstrap_x86_64'])

  buildbot_common.MakeDir(os.path.join(pepperdir, 'tools'))

  # Add .exe extensions to all windows tools
  for pair in tools_files:
    if platform == 'win' and not pair[0].endswith('.nexe'):
      pair[0] += '.exe'
      pair[1] += '.exe'

  InstallFiles(ninja_out_dir, os.path.join(pepperdir, 'tools'), tools_files)

  for tc in set(toolchains) & set(['newlib', 'glibc']):
    for archname in ('arm', '32', '64'):
      if tc == 'glibc' and archname == 'arm':
        continue
      tc_dir = 'tc_' + tc
      lib_dir = 'lib' + archname
      if archname == 'arm':
        build_dir = GYPBUILD_DIR + '-arm'
        tcdir = '%s_arm_%s' % (platform, tc)
      else:
        build_dir = GYPBUILD_DIR
        tcdir = '%s_x86_%s' % (platform, tc)

      ninja_out_dir = os.path.join(OUT_DIR, build_dir, 'Release')
      src_dir = os.path.join(ninja_out_dir, 'gen', tc_dir, lib_dir)
      tcpath = os.path.join(pepperdir, 'toolchain', tcdir)
      dst_dir = GetToolchainNaClLib(tc, tcpath, 'x86', archname)

      InstallFiles(src_dir, dst_dir, TOOLCHAIN_LIBS[tc])

      ninja_tcpath = os.path.join(ninja_out_dir, 'gen', 'sdk', 'toolchain',
                      tcdir)
      lib_dir = GetToolchainNaClLib(tc, ninja_tcpath, 'x86', archname)
      buildbot_common.CopyFile(os.path.join(lib_dir, 'crt1.o'), dst_dir)



def GypNinjaBuild_NaCl(platform, rel_out_dir):
  gyp_py = os.path.join(NACL_DIR, 'build', 'gyp_nacl')
  nacl_core_sdk_gyp = os.path.join(NACL_DIR, 'build', 'nacl_core_sdk.gyp')
  all_gyp = os.path.join(NACL_DIR, 'build', 'all.gyp')

  out_dir = MakeNinjaRelPath(rel_out_dir)
  out_dir_arm = MakeNinjaRelPath(rel_out_dir + '-arm')
  GypNinjaBuild('ia32', gyp_py, nacl_core_sdk_gyp, 'nacl_core_sdk', out_dir)
  GypNinjaBuild('arm', gyp_py, nacl_core_sdk_gyp, 'nacl_core_sdk', out_dir_arm)
  GypNinjaBuild('ia32', gyp_py, all_gyp, 'ncval_new', out_dir)

  if platform == 'win':
    NinjaBuild('sel_ldr64', out_dir)
  elif platform == 'linux':
    out_dir_64 = MakeNinjaRelPath(rel_out_dir + '-64')
    GypNinjaBuild('x64', gyp_py, nacl_core_sdk_gyp, 'sel_ldr', out_dir_64)

    # We only need sel_ldr from the 64-bit out directory.
    # sel_ldr needs to be renamed, so we'll call it sel_ldr64.
    files_to_copy = [
      ('sel_ldr', 'sel_ldr64'),
      ('nacl_helper_bootstrap', 'nacl_helper_bootstrap64'),
    ]

    for src, dst in files_to_copy:
      buildbot_common.CopyFile(
          os.path.join(SRC_DIR, out_dir_64, 'Release', src),
          os.path.join(SRC_DIR, out_dir, 'Release', dst))


def GypNinjaBuild_Breakpad(platform, rel_out_dir):
  # TODO(binji): dump_syms doesn't currently build on Windows. See
  # http://crbug.com/245456
  if platform == 'win':
    return

  gyp_py = os.path.join(SRC_DIR, 'build', 'gyp_chromium')
  out_dir = MakeNinjaRelPath(rel_out_dir)
  gyp_file = os.path.join(SRC_DIR, 'breakpad', 'breakpad.gyp')
  build_list = ['dump_syms', 'minidump_dump', 'minidump_stackwalk']
  GypNinjaBuild('ia32', gyp_py, gyp_file, build_list, out_dir)


def GypNinjaBuild_PPAPI(arch, rel_out_dir):
  gyp_py = os.path.join(SRC_DIR, 'build', 'gyp_chromium')
  out_dir = MakeNinjaRelPath(rel_out_dir)
  gyp_file = os.path.join(SRC_DIR, 'ppapi', 'native_client',
                          'native_client.gyp')
  GypNinjaBuild(arch, gyp_py, gyp_file, 'ppapi_lib', out_dir)


def GypNinjaBuild_Pnacl(rel_out_dir, target_arch):
  # TODO(binji): This will build the pnacl_irt_shim twice; once as part of the
  # Chromium build, and once here. When we move more of the SDK build process
  # to gyp, we can remove this.
  gyp_py = os.path.join(SRC_DIR, 'build', 'gyp_chromium')

  out_dir = MakeNinjaRelPath(rel_out_dir)
  gyp_file = os.path.join(SRC_DIR, 'ppapi', 'native_client', 'src',
                          'untrusted', 'pnacl_irt_shim', 'pnacl_irt_shim.gyp')
  targets = ['pnacl_irt_shim']
  GypNinjaBuild(target_arch, gyp_py, gyp_file, targets, out_dir, False)

  gyp_py = os.path.join(NACL_DIR, 'build', 'gyp_nacl')
  gyp_file = os.path.join(NACL_DIR, 'src', 'untrusted', 'minidump_generator',
                          'minidump_generator.gyp')
  targets = ['minidump_generator_lib']
  GypNinjaBuild(target_arch, gyp_py, gyp_file, targets, out_dir, False)

  gyp_file = os.path.join(NACL_DIR, 'src', 'untrusted', 'nacl', 'nacl.gyp')
  targets = ['nacl_exception_lib']
  GypNinjaBuild(target_arch, gyp_py, gyp_file, targets, out_dir, False)


def GypNinjaBuild(arch, gyp_py_script, gyp_file, targets,
                  out_dir, force_arm_gcc=True):
  gyp_env = copy.copy(os.environ)
  gyp_env['GYP_GENERATORS'] = 'ninja'
  gyp_defines = []
  if options.mac_sdk:
    gyp_defines.append('mac_sdk=%s' % options.mac_sdk)
  if arch:
    gyp_defines.append('target_arch=%s' % arch)
    if arch == 'arm':
      gyp_defines += ['armv7=1', 'arm_thumb=0', 'arm_neon=1']
      if force_arm_gcc:
        gyp_defines += ['nacl_enable_arm_gcc=1']

  gyp_env['GYP_DEFINES'] = ' '.join(gyp_defines)
  for key in ['GYP_GENERATORS', 'GYP_DEFINES']:
    value = gyp_env[key]
    print '%s="%s"' % (key, value)
  gyp_generator_flags = ['-G', 'output_dir=%s' % (out_dir,)]
  gyp_depth = '--depth=.'
  buildbot_common.Run(
      [sys.executable, gyp_py_script, gyp_file, gyp_depth] + \
          gyp_generator_flags,
      cwd=SRC_DIR,
      env=gyp_env)
  NinjaBuild(targets, out_dir)


def NinjaBuild(targets, out_dir):
  if type(targets) is not list:
    targets = [targets]
  out_config_dir = os.path.join(out_dir, 'Release')
  buildbot_common.Run(['ninja', '-C', out_config_dir] + targets, cwd=SRC_DIR)


def BuildStepBuildToolchains(pepperdir, platform, toolchains):
  buildbot_common.BuildStep('SDK Items')

  GypNinjaBuild_NaCl(platform, GYPBUILD_DIR)
  GypNinjaBuild_Breakpad(platform, GYPBUILD_DIR)

  tcname = platform + '_x86'
  newlibdir = os.path.join(pepperdir, 'toolchain', tcname + '_newlib')
  glibcdir = os.path.join(pepperdir, 'toolchain', tcname + '_glibc')
  pnacldir = os.path.join(pepperdir, 'toolchain', tcname + '_pnacl')

  if set(toolchains) & set(['glibc', 'newlib']):
    GypNinjaBuild_PPAPI('ia32', GYPBUILD_DIR)

  if 'arm' in toolchains:
    GypNinjaBuild_PPAPI('arm', GYPBUILD_DIR + '-arm')

  GypNinjaInstall(pepperdir, platform, toolchains)

  if 'newlib' in toolchains:
    InstallNaClHeaders(GetToolchainNaClInclude('newlib', newlibdir, 'x86'),
                       'newlib')

  if 'glibc' in toolchains:
    InstallNaClHeaders(GetToolchainNaClInclude('glibc', glibcdir, 'x86'),
                       'glibc')

  if 'arm' in toolchains:
    tcname = platform + '_arm_newlib'
    armdir = os.path.join(pepperdir, 'toolchain', tcname)
    InstallNaClHeaders(GetToolchainNaClInclude('newlib', armdir, 'arm'),
                       'arm')

  if 'pnacl' in toolchains:
    # shell=True is needed on windows to enable searching of the PATH:
    # http://bugs.python.org/issue8557
    shell = platform == 'win'
    buildbot_common.Run(
        GetSconsArgs(pnacldir, pepperdir, 'x86', '32'),
        cwd=NACL_DIR,
        shell=shell)

    for arch in ('ia32', 'arm'):
      # Fill in the latest native pnacl shim library from the chrome build.
      build_dir = GYPBUILD_DIR + '-pnacl-' + arch
      GypNinjaBuild_Pnacl(build_dir, arch)
      pnacl_libdir_map = {'ia32': 'x86-64', 'arm': 'arm'}
      release_build_dir = os.path.join(OUT_DIR, build_dir, 'Release',
                                       'gen', 'tc_pnacl_translate',
                                       'lib-' + pnacl_libdir_map[arch])

      buildbot_common.CopyFile(
          os.path.join(release_build_dir, 'libpnacl_irt_shim.a'),
          GetPNaClNativeLib(pnacldir, pnacl_libdir_map[arch]))

      release_build_dir = os.path.join(OUT_DIR, build_dir, 'Release',
                                       'gen', 'tc_pnacl_newlib', 'lib')
      buildbot_common.CopyFile(
          os.path.join(release_build_dir, 'libminidump_generator.a'),
          GetPNaClNativeLib(pnacldir, pnacl_libdir_map[arch]))

      buildbot_common.CopyFile(
          os.path.join(release_build_dir, 'libnacl_exception.a'),
          GetPNaClNativeLib(pnacldir, pnacl_libdir_map[arch]))

    InstallNaClHeaders(GetToolchainNaClInclude('pnacl', pnacldir, 'x86'),
                       'newlib')




def MakeDirectoryOrClobber(pepperdir, dirname, clobber):
  dirpath = os.path.join(pepperdir, dirname)
  if clobber:
    buildbot_common.RemoveDir(dirpath)
  buildbot_common.MakeDir(dirpath)

  return dirpath


def BuildStepUpdateHelpers(pepperdir, platform, clobber):
  buildbot_common.BuildStep('Update project helpers')
  build_projects.UpdateHelpers(pepperdir, platform, clobber=clobber)


def BuildStepUpdateUserProjects(pepperdir, platform, toolchains,
                                build_experimental, clobber):
  buildbot_common.BuildStep('Update examples and libraries')

  filters = {}
  if not build_experimental:
    filters['EXPERIMENTAL'] = False
  if toolchains:
    toolchains = toolchains[:]

    # arm isn't a valid toolchain for build_projects
    if 'arm' in toolchains:
      toolchains.remove('arm')

    if 'host' in toolchains:
      toolchains.remove('host')
      toolchains.append(platform)

    filters['TOOLS'] = toolchains

  # Update examples and libraries
  filters['DEST'] = [
    'examples/api',
    'examples/demo',
    'examples/getting_started',
    'examples/tutorial',
    'src'
  ]

  tree = parse_dsc.LoadProjectTree(SDK_SRC_DIR, filters=filters)
  build_projects.UpdateProjects(pepperdir, platform, tree, clobber=clobber,
                                toolchains=toolchains)


def BuildStepMakeAll(pepperdir, platform, directory, step_name,
                     clean=False, deps=True, config='Debug'):
  buildbot_common.BuildStep(step_name)
  make_dir = os.path.join(pepperdir, directory)

  print "\n\nMake: " + make_dir
  if platform == 'win':
    make = os.path.join(make_dir, 'make.bat')
  else:
    make = 'make'

  extra_args = ['CONFIG='+config]
  if not deps:
    extra_args += ['IGNORE_DEPS=1']

  buildbot_common.Run([make, '-j8', 'TOOLCHAIN=all'] + extra_args,
                      cwd=make_dir)
  if clean:
    # Clean to remove temporary files but keep the built libraries.
    buildbot_common.Run([make, '-j8', 'clean', 'TOOLCHAIN=all'] + extra_args,
                        cwd=make_dir)


def BuildStepBuildLibraries(pepperdir, platform, directory):
  BuildStepMakeAll(pepperdir, platform, directory, 'Build Libraries Debug',
      clean=True, config='Debug')
  BuildStepMakeAll(pepperdir, platform, directory, 'Build Libraries Release',
      clean=True, config='Release')


def GenerateNotice(fileroot, output_filename='NOTICE', extra_files=None):
  # Look for LICENSE files
  license_filenames_re = re.compile('LICENSE|COPYING|COPYRIGHT')

  license_files = []
  for root, _, files in os.walk(fileroot):
    for filename in files:
      if license_filenames_re.match(filename):
        path = os.path.join(root, filename)
        license_files.append(path)

  if extra_files:
    license_files += [os.path.join(fileroot, f) for f in extra_files]
  print '\n'.join(license_files)

  if not os.path.isabs(output_filename):
    output_filename = os.path.join(fileroot, output_filename)
  generate_notice.Generate(output_filename, fileroot, license_files)


def BuildStepVerifyFilelist(pepperdir, platform):
  buildbot_common.BuildStep('Verify SDK Files')
  file_list_path = os.path.join(SCRIPT_DIR, 'sdk_files.list')
  try:
    verify_filelist.Verify(platform, file_list_path, pepperdir)
    print 'OK'
  except verify_filelist.ParseException, e:
    buildbot_common.ErrorExit('Parsing sdk_files.list failed:\n\n%s' % e)
  except verify_filelist.VerifyException, e:
    file_list_rel = os.path.relpath(file_list_path)
    verify_filelist_py = os.path.splitext(verify_filelist.__file__)[0] + '.py'
    verify_filelist_py = os.path.relpath(verify_filelist_py)
    pepperdir_rel = os.path.relpath(pepperdir)

    msg = """\
SDK verification failed:

%s
Add/remove files from %s to fix.

Run:
    ./%s %s %s
to test.""" % (e, file_list_rel, verify_filelist_py, file_list_rel,
               pepperdir_rel)
    buildbot_common.ErrorExit(msg)



def BuildStepTarBundle(pepper_ver, tarfile):
  buildbot_common.BuildStep('Tar Pepper Bundle')
  buildbot_common.MakeDir(os.path.dirname(tarfile))
  buildbot_common.Run([sys.executable, CYGTAR, '-C', OUT_DIR, '-cjf', tarfile,
       'pepper_' + pepper_ver], cwd=NACL_DIR)



def GetManifestBundle(pepper_ver, revision, tarfile, archive_url):
  with open(tarfile, 'rb') as tarfile_stream:
    archive_sha1, archive_size = manifest_util.DownloadAndComputeHash(
        tarfile_stream)

  archive = manifest_util.Archive(manifest_util.GetHostOS())
  archive.url = archive_url
  archive.size = archive_size
  archive.checksum = archive_sha1

  bundle = manifest_util.Bundle('pepper_' + pepper_ver)
  bundle.revision = int(revision)
  bundle.repath = 'pepper_' + pepper_ver
  bundle.version = int(pepper_ver)
  bundle.description = 'Chrome %s bundle, revision %s' % (pepper_ver, revision)
  bundle.stability = 'dev'
  bundle.recommended = 'no'
  bundle.archives = [archive]
  return bundle


def BuildStepArchiveBundle(name, pepper_ver, revision, tarfile):
  buildbot_common.BuildStep('Archive %s' % name)
  bucket_path = 'nativeclient-mirror/nacl/nacl_sdk/%s' % (
      build_version.ChromeVersion(),)
  tarname = os.path.basename(tarfile)
  tarfile_dir = os.path.dirname(tarfile)
  buildbot_common.Archive(tarname, bucket_path, tarfile_dir)

  # generate "manifest snippet" for this archive.
  archive_url = GSTORE + 'nacl_sdk/%s/%s' % (
      build_version.ChromeVersion(), tarname)
  bundle = GetManifestBundle(pepper_ver, revision, tarfile, archive_url)

  manifest_snippet_file = os.path.join(OUT_DIR, tarname + '.json')
  with open(manifest_snippet_file, 'wb') as manifest_snippet_stream:
    manifest_snippet_stream.write(bundle.GetDataAsString())

  buildbot_common.Archive(tarname + '.json', bucket_path, OUT_DIR,
                          step_link=False)


def BuildStepArchiveSDKTools():
  # Only push up sdk_tools.tgz and nacl_sdk.zip on the linux buildbot.
  builder_name = os.getenv('BUILDBOT_BUILDERNAME', '')
  if builder_name == 'linux-sdk-multi':
    buildbot_common.BuildStep('Build SDK Tools')
    build_updater.BuildUpdater(OUT_DIR)

    buildbot_common.BuildStep('Archive SDK Tools')
    bucket_path = 'nativeclient-mirror/nacl/nacl_sdk/%s' % (
        build_version.ChromeVersion(),)
    buildbot_common.Archive('sdk_tools.tgz', bucket_path, OUT_DIR,
                            step_link=False)
    buildbot_common.Archive('nacl_sdk.zip', bucket_path, OUT_DIR,
                            step_link=False)


def BuildStepSyncNaClPorts():
  """Pull the pinned revision of naclports from SVN."""
  buildbot_common.BuildStep('Sync naclports')
  if not os.path.exists(NACLPORTS_DIR):
    # checkout new copy of naclports
    cmd = ['svn', 'checkout', '-q', '-r', str(NACLPORTS_REV), NACLPORTS_URL,
           'naclports']
    buildbot_common.Run(cmd, cwd=os.path.dirname(NACLPORTS_DIR))
  else:
    # sync existing copy to pinned revision.
    cmd = ['svn', 'update', '-r', str(NACLPORTS_REV)]
    buildbot_common.Run(cmd, cwd=NACLPORTS_DIR)


def BuildStepBuildNaClPorts(pepper_ver, pepperdir):
  """Build selected naclports in all configurations."""
  # TODO(sbc): currently naclports doesn't know anything about
  # Debug builds so the Debug subfolders are all empty.
  bundle_dir = os.path.join(NACLPORTS_DIR, 'out', 'sdk_bundle')

  env = dict(os.environ)
  env['NACL_SDK_ROOT'] = pepperdir
  env['NACLPORTS_NO_ANNOTATE'] = "1"
  env['NACLPORTS_NO_UPLOAD'] = "1"

  build_script = 'build_tools/bots/linux/naclports-linux-sdk-bundle.sh'
  buildbot_common.BuildStep('Build naclports')
  buildbot_common.Run([build_script], env=env, cwd=NACLPORTS_DIR)

  out_dir = os.path.join(bundle_dir, 'pepper_%s' % pepper_ver)

  # Some naclports do not include a standalone LICENSE/COPYING file
  # so we explicitly list those here for inclusion.
  extra_licenses = ('tinyxml/readme.txt',
                    'jpeg-8d/README',
                    'zlib-1.2.3/README')
  src_root = os.path.join(NACLPORTS_DIR, 'out', 'repository-i686')
  output_license = os.path.join(out_dir, 'ports', 'LICENSE')
  GenerateNotice(src_root , output_license, extra_licenses)
  readme = os.path.join(out_dir, 'ports', 'README')
  oshelpers.Copy(['-v', os.path.join(SDK_SRC_DIR, 'README.naclports'), readme])


def BuildStepTarNaClPorts(pepper_ver, tarfile):
  """Create tar archive containing headers and libs from naclports build."""
  buildbot_common.BuildStep('Tar naclports Bundle')
  buildbot_common.MakeDir(os.path.dirname(tarfile))
  pepper_dir = 'pepper_%s' % pepper_ver
  archive_dirs = [os.path.join(pepper_dir, 'ports')]

  ports_out = os.path.join(NACLPORTS_DIR, 'out', 'sdk_bundle')
  cmd = [sys.executable, CYGTAR, '-C', ports_out, '-cjf', tarfile]
  cmd += archive_dirs
  buildbot_common.Run(cmd, cwd=NACL_DIR)


def main(args):
  parser = optparse.OptionParser()
  parser.add_option('--skip-tar', help='Skip generating a tarball.',
      action='store_true')
  parser.add_option('--archive', help='Force the archive step.',
      action='store_true')
  parser.add_option('--gyp',
      help='Use gyp to build examples/libraries/Makefiles.',
      action='store_true')
  parser.add_option('--release', help='PPAPI release version.',
      dest='release', default=None)
  parser.add_option('--build-ports',
      help='Build naclport bundle.', action='store_true')
  parser.add_option('--experimental',
      help='build experimental examples and libraries', action='store_true',
      dest='build_experimental')
  parser.add_option('--skip-toolchain', help='Skip toolchain untar',
      action='store_true')
  parser.add_option('--mac_sdk',
      help='Set the mac_sdk (e.g. 10.6) to use when building with ninja.',
      dest='mac_sdk')

  global options
  options, args = parser.parse_args(args[1:])
  platform = getos.GetPlatform()
  arch = 'x86'

  generate_make.use_gyp = options.gyp
  if buildbot_common.IsSDKBuilder():
    options.archive = True
    options.build_ports = True

  toolchains = ['newlib', 'glibc', 'arm', 'pnacl', 'host']
  print 'Building: ' + ' '.join(toolchains)

  if options.archive and options.skip_tar:
    parser.error('Incompatible arguments with archive.')

  chrome_version = int(build_version.ChromeMajorVersion())
  clnumber = build_version.ChromeRevision()
  pepper_ver = str(chrome_version)
  pepper_old = str(chrome_version - 1)
  pepperdir = os.path.join(OUT_DIR, 'pepper_' + pepper_ver)
  pepperdir_old = os.path.join(OUT_DIR, 'pepper_' + pepper_old)
  tarname = 'naclsdk_' + platform + '.tar.bz2'
  tarfile = os.path.join(OUT_DIR, tarname)

  if options.release:
    pepper_ver = options.release
  print 'Building PEPPER %s at %s' % (pepper_ver, clnumber)

  if 'NACL_SDK_ROOT' in os.environ:
    # We don't want the currently configured NACL_SDK_ROOT to have any effect
    # of the build.
    del os.environ['NACL_SDK_ROOT']

  BuildStepCleanPepperDirs(pepperdir, pepperdir_old)
  BuildStepMakePepperDirs(pepperdir, ['include', 'toolchain', 'tools'])

  if not options.skip_toolchain:
    BuildStepDownloadToolchains()
    BuildStepUntarToolchains(pepperdir, platform, arch, toolchains)

  BuildStepCopyTextFiles(pepperdir, pepper_ver, clnumber)
  BuildStepBuildToolchains(pepperdir, platform, toolchains)
  InstallCommonHeaders(os.path.join(pepperdir, 'include'))

  BuildStepUpdateHelpers(pepperdir, platform, True)
  BuildStepUpdateUserProjects(pepperdir, platform, toolchains,
                              options.build_experimental, True)

  # Ship with libraries prebuilt, so run that first.
  BuildStepBuildLibraries(pepperdir, platform, 'src')
  GenerateNotice(pepperdir)

  # Verify the SDK contains what we expect.
  BuildStepVerifyFilelist(pepperdir, platform)

  if not options.skip_tar:
    BuildStepTarBundle(pepper_ver, tarfile)

  if options.build_ports and platform == 'linux':
    ports_tarfile = os.path.join(OUT_DIR, 'naclports.tar.bz2')
    BuildStepSyncNaClPorts()
    BuildStepBuildNaClPorts(pepper_ver, pepperdir)
    if not options.skip_tar:
      BuildStepTarNaClPorts(pepper_ver, ports_tarfile)

  # Archive on non-trybots.
  if options.archive:
    BuildStepArchiveBundle('build', pepper_ver, clnumber, tarfile)
    if options.build_ports and platform == 'linux':
      BuildStepArchiveBundle('naclports', pepper_ver, clnumber, ports_tarfile)
    BuildStepArchiveSDKTools()

  return 0


if __name__ == '__main__':
  try:
    sys.exit(main(sys.argv))
  except KeyboardInterrupt:
    buildbot_common.ErrorExit('build_sdk: interrupted')
