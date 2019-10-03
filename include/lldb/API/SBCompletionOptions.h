//===-- SBCompletionOptions.h -----------------------------------*- C++ -*-===//
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

#ifndef LLDB_SBCompletionOptions_h_
#define LLDB_SBCompletionOptions_h_

#include "lldb/API/SBDefines.h"
#include "lldb/Target/CompletionOptions.h"

namespace lldb {

class LLDB_API SBCompletionOptions {
public:
  SBCompletionOptions();

  SBCompletionOptions(const SBCompletionOptions &rhs);

  const SBCompletionOptions &operator=(const SBCompletionOptions &rhs);

  lldb::LanguageType GetLanguage() const;
  void SetLanguage(lldb::LanguageType Language);

  bool GetInsertableLSPSnippets() const;
  void SetInsertableLSPSnippets(bool Value);

protected:
  friend class SBTarget;

  SBCompletionOptions(const lldb_private::CompletionOptions *options);

  lldb_private::CompletionOptions *GetPointer() const;

private:
  std::unique_ptr<lldb_private::CompletionOptions> m_opaque_ap;
};

} // namespace lldb

#endif // LLDB_SBCompletionOptions_h_
