//===-- SBCompletionMatch.h -------------------------------------*- C++ -*-===//
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

#ifndef LLDB_SBCompletionMatch_h_
#define LLDB_SBCompletionMatch_h_

#include "lldb/API/SBDefines.h"
#include "lldb/Target/CompletionMatch.h"

namespace lldb {

class LLDB_API SBCompletionMatch {
public:
  SBCompletionMatch();

  SBCompletionMatch(const SBCompletionMatch &rhs);

  const SBCompletionMatch &operator=(const SBCompletionMatch &rhs);

  const char *GetDisplay() const;

  const char *GetInsertable() const;

protected:
  friend class SBCompletionResponse;

  SBCompletionMatch(const lldb_private::CompletionMatch *response);

private:
  std::unique_ptr<lldb_private::CompletionMatch> m_opaque_ap;
};

} // namespace lldb

#endif // LLDB_SBCompletionMatch_h_
