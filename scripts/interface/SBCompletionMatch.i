//===-- SWIG Interface for SBCompletionMatch --------------------*- C++ -*-===//
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

namespace lldb {

%feature("docstring", "
Represents a single possible completion.
") SBCompletionMatch;
class SBCompletionMatch
{
public:
    %feature("docstring", "
    Returns a detailed string describing this completion that is suitable for
    displaying to a user in a list of completion possibilities. This string
    is not suitable for inserting into the code because it may contain
    information about the match (like type information) that doesn't go in
    code.
    ") GetDisplay;
    const char *
    GetDisplay () const;

    %feature("docstring", "
    Returns a string that can be inserted into the code.
    ") GetInsertable;
    const char *
    GetInsertable () const;
};

} // namespace lldb
