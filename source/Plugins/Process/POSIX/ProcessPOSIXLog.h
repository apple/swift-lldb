//===-- ProcessPOSIXLog.h -----------------------------------------*- C++
//-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_ProcessPOSIXLog_h_
#define liblldb_ProcessPOSIXLog_h_

// C Includes
// C++ Includes
// Other libraries and framework includes

// Project includes
#include "lldb/Core/Log.h"

#define POSIX_LOG_PROCESS (1u << 1)
#define POSIX_LOG_THREAD (1u << 2)
#define POSIX_LOG_PACKETS (1u << 3)
#define POSIX_LOG_MEMORY (1u << 4) // Log memory reads/writes calls
#define POSIX_LOG_MEMORY_DATA_SHORT                                            \
  (1u << 5) // Log short memory reads/writes bytes
#define POSIX_LOG_MEMORY_DATA_LONG                                             \
  (1u << 6) // Log all memory reads/writes bytes
#define POSIX_LOG_BREAKPOINTS (1u << 7)
#define POSIX_LOG_WATCHPOINTS (1u << 8)
#define POSIX_LOG_STEP (1u << 9)
#define POSIX_LOG_COMM (1u << 10)
#define POSIX_LOG_ASYNC (1u << 11)
#define POSIX_LOG_PTRACE (1u << 12)
#define POSIX_LOG_REGISTERS (1u << 13)
#define POSIX_LOG_ALL (UINT32_MAX)
#define POSIX_LOG_DEFAULT POSIX_LOG_PACKETS

// The size which determines "short memory reads/writes".
#define POSIX_LOG_MEMORY_SHORT_BYTES (4 * sizeof(ptrdiff_t))

class ProcessPOSIXLog {
  static const char *m_pluginname;

public:
  // ---------------------------------------------------------------------
  // Public Static Methods
  // ---------------------------------------------------------------------
  static void Initialize(lldb_private::ConstString name);

  static void RegisterPluginName(const char *pluginName) {
    m_pluginname = pluginName;
  }

  static void RegisterPluginName(lldb_private::ConstString pluginName) {
    m_pluginname = pluginName.GetCString();
  }

  static lldb_private::Log *GetLogIfAllCategoriesSet(uint32_t mask = 0);

  static void DisableLog(const char **args,
                         lldb_private::Stream *feedback_strm);

  static lldb_private::Log *
  EnableLog(const std::shared_ptr<llvm::raw_ostream> &log_stream_sp,
            uint32_t log_options, const char **args,
            lldb_private::Stream *feedback_strm);

  static void ListLogCategories(lldb_private::Stream *strm);
};

#endif // liblldb_ProcessPOSIXLog_h_
