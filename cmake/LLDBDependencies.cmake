set( LLDB_USED_LIBS
  lldbBase
  lldbBreakpoint
  lldbCommands
  lldbDataFormatters
  lldbHost
  lldbCore
  lldbExpression
  lldbInitialization
  lldbInterpreter
  lldbSymbol
  lldbTarget
  lldbUtility

  # Plugins
  lldbPluginDisassemblerLLVM
  lldbPluginSymbolFileDWARF
  lldbPluginSymbolFilePDB
  lldbPluginSymbolFileSymtab
  lldbPluginDynamicLoaderStatic
  lldbPluginDynamicLoaderPosixDYLD
  lldbPluginDynamicLoaderHexagonDYLD
  lldbPluginDynamicLoaderWindowsDYLD

  lldbPluginCPlusPlusLanguage
  lldbPluginGoLanguage
  lldbPluginJavaLanguage
  lldbPluginObjCLanguage
  lldbPluginObjCPlusPlusLanguage
  lldbPluginSwiftLanguage
  lldbPluginOCamlLanguage

  lldbPluginObjectFileELF
  lldbPluginObjectFileJIT
  lldbPluginSymbolVendorELF
  lldbPluginObjectContainerBSDArchive
  lldbPluginObjectContainerMachOArchive
  lldbPluginProcessGDBRemote
  lldbPluginProcessUtility
  lldbPluginPlatformAndroid
  lldbPluginPlatformGDB
  lldbPluginPlatformFreeBSD
  lldbPluginPlatformKalimba
  lldbPluginPlatformLinux
  lldbPluginPlatformNetBSD
  lldbPluginPlatformPOSIX
  lldbPluginPlatformWindows
  lldbPluginObjectContainerMachOArchive
  lldbPluginObjectContainerBSDArchive
  lldbPluginPlatformMacOSX
  lldbPluginStructuredDataDarwinLog
  lldbPluginDynamicLoaderMacOSXDYLD
  lldbPluginUnwindAssemblyInstEmulation
  lldbPluginUnwindAssemblyX86
  lldbPluginAppleObjCRuntime
  lldbPluginRenderScriptRuntime
  lldbPluginLanguageRuntimeGo
  lldbPluginLanguageRuntimeJava
  lldbPluginCXXItaniumABI
  lldbPluginABIMacOSX_arm
  lldbPluginABIMacOSX_arm64
  lldbPluginABIMacOSX_i386
  lldbPluginABISysV_arm
  lldbPluginABISysV_arm64
  lldbPluginABISysV_i386
  lldbPluginABISysV_x86_64
  lldbPluginABISysV_hexagon
  lldbPluginABISysV_ppc
  lldbPluginABISysV_ppc64
  lldbPluginABISysV_mips
  lldbPluginABISysV_mips64
  lldbPluginABISysV_s390x
  lldbPluginInstructionARM
  lldbPluginInstructionARM64
  lldbPluginInstructionMIPS
  lldbPluginInstructionMIPS64
  lldbPluginObjectFilePECOFF
  lldbPluginOSGo
  lldbPluginOSPython
  lldbPluginMemoryHistoryASan
  lldbPluginInstrumentationRuntimeAddressSanitizer
  lldbPluginInstrumentationRuntimeThreadSanitizer
  lldbPluginSystemRuntimeMacOSX
  lldbPluginProcessElfCore
  lldbPluginProcessMinidump
  lldbPluginJITLoaderGDB
  lldbPluginExpressionParserClang
  lldbPluginExpressionParserGo
  lldbPluginExpressionParserSwift
  )

# Windows-only libraries
if ( CMAKE_SYSTEM_NAME MATCHES "Windows" )
  list(APPEND LLDB_USED_LIBS
    lldbPluginProcessWindowsCommon
    ws2_32
    rpcrt4
    )
endif ()

# Linux-only libraries
if ( CMAKE_SYSTEM_NAME MATCHES "Linux|Android" )
  list(APPEND LLDB_USED_LIBS
    lldbPluginProcessLinux
    lldbPluginProcessPOSIX
   )
endif ()

# FreeBSD-only libraries
if ( CMAKE_SYSTEM_NAME MATCHES "FreeBSD" )
  list(APPEND LLDB_USED_LIBS
    lldbPluginProcessFreeBSD
    lldbPluginProcessPOSIX
    )
endif ()

# NetBSD-only libraries
if ( CMAKE_SYSTEM_NAME MATCHES "NetBSD" )
  list(APPEND LLDB_USED_LIBS
    lldbPluginProcessPOSIX
    )
endif ()

# Darwin-only libraries
if ( CMAKE_SYSTEM_NAME MATCHES "Darwin" )
  list(APPEND LLDB_USED_LIBS
    lldbPluginDynamicLoaderDarwinKernel
    lldbPluginObjectFileMachO
    lldbPluginProcessMachCore
    lldbPluginProcessMacOSXKernel
    lldbPluginSymbolVendorMacOSX
    )
endif()

macro(add_libs_from_path build_dir lib_prefix lib_list)
  file(TO_CMAKE_PATH ${build_dir} build_dir_cmake)
  
  file(GLOB built_libs
    RELATIVE "${build_dir_cmake}/lib${LLVM_LIBDIR_SUFFIX}"
    ${build_dir_cmake}/lib/lib${lib_prefix}*.a)

  set(${lib_list})
  
  foreach(built_lib ${built_libs})
    string(REGEX REPLACE ".*lib(${lib_prefix}[^.]+)\\..*" "\\1" built_lib_no_extension ${built_lib})
    list(APPEND ${lib_list} ${built_lib_no_extension})
  endforeach()
endmacro(add_libs_from_path)

if (LLDB_BUILT_STANDALONE)
  add_libs_from_path(${LLDB_PATH_TO_SWIFT_BUILD} "swift" SWIFT_ALL_LIBS)
  add_libs_from_path(${LLDB_PATH_TO_CLANG_BUILD} "clang" CLANG_ALL_LIBS)
  add_libs_from_path(${LLDB_PATH_TO_LLVM_BUILD} "LLVM" LLVM_ALL_LIBS)
else()
  set(CLANG_ALL_LIBS
    clangAnalysis
    clangAST
    clangBasic
    clangCodeGen
    clangDriver
    clangEdit
    clangFrontend
    clangLex
    clangParse
    clangRewrite
    clangRewriteFrontend
    clangSema
    clangSerialization)
  set(SWIFT_ALL_LIBS
    swiftBasic
    swiftAST
    swiftIDE
    swiftFrontend
    swiftSerialization
    swiftClangImporter
    swiftParse
    swiftSIL
    swiftSILOptimizer
    swiftASTSectionImporter
    swiftRemoteAST
    )
endif()

set(LLDB_SYSTEM_LIBS)
if (NOT LLDB_DISABLE_LIBEDIT)
  list(APPEND LLDB_SYSTEM_LIBS edit)
endif()
if (NOT LLDB_DISABLE_CURSES)
  list(APPEND LLDB_SYSTEM_LIBS ${CURSES_LIBRARIES})
  if(LLVM_ENABLE_TERMINFO AND HAVE_TERMINFO)
    list(APPEND LLDB_SYSTEM_LIBS ${TERMINFO_LIBS})
  endif()
endif()

if (NOT HAVE_CXX_ATOMICS64_WITHOUT_LIB )
    list(APPEND LLDB_SYSTEM_LIBS atomic)
endif()

list(APPEND LLDB_SYSTEM_LIBS ${Backtrace_LIBRARY})

if (NOT LLDB_DISABLE_PYTHON AND NOT LLVM_BUILD_STATIC)
  list(APPEND LLDB_SYSTEM_LIBS ${PYTHON_LIBRARIES})
endif()

list(APPEND LLDB_SYSTEM_LIBS ${system_libs})

if (LLVM_BUILD_STATIC)
  if (NOT LLDB_DISABLE_PYTHON)
    list(APPEND LLDB_SYSTEM_LIBS python2.7 util)
  endif()
  if (NOT LLDB_DISABLE_CURSES)
    list(APPEND LLDB_SYSTEM_LIBS gpm)
  endif()
endif()

# we should do this with a configuration script, but there is none for UUID
if (NOT CMAKE_SYSTEM_NAME MATCHES "Darwin")
list(APPEND LLDB_SYSTEM_LIBS uuid)
endif()

if(LLDB_BUILT_STANDALONE)
  # this needs to be linked statially
  list(APPEND LLDB_SYSTEM_LIBS ${PATH_TO_CMARK_BUILD}/src/libcmark.a)
else()
  list(APPEND LLDB_SYSTEM_LIBS libcmark_static)
endif()

set(LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  interpreter
  asmparser
  bitreader
  bitwriter
  codegen
  ipo
  selectiondag
  bitreader
  mc
  mcjit
  core
  mcdisassembler
  executionengine
  runtimedyld
  option
  support
  coverage
  target
  )

if ( NOT LLDB_DISABLE_PYTHON )
  set_source_files_properties(${LLDB_WRAP_PYTHON} PROPERTIES GENERATED 1)
  if (CLANG_CL)
    set_source_files_properties(${LLDB_WRAP_PYTHON} PROPERTIES COMPILE_FLAGS -Wno-unused-function)
  endif()
  if (LLVM_COMPILER_IS_GCC_COMPATIBLE AND
      NOT "${CMAKE_SYSTEM_NAME}" MATCHES "Darwin")
    set_property(SOURCE ${LLDB_WRAP_PYTHON}
                 APPEND_STRING PROPERTY COMPILE_FLAGS " -Wno-sequence-point -Wno-cast-qual")
  endif ()
endif()
