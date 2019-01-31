//===-- SBCompletionResponse.h ----------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_SBCompletionResponse_h_
#define LLDB_SBCompletionResponse_h_

#include "lldb/API/SBDefines.h"
#include "lldb/Target/CompletionResponse.h"

namespace lldb {

class LLDB_API SBCompletionResponse {
public:
  SBCompletionResponse();

  SBCompletionResponse(const SBCompletionResponse &rhs);

  const SBCompletionResponse &operator=(const SBCompletionResponse &rhs);

  const char *GetErrorMessage() const;

  const char *GetPrefix() const;

  uint32_t GetNumMatches() const;

  SBCompletionMatch GetMatchAtIndex(size_t idx) const;

protected:
  friend class SBTarget;

  SBCompletionResponse(const lldb_private::CompletionResponse *response);

private:
  std::unique_ptr<lldb_private::CompletionResponse> m_opaque_ap;
};

} // namespace lldb

#endif // LLDB_SBCompletionResponse_h_
