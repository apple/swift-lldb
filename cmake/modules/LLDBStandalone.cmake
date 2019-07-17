project(lldb)

option(LLVM_INSTALL_TOOLCHAIN_ONLY "Only include toolchain files in the 'install' target." OFF)

set(LLDB_PATH_TO_LLVM_BUILD "" CACHE PATH "Path to LLVM build tree")
set(LLDB_PATH_TO_CLANG_BUILD "${LLDB_PATH_TO_LLVM_BUILD}" CACHE PATH "Path to Clang build tree")
set(LLDB_PATH_TO_SWIFT_BUILD "" CACHE PATH "Path to Swift build tree")
set(LLDB_PATH_TO_SWIFT_SOURCE "" CACHE PATH "Path to Swift source tree")

file(TO_CMAKE_PATH "${LLDB_PATH_TO_LLVM_BUILD}" LLDB_PATH_TO_LLVM_BUILD)
file(TO_CMAKE_PATH "${LLDB_PATH_TO_CLANG_BUILD}" LLDB_PATH_TO_CLANG_BUILD)
file(TO_CMAKE_PATH "${LLDB_PATH_TO_SWIFT_BUILD}" LLDB_PATH_TO_SWIFT_BUILD)

file(TO_CMAKE_PATH "${LLDB_PATH_TO_SWIFT_SOURCE}" LLDB_PATH_TO_SWIFT_SOURCE)

find_package(LLVM REQUIRED CONFIG
  HINTS "${LLDB_PATH_TO_LLVM_BUILD}" NO_CMAKE_FIND_ROOT_PATH)
find_package(Clang REQUIRED CONFIG
  HINTS "${LLDB_PATH_TO_CLANG_BUILD}" NO_CMAKE_FIND_ROOT_PATH)

# We set LLVM_CMAKE_PATH so that GetSVN.cmake is found correctly when building SVNVersion.inc
set(LLVM_CMAKE_PATH ${LLVM_CMAKE_DIR} CACHE PATH "Path to LLVM CMake modules")

set(LLVM_MAIN_SRC_DIR ${LLVM_BUILD_MAIN_SRC_DIR} CACHE PATH "Path to LLVM source tree")
set(LLVM_MAIN_INCLUDE_DIR ${LLVM_MAIN_INCLUDE_DIR} CACHE PATH "Path to llvm/include")
set(LLVM_BINARY_DIR ${LLVM_BINARY_DIR} CACHE PATH "Path to LLVM build tree")

set(lit_file_name "llvm-lit")
if(CMAKE_HOST_WIN32 AND NOT CYGWIN)
  set(lit_file_name "${lit_file_name}.py")
endif()

function(append_configuration_directories input_dir output_dirs)
  set(dirs_list ${input_dir})
  foreach(config_type ${LLVM_CONFIGURATION_TYPES})
    string(REPLACE ${CMAKE_CFG_INTDIR} ${config_type} dir ${input_dir})
    list(APPEND dirs_list ${dir})
  endforeach()
  set(${output_dirs} ${dirs_list} PARENT_SCOPE)
endfunction()


append_configuration_directories(${LLVM_TOOLS_BINARY_DIR} config_dirs)
find_program(lit_full_path ${lit_file_name} ${config_dirs} NO_DEFAULT_PATH)
set(LLVM_DEFAULT_EXTERNAL_LIT ${lit_full_path} CACHE PATH "Path to llvm-lit")

if(CMAKE_CROSSCOMPILING)
  set(LLVM_NATIVE_BUILD "${LLDB_PATH_TO_LLVM_BUILD}/NATIVE")
  if (NOT EXISTS "${LLVM_NATIVE_BUILD}")
    message(FATAL_ERROR
      "Attempting to cross-compile LLDB standalone but no native LLVM build
      found. Please cross-compile LLVM as well.")
  endif()

  if (CMAKE_HOST_SYSTEM_NAME MATCHES "Windows")
    set(HOST_EXECUTABLE_SUFFIX ".exe")
  endif()

  if (NOT CMAKE_CONFIGURATION_TYPES)
    set(LLVM_TABLEGEN_EXE
      "${LLVM_NATIVE_BUILD}/bin/llvm-tblgen${HOST_EXECUTABLE_SUFFIX}")
  else()
    # NOTE: LLVM NATIVE build is always built Release, as is specified in
    # CrossCompile.cmake
    set(LLVM_TABLEGEN_EXE
      "${LLVM_NATIVE_BUILD}/Release/bin/llvm-tblgen${HOST_EXECUTABLE_SUFFIX}")
  endif()
else()
  set(tblgen_file_name "llvm-tblgen${CMAKE_EXECUTABLE_SUFFIX}")
  append_configuration_directories(${LLVM_TOOLS_BINARY_DIR} config_dirs)
  find_program(LLVM_TABLEGEN_EXE ${tblgen_file_name} ${config_dirs} NO_DEFAULT_PATH)
endif()

# They are used as destination of target generators.
set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/lib${LLVM_LIBDIR_SUFFIX})
if(WIN32 OR CYGWIN)
  # DLL platform -- put DLLs into bin.
  set(LLVM_SHLIB_OUTPUT_INTDIR ${LLVM_RUNTIME_OUTPUT_INTDIR})
else()
  set(LLVM_SHLIB_OUTPUT_INTDIR ${LLVM_LIBRARY_OUTPUT_INTDIR})
endif()

# We append the directory in which LLVMConfig.cmake lives. We expect LLVM's
# CMake modules to be in that directory as well.
list(APPEND CMAKE_MODULE_PATH "${LLVM_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLDB_PATH_TO_SWIFT_SOURCE}/cmake/modules/")
include(AddLLVM)
include(TableGen)
include(HandleLLVMOptions)
include(CheckAtomic)
include(LLVMDistributionSupport)

if (PYTHON_EXECUTABLE STREQUAL "")
  set(Python_ADDITIONAL_VERSIONS 3.7. 3.6 3.4 2.7)
  include(FindPythonInterp)
  if( NOT PYTHONINTERP_FOUND )
    message(FATAL_ERROR
            "Unable to find Python interpreter, required for builds and testing.
              Please install Python or specify the PYTHON_EXECUTABLE CMake variable.")
  endif()
else()
  message(STATUS "Found PythonInterp: ${PYTHON_EXECUTABLE}")
endif()

# Start Swift Mods
find_package(Swift REQUIRED CONFIG
  HINTS "${LLDB_PATH_TO_SWIFT_BUILD}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
# End Swift Mods

set(PACKAGE_VERSION "${LLVM_PACKAGE_VERSION}")

option(LLVM_USE_FOLDERS "Enable solution folders in Visual Studio. Disable for Express versions." ON)
if(LLVM_USE_FOLDERS)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

set_target_properties(clang-tablegen-targets PROPERTIES FOLDER "lldb misc")
set_target_properties(intrinsics_gen PROPERTIES FOLDER "lldb misc")

set(CMAKE_INCLUDE_CURRENT_DIR ON)
include_directories(
  "${CMAKE_BINARY_DIR}/include"
  "${LLVM_INCLUDE_DIRS}"
  "${CLANG_INCLUDE_DIRS}"
  "${LLDB_PATH_TO_SWIFT_BUILD}/include"
  "${LLDB_PATH_TO_SWIFT_SOURCE}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/source")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib${LLVM_LIBDIR_SUFFIX})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib${LLVM_LIBDIR_SUFFIX})

set(LLDB_BUILT_STANDALONE 1)
