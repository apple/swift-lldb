//===-- SBCompletionOptions.cpp ---------------------------------*- C++ -*-===//
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

#include "lldb/API/SBCompletionOptions.h"

using namespace lldb;
using namespace lldb_private;

SBCompletionOptions::SBCompletionOptions()
    : m_opaque_ap(new lldb_private::CompletionOptions()) {}

SBCompletionOptions::SBCompletionOptions(const SBCompletionOptions &rhs)
    : m_opaque_ap(new lldb_private::CompletionOptions(*rhs.m_opaque_ap)) {}

const SBCompletionOptions &SBCompletionOptions::
operator=(const SBCompletionOptions &rhs) {
  m_opaque_ap.reset(new lldb_private::CompletionOptions(*rhs.m_opaque_ap));
  return *this;
}

lldb::LanguageType SBCompletionOptions::GetLanguage() const {
  return m_opaque_ap->Language;
}

void SBCompletionOptions::SetLanguage(lldb::LanguageType Language) {
  m_opaque_ap->Language = Language;
}

bool SBCompletionOptions::GetInsertableLSPSnippets() const {
  return m_opaque_ap->InsertableLSPSnippets;
}

void SBCompletionOptions::SetInsertableLSPSnippets(bool Value) {
  m_opaque_ap->InsertableLSPSnippets = Value;
}

SBCompletionOptions::SBCompletionOptions(
    const lldb_private::CompletionOptions *options)
    : m_opaque_ap(new lldb_private::CompletionOptions(*options)) {}


lldb_private::CompletionOptions *SBCompletionOptions::GetPointer() const {
  return m_opaque_ap.get();
}
