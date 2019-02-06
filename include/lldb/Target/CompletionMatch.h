//===--- CompletionMatch.h --------------------------------------*- C++ -*-===//
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

#ifndef CompletionMatch_h_
#define CompletionMatch_h_

#include <string>

namespace lldb_private {

struct CompletionMatch {
  std::string Display;
  std::string Insertable;
};

} // namespace lldb_private

#endif // CompletionMatch_h_
