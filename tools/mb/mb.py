#!/usr/bin/env python
# Copyright 2015 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""MB - the Meta-Build wrapper around GN.

MB is a wrapper script for GN that can be used to generate build files
for sets of canned configurations and analyze them.
"""

from __future__ import print_function

import argparse
import ast
import errno
import json
import os
import pipes
import platform
import pprint
import re
import shutil
import sys
import subprocess
import tempfile
import traceback
import urllib2
import zipfile

from collections import OrderedDict

CHROMIUM_SRC_DIR = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path = [os.path.join(CHROMIUM_SRC_DIR, 'build')] + sys.path

import gn_helpers


def main(args):
  mbw = MetaBuildWrapper()
  return mbw.Main(args)


class MetaBuildWrapper(object):
  def __init__(self):
    self.chromium_src_dir = CHROMIUM_SRC_DIR
    self.default_config = os.path.join(self.chromium_src_dir, 'tools', 'mb',
                                       'mb_config.pyl')
    self.default_isolate_map = os.path.join(self.chromium_src_dir, 'testing',
                                            'buildbot', 'gn_isolate_map.pyl')
    self.executable = sys.executable
    self.platform = sys.platform
    self.sep = os.sep
    self.args = argparse.Namespace()
    self.configs = {}
    self.masters = {}
    self.mixins = {}

  def Main(self, args):
    self.ParseArgs(args)
    try:
      ret = self.args.func()
      if ret:
        self.DumpInputFiles()
      return ret
    except KeyboardInterrupt:
      self.Print('interrupted, exiting')
      return 130
    except Exception:
      self.DumpInputFiles()
      s = traceback.format_exc()
      for l in s.splitlines():
        self.Print(l)
      return 1

  def ParseArgs(self, argv):
    def AddCommonOptions(subp):
      subp.add_argument('-b', '--builder',
                        help='builder name to look up config from')
      subp.add_argument('-m', '--master',
                        help='master name to look up config from')
      subp.add_argument('-c', '--config',
                        help='configuration to analyze')
      subp.add_argument('--phase',
                        help='optional phase name (used when builders '
                             'do multiple compiles with different '
                             'arguments in a single build)')
      subp.add_argument('-f', '--config-file', metavar='PATH',
                        default=self.default_config,
                        help='path to config file '
                             '(default is %(default)s)')
      subp.add_argument('-i', '--isolate-map-file', metavar='PATH',
                        help='path to isolate map file '
                             '(default is %(default)s)',
                        default=[],
                        action='append',
                        dest='isolate_map_files')
      subp.add_argument('-g', '--goma-dir',
                        help='path to goma directory')
      subp.add_argument('--android-version-code',
                        help='Sets GN arg android_default_version_code')
      subp.add_argument('--android-version-name',
                        help='Sets GN arg android_default_version_name')
      subp.add_argument('-n', '--dryrun', action='store_true',
                        help='Do a dry run (i.e., do nothing, just print '
                             'the commands that will run)')
      subp.add_argument('-v', '--verbose', action='store_true',
                        help='verbose logging')

    parser = argparse.ArgumentParser(prog='mb')
    subps = parser.add_subparsers()

    subp = subps.add_parser('analyze',
                            help='analyze whether changes to a set of files '
                                 'will cause a set of binaries to be rebuilt.')
    AddCommonOptions(subp)
    subp.add_argument('path',
                      help='path build was generated into.')
    subp.add_argument('input_path',
                      help='path to a file containing the input arguments '
                           'as a JSON object.')
    subp.add_argument('output_path',
                      help='path to a file containing the output arguments '
                           'as a JSON object.')
    subp.set_defaults(func=self.CmdAnalyze)

    subp = subps.add_parser('export',
                            help='print out the expanded configuration for'
                                 'each builder as a JSON object')
    subp.add_argument('-f', '--config-file', metavar='PATH',
                      default=self.default_config,
                      help='path to config file (default is %(default)s)')
    subp.add_argument('-g', '--goma-dir',
                      help='path to goma directory')
    subp.set_defaults(func=self.CmdExport)

    subp = subps.add_parser('gen',
                            help='generate a new set of build files')
    AddCommonOptions(subp)
    subp.add_argument('--swarming-targets-file',
                      help='save runtime dependencies for targets listed '
                           'in file.')
    subp.add_argument('path',
                      help='path to generate build into')
    subp.set_defaults(func=self.CmdGen)

    subp = subps.add_parser('isolate-everything',
                            help='generates a .isolate for all targets. '
                                 'Requires that mb.py gen has already been '
                                 'run.')
    AddCommonOptions(subp)
    subp.set_defaults(func=self.CmdIsolateEverything)
    subp.add_argument('path',
                      help='path build was generated into')

    subp = subps.add_parser('isolate',
                            help='generate the .isolate files for a given'
                                 'binary')
    AddCommonOptions(subp)
    subp.add_argument('--no-build', dest='build', default=True,
                      action='store_false',
                      help='Do not build, just isolate')
    subp.add_argument('-j', '--jobs', type=int,
                      help='Number of jobs to pass to ninja')
    subp.add_argument('path',
                      help='path build was generated into')
    subp.add_argument('target',
                      help='ninja target to generate the isolate for')
    subp.set_defaults(func=self.CmdIsolate)

    subp = subps.add_parser('lookup',
                            help='look up the command for a given config or '
                                 'builder')
    AddCommonOptions(subp)
    subp.set_defaults(func=self.CmdLookup)

    subp = subps.add_parser(
        'run',
        help='build and run the isolated version of a '
             'binary',
        formatter_class=argparse.RawDescriptionHelpFormatter)
    subp.description = (
        'Build, isolate, and run the given binary with the command line\n'
        'listed in the isolate. You may pass extra arguments after the\n'
        'target; use "--" if the extra arguments need to include switches.\n'
        '\n'
        'Examples:\n'
        '\n'
        '  % tools/mb/mb.py run -m chromium.linux -b "Linux Builder" \\\n'
        '    //out/Default content_browsertests\n'
        '\n'
        '  % tools/mb/mb.py run out/Default content_browsertests\n'
        '\n'
        '  % tools/mb/mb.py run out/Default content_browsertests -- \\\n'
        '    --test-launcher-retry-limit=0'
        '\n'
    )
    AddCommonOptions(subp)
    subp.add_argument('-j', '--jobs', type=int,
                      help='Number of jobs to pass to ninja')
    subp.add_argument('--no-build', dest='build', default=True,
                      action='store_false',
                      help='Do not build, just isolate and run')
    subp.add_argument('path',
                      help=('path to generate build into (or use).'
                            ' This can be either a regular path or a '
                            'GN-style source-relative path like '
                            '//out/Default.'))
    subp.add_argument('-s', '--swarmed', action='store_true',
                      help='Run under swarming with the default dimensions')
    subp.add_argument('-d', '--dimension', default=[], action='append', nargs=2,
                      dest='dimensions', metavar='FOO bar',
                      help='dimension to filter on')
    subp.add_argument('--no-default-dimensions', action='store_false',
                      dest='default_dimensions', default=True,
                      help='Do not automatically add dimensions to the task')
    subp.add_argument('target',
                      help='ninja target to build and run')
    subp.add_argument('extra_args', nargs='*',
                      help=('extra args to pass to the isolate to run. Use '
                            '"--" as the first arg if you need to pass '
                            'switches'))
    subp.set_defaults(func=self.CmdRun)

    subp = subps.add_parser('validate',
                            help='validate the config file')
    subp.add_argument('-f', '--config-file', metavar='PATH',
                      default=self.default_config,
                      help='path to config file (default is %(default)s)')
    subp.set_defaults(func=self.CmdValidate)

    subp = subps.add_parser('zip',
                            help='generate a .zip containing the files needed '
                                 'for a given binary')
    AddCommonOptions(subp)
    subp.add_argument('--no-build', dest='build', default=True,
                      action='store_false',
                      help='Do not build, just isolate')
    subp.add_argument('-j', '--jobs', type=int,
                      help='Number of jobs to pass to ninja')
    subp.add_argument('path',
                      help='path build was generated into')
    subp.add_argument('target',
                      help='ninja target to generate the isolate for')
    subp.add_argument('zip_path',
                      help='path to zip file to create')
    subp.set_defaults(func=self.CmdZip)

    subp = subps.add_parser('help',
                            help='Get help on a subcommand.')
    subp.add_argument(nargs='?', action='store', dest='subcommand',
                      help='The command to get help for.')
    subp.set_defaults(func=self.CmdHelp)

    self.args = parser.parse_args(argv)

  def DumpInputFiles(self):

    def DumpContentsOfFilePassedTo(arg_name, path):
      if path and self.Exists(path):
        self.Print("\n# To recreate the file passed to %s:" % arg_name)
        self.Print("%% cat > %s <<EOF" % path)
        contents = self.ReadFile(path)
        self.Print(contents)
        self.Print("EOF\n%\n")

    if getattr(self.args, 'input_path', None):
      DumpContentsOfFilePassedTo(
          'argv[0] (input_path)', self.args.input_path)
    if getattr(self.args, 'swarming_targets_file', None):
      DumpContentsOfFilePassedTo(
          '--swarming-targets-file', self.args.swarming_targets_file)

  def CmdAnalyze(self):
    vals = self.Lookup()
    return self.RunGNAnalyze(vals)

  def CmdExport(self):
    self.ReadConfigFile()
    obj = {}
    for master, builders in self.masters.items():
      obj[master] = {}
      for builder in builders:
        config = self.masters[master][builder]
        if not config:
          continue

        if isinstance(config, dict):
          args = {k: self.FlattenConfig(v)['gn_args']
                  for k, v in config.items()}
        elif config.startswith('//'):
          args = config
        else:
          args = self.FlattenConfig(config)['gn_args']
          if 'error' in args:
            continue

        obj[master][builder] = args

    # Dump object and trim trailing whitespace.
    s = '\n'.join(l.rstrip() for l in
                  json.dumps(obj, sort_keys=True, indent=2).splitlines())
    self.Print(s)
    return 0

  def CmdGen(self):
    vals = self.Lookup()
    return self.RunGNGen(vals)

  def CmdIsolateEverything(self):
    vals = self.Lookup()
    return self.RunGNGenAllIsolates(vals)

  def CmdHelp(self):
    if self.args.subcommand:
      self.ParseArgs([self.args.subcommand, '--help'])
    else:
      self.ParseArgs(['--help'])

  def CmdIsolate(self):
    vals = self.GetConfig()
    if not vals:
      return 1
    if self.args.build:
      ret = self.Build(self.args.target)
      if ret:
        return ret
    return self.RunGNIsolate(vals)

  def CmdLookup(self):
    vals = self.Lookup()
    cmd = self.GNCmd('gen', '_path_')
    gn_args = self.GNArgs(vals)
    self.Print('\nWriting """\\\n%s""" to _path_/args.gn.\n' % gn_args)
    env = None

    self.PrintCmd(cmd, env)
    return 0

  def CmdRun(self):
    vals = self.GetConfig()
    if not vals:
      return 1
    if self.args.build:
      ret = self.Build(self.args.target)
      if ret:
        return ret
    ret = self.RunGNIsolate(vals)
    if ret:
      return ret

    if self.args.swarmed:
      return self._RunUnderSwarming(self.args.path, self.args.target)
    else:
      return self._RunLocallyIsolated(self.args.path, self.args.target)

  def CmdZip(self):
      ret = self.CmdIsolate()
      if ret:
          return ret

      zip_dir = None
      try:
          zip_dir = self.TempDir()
          remap_cmd = [
            self.executable,
            self.PathJoin(self.chromium_src_dir, 'tools', 'swarming_client',
                          'isolate.py'),
            'remap',
            '--collapse_symlinks',
            '-s', self.PathJoin(self.args.path, self.args.target + '.isolated'),
            '-o', zip_dir
          ]
          self.Run(remap_cmd)

          zip_path = self.args.zip_path
          with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as fp:
              for root, _, files in os.walk(zip_dir):
                  for filename in files:
                      path = self.PathJoin(root, filename)
                      fp.write(path, self.RelPath(path, zip_dir))
      finally:
          if zip_dir:
              self.RemoveDirectory(zip_dir)

  @staticmethod
  def _AddBaseSoftware(cmd):
    # HACK(iannucci): These packages SHOULD NOT BE HERE.
    # Remove method once Swarming Pool Task Templates are implemented.
    # crbug.com/812428

    # Add in required base software. This should be kept in sync with the
    # `chromium_swarming` recipe module in build.git. All references to
    # `swarming_module` below are purely due to this.
    cipd_packages = [
      ('infra/python/cpython/${platform}',
       'version:2.7.14.chromium14'),
      ('infra/tools/luci/logdog/butler/${platform}',
       'git_revision:e1abc57be62d198b5c2f487bfb2fa2d2eb0e867c'),
      ('infra/tools/luci/vpython-native/${platform}',
       'git_revision:0bff6ebf817352838b0e6f65fd6460b38c505c9c'),
      ('infra/tools/luci/vpython/${platform}',
       'git_revision:0bff6ebf817352838b0e6f65fd6460b38c505c9c'),
    ]
    for pkg, vers in cipd_packages:
      cmd.append('--cipd-package=.swarming_module:%s:%s' % (pkg, vers))

    # Add packages to $PATH
    cmd.extend([
      '--env-prefix=PATH', '.swarming_module',
      '--env-prefix=PATH', '.swarming_module/bin',
    ])

    # Add cache directives for vpython.
    vpython_cache_path = '.swarming_module_cache/vpython'
    cmd.extend([
      '--named-cache=swarming_module_cache_vpython', vpython_cache_path,
      '--env-prefix=VPYTHON_VIRTUALENV_ROOT', vpython_cache_path,
    ])

  def _RunUnderSwarming(self, build_dir, target):
    isolate_server = 'isolateserver.appspot.com'
    namespace = 'default-gzip'
    swarming_server = 'chromium-swarm.appspot.com'
    # TODO(dpranke): Look up the information for the target in
    # the //testing/buildbot.json file, if possible, so that we
    # can determine the isolate target, command line, and additional
    # swarming parameters, if possible.
    #
    # TODO(dpranke): Also, add support for sharding and merging results.
    dimensions = []
    for k, v in self._DefaultDimensions() + self.args.dimensions:
      dimensions += ['-d', k, v]

    cmd = [
        self.executable,
        self.PathJoin('tools', 'swarming_client', 'isolate.py'),
        'archive',
        '-s', self.ToSrcRelPath('%s/%s.isolated' % (build_dir, target)),
        '-I', isolate_server,
        '--namespace', namespace,
      ]
    ret, out, _ = self.Run(cmd, force_verbose=False)
    if ret:
      return ret

    isolated_hash = out.splitlines()[0].split()[0]
    cmd = [
        self.executable,
        self.PathJoin('tools', 'swarming_client', 'swarming.py'),
          'run',
          '-s', isolated_hash,
          '-I', isolate_server,
          '--namespace', namespace,
          '-S', swarming_server,
      ] + dimensions
    self._AddBaseSoftware(cmd)
    if self.args.extra_args:
      cmd += ['--'] + self.args.extra_args
    ret, _, _ = self.Run(cmd, force_verbose=True, buffer_output=False)
    return ret

  def _RunLocallyIsolated(self, build_dir, target):
    cmd = [
        self.executable,
        self.PathJoin('tools', 'swarming_client', 'isolate.py'),
        'run',
        '-s',
        self.ToSrcRelPath('%s/%s.isolated' % (build_dir, target)),
      ]
    if self.args.extra_args:
      cmd += ['--'] + self.args.extra_args
    ret, _, _ = self.Run(cmd, force_verbose=True, buffer_output=False)
    return ret

  def _DefaultDimensions(self):
    if not self.args.default_dimensions:
      return []

    # This code is naive and just picks reasonable defaults per platform.
    if self.platform == 'darwin':
      os_dim = ('os', 'Mac-10.13')
    elif self.platform.startswith('linux'):
      os_dim = ('os', 'Ubuntu-14.04')
    elif self.platform == 'win32':
      os_dim = ('os', 'Windows-10')
    else:
      raise MBErr('unrecognized platform string "%s"' % self.platform)

    return [('pool', 'Chrome'),
            ('cpu', 'x86-64'),
            os_dim]

  def CmdValidate(self, print_ok=True):
    errs = []

    # Read the file to make sure it parses.
    self.ReadConfigFile()

    # Build a list of all of the configs referenced by builders.
    all_configs = {}
    for master in self.masters:
      for config in self.masters[master].values():
        if isinstance(config, dict):
          for c in config.values():
            all_configs[c] = master
        else:
          all_configs[config] = master

    # Check that every referenced args file or config actually exists.
    for config, loc in all_configs.items():
      if config.startswith('//'):
        if not self.Exists(self.ToAbsPath(config)):
          errs.append('Unknown args file "%s" referenced from "%s".' %
                      (config, loc))
      elif not config in self.configs:
        errs.append('Unknown config "%s" referenced from "%s".' %
                    (config, loc))

    # Check that every actual config is actually referenced.
    for config in self.configs:
      if not config in all_configs:
        errs.append('Unused config "%s".' % config)

    # Figure out the whole list of mixins, and check that every mixin
    # listed by a config or another mixin actually exists.
    referenced_mixins = set()
    for config, mixins in self.configs.items():
      for mixin in mixins:
        if not mixin in self.mixins:
          errs.append('Unknown mixin "%s" referenced by config "%s".' %
                      (mixin, config))
        referenced_mixins.add(mixin)

    for mixin in self.mixins:
      for sub_mixin in self.mixins[mixin].get('mixins', []):
        if not sub_mixin in self.mixins:
          errs.append('Unknown mixin "%s" referenced by mixin "%s".' %
                      (sub_mixin, mixin))
        referenced_mixins.add(sub_mixin)

    # Check that every mixin defined is actually referenced somewhere.
    for mixin in self.mixins:
      if not mixin in referenced_mixins:
        errs.append('Unreferenced mixin "%s".' % mixin)

    # If we're checking the Chromium config, check that the 'chromium' bots
    # which build public artifacts do not include the chrome_with_codecs mixin.
    if self.args.config_file == self.default_config:
      if 'chromium' in self.masters:
        for builder in self.masters['chromium']:
          config = self.masters['chromium'][builder]
          def RecurseMixins(current_mixin):
            if current_mixin == 'chrome_with_codecs':
              errs.append('Public artifact builder "%s" can not contain the '
                          '"chrome_with_codecs" mixin.' % builder)
              return
            if not 'mixins' in self.mixins[current_mixin]:
              return
            for mixin in self.mixins[current_mixin]['mixins']:
              RecurseMixins(mixin)

          for mixin in self.configs[config]:
            RecurseMixins(mixin)
      else:
        errs.append('Missing "chromium" master. Please update this '
                    'proprietary codecs check with the name of the master '
                    'responsible for public build artifacts.')

    if errs:
      raise MBErr(('mb config file %s has problems:' % self.args.config_file) +
                    '\n  ' + '\n  '.join(errs))

    if print_ok:
      self.Print('mb config file %s looks ok.' % self.args.config_file)
    return 0

  def GetConfig(self):
    build_dir = self.args.path

    vals = self.DefaultVals()
    if self.args.builder or self.args.master or self.args.config:
      vals = self.Lookup()
      # Re-run gn gen in order to ensure the config is consistent with the
      # build dir.
      self.RunGNGen(vals)
      return vals

    toolchain_path = self.PathJoin(self.ToAbsPath(build_dir),
                                   'toolchain.ninja')
    if not self.Exists(toolchain_path):
      self.Print('Must either specify a path to an existing GN build dir '
                 'or pass in a -m/-b pair or a -c flag to specify the '
                 'configuration')
      return {}

    vals['gn_args'] = self.GNArgsFromDir(build_dir)
    return vals

  def GNArgsFromDir(self, build_dir):
    args_contents = ""
    gn_args_path = self.PathJoin(self.ToAbsPath(build_dir), 'args.gn')
    if self.Exists(gn_args_path):
      args_contents = self.ReadFile(gn_args_path)
    gn_args = []
    for l in args_contents.splitlines():
      fields = l.split(' ')
      name = fields[0]
      val = ' '.join(fields[2:])
      gn_args.append('%s=%s' % (name, val))

    return ' '.join(gn_args)

  def Lookup(self):
    vals = self.ReadIOSBotConfig()
    if not vals:
      self.ReadConfigFile()
      config = self.ConfigFromArgs()
      if config.startswith('//'):
        if not self.Exists(self.ToAbsPath(config)):
          raise MBErr('args file "%s" not found' % config)
        vals = self.DefaultVals()
        vals['args_file'] = config
      else:
        if not config in self.configs:
          raise MBErr('Config "%s" not found in %s' %
                      (config, self.args.config_file))
        vals = self.FlattenConfig(config)
    return vals

  def ReadIOSBotConfig(self):
    if not self.args.master or not self.args.builder:
      return {}
    path = self.PathJoin(self.chromium_src_dir, 'ios', 'build', 'bots',
                         self.args.master, self.args.builder + '.json')
    if not self.Exists(path):
      return {}

    contents = json.loads(self.ReadFile(path))
    gn_args = ' '.join(contents.get('gn_args', []))

    vals = self.DefaultVals()
    vals['gn_args'] = gn_args
    return vals

  def ReadConfigFile(self):
    if not self.Exists(self.args.config_file):
      raise MBErr('config file not found at %s' % self.args.config_file)

    try:
      contents = ast.literal_eval(self.ReadFile(self.args.config_file))
    except SyntaxError as e:
      raise MBErr('Failed to parse config file "%s": %s' %
                 (self.args.config_file, e))

    self.configs = contents['configs']
    self.masters = contents['masters']
    self.mixins = contents['mixins']

  def ReadIsolateMap(self):
    if not self.args.isolate_map_files:
      self.args.isolate_map_files = [self.default_isolate_map]

    for f in self.args.isolate_map_files:
      if not self.Exists(f):
        raise MBErr('isolate map file not found at %s' % f)
    isolate_maps = {}
    for isolate_map in self.args.isolate_map_files:
      try:
        isolate_map = ast.literal_eval(self.ReadFile(isolate_map))
        duplicates = set(isolate_map).intersection(isolate_maps)
        if duplicates:
          raise MBErr(
              'Duplicate targets in isolate map files: %s.' %
              ', '.join(duplicates))
        isolate_maps.update(isolate_map)
      except SyntaxError as e:
        raise MBErr(
            'Failed to parse isolate map file "%s": %s' % (isolate_map, e))
    return isolate_maps

  def ConfigFromArgs(self):
    if self.args.config:
      if self.args.master or self.args.builder:
        raise MBErr('Can not specific both -c/--config and -m/--master or '
                    '-b/--builder')

      return self.args.config

    if not self.args.master or not self.args.builder:
      raise MBErr('Must specify either -c/--config or '
                  '(-m/--master and -b/--builder)')

    if not self.args.master in self.masters:
      raise MBErr('Master name "%s" not found in "%s"' %
                  (self.args.master, self.args.config_file))

    if not self.args.builder in self.masters[self.args.master]:
      raise MBErr('Builder name "%s"  not found under masters[%s] in "%s"' %
                  (self.args.builder, self.args.master, self.args.config_file))

    config = self.masters[self.args.master][self.args.builder]
    if isinstance(config, dict):
      if self.args.phase is None:
        raise MBErr('Must specify a build --phase for %s on %s' %
                    (self.args.builder, self.args.master))
      phase = str(self.args.phase)
      if phase not in config:
        raise MBErr('Phase %s doesn\'t exist for %s on %s' %
                    (phase, self.args.builder, self.args.master))
      return config[phase]

    if self.args.phase is not None:
      raise MBErr('Must not specify a build --phase for %s on %s' %
                  (self.args.builder, self.args.master))
    return config

  def FlattenConfig(self, config):
    mixins = self.configs[config]
    vals = self.DefaultVals()

    visited = []
    self.FlattenMixins(mixins, vals, visited)
    return vals

  def DefaultVals(self):
    return {
      'args_file': '',
      'cros_passthrough': False,
      'gn_args': '',
    }

  def FlattenMixins(self, mixins, vals, visited):
    for m in mixins:
      if m not in self.mixins:
        raise MBErr('Unknown mixin "%s"' % m)

      visited.append(m)

      mixin_vals = self.mixins[m]

      if 'cros_passthrough' in mixin_vals:
        vals['cros_passthrough'] = mixin_vals['cros_passthrough']
      if 'args_file' in mixin_vals:
        if vals['args_file']:
            raise MBErr('args_file specified multiple times in mixins '
                        'for %s on %s' % (self.args.builder, self.args.master))
        vals['args_file'] = mixin_vals['args_file']
      if 'gn_args' in mixin_vals:
        if vals['gn_args']:
          vals['gn_args'] += ' ' + mixin_vals['gn_args']
        else:
          vals['gn_args'] = mixin_vals['gn_args']

      if 'mixins' in mixin_vals:
        self.FlattenMixins(mixin_vals['mixins'], vals, visited)
    return vals

  def RunGNGen(self, vals, compute_inputs_for_analyze=False, check=True):
    build_dir = self.args.path

    if check:
      cmd = self.GNCmd('gen', build_dir, '--check')
    else:
      cmd = self.GNCmd('gen', build_dir)
    gn_args = self.GNArgs(vals)
    if compute_inputs_for_analyze:
      gn_args += ' compute_inputs_for_analyze=true'

    # Since GN hasn't run yet, the build directory may not even exist.
    self.MaybeMakeDirectory(self.ToAbsPath(build_dir))

    gn_args_path = self.ToAbsPath(build_dir, 'args.gn')
    self.WriteFile(gn_args_path, gn_args, force_verbose=True)

    if getattr(self.args, 'swarming_targets_file', None):
      # We need GN to generate the list of runtime dependencies for
      # the compile targets listed (one per line) in the file so
      # we can run them via swarming. We use gn_isolate_map.pyl to convert
      # the compile targets to the matching GN labels.
      path = self.args.swarming_targets_file
      if not self.Exists(path):
        self.WriteFailureAndRaise('"%s" does not exist' % path,
                                  output_path=None)
      contents = self.ReadFile(path)
      isolate_targets = set(contents.splitlines())

      isolate_map = self.ReadIsolateMap()
      self.RemovePossiblyStaleRuntimeDepsFiles(vals, isolate_targets,
                                               isolate_map, build_dir)

      err, labels = self.MapTargetsToLabels(isolate_map, isolate_targets)
      if err:
        raise MBErr(err)

      gn_runtime_deps_path = self.ToAbsPath(build_dir, 'runtime_deps')
      self.WriteFile(gn_runtime_deps_path, '\n'.join(labels) + '\n')
      cmd.append('--runtime-deps-list-file=%s' % gn_runtime_deps_path)

    ret, _, _ = self.Run(cmd)
    if ret:
      # If `gn gen` failed, we should exit early rather than trying to
      # generate isolates. Run() will have already logged any error output.
      self.Print('GN gen failed: %d' % ret)
      return ret

    if getattr(self.args, 'swarming_targets_file', None):
      self.GenerateIsolates(vals, isolate_targets, isolate_map, build_dir)

    return 0

  def RunGNGenAllIsolates(self, vals):
    """
    This command generates all .isolate files.

    This command assumes that "mb.py gen" has already been run, as it relies on
    "gn ls" to fetch all gn targets. If uses that output, combined with the
    isolate_map, to determine all isolates that can be generated for the current
    gn configuration.
    """
    build_dir = self.args.path
    ret, output, _ = self.Run(self.GNCmd('ls', build_dir),
                              force_verbose=False)
    if ret:
        # If `gn ls` failed, we should exit early rather than trying to
        # generate isolates.
        self.Print('GN ls failed: %d' % ret)
        return ret

    # Create a reverse map from isolate label to isolate dict.
    isolate_map = self.ReadIsolateMap()
    isolate_dict_map = {}
    for key, isolate_dict in isolate_map.iteritems():
      isolate_dict_map[isolate_dict['label']] = isolate_dict
      isolate_dict_map[isolate_dict['label']]['isolate_key'] = key

    runtime_deps = []

    isolate_targets = []
    # For every GN target, look up the isolate dict.
    for line in output.splitlines():
      target = line.strip()
      if target in isolate_dict_map:
        if isolate_dict_map[target]['type'] == 'additional_compile_target':
          # By definition, additional_compile_targets are not tests, so we
          # shouldn't generate isolates for them.
          continue

        isolate_targets.append(isolate_dict_map[target]['isolate_key'])
        runtime_deps.append(target)

    self.RemovePossiblyStaleRuntimeDepsFiles(vals, isolate_targets,
                                             isolate_map, build_dir)

    gn_runtime_deps_path = self.ToAbsPath(build_dir, 'runtime_deps')
    self.WriteFile(gn_runtime_deps_path, '\n'.join(runtime_deps) + '\n')
    cmd = self.GNCmd('gen', build_dir)
    cmd.append('--runtime-deps-list-file=%s' % gn_runtime_deps_path)
    self.Run(cmd)

    return self.GenerateIsolates(vals, isolate_targets, isolate_map, build_dir)

  def RemovePossiblyStaleRuntimeDepsFiles(self, vals, targets, isolate_map,
                                          build_dir):
    # TODO(crbug.com/932700): Because `gn gen --runtime-deps-list-file`
    # puts the runtime_deps file in different locations based on the actual
    # type of a target, we may end up with multiple possible runtime_deps
    # files in a given build directory, where some of the entries might be
    # stale (since we might be reusing an existing build directory).
    #
    # We need to be able to get the right one reliably; you might think
    # we can just pick the newest file, but because GN won't update timestamps
    # if the contents of the files change, an older runtime_deps
    # file might actually be the one we should use over a newer one (see
    # crbug.com/932387 for a more complete explanation and example).
    #
    # In order to avoid this, we need to delete any possible runtime_deps
    # files *prior* to running GN. As long as the files aren't actually
    # needed during the build, this hopefully will not cause unnecessary
    # build work, and so it should be safe.
    #
    # Ultimately, we should just make sure we get the runtime_deps files
    # in predictable locations so we don't have this issue at all, and
    # that's what crbug.com/932700 is for.
    possible_rpaths = self.PossibleRuntimeDepsPaths(vals, targets, isolate_map)
    for rpaths in possible_rpaths.values():
      for rpath in rpaths:
        path = self.ToAbsPath(build_dir, rpath)
        if self.Exists(path):
          self.RemoveFile(path)

  def GenerateIsolates(self, vals, ninja_targets, isolate_map, build_dir):
    """
    Generates isolates for a list of ninja targets.

    Ninja targets are transformed to GN targets via isolate_map.

    This function assumes that a previous invocation of "mb.py gen" has
    generated runtime deps for all targets.
    """
    possible_rpaths = self.PossibleRuntimeDepsPaths(vals, ninja_targets,
                                                    isolate_map)

    for target, rpaths in possible_rpaths.items():
      # TODO(crbug.com/932700): We don't know where each .runtime_deps
      # file might be, but assuming we called
      # RemovePossiblyStaleRuntimeDepsFiles prior to calling `gn gen`,
      # there should only be one file.
      found_one = False
      path_to_use = None
      for r in rpaths:
        path = self.ToAbsPath(build_dir, r)
        if self.Exists(path):
          if found_one:
            raise MBErr('Found more than one of %s' % ', '.join(rpaths))
          path_to_use = path
          found_one = True

      if not found_one:
        raise MBErr('Did not find any of %s' % ', '.join(rpaths))

      command, extra_files = self.GetIsolateCommand(target, vals)
      runtime_deps = self.ReadFile(path_to_use).splitlines()

      canonical_target = target.replace(':','_').replace('/','_')
      self.WriteIsolateFiles(build_dir, command, canonical_target, runtime_deps,
                             extra_files)

  def PossibleRuntimeDepsPaths(self, vals, ninja_targets, isolate_map):
    """Returns a map of targets to possible .runtime_deps paths.

    Each ninja target maps on to a GN label, but depending on the type
    of the GN target, `gn gen --runtime-deps-list-file` will write
    the .runtime_deps files into different locations. Unfortunately, in
    some cases we don't actually know which of multiple locations will
    actually be used, so we return all plausible candidates.

    The paths that are returned are relative to the build directory.
    """

    android = 'target_os="android"' in vals['gn_args']
    ios = 'target_os="ios"' in vals['gn_args']
    fuchsia = 'target_os="fuchsia"' in vals['gn_args']
    win = self.platform == 'win32' or 'target_os="win"' in vals['gn_args']
    possible_runtime_deps_rpaths = {}
    for target in ninja_targets:
      target_type = isolate_map[target]['type']
      label = isolate_map[target]['label']
      stamp_runtime_deps = 'obj/%s.stamp.runtime_deps' % label.replace(':', '/')
      # TODO(https://crbug.com/876065): 'official_tests' use
      # type='additional_compile_target' to isolate tests. This is not the
      # intended use for 'additional_compile_target'.
      if (target_type == 'additional_compile_target' and
          target != 'official_tests'):
        # By definition, additional_compile_targets are not tests, so we
        # shouldn't generate isolates for them.
        raise MBErr('Cannot generate isolate for %s since it is an '
                    'additional_compile_target.' % target)
      elif fuchsia or ios or target_type == 'generated_script':
        # iOS and Fuchsia targets end up as groups.
        # generated_script targets are always actions.
        rpaths = [stamp_runtime_deps]
      elif android:
        # Android targets may be either android_apk or executable. The former
        # will result in runtime_deps associated with the stamp file, while the
        # latter will result in runtime_deps associated with the executable.
        label = isolate_map[target]['label']
        rpaths = [
            target + '.runtime_deps',
            stamp_runtime_deps]
      elif (target_type == 'script' or
            target_type == 'fuzzer' or
            isolate_map[target].get('label_type') == 'group'):
        # For script targets, the build target is usually a group,
        # for which gn generates the runtime_deps next to the stamp file
        # for the label, which lives under the obj/ directory, but it may
        # also be an executable.
        label = isolate_map[target]['label']
        rpaths = [stamp_runtime_deps]
        if win:
          rpaths += [ target + '.exe.runtime_deps' ]
        else:
          rpaths += [ target + '.runtime_deps' ]
      elif win:
        rpaths = [target + '.exe.runtime_deps']
      else:
        rpaths = [target + '.runtime_deps']

      possible_runtime_deps_rpaths[target] = rpaths

    return possible_runtime_deps_rpaths

  def RunGNIsolate(self, vals):
    target = self.args.target
    isolate_map = self.ReadIsolateMap()
    err, labels = self.MapTargetsToLabels(isolate_map, [target])
    if err:
      raise MBErr(err)

    label = labels[0]

    build_dir = self.args.path
    command, extra_files = self.GetIsolateCommand(target, vals)

    cmd = self.GNCmd('desc', build_dir, label, 'runtime_deps')
    ret, out, _ = self.Call(cmd)
    if ret:
      if out:
        self.Print(out)
      return ret

    runtime_deps = out.splitlines()

    self.WriteIsolateFiles(build_dir, command, target, runtime_deps,
                           extra_files)

    ret, _, _ = self.Run([
        self.executable,
        self.PathJoin('tools', 'swarming_client', 'isolate.py'),
        'check',
        '-i',
        self.ToSrcRelPath('%s/%s.isolate' % (build_dir, target)),
        '-s',
        self.ToSrcRelPath('%s/%s.isolated' % (build_dir, target))],
        buffer_output=False)

    return ret

  def WriteIsolateFiles(self, build_dir, command, target, runtime_deps,
                        extra_files):
    isolate_path = self.ToAbsPath(build_dir, target + '.isolate')
    self.WriteFile(isolate_path,
      pprint.pformat({
        'variables': {
          'command': command,
          'files': sorted(runtime_deps + extra_files),
        }
      }) + '\n')

    self.WriteJSON(
      {
        'args': [
          '--isolated',
          self.ToSrcRelPath('%s/%s.isolated' % (build_dir, target)),
          '--isolate',
          self.ToSrcRelPath('%s/%s.isolate' % (build_dir, target)),
        ],
        'dir': self.chromium_src_dir,
        'version': 1,
      },
      isolate_path + 'd.gen.json',
    )

  def MapTargetsToLabels(self, isolate_map, targets):
    labels = []
    err = ''

    for target in targets:
      if target == 'all':
        labels.append(target)
      elif target.startswith('//'):
        labels.append(target)
      else:
        if target in isolate_map:
          if isolate_map[target]['type'] == 'unknown':
            err += ('test target "%s" type is unknown\n' % target)
          else:
            labels.append(isolate_map[target]['label'])
        else:
          err += ('target "%s" not found in '
                  '//testing/buildbot/gn_isolate_map.pyl\n' % target)

    return err, labels

  def GNCmd(self, subcommand, path, *args):
    if self.platform == 'linux2':
      subdir, exe = 'linux64', 'gn'
    elif self.platform == 'darwin':
      subdir, exe = 'mac', 'gn'
    elif self.platform == 'aix6':
      subdir, exe = 'aix', 'gn'
    else:
      subdir, exe = 'win', 'gn.exe'

    gn_path = self.PathJoin(self.chromium_src_dir, 'buildtools', subdir, exe)
    return [gn_path, subcommand, path] + list(args)


  def GNArgs(self, vals):
    if vals['cros_passthrough']:
      if not 'GN_ARGS' in os.environ:
        raise MBErr('MB is expecting GN_ARGS to be in the environment')
      gn_args = os.environ['GN_ARGS']
      if not re.search('target_os.*=.*"chromeos"', gn_args):
        raise MBErr('GN_ARGS is missing target_os = "chromeos": (GN_ARGS=%s)' %
                    gn_args)
      if vals['gn_args']:
        gn_args += ' ' + vals['gn_args']
    else:
      gn_args = vals['gn_args']

    if self.args.goma_dir:
      gn_args += ' goma_dir="%s"' % self.args.goma_dir

    android_version_code = self.args.android_version_code
    if android_version_code:
      gn_args += ' android_default_version_code="%s"' % android_version_code

    android_version_name = self.args.android_version_name
    if android_version_name:
      gn_args += ' android_default_version_name="%s"' % android_version_name

    # Canonicalize the arg string into a sorted, newline-separated list
    # of key-value pairs, and de-dup the keys if need be so that only
    # the last instance of each arg is listed.
    gn_args = gn_helpers.ToGNString(gn_helpers.FromGNArgs(gn_args))

    # If we're using the Simple Chrome SDK, add a comment at the top that
    # points to the doc. This must happen after the gn_helpers.ToGNString()
    # call above since gn_helpers strips comments.
    if vals['cros_passthrough']:
      simplechrome_comment = [
          '# These args are generated via the Simple Chrome SDK. See the link',
          '# below for more details:',
          '# https://chromium.googlesource.com/chromiumos/docs/+/master/simple_chrome_workflow.md',  # pylint: disable=line-too-long
      ]
      gn_args = '%s\n%s' % ('\n'.join(simplechrome_comment), gn_args)

    args_file = vals.get('args_file', None)
    if args_file:
      gn_args = ('import("%s")\n' % vals['args_file']) + gn_args
    return gn_args

  def GetIsolateCommand(self, target, vals):
    isolate_map = self.ReadIsolateMap()

    is_android = 'target_os="android"' in vals['gn_args']
    is_simplechrome = vals.get('cros_passthrough', False)
    is_fuchsia = 'target_os="fuchsia"' in vals['gn_args']
    is_win = self.platform == 'win32' or 'target_os="win"' in vals['gn_args']

    # This should be true if tests with type='windowed_test_launcher' are
    # expected to run using xvfb. For example, Linux Desktop, X11 CrOS and
    # Ozone CrOS builds. Note that one Ozone build can be used to run differen
    # backends. Currently, tests are executed for the headless and X11 backends
    # and both can run under Xvfb.
    # TODO(tonikitoo,msisov,fwang): Find a way to run tests for the Wayland
    # backend.
    use_xvfb = self.platform == 'linux2' and not is_android and not is_fuchsia

    asan = 'is_asan=true' in vals['gn_args']
    msan = 'is_msan=true' in vals['gn_args']
    tsan = 'is_tsan=true' in vals['gn_args']
    cfi_diag = 'use_cfi_diag=true' in vals['gn_args']

    test_type = isolate_map[target]['type']

    executable = isolate_map[target].get('executable', target)
    executable_suffix = isolate_map[target].get(
        'executable_suffix', '.exe' if is_win else '')

    cmdline = []
    extra_files = [
      '../../.vpython',
      '../../testing/test_env.py',
    ]

    if test_type == 'nontest':
      self.WriteFailureAndRaise('We should not be isolating %s.' % target,
                                output_path=None)

    if test_type == 'generated_script':
      cmdline = [
          '../../testing/test_env.py',
          isolate_map[target]['script'],
      ]
    elif test_type == 'fuzzer':
      cmdline = [
        '../../testing/test_env.py',
        '../../tools/code_coverage/run_fuzz_target.py',
        '--fuzzer', './' + target,
        '--output-dir', '${ISOLATED_OUTDIR}',
        '--timeout', '3600']
    elif is_android and test_type != "script":
      cmdline = []
      if asan:
        cmdline += [os.path.join('bin', 'run_with_asan'), '--']
      cmdline += [
          '../../testing/test_env.py',
          '../../build/android/test_wrapper/logdog_wrapper.py',
          '--target', target,
          '--logdog-bin-cmd', '../../bin/logdog_butler',
          '--store-tombstones']
    elif is_fuchsia and test_type != 'script':
      cmdline = [
          '../../testing/test_env.py',
          os.path.join('bin', 'run_%s' % target),
          '--test-launcher-bot-mode',
      ]
    elif is_simplechrome and test_type != 'script':
      cmdline = [
          '../../testing/test_env.py',
          os.path.join('bin', 'run_%s' % target),
      ]
    elif use_xvfb and test_type == 'windowed_test_launcher':
      extra_files.append('../../testing/xvfb.py')
      cmdline = [
        '../../testing/xvfb.py',
        './' + str(executable) + executable_suffix,
        '--test-launcher-bot-mode',
        '--asan=%d' % asan,
        '--msan=%d' % msan,
        '--tsan=%d' % tsan,
        '--cfi-diag=%d' % cfi_diag,
      ]
    elif test_type in ('windowed_test_launcher', 'console_test_launcher'):
      cmdline = [
          '../../testing/test_env.py',
          './' + str(executable) + executable_suffix,
          '--test-launcher-bot-mode',
          '--asan=%d' % asan,
          '--msan=%d' % msan,
          '--tsan=%d' % tsan,
          '--cfi-diag=%d' % cfi_diag,
      ]
    elif test_type == 'script':
      cmdline = []
      # If we're testing a CrOS simplechrome build, assume we need to launch a
      # VM first. So prepend the command to run with the VM launcher.
      # TODO(bpastene): Differentiate between CrOS VM and hardware tests.
      if is_simplechrome:
        cmdline = [os.path.join('bin', 'launch_cros_vm')]
      cmdline += [
          '../../testing/test_env.py',
          '../../' + self.ToSrcRelPath(isolate_map[target]['script'])
      ]
    elif test_type in ('raw', 'additional_compile_target'):
      cmdline = [
          './' + str(target) + executable_suffix,
      ]
    else:
      self.WriteFailureAndRaise('No command line for %s found (test type %s).'
                                % (target, test_type), output_path=None)

    if is_win and asan:
      # Sandbox is not yet supported by ASAN for Windows.
      # Perhaps this is only needed for tests that use the sandbox?
      cmdline.append('--no-sandbox')

    cmdline += isolate_map[target].get('args', [])

    return cmdline, extra_files

  def ToAbsPath(self, build_path, *comps):
    return self.PathJoin(self.chromium_src_dir,
                         self.ToSrcRelPath(build_path),
                         *comps)

  def ToSrcRelPath(self, path):
    """Returns a relative path from the top of the repo."""
    if path.startswith('//'):
      return path[2:].replace('/', self.sep)
    return self.RelPath(path, self.chromium_src_dir)

  def RunGNAnalyze(self, vals):
    # Analyze runs before 'gn gen' now, so we need to run gn gen
    # in order to ensure that we have a build directory.
    ret = self.RunGNGen(vals, compute_inputs_for_analyze=True, check=False)
    if ret:
      return ret

    build_path = self.args.path
    input_path = self.args.input_path
    gn_input_path = input_path + '.gn'
    output_path = self.args.output_path
    gn_output_path = output_path + '.gn'

    inp = self.ReadInputJSON(['files', 'test_targets',
                              'additional_compile_targets'])
    if self.args.verbose:
      self.Print()
      self.Print('analyze input:')
      self.PrintJSON(inp)
      self.Print()


    # This shouldn't normally happen, but could due to unusual race conditions,
    # like a try job that gets scheduled before a patch lands but runs after
    # the patch has landed.
    if not inp['files']:
      self.Print('Warning: No files modified in patch, bailing out early.')
      self.WriteJSON({
            'status': 'No dependency',
            'compile_targets': [],
            'test_targets': [],
          }, output_path)
      return 0

    gn_inp = {}
    gn_inp['files'] = ['//' + f for f in inp['files'] if not f.startswith('//')]

    isolate_map = self.ReadIsolateMap()
    err, gn_inp['additional_compile_targets'] = self.MapTargetsToLabels(
        isolate_map, inp['additional_compile_targets'])
    if err:
      raise MBErr(err)

    err, gn_inp['test_targets'] = self.MapTargetsToLabels(
        isolate_map, inp['test_targets'])
    if err:
      raise MBErr(err)
    labels_to_targets = {}
    for i, label in enumerate(gn_inp['test_targets']):
      labels_to_targets[label] = inp['test_targets'][i]

    try:
      self.WriteJSON(gn_inp, gn_input_path)
      cmd = self.GNCmd('analyze', build_path, gn_input_path, gn_output_path)
      ret, _, _ = self.Run(cmd, force_verbose=True)
      if ret:
        return ret

      gn_outp_str = self.ReadFile(gn_output_path)
      try:
        gn_outp = json.loads(gn_outp_str)
      except Exception as e:
        self.Print("Failed to parse the JSON string GN returned: %s\n%s"
                   % (repr(gn_outp_str), str(e)))
        raise

      outp = {}
      if 'status' in gn_outp:
        outp['status'] = gn_outp['status']
      if 'error' in gn_outp:
        outp['error'] = gn_outp['error']
      if 'invalid_targets' in gn_outp:
        outp['invalid_targets'] = gn_outp['invalid_targets']
      if 'compile_targets' in gn_outp:
        all_input_compile_targets = sorted(
            set(inp['test_targets'] + inp['additional_compile_targets']))

        # If we're building 'all', we can throw away the rest of the targets
        # since they're redundant.
        if 'all' in gn_outp['compile_targets']:
          outp['compile_targets'] = ['all']
        else:
          outp['compile_targets'] = gn_outp['compile_targets']

        # crbug.com/736215: When GN returns targets back, for targets in
        # the default toolchain, GN will have generated a phony ninja
        # target matching the label, and so we can safely (and easily)
        # transform any GN label into the matching ninja target. For
        # targets in other toolchains, though, GN doesn't generate the
        # phony targets, and we don't know how to turn the labels into
        # compile targets. In this case, we also conservatively give up
        # and build everything. Probably the right thing to do here is
        # to have GN return the compile targets directly.
        if any("(" in target for target in outp['compile_targets']):
          self.Print('WARNING: targets with non-default toolchains were '
                     'found, building everything instead.')
          outp['compile_targets'] = all_input_compile_targets
        else:
          outp['compile_targets'] = [
              label.replace('//', '') for label in outp['compile_targets']]

        # Windows has a maximum command line length of 8k; even Linux
        # maxes out at 128k; if analyze returns a *really long* list of
        # targets, we just give up and conservatively build everything instead.
        # Probably the right thing here is for ninja to support response
        # files as input on the command line
        # (see https://github.com/ninja-build/ninja/issues/1355).
        if len(' '.join(outp['compile_targets'])) > 7*1024:
          self.Print('WARNING: Too many compile targets were affected.')
          self.Print('WARNING: Building everything instead to avoid '
                     'command-line length issues.')
          outp['compile_targets'] = all_input_compile_targets


      if 'test_targets' in gn_outp:
        outp['test_targets'] = [
          labels_to_targets[label] for label in gn_outp['test_targets']]

      if self.args.verbose:
        self.Print()
        self.Print('analyze output:')
        self.PrintJSON(outp)
        self.Print()

      self.WriteJSON(outp, output_path)

    finally:
      if self.Exists(gn_input_path):
        self.RemoveFile(gn_input_path)
      if self.Exists(gn_output_path):
        self.RemoveFile(gn_output_path)

    return 0

  def ReadInputJSON(self, required_keys):
    path = self.args.input_path
    output_path = self.args.output_path
    if not self.Exists(path):
      self.WriteFailureAndRaise('"%s" does not exist' % path, output_path)

    try:
      inp = json.loads(self.ReadFile(path))
    except Exception as e:
      self.WriteFailureAndRaise('Failed to read JSON input from "%s": %s' %
                                (path, e), output_path)

    for k in required_keys:
      if not k in inp:
        self.WriteFailureAndRaise('input file is missing a "%s" key' % k,
                                  output_path)

    return inp

  def WriteFailureAndRaise(self, msg, output_path):
    if output_path:
      self.WriteJSON({'error': msg}, output_path, force_verbose=True)
    raise MBErr(msg)

  def WriteJSON(self, obj, path, force_verbose=False):
    try:
      self.WriteFile(path, json.dumps(obj, indent=2, sort_keys=True) + '\n',
                     force_verbose=force_verbose)
    except Exception as e:
      raise MBErr('Error %s writing to the output path "%s"' %
                 (e, path))

  def CheckCompile(self, master, builder):
    url_template = self.args.url_template + '/{builder}/builds/_all?as_text=1'
    url = urllib2.quote(url_template.format(master=master, builder=builder),
                        safe=':/()?=')
    try:
      builds = json.loads(self.Fetch(url))
    except Exception as e:
      return str(e)
    successes = sorted(
        [int(x) for x in builds.keys() if "text" in builds[x] and
          cmp(builds[x]["text"][:2], ["build", "successful"]) == 0],
        reverse=True)
    if not successes:
      return "no successful builds"
    build = builds[str(successes[0])]
    step_names = set([step["name"] for step in build["steps"]])
    compile_indicators = set(["compile", "compile (with patch)", "analyze"])
    if compile_indicators & step_names:
      return "compiles"
    return "does not compile"

  def PrintCmd(self, cmd, env):
    if self.platform == 'win32':
      env_prefix = 'set '
      env_quoter = QuoteForSet
      shell_quoter = QuoteForCmd
    else:
      env_prefix = ''
      env_quoter = pipes.quote
      shell_quoter = pipes.quote

    def print_env(var):
      if env and var in env:
        self.Print('%s%s=%s' % (env_prefix, var, env_quoter(env[var])))

    print_env('LLVM_FORCE_HEAD_REVISION')

    if cmd[0] == self.executable:
      cmd = ['python'] + cmd[1:]
    self.Print(*[shell_quoter(arg) for arg in cmd])

  def PrintJSON(self, obj):
    self.Print(json.dumps(obj, indent=2, sort_keys=True))

  def Build(self, target):
    build_dir = self.ToSrcRelPath(self.args.path)
    if self.platform == 'win32':
      # On Windows use the batch script since there is no exe
      ninja_cmd = ['autoninja.bat', '-C', build_dir]
    else:
      ninja_cmd = ['autoninja', '-C', build_dir]
    if self.args.jobs:
      ninja_cmd.extend(['-j', '%d' % self.args.jobs])
    ninja_cmd.append(target)
    ret, _, _ = self.Run(ninja_cmd, force_verbose=False, buffer_output=False)
    return ret

  def Run(self, cmd, env=None, force_verbose=True, buffer_output=True):
    # This function largely exists so it can be overridden for testing.
    if self.args.dryrun or self.args.verbose or force_verbose:
      self.PrintCmd(cmd, env)
    if self.args.dryrun:
      return 0, '', ''

    ret, out, err = self.Call(cmd, env=env, buffer_output=buffer_output)
    if self.args.verbose or force_verbose:
      if ret:
        self.Print('  -> returned %d' % ret)
      if out:
        self.Print(out, end='')
      if err:
        self.Print(err, end='', file=sys.stderr)
    return ret, out, err

  def Call(self, cmd, env=None, buffer_output=True):
    if buffer_output:
      p = subprocess.Popen(cmd, shell=False, cwd=self.chromium_src_dir,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env=env)
      out, err = p.communicate()
    else:
      p = subprocess.Popen(cmd, shell=False, cwd=self.chromium_src_dir,
                           env=env)
      p.wait()
      out = err = ''
    return p.returncode, out, err

  def ExpandUser(self, path):
    # This function largely exists so it can be overridden for testing.
    return os.path.expanduser(path)

  def Exists(self, path):
    # This function largely exists so it can be overridden for testing.
    return os.path.exists(path)

  def Fetch(self, url):
    # This function largely exists so it can be overridden for testing.
    f = urllib2.urlopen(url)
    contents = f.read()
    f.close()
    return contents

  def MaybeMakeDirectory(self, path):
    try:
      os.makedirs(path)
    except OSError, e:
      if e.errno != errno.EEXIST:
        raise

  def PathJoin(self, *comps):
    # This function largely exists so it can be overriden for testing.
    return os.path.join(*comps)

  def Print(self, *args, **kwargs):
    # This function largely exists so it can be overridden for testing.
    print(*args, **kwargs)
    if kwargs.get('stream', sys.stdout) == sys.stdout:
      sys.stdout.flush()

  def ReadFile(self, path):
    # This function largely exists so it can be overriden for testing.
    with open(path) as fp:
      return fp.read()

  def RelPath(self, path, start='.'):
    # This function largely exists so it can be overriden for testing.
    return os.path.relpath(path, start)

  def RemoveFile(self, path):
    # This function largely exists so it can be overriden for testing.
    os.remove(path)

  def RemoveDirectory(self, abs_path):
    if self.platform == 'win32':
      # In other places in chromium, we often have to retry this command
      # because we're worried about other processes still holding on to
      # file handles, but when MB is invoked, it will be early enough in the
      # build that their should be no other processes to interfere. We
      # can change this if need be.
      self.Run(['cmd.exe', '/c', 'rmdir', '/q', '/s', abs_path])
    else:
      shutil.rmtree(abs_path, ignore_errors=True)

  def TempDir(self):
    # This function largely exists so it can be overriden for testing.
    return tempfile.mkdtemp(prefix='mb_')

  def TempFile(self, mode='w'):
    # This function largely exists so it can be overriden for testing.
    return tempfile.NamedTemporaryFile(mode=mode, delete=False)

  def WriteFile(self, path, contents, force_verbose=False):
    # This function largely exists so it can be overriden for testing.
    if self.args.dryrun or self.args.verbose or force_verbose:
      self.Print('\nWriting """\\\n%s""" to %s.\n' % (contents, path))
    with open(path, 'w') as fp:
      return fp.write(contents)


class MBErr(Exception):
  pass


# See http://goo.gl/l5NPDW and http://goo.gl/4Diozm for the painful
# details of this next section, which handles escaping command lines
# so that they can be copied and pasted into a cmd window.
UNSAFE_FOR_SET = set('^<>&|')
UNSAFE_FOR_CMD = UNSAFE_FOR_SET.union(set('()%'))
ALL_META_CHARS = UNSAFE_FOR_CMD.union(set('"'))


def QuoteForSet(arg):
  if any(a in UNSAFE_FOR_SET for a in arg):
    arg = ''.join('^' + a if a in UNSAFE_FOR_SET else a for a in arg)
  return arg


def QuoteForCmd(arg):
  # First, escape the arg so that CommandLineToArgvW will parse it properly.
  if arg == '' or ' ' in arg or '"' in arg:
    quote_re = re.compile(r'(\\*)"')
    arg = '"%s"' % (quote_re.sub(lambda mo: 2 * mo.group(1) + '\\"', arg))

  # Then check to see if the arg contains any metacharacters other than
  # double quotes; if it does, quote everything (including the double
  # quotes) for safety.
  if any(a in UNSAFE_FOR_CMD for a in arg):
    arg = ''.join('^' + a if a in ALL_META_CHARS else a for a in arg)
  return arg


if __name__ == '__main__':
  sys.exit(main(sys.argv[1:]))
