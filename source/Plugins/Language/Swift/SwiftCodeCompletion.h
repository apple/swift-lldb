//===--- SwiftCodeCompletion.h - Code Completion for Swift ------*- C++ -*-===//
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

#ifndef SwiftCodeCompletion_h_
#define SwiftCodeCompletion_h_

#include "lldb/Target/CompletionResponse.h"
#include "lldb/Target/Target.h"

#include "llvm/ADT/StringRef.h"

#include "swift/AST/Module.h"

namespace lldb_private {

CompletionResponse
SwiftCompleteCode(SwiftASTContext &SwiftCtx,
                  SwiftPersistentExpressionState &PersistentExpressionState,
                  llvm::StringRef EnteredCode);

} // namespace lldb_private

#endif // SwiftCodeCompletion_h_
