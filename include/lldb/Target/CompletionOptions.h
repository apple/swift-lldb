//===--- CompletionOptions.h ------------------------------------*- C++ -*-===//
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

#ifndef CompletionOptions_h_
#define CompletionOptions_h_

namespace lldb_private {

struct CompletionOptions {
  lldb::LanguageType Language;
  bool InsertableLSPSnippets = false;
};

} // namespace lldb_private

#endif // CompletionOptions_h_
