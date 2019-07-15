include(${CMAKE_CURRENT_LIST_DIR}/Apple-lldb-base.cmake)

set(LLVM_DISTRIBUTION_COMPONENTS
  lldb
  liblldb
  lldb-argdumper
  lldb-server
  repl_swift
  CACHE STRING "")

set(LLDB_ALLOW_STATIC_BINDINGS ON CACHE BOOL "")
