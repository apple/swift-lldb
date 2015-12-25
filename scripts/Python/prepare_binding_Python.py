"""
                     The LLVM Compiler Infrastructure

This file is distributed under the University of Illinois Open Source
License. See LICENSE.TXT for details.

Python binding preparation script.
"""

# Python modules:
from __future__ import print_function

import logging
import os
import re
import shutil
import subprocess
import sys


class SwigSettings(object):
    """Provides a single object to represent swig files and settings."""
    def __init__(self):
        self.extensions_file = None
        self.header_files = None
        self.input_file = None
        self.interface_files = None
        self.output_file = None
        self.safecast_file = None
        self.typemaps_file = None
        self.wrapper_file = None

    @classmethod
    def _any_files_newer(cls, files, check_mtime):
        """Returns if any of the given files has a newer modified time.

        @param cls the class
        @param files a list of zero or more file paths to check
        @param check_mtime the modification time to use as a reference.

        @return True if any file's modified time is newer than check_mtime.
        """
        for path in files:
            path_mtime = os.path.getmtime(path)
            if path_mtime > check_mtime:
                # This path was modified more recently than the
                # check_mtime.
                return True
        # If we made it here, nothing was newer than the check_mtime
        return False

    @classmethod
    def _file_newer(cls, path, check_mtime):
        """Tests how recently a file has been modified.

        @param cls the class
        @param path a file path to check
        @param check_mtime the modification time to use as a reference.

        @return True if the file's modified time is newer than check_mtime.
        """
        path_mtime = os.path.getmtime(path)
        return path_mtime > check_mtime

    def output_out_of_date(self):
        """Returns whether the output file is out of date.

        Compares output file time to all the input files.

        @return True if any of the input files are newer than
        the output file, or if the output file doesn't exist;
        False otherwise.
        """
        if not os.path.exists(self.output_file):
            logging.info("will generate, missing binding output file")
            return True
        output_mtime = os.path.getmtime(self.output_file)
        if self._any_files_newer(self.header_files, output_mtime):
            logging.info("will generate, header files newer")
            return True
        if self._any_files_newer(self.interface_files, output_mtime):
            logging.info("will generate, interface files newer")
            return True
        if self._file_newer(self.input_file, output_mtime):
            logging.info("will generate, swig input file newer")
            return True
        if self._file_newer(self.extensions_file, output_mtime):
            logging.info("will generate, swig extensions file newer")
            return True
        if self._file_newer(self.wrapper_file, output_mtime):
            logging.info("will generate, swig wrapper file newer")
            return True
        if self._file_newer(self.typemaps_file, output_mtime):
            logging.info("will generate, swig typemaps file newer")
            return True
        if self._file_newer(self.safecast_file, output_mtime):
            logging.info("will generate, swig safecast file newer")
            return True

        # If we made it here, nothing is newer than the output file.
        # Thus, the output file is not out of date.
        return False


def get_header_files(options):
    """Returns a list of paths to C++ header files for the LLDB API.

    These are the files that define the C++ API that will be wrapped by Python.

    @param options the dictionary of options parsed from the command line.

    @return a list of full paths to the include files used to define the public
    LLDB C++ API.
    """

    header_file_paths = []
    header_base_dir = os.path.join(options.src_root, "include", "lldb")

    # Specify the include files in include/lldb that are not easy to
    # grab programatically.
    for header in [
            "lldb-defines.h",
            "lldb-enumerations.h",
            "lldb-forward.h",
            "lldb-types.h"]:
        header_file_paths.append(os.path.normcase(
            os.path.join(header_base_dir, header)))

    # Include the main LLDB.h file.
    api_dir = os.path.join(header_base_dir, "API")
    header_file_paths.append(os.path.normcase(
        os.path.join(api_dir, "LLDB.h")))

    filename_regex = re.compile(r"^SB.+\.h$")

    # Include all the SB*.h files in the API dir.
    for filename in os.listdir(api_dir):
        if filename_regex.match(filename):
            header_file_paths.append(
                os.path.normcase(os.path.join(api_dir, filename)))

    logging.debug("found public API header file paths: %s", header_file_paths)
    return header_file_paths


def get_interface_files(options):
    """Returns a list of interface files used as input to swig.

    @param options the options dictionary parsed from the command line args.

    @return a list of full paths to the interface (.i) files used to describe
    the public API language binding.
    """
    interface_file_paths = []
    interface_dir = os.path.join(options.src_root, "scripts", "interface")

    for filepath in [f for f in os.listdir(interface_dir)
                     if os.path.splitext(f)[1] == ".i"]:
        interface_file_paths.append(
            os.path.normcase(os.path.join(interface_dir, filepath)))

    logging.debug("found swig interface files: %s", interface_file_paths)
    return interface_file_paths


def remove_ignore_enoent(filename):
    """Removes given file, ignoring error if it doesn't exist.

    @param filename the path of the file to remove.
    """
    try:
        os.remove(filename)
    except OSError as error:
        import errno
        if error.errno != errno.ENOENT:
            raise


def do_swig_rebuild(options, dependency_file, config_build_dir, settings):
    """Generates Python bindings file from swig.

    This method will do a sys.exit() if something fails.  If it returns to
    the caller, it succeeded.

    @param options the parsed command line options structure.
    @param dependency_file path to the bindings dependency file
    to be generated; otherwise, None if a dependency file is not
    to be generated.
    @param config_build_dir used as the output directory used by swig
    @param settings the SwigSettings that specify a number of aspects used
    to configure building the Python binding with swig (mostly paths)
    """
    if options.generate_dependency_file:
        temp_dep_file_path = dependency_file + ".tmp"

    # Build the SWIG args list
    command = [
        options.swig_executable,
        "-c++",
        "-shadow",
        "-python",
        "-threads",
        "-I\"%s\"" % os.path.normcase(
            os.path.join(options.src_root, "include")),
        "-I\"%s\"" % os.path.normcase("./."),
        "-D__STDC_LIMIT_MACROS",
        "-D__STDC_CONSTANT_MACROS"]
    if options.generate_dependency_file:
        command.append("-MMD -MF \"%s\"" % temp_dep_file_path)
    command.extend([
        "-outdir", "\"%s\"" % config_build_dir,
        "-o", "\"%s\"" % settings.output_file,
        "\"%s\"" % settings.input_file
        ])
    logging.info("running swig with: %s", command)

    # Execute swig
    process = subprocess.Popen(
        ' '.join(command),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        shell=True)
    # Wait for SWIG process to terminate
    swig_stdout, swig_stderr = process.communicate()
    return_code = process.returncode
    if return_code != 0:
        logging.error(
            "swig failed with error code %d: stdout=%s, stderr=%s",
            return_code,
            swig_stdout,
            swig_stderr)
        logging.error(
            "command line:\n%s", ' '.join(command))
        sys.exit(return_code)

    logging.info("swig generation succeeded")
    if swig_stdout is not None and len(swig_stdout) > 0:
        logging.info("swig output: %s", swig_stdout)

    # Move the depedency file we just generated to the proper location.
    if options.generate_dependency_file:
        if os.path.exists(temp_dep_file_path):
            shutil.move(temp_dep_file_path, dependency_file)
        else:
            logging.error(
                "failed to generate Python binding depedency file '%s'",
                temp_dep_file_path)
            if os.path.exists(dependency_file):
                # Delete the old one.
                os.remove(dependency_file)
            sys.exit(-10)


def static_binding_paths(options):
    """Returns the full VCS path to the Python .cpp and .py static bindings."""
    lldb_wrap_python_src_path = os.path.join(
        options.src_root,
        "scripts",
        "Python",
        options.static_binding_dir,
        "LLDBWrapPython.cpp")
    lldb_py_src_path = os.path.join(
        options.src_root,
        "scripts",
        "Python",
        options.static_binding_dir,
        "lldb.py")
    return (lldb_wrap_python_src_path, lldb_py_src_path)


def copy_static_bindings(options, config_build_dir, settings):
    """Copies the static Python bindings over to the build dir.
    """
    lldb_wrap_python_src_path, lldb_py_src_path = static_binding_paths(options)
    # Copy the LLDBWrapPython.cpp C++ binding file impl over.
    if not os.path.exists(lldb_wrap_python_src_path):
        logging.error(
            "failed to find static Python binding .cpp file at '%s'",
            lldb_wrap_python_src_path)
        sys.exit(-12)
    shutil.copyfile(lldb_wrap_python_src_path, settings.output_file)

    # Copy the lldb.py impl over.
    if not os.path.exists(lldb_py_src_path):
        logging.error(
            "failed to find static Python binding .py file at '%s'",
            lldb_py_src_path)
        sys.exit(-13)
    lldb_py_dest_path = os.path.join(
        os.path.dirname(settings.output_file),
        "lldb.py")
    shutil.copyfile(lldb_py_src_path, lldb_py_dest_path)


def static_bindings_require_refresh(options, config_build_dir, settings):
    """Returns whether existing static bindings require an update."""
    lldb_wrap_python_src_path, lldb_py_src_path = static_binding_paths(options)
    # Check if LLDBWrapPython.cpp C++ static binding is different than
    # in the build dir.
    if not os.path.exists(lldb_wrap_python_src_path):
        logging.error(
            "failed to find static Python binding .cpp file at '%s'",
            lldb_wrap_python_src_path)
        sys.exit(-12)
    if not os.path.exists(settings.output_file):
        # We for sure need an update.
        # Note this should already be True - we don't check
        # for a refresh if we already know we have to generate them.
        return True
    import filecmp
    if not filecmp.cmp(
            lldb_wrap_python_src_path, settings.output_file, shallow=False):
        return True

    # Check if the lldb.py Python static binding is different than
    # in the build dir.
    if not os.path.exists(lldb_py_src_path):
        logging.error(
            "failed to find static Python binding .py file at '%s'",
            lldb_py_src_path)
        sys.exit(-13)
    lldb_py_dest_path = os.path.join(
        os.path.dirname(settings.output_file),
        "lldb.py")
    if not os.path.exists(lldb_py_dest_path):
        # We for sure need an update.
        # Note this should already be True - we don't check
        # for a refresh if we already know we have to generate them.
        return True
    if not filecmp.cmp(
            lldb_py_src_path, lldb_py_dest_path, shallow=False):
        return True

    # If we made it here, we don't need to update.
    return False


def run_python_script(script_and_args):
    """Runs a python script, logging appropriately.

    If the command returns anything non-zero, it is registered as
    an error and exits the program.

    @param script_and_args the python script to execute, along with
    the command line arguments to pass to it.
    """
    command_line = "%s %s" % (sys.executable, script_and_args)
    process = subprocess.Popen(command_line, shell=True)
    script_stdout, script_stderr = process.communicate()
    return_code = process.returncode
    if return_code != 0:
        logging.error("failed to run '%s': %s", command_line, script_stderr)
        sys.exit(return_code)
    else:
        logging.info("ran script '%s'", command_line)
        if script_stdout is not None:
            logging.info("output: %s", script_stdout)


def do_modify_python_lldb(options, config_build_dir):
    """Executes the modify-python-lldb.py script.

    @param options the parsed command line arguments
    @param config_build_dir the directory where the Python output was created.
    """
    script_path = os.path.normcase(
        os.path.join(
            options.src_root,
            "scripts",
            "Python",
            "modify-python-lldb.py"))

    if not os.path.exists(script_path):
        logging.error("failed to find python script: '%s'", script_path)
        sys.exit(-11)

    script_invocation = "%s %s" % (script_path, config_build_dir)
    run_python_script(script_invocation)


def get_python_module_path(options):
    """Returns the location where the lldb Python module should be placed.

    @param options dictionary of options parsed from the command line.

    @return the directory where the lldb module should be placed.
    """
    if options.framework:
        # Caller wants to use the OS X framework packaging.

        # We are packaging in an OS X-style framework bundle. The
        # module dir will be within the
        # LLDB.framework/Resources/Python subdirectory.
        return os.path.join(
            options.target_dir,
            "LLDB.framework",
            "Resources",
            "Python",
            "lldb")
    else:
        from distutils.sysconfig import get_python_lib

        if options.prefix is not None:
            module_path = get_python_lib(True, False, options.prefix)
        else:
            module_path = get_python_lib(True, False)
        return os.path.normcase(
            os.path.join(module_path, "lldb"))


def main(options):
    """Pepares the Python language binding to LLDB.

    @param options the parsed command line argument dictionary
    """
    # Setup generated dependency file options.
    if options.generate_dependency_file:
        dependency_file = os.path.normcase(os.path.join(
            options.target_dir, "LLDBWrapPython.cpp.d"))
    else:
        dependency_file = None

    # Keep track of all the swig-related settings.
    settings = SwigSettings()

    # Determine the final binding file path.
    settings.output_file = os.path.normcase(
        os.path.join(options.target_dir, "LLDBWrapPython.cpp"))

    # Touch the output file (but don't really generate it) if python
    # is disabled.
    disable_python = os.getenv("LLDB_DISABLE_PYTHON", None)
    if disable_python is not None and disable_python == "1":
        remove_ignore_enoent(settings.output_file)
        # Touch the file.
        open(settings.output_file, 'w').close()
        logging.info(
            "Created empty python binding file due to LLDB_DISABLE_PYTHON "
            "being set")
        return

    # We also check the GCC_PREPROCESSOR_DEFINITIONS to see if it
    # contains LLDB_DISABLE_PYTHON.  If so, we skip generating
    # the binding.
    gcc_preprocessor_defs = os.getenv("GCC_PREPROCESSOR_DEFINITIONS", None)
    if gcc_preprocessor_defs is not None:
        if re.search(r"LLDB_DISABLE_PYTHON", gcc_preprocessor_defs):
            remove_ignore_enoent(settings.output_file)
            # Touch the file
            open(settings.output_file, 'w').close()
            logging.info(
                "Created empty python binding file due to "
                "finding LLDB_DISABLE_PYTHON in GCC_PREPROCESSOR_DEFINITIONS")
            return

    # Setup paths used during swig invocation.
    settings.input_file = os.path.normcase(
        os.path.join(options.src_root, "scripts", "lldb.swig"))
    scripts_python_dir = os.path.dirname(os.path.realpath(__file__))
    settings.extensions_file = os.path.normcase(
        os.path.join(scripts_python_dir, "python-extensions.swig"))
    settings.wrapper_file = os.path.normcase(
        os.path.join(scripts_python_dir, "python-wrapper.swig"))
    settings.typemaps_file = os.path.normcase(
        os.path.join(scripts_python_dir, "python-typemaps.swig"))
    settings.safecast_file = os.path.normcase(
        os.path.join(scripts_python_dir, "python-swigsafecast.swig"))

    settings.header_files = get_header_files(options)
    settings.interface_files = get_interface_files(options)

    generate_output = settings.output_out_of_date()

    # Determine where to put the module.
    python_module_path = get_python_module_path(options)
    logging.info("python module path: %s", python_module_path)

    # Handle the configuration build dir.
    if options.config_build_dir is not None:
        config_build_dir = options.config_build_dir
    else:
        config_build_dir = python_module_path

    # Allow missing/non-link _lldb.so to force regeneration.
    if not generate_output:
        # Ensure the _lldb.so file exists.
        so_path = os.path.join(python_module_path, "_lldb.so")
        if not os.path.exists(so_path) or not os.path.islink(so_path):
            logging.info("_lldb.so doesn't exist or isn't a symlink")
            generate_output = True

    # Allow missing __init__.py to force regeneration.
    if not generate_output:
        # Ensure the __init__.py for the lldb module can be found.
        init_path = os.path.join(python_module_path, "__init__.py")
        if not os.path.exists(init_path):
            logging.info("__init__.py doesn't exist")
            generate_output = True

    # Figure out if we would be using static bindings
    use_static_bindings = (
        not options.swig_executable or
        not os.path.exists(options.swig_executable))
    if use_static_bindings and not generate_output:
        # If the contents of the VCS static binding are different from what
        # we have in the build dir, we should copy them regardless.
        if static_bindings_require_refresh(
                options, config_build_dir, settings):
            # Force the static bindings to be copied later on, thus preventing
            # early exit from this method.
            logging.info("updating static binding due to VCS binding changes")
            generate_output = True

    if not generate_output:
        logging.info(
            "Skipping Python binding generation: everything is up to date")
        return

    # Generate the Python binding with swig, or use the static bindings if
    # no swig.
    if use_static_bindings:
        # Copy over the static bindings.  We capture the the modified (
        # i.e. post-processed) binding, so we don't do the modify step
        # here - the modifications have already been applied.
        copy_static_bindings(options, config_build_dir, settings)
    else:
        # Generate the bindings with swig.
        logging.info("Python binding is out of date, regenerating")
        do_swig_rebuild(options, dependency_file, config_build_dir, settings)
        if options.generate_dependency_file:
            return

        # Post process the swig-generated file.
        do_modify_python_lldb(options, config_build_dir)


# This script can be called by another Python script by calling the main()
# function directly
if __name__ == "__main__":
    print("Script cannot be called directly.")
    sys.exit(-1)
