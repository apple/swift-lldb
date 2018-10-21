add_lldb_library(lldbPluginInstrumentationRuntimeSwiftRuntimeReporting PLUGIN
  SwiftRuntimeReporting.cpp

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
  add_dependencies(lldbPluginInstrumentationRuntimeSwiftRuntimeReporting
    swift-syntax-generated-headers)
endif()
