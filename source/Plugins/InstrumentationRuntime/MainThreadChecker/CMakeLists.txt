add_lldb_library(lldbPluginInstrumentationRuntimeMainThreadChecker PLUGIN
  MainThreadCheckerRuntime.cpp

  LINK_LIBS
    lldbBreakpoint
    lldbCore
    lldbExpression
    lldbInterpreter
    lldbSymbol
    lldbTarget
  LINK_COMPONENTS
    Support
  )
if(NOT LLDB_BUILT_STANDALONE)
  add_dependencies(lldbPluginInstrumentationRuntimeMainThreadChecker
    swift-syntax-generated-headers)
endif()
