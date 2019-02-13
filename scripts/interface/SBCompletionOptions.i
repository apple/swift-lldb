//===-- SWIG Interface for SBCompletionOptions ------------------*- C++ -*-===//
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
Options for a completion request
") SBCompletionOptions;
class SBCompletionOptions
{
public:
    %feature("docstring", "The language used for completions.")
    GetLanguage;
    lldb::LanguageType GetLanguage () const;

    %feature("docstring", "Set the language used for completions.")
    SetLanguage;
    void SetLanguage (lldb::LanguageType Language);

    %feature("docstring", "
    When this is false, the insertable text in completion matches is plain text
    that can be inserted directly into the code. When this is true, the
    insertable text in completion matches is a Language Server Protocol Snippet
    (https://microsoft.github.io/language-server-protocol/specification#textDocument_completion),
    which includes things like placeholder tabstops for function arguments.
    ") GetInsertableLSPSnippets;
    bool GetInsertableLSPSnippets () const;

    %feature("docstring", "
    Set whether the insertable text in completion matches is a Language Server
    Protocol Snippet. See the getter docstring for more information.
    ") SetInsertableLSPSnippets;
    void SetInsertableLSPSnippets (bool Value);
};

} // namespace lldb
