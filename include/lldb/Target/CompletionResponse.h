//===--- CompletionResponse.h -----------------------------------*- C++ -*-===//
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

#ifndef CompletionResponse_h_
#define CompletionResponse_h_

#include "lldb/Target/CompletionMatch.h"

#include <string>
#include <vector>

namespace lldb_private {

struct CompletionResponse {
  std::string ErrorMessage;
  std::string Prefix;
  std::vector<CompletionMatch> Matches;

  static CompletionResponse error(const std::string &ErrorMessage) {
    CompletionResponse Response;
    Response.ErrorMessage = ErrorMessage;
    return Response;
  }
};

} // namespace lldb_private

#endif // CompletionResponse_h_
