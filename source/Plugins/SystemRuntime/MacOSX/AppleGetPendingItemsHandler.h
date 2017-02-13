//===-- AppleGetPendingItemsHandler.h ----------------------------*- C++
//-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef lldb_AppleGetPendingItemsHandler_h_
#define lldb_AppleGetPendingItemsHandler_h_

// C Includes
// C++ Includes
#include <map>
#include <mutex>
#include <vector>

// Other libraries and framework includes
// Project includes
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Utility/Error.h"
#include "lldb/lldb-public.h"

// This class will insert a UtilityFunction into the inferior process for
// calling libBacktraceRecording's
// __introspection_dispatch_queue_get_pending_items()
// function.  The function in the inferior will return a struct by value
// with these members:
//
//     struct get_pending_items_return_values
//     {
//         introspection_dispatch_item_info_ref *items_buffer;
//         uint64_t items_buffer_size;
//         uint64_t count;
//     };
//
// The items_buffer pointer is an address in the inferior program's address
// space (items_buffer_size in size) which must be mach_vm_deallocate'd by
// lldb.  count is the number of items that were stored in the buffer.
//
// The AppleGetPendingItemsHandler object should persist so that the
// UtilityFunction
// can be reused multiple times.

namespace lldb_private {

class AppleGetPendingItemsHandler {
public:
  AppleGetPendingItemsHandler(lldb_private::Process *process);

  ~AppleGetPendingItemsHandler();

  struct GetPendingItemsReturnInfo {
    lldb::addr_t items_buffer_ptr; /* the address of the pending items buffer
                                      from libBacktraceRecording */
    lldb::addr_t
        items_buffer_size; /* the size of the pending items buffer from
                              libBacktraceRecording */
    uint64_t count; /* the number of pending items included in the buffer */

    GetPendingItemsReturnInfo()
        : items_buffer_ptr(LLDB_INVALID_ADDRESS), items_buffer_size(0),
          count(0) {}
  };

  //----------------------------------------------------------
  /// Get the list of pending items for a given queue via a call to
  /// __introspection_dispatch_queue_get_pending_items.  If there's a page of
  /// memory that needs to be freed, pass in the address and size and it will
  /// be freed before getting the list of queues.
  ///
  /// @param [in] thread
  ///     The thread to run this plan on.
  ///
  /// @param [in] queue
  ///     The dispatch_queue_t value for the queue of interest.
  ///
  /// @param [in] page_to_free
  ///     An address of an inferior process vm page that needs to be
  ///     deallocated,
  ///     LLDB_INVALID_ADDRESS if this is not needed.
  ///
  /// @param [in] page_to_free_size
  ///     The size of the vm page that needs to be deallocated if an address was
  ///     passed in to page_to_free.
  ///
  /// @param [out] error
  ///     This object will be updated with the error status / error string from
  ///     any failures encountered.
  ///
  /// @returns
  ///     The result of the inferior function call execution.  If there was a
  ///     failure of any kind while getting
  ///     the information, the items_buffer_ptr value will be
  ///     LLDB_INVALID_ADDRESS.
  //----------------------------------------------------------
  GetPendingItemsReturnInfo GetPendingItems(Thread &thread, lldb::addr_t queue,
                                            lldb::addr_t page_to_free,
                                            uint64_t page_to_free_size,
                                            lldb_private::Error &error);

  void Detach();

private:
  lldb::addr_t
  SetupGetPendingItemsFunction(Thread &thread,
                               ValueList &get_pending_items_arglist);

  static const char *g_get_pending_items_function_name;
  static const char *g_get_pending_items_function_code;

  lldb_private::Process *m_process;
  std::unique_ptr<UtilityFunction> m_get_pending_items_impl_code;
  std::mutex m_get_pending_items_function_mutex;

  lldb::addr_t m_get_pending_items_return_buffer_addr;
  std::mutex m_get_pending_items_retbuffer_mutex;
};

} // using namespace lldb_private

#endif // lldb_AppleGetPendingItemsHandler_h_
