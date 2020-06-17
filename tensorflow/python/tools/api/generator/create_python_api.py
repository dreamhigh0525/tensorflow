# Copyright 2017 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# =============================================================================
"""Generates and prints out imports and constants for new TensorFlow python api."""
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import argparse
import collections
import importlib
import os
import sys

from tensorflow.python.tools.api.generator import doc_srcs
from tensorflow.python.util import tf_decorator
from tensorflow.python.util import tf_export

API_ATTRS = tf_export.API_ATTRS
API_ATTRS_V1 = tf_export.API_ATTRS_V1

_LAZY_LOADING = False
_API_VERSIONS = [1, 2]
_COMPAT_MODULE_TEMPLATE = 'compat.v%d'
_SUBCOMPAT_MODULE_TEMPLATE = 'compat.v%d.compat.v%d'
_COMPAT_MODULE_PREFIX = 'compat.v'
_DEFAULT_PACKAGE = 'tensorflow.python'
_GENFILES_DIR_SUFFIX = 'genfiles/'
_SYMBOLS_TO_SKIP_EXPLICITLY = {
    # Overrides __getattr__, so that unwrapping tf_decorator
    # would have side effects.
    'tensorflow.python.platform.flags.FLAGS'
}
_GENERATED_FILE_HEADER = """# This file is MACHINE GENERATED! Do not edit.
# Generated by: tensorflow/python/tools/api/generator/create_python_api.py script.
\"\"\"%s
\"\"\"

from __future__ import print_function as _print_function

import sys as _sys

"""
_GENERATED_FILE_FOOTER = '\n\ndel _print_function\n'
_DEPRECATION_FOOTER = """
from tensorflow.python.util import module_wrapper as _module_wrapper

if not isinstance(_sys.modules[__name__], _module_wrapper.TFModuleWrapper):
  _sys.modules[__name__] = _module_wrapper.TFModuleWrapper(
      _sys.modules[__name__], "%s", public_apis=%s, deprecation=%s,
      has_lite=%s)
"""
_LAZY_LOADING_MODULE_TEXT_TEMPLATE = """
# Inform pytype that this module is dynamically populated (b/111239204).
_HAS_DYNAMIC_ATTRIBUTES = True
_PUBLIC_APIS = {
%s
}
"""


class SymbolExposedTwiceError(Exception):
  """Raised when different symbols are exported with the same name."""
  pass


def get_canonical_import(import_set):
  """Obtain one single import from a set of possible sources of a symbol.

  One symbol might come from multiple places as it is being imported and
  reexported. To simplify API changes, we always use the same import for the
  same module, and give preference based on higher priority and alphabetical
  ordering.

  Args:
    import_set: (set) Imports providing the same symbol. This is a set of tuples
      in the form (import, priority). We want to pick an import with highest
      priority.

  Returns:
    A module name to import
  """
  # We use the fact that list sorting is stable, so first we convert the set to
  # a sorted list of the names and then we resort this list to move elements
  # not in core tensorflow to the end.
  # Here we sort by priority (higher preferred) and then  alphabetically by
  # import string.
  import_list = sorted(
      import_set,
      key=lambda imp_and_priority: (-imp_and_priority[1], imp_and_priority[0]))
  return import_list[0][0]


class _ModuleInitCodeBuilder(object):
  """Builds a map from module name to imports included in that module."""

  def __init__(self,
               output_package,
               api_version,
               lazy_loading=_LAZY_LOADING,
               use_relative_imports=False):
    self._output_package = output_package
    # Maps API module to API symbol name to set of tuples of the form
    # (module name, priority).
    # The same symbol can be imported from multiple locations. Higher
    # "priority" indicates that import location is preferred over others.
    self._module_imports = collections.defaultdict(
        lambda: collections.defaultdict(set))
    self._dest_import_to_id = collections.defaultdict(int)
    # Names that start with underscore in the root module.
    self._underscore_names_in_root = []
    self._api_version = api_version
    # Controls whether or not exported symbols are lazily loaded or statically
    # imported.
    self._lazy_loading = lazy_loading
    self._use_relative_imports = use_relative_imports

  def _check_already_imported(self, symbol_id, api_name):
    if (api_name in self._dest_import_to_id and
        symbol_id != self._dest_import_to_id[api_name] and symbol_id != -1):
      raise SymbolExposedTwiceError(
          'Trying to export multiple symbols with same name: %s.' % api_name)
    self._dest_import_to_id[api_name] = symbol_id

  def add_import(self, symbol, source_module_name, source_name,
                 dest_module_name, dest_name):
    """Adds this import to module_imports.

    Args:
      symbol: TensorFlow Python symbol.
      source_module_name: (string) Module to import from.
      source_name: (string) Name of the symbol to import.
      dest_module_name: (string) Module name to add import to.
      dest_name: (string) Import the symbol using this name.

    Raises:
      SymbolExposedTwiceError: Raised when an import with the same
        dest_name has already been added to dest_module_name.
    """
    # modules_with_exports.py is only used during API generation and
    # won't be available when actually importing tensorflow.
    if source_module_name.endswith('python.modules_with_exports'):
      source_module_name = symbol.__module__
    import_str = self.format_import(source_module_name, source_name, dest_name)

    # Check if we are trying to expose two different symbols with same name.
    full_api_name = dest_name
    if dest_module_name:
      full_api_name = dest_module_name + '.' + full_api_name
    symbol_id = -1 if not symbol else id(symbol)
    self._check_already_imported(symbol_id, full_api_name)

    if not dest_module_name and dest_name.startswith('_'):
      self._underscore_names_in_root.append(dest_name)

    # The same symbol can be available in multiple modules.
    # We store all possible ways of importing this symbol and later pick just
    # one.
    priority = 0
    if symbol:
      # Give higher priority to source module if it matches
      # symbol's original module.
      if hasattr(symbol, '__module__'):
        priority = int(source_module_name == symbol.__module__)
      # Give higher priority if symbol name matches its __name__.
      if hasattr(symbol, '__name__'):
        priority += int(source_name == symbol.__name__)
    self._module_imports[dest_module_name][full_api_name].add(
        (import_str, priority))

  def _import_submodules(self):
    """Add imports for all destination modules in self._module_imports."""
    # Import all required modules in their parent modules.
    # For e.g. if we import 'foo.bar.Value'. Then, we also
    # import 'bar' in 'foo'.
    imported_modules = set(self._module_imports.keys())
    for module in imported_modules:
      if not module:
        continue
      module_split = module.split('.')
      parent_module = ''  # we import submodules in their parent_module

      for submodule_index in range(len(module_split)):
        if submodule_index > 0:
          submodule = module_split[submodule_index - 1]
          parent_module += '.' + submodule if parent_module else submodule
        import_from = self._output_package
        if self._lazy_loading:
          import_from += '.' + '.'.join(module_split[:submodule_index + 1])
          self.add_import(
              symbol=None,
              source_module_name='',
              source_name=import_from,
              dest_module_name=parent_module,
              dest_name=module_split[submodule_index])
        else:
          if self._use_relative_imports:
            import_from = '.'
          elif submodule_index > 0:
            import_from += '.' + '.'.join(module_split[:submodule_index])
          self.add_import(
              symbol=None,
              source_module_name=import_from,
              source_name=module_split[submodule_index],
              dest_module_name=parent_module,
              dest_name=module_split[submodule_index])

  def build(self):
    """Get a map from destination module to __init__.py code for that module.

    Returns:
      A dictionary where
        key: (string) destination module (for e.g. tf or tf.consts).
        value: (string) text that should be in __init__.py files for
          corresponding modules.
    """
    self._import_submodules()
    module_text_map = {}
    footer_text_map = {}
    for dest_module, dest_name_to_imports in self._module_imports.items():
      # Sort all possible imports for a symbol and pick the first one.
      imports_list = [
          get_canonical_import(imports)
          for _, imports in dest_name_to_imports.items()
      ]
      if self._lazy_loading:
        module_text_map[
            dest_module] = _LAZY_LOADING_MODULE_TEXT_TEMPLATE % '\n'.join(
                sorted(imports_list))
      else:
        module_text_map[dest_module] = '\n'.join(sorted(imports_list))

    # Expose exported symbols with underscores in root module since we import
    # from it using * import. Don't need this for lazy_loading because the
    # underscore symbols are already included in __all__ when passed in and
    # handled by TFModuleWrapper.
    root_module_footer = ''
    if not self._lazy_loading:
      underscore_names_str = ', '.join(
          '\'%s\'' % name for name in self._underscore_names_in_root)

      root_module_footer = """
_names_with_underscore = [%s]
__all__ = [_s for _s in dir() if not _s.startswith('_')]
__all__.extend([_s for _s in _names_with_underscore])
""" % underscore_names_str

    # Add module wrapper if we need to print deprecation messages
    # or if we use lazy loading.
    if self._api_version == 1 or self._lazy_loading:
      for dest_module, _ in self._module_imports.items():
        deprecation = 'False'
        has_lite = 'False'
        if self._api_version == 1:  # Add 1.* deprecations.
          if not dest_module.startswith(_COMPAT_MODULE_PREFIX):
            deprecation = 'True'
        # Workaround to make sure not load lite from lite/__init__.py
        if (not dest_module and 'lite' in self._module_imports and
            self._lazy_loading):
          has_lite = 'True'
        if self._lazy_loading:
          public_apis_name = '_PUBLIC_APIS'
        else:
          public_apis_name = 'None'
        footer_text_map[dest_module] = _DEPRECATION_FOOTER % (
            dest_module, public_apis_name, deprecation, has_lite)

    return module_text_map, footer_text_map, root_module_footer

  def format_import(self, source_module_name, source_name, dest_name):
    """Formats import statement.

    Args:
      source_module_name: (string) Source module to import from.
      source_name: (string) Source symbol name to import.
      dest_name: (string) Destination alias name.

    Returns:
      An import statement string.
    """
    if self._lazy_loading:
      return "  '%s': ('%s', '%s')," % (dest_name, source_module_name,
                                        source_name)
    else:
      if source_module_name:
        if source_name == dest_name:
          return 'from %s import %s' % (source_module_name, source_name)
        else:
          return 'from %s import %s as %s' % (source_module_name, source_name,
                                              dest_name)
      else:
        if source_name == dest_name:
          return 'import %s' % source_name
        else:
          return 'import %s as %s' % (source_name, dest_name)

  def get_destination_modules(self):
    return set(self._module_imports.keys())

  def copy_imports(self, from_dest_module, to_dest_module):
    self._module_imports[to_dest_module] = (
        self._module_imports[from_dest_module].copy())


def add_nested_compat_imports(module_builder, compat_api_versions,
                              output_package):
  """Adds compat.vN.compat.vK modules to module builder.

  To avoid circular imports, we want to add __init__.py files under
  compat.vN.compat.vK and under compat.vN.compat.vK.compat. For all other
  imports, we point to corresponding modules under compat.vK.

  Args:
    module_builder: `_ModuleInitCodeBuilder` instance.
    compat_api_versions: Supported compatibility versions.
    output_package: Base output python package where generated API will be
      added.
  """
  imported_modules = module_builder.get_destination_modules()

  # Copy over all imports in compat.vK to compat.vN.compat.vK and
  # all imports in compat.vK.compat to compat.vN.compat.vK.compat.
  for v in compat_api_versions:
    for sv in compat_api_versions:
      subcompat_module = _SUBCOMPAT_MODULE_TEMPLATE % (v, sv)
      compat_module = _COMPAT_MODULE_TEMPLATE % sv
      module_builder.copy_imports(compat_module, subcompat_module)
      module_builder.copy_imports('%s.compat' % compat_module,
                                  '%s.compat' % subcompat_module)

  # Prefixes of modules under compatibility packages, for e.g. "compat.v1.".
  compat_prefixes = tuple(
      _COMPAT_MODULE_TEMPLATE % v + '.' for v in compat_api_versions)

  # Above, we only copied function, class and constant imports. Here
  # we also add imports for child modules.
  for imported_module in imported_modules:
    if not imported_module.startswith(compat_prefixes):
      continue
    module_split = imported_module.split('.')

    # Handle compat.vN.compat.vK.compat.foo case. That is,
    # import compat.vK.compat.foo in compat.vN.compat.vK.compat.
    if len(module_split) > 3 and module_split[2] == 'compat':
      src_module = '.'.join(module_split[:3])
      src_name = module_split[3]
      assert src_name != 'v1' and src_name != 'v2', imported_module
    else:  # Handle compat.vN.compat.vK.foo case.
      src_module = '.'.join(module_split[:2])
      src_name = module_split[2]
      if src_name == 'compat':
        continue  # compat.vN.compat.vK.compat is handled separately

    for compat_api_version in compat_api_versions:
      module_builder.add_import(
          symbol=None,
          source_module_name='%s.%s' % (output_package, src_module),
          source_name=src_name,
          dest_module_name='compat.v%d.%s' % (compat_api_version, src_module),
          dest_name=src_name)


def _get_name_and_module(full_name):
  """Split full_name into module and short name.

  Args:
    full_name: Full name of symbol that includes module.

  Returns:
    Full module name and short symbol name.
  """
  name_segments = full_name.split('.')
  return '.'.join(name_segments[:-1]), name_segments[-1]


def _join_modules(module1, module2):
  """Concatenate 2 module components.

  Args:
    module1: First module to join.
    module2: Second module to join.

  Returns:
    Given two modules aaa.bbb and ccc.ddd, returns a joined
    module aaa.bbb.ccc.ddd.
  """
  if not module1:
    return module2
  if not module2:
    return module1
  return '%s.%s' % (module1, module2)


def add_imports_for_symbol(module_code_builder,
                           symbol,
                           source_module_name,
                           source_name,
                           api_name,
                           api_version,
                           output_module_prefix=''):
  """Add imports for the given symbol to `module_code_builder`.

  Args:
    module_code_builder: `_ModuleInitCodeBuilder` instance.
    symbol: A symbol.
    source_module_name: Module that we can import the symbol from.
    source_name: Name we can import the symbol with.
    api_name: API name. Currently, must be either `tensorflow` or `estimator`.
    api_version: API version.
    output_module_prefix: Prefix to prepend to destination module.
  """
  if api_version == 1:
    names_attr = API_ATTRS_V1[api_name].names
    constants_attr = API_ATTRS_V1[api_name].constants
  else:
    names_attr = API_ATTRS[api_name].names
    constants_attr = API_ATTRS[api_name].constants

  # If symbol is _tf_api_constants attribute, then add the constants.
  if source_name == constants_attr:
    for exports, name in symbol:
      for export in exports:
        dest_module, dest_name = _get_name_and_module(export)
        dest_module = _join_modules(output_module_prefix, dest_module)
        module_code_builder.add_import(None, source_module_name, name,
                                       dest_module, dest_name)

  # If symbol has _tf_api_names attribute, then add import for it.
  if (hasattr(symbol, '__dict__') and names_attr in symbol.__dict__):

    # Generate import statements for symbols.
    for export in getattr(symbol, names_attr):  # pylint: disable=protected-access
      dest_module, dest_name = _get_name_and_module(export)
      dest_module = _join_modules(output_module_prefix, dest_module)
      module_code_builder.add_import(symbol, source_module_name, source_name,
                                     dest_module, dest_name)


def get_api_init_text(packages,
                      output_package,
                      api_name,
                      api_version,
                      compat_api_versions=None,
                      lazy_loading=_LAZY_LOADING,
                      use_relative_imports=False):
  """Get a map from destination module to __init__.py code for that module.

  Args:
    packages: Base python packages containing python with target tf_export
      decorators.
    output_package: Base output python package where generated API will be
      added.
    api_name: API you want to generate (e.g. `tensorflow` or `estimator`).
    api_version: API version you want to generate (1 or 2).
    compat_api_versions: Additional API versions to generate under compat/
      directory.
    lazy_loading: Boolean flag. If True, a lazy loading `__init__.py` file is
      produced and if `False`, static imports are used.
    use_relative_imports: True if we should use relative imports when importing
      submodules.

  Returns:
    A dictionary where
      key: (string) destination module (for e.g. tf or tf.consts).
      value: (string) text that should be in __init__.py files for
        corresponding modules.
  """
  if compat_api_versions is None:
    compat_api_versions = []
  module_code_builder = _ModuleInitCodeBuilder(output_package, api_version,
                                               lazy_loading,
                                               use_relative_imports)

  # Traverse over everything imported above. Specifically,
  # we want to traverse over TensorFlow Python modules.

  def in_packages(m):
    return any(package in m for package in packages)

  for module in list(sys.modules.values()):
    # Only look at tensorflow modules.
    if (not module or not hasattr(module, '__name__') or
        module.__name__ is None or not in_packages(module.__name__)):
      continue
    # Do not generate __init__.py files for contrib modules for now.
    if (('.contrib.' in module.__name__ or module.__name__.endswith('.contrib'))
        and '.lite' not in module.__name__):
      continue

    for module_contents_name in dir(module):
      if (module.__name__ + '.' +
          module_contents_name in _SYMBOLS_TO_SKIP_EXPLICITLY):
        continue
      attr = getattr(module, module_contents_name)
      _, attr = tf_decorator.unwrap(attr)

      add_imports_for_symbol(module_code_builder, attr, module.__name__,
                             module_contents_name, api_name, api_version)
      for compat_api_version in compat_api_versions:
        add_imports_for_symbol(module_code_builder, attr, module.__name__,
                               module_contents_name, api_name,
                               compat_api_version,
                               _COMPAT_MODULE_TEMPLATE % compat_api_version)

  if compat_api_versions:
    add_nested_compat_imports(module_code_builder, compat_api_versions,
                              output_package)
  return module_code_builder.build()


def get_module(dir_path, relative_to_dir):
  """Get module that corresponds to path relative to relative_to_dir.

  Args:
    dir_path: Path to directory.
    relative_to_dir: Get module relative to this directory.

  Returns:
    Name of module that corresponds to the given directory.
  """
  dir_path = dir_path[len(relative_to_dir):]
  # Convert path separators to '/' for easier parsing below.
  dir_path = dir_path.replace(os.sep, '/')
  return dir_path.replace('/', '.').strip('.')


def get_module_docstring(module_name, package, api_name):
  """Get docstring for the given module.

  This method looks for docstring in the following order:
  1. Checks if module has a docstring specified in doc_srcs.
  2. Checks if module has a docstring source module specified
     in doc_srcs. If it does, gets docstring from that module.
  3. Checks if module with module_name exists under base package.
     If it does, gets docstring from that module.
  4. Returns a default docstring.

  Args:
    module_name: module name relative to tensorflow (excluding 'tensorflow.'
      prefix) to get a docstring for.
    package: Base python package containing python with target tf_export
      decorators.
    api_name: API you want to generate (e.g. `tensorflow` or `estimator`).

  Returns:
    One-line docstring to describe the module.
  """
  # Get the same module doc strings for any version. That is, for module
  # 'compat.v1.foo' we can get docstring from module 'foo'.
  for version in _API_VERSIONS:
    compat_prefix = _COMPAT_MODULE_TEMPLATE % version
    if module_name.startswith(compat_prefix):
      module_name = module_name[len(compat_prefix):].strip('.')

  # Module under base package to get a docstring from.
  docstring_module_name = module_name

  doc_sources = doc_srcs.get_doc_sources(api_name)

  if module_name in doc_sources:
    docsrc = doc_sources[module_name]
    if docsrc.docstring:
      return docsrc.docstring
    if docsrc.docstring_module_name:
      docstring_module_name = docsrc.docstring_module_name

  docstring_module_name = package + '.' + docstring_module_name
  if (docstring_module_name in sys.modules and
      sys.modules[docstring_module_name].__doc__):
    return sys.modules[docstring_module_name].__doc__

  return 'Public API for tf.%s namespace.' % module_name


def create_api_files(output_files,
                     packages,
                     root_init_template,
                     output_dir,
                     output_package,
                     api_name,
                     api_version,
                     compat_api_versions,
                     compat_init_templates,
                     lazy_loading=_LAZY_LOADING,
                     use_relative_imports=False):
  """Creates __init__.py files for the Python API.

  Args:
    output_files: List of __init__.py file paths to create.
    packages: Base python packages containing python with target tf_export
      decorators.
    root_init_template: Template for top-level __init__.py file. "# API IMPORTS
      PLACEHOLDER" comment in the template file will be replaced with imports.
    output_dir: output API root directory.
    output_package: Base output package where generated API will be added.
    api_name: API you want to generate (e.g. `tensorflow` or `estimator`).
    api_version: API version to generate (`v1` or `v2`).
    compat_api_versions: Additional API versions to generate in compat/
      subdirectory.
    compat_init_templates: List of templates for top level compat init files in
      the same order as compat_api_versions.
    lazy_loading: Boolean flag. If True, a lazy loading `__init__.py` file is
      produced and if `False`, static imports are used.
    use_relative_imports: True if we should use relative imports when import
      submodules.

  Raises:
    ValueError: if output_files list is missing a required file.
  """
  module_name_to_file_path = {}
  for output_file in output_files:
    module_name = get_module(os.path.dirname(output_file), output_dir)
    module_name_to_file_path[module_name] = os.path.normpath(output_file)

  # Create file for each expected output in genrule.
  for module, file_path in module_name_to_file_path.items():
    if not os.path.isdir(os.path.dirname(file_path)):
      os.makedirs(os.path.dirname(file_path))
    open(file_path, 'a').close()

  (
      module_text_map,
      deprecation_footer_map,
      root_module_footer,
  ) = get_api_init_text(packages, output_package, api_name, api_version,
                        compat_api_versions, lazy_loading, use_relative_imports)

  # Add imports to output files.
  missing_output_files = []
  # Root modules are "" and "compat.v*".
  root_module = ''
  compat_module_to_template = {
      _COMPAT_MODULE_TEMPLATE % v: t
      for v, t in zip(compat_api_versions, compat_init_templates)
  }
  for v in compat_api_versions:
    compat_module_to_template.update({
        _SUBCOMPAT_MODULE_TEMPLATE % (v, vs): t
        for vs, t in zip(compat_api_versions, compat_init_templates)
    })

  for module, text in module_text_map.items():
    # Make sure genrule output file list is in sync with API exports.
    if module not in module_name_to_file_path:
      module_file_path = '"%s/__init__.py"' % (module.replace('.', '/'))
      missing_output_files.append(module_file_path)
      continue

    contents = ''
    if module == root_module and root_init_template:
      # Read base init file for root module
      with open(root_init_template, 'r') as root_init_template_file:
        contents = root_init_template_file.read()
        contents = contents.replace('# API IMPORTS PLACEHOLDER', text)
        contents = contents.replace('# __all__ PLACEHOLDER', root_module_footer)
    elif module in compat_module_to_template:
      # Read base init file for compat module
      with open(compat_module_to_template[module], 'r') as init_template_file:
        contents = init_template_file.read()
        contents = contents.replace('# API IMPORTS PLACEHOLDER', text)
    else:
      contents = (
          _GENERATED_FILE_HEADER %
          get_module_docstring(module, packages[0], api_name) + text +
          _GENERATED_FILE_FOOTER)
    if module in deprecation_footer_map:
      if '# WRAPPER_PLACEHOLDER' in contents:
        contents = contents.replace('# WRAPPER_PLACEHOLDER',
                                    deprecation_footer_map[module])
      else:
        contents += deprecation_footer_map[module]
    with open(module_name_to_file_path[module], 'w') as fp:
      fp.write(contents)

  if missing_output_files:
    raise ValueError(
        """Missing outputs for genrule:\n%s. Be sure to add these targets to
tensorflow/python/tools/api/generator/api_init_files_v1.bzl and
tensorflow/python/tools/api/generator/api_init_files.bzl (tensorflow repo), or
tensorflow_estimator/python/estimator/api/api_gen.bzl (estimator repo)""" %
        ',\n'.join(sorted(missing_output_files)))


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      'outputs',
      metavar='O',
      type=str,
      nargs='+',
      help='If a single file is passed in, then we we assume it contains a '
      'semicolon-separated list of Python files that we expect this script to '
      'output. If multiple files are passed in, then we assume output files '
      'are listed directly as arguments.')
  parser.add_argument(
      '--packages',
      default=_DEFAULT_PACKAGE,
      type=str,
      help='Base packages that import modules containing the target tf_export '
      'decorators.')
  parser.add_argument(
      '--root_init_template',
      default='',
      type=str,
      help='Template for top level __init__.py file. '
      '"#API IMPORTS PLACEHOLDER" comment will be replaced with imports.')
  parser.add_argument(
      '--apidir',
      type=str,
      required=True,
      help='Directory where generated output files are placed. '
      'gendir should be a prefix of apidir. Also, apidir '
      'should be a prefix of every directory in outputs.')
  parser.add_argument(
      '--apiname',
      required=True,
      type=str,
      choices=API_ATTRS.keys(),
      help='The API you want to generate.')
  parser.add_argument(
      '--apiversion',
      default=2,
      type=int,
      choices=_API_VERSIONS,
      help='The API version you want to generate.')
  parser.add_argument(
      '--compat_apiversions',
      default=[],
      type=int,
      action='append',
      help='Additional versions to generate in compat/ subdirectory. '
      'If set to 0, then no additional version would be generated.')
  parser.add_argument(
      '--compat_init_templates',
      default=[],
      type=str,
      action='append',
      help='Templates for top-level __init__ files under compat modules. '
      'The list of init file templates must be in the same order as '
      'list of versions passed with compat_apiversions.')
  parser.add_argument(
      '--output_package',
      default='tensorflow',
      type=str,
      help='Root output package.')
  parser.add_argument(
      '--loading',
      default='default',
      type=str,
      choices=['lazy', 'static', 'default'],
      help='Controls how the generated __init__.py file loads the exported '
      'symbols. \'lazy\' means the symbols are loaded when first used. '
      '\'static\' means all exported symbols are loaded in the '
      '__init__.py file. \'default\' uses the value of the '
      '_LAZY_LOADING constant in create_python_api.py.')
  parser.add_argument(
      '--use_relative_imports',
      default=False,
      type=bool,
      help='Whether to import submodules using relative imports or absolute '
      'imports')
  args = parser.parse_args()

  if len(args.outputs) == 1:
    # If we only get a single argument, then it must be a file containing
    # list of outputs.
    with open(args.outputs[0]) as output_list_file:
      outputs = [line.strip() for line in output_list_file.read().split(';')]
  else:
    outputs = args.outputs

  # Populate `sys.modules` with modules containing tf_export().
  packages = args.packages.split(',')
  for package in packages:
    importlib.import_module(package)

  # Determine if the modules shall be loaded lazily or statically.
  if args.loading == 'default':
    lazy_loading = _LAZY_LOADING
  elif args.loading == 'lazy':
    lazy_loading = True
  elif args.loading == 'static':
    lazy_loading = False
  else:
    # This should never happen (tm).
    raise ValueError('Invalid value for --loading flag: %s. Must be one of '
                     'lazy, static, default.' % args.loading)

  create_api_files(outputs, packages, args.root_init_template, args.apidir,
                   args.output_package, args.apiname, args.apiversion,
                   args.compat_apiversions, args.compat_init_templates,
                   lazy_loading, args.use_relative_imports)


if __name__ == '__main__':
  main()
