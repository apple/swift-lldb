//===-- SWIG Interface for SBCompletionResponse -----------------*- C++ -*-===//
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
Represents a set of completions, or an error describing why completions could
not be calculated."
) SBCompletionResponse;
class SBCompletionResponse
{
public:
    %feature("docstring", "
    Return the error message, if any. The length is 0 if and only if the
    completion calculation was a success.
    ") GetErrorMessage;
    const char *
    GetErrorMessage() const;

    %feature("docstring", "
    Returns a common prefix of all the matches. This prefix is present in the
    code for which the completion was requested.
    ") GetPrefix;
    const char *
    GetPrefix () const;

    %feature("docstring", "
    Returns the number of matches that this response contains.
    ") GetNumMatches;
    uint32_t
    GetNumMatches () const;

    %feature("docstring", "
    Returns the match at `idx`.
    ") SBCompletionMatch;
    lldb::SBCompletionMatch
    GetMatchAtIndex (size_t idx) const;
};

} // namespace lldb
