//===-- SBCompletionMatch.cpp -----------------------------------*- C++ -*-===//
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

#include "lldb/API/SBCompletionMatch.h"

using namespace lldb;
using namespace lldb_private;

SBCompletionMatch::SBCompletionMatch()
    : m_opaque_ap(new lldb_private::CompletionMatch()) {}

SBCompletionMatch::SBCompletionMatch(const SBCompletionMatch &rhs)
    : m_opaque_ap(new lldb_private::CompletionMatch(*rhs.m_opaque_ap)) {}

const SBCompletionMatch &SBCompletionMatch::
operator=(const SBCompletionMatch &rhs) {
  m_opaque_ap.reset(new lldb_private::CompletionMatch(*rhs.m_opaque_ap));
  return *this;
}

const char *SBCompletionMatch::GetDisplay() const {
  return m_opaque_ap->Display.c_str();
}

const char *SBCompletionMatch::GetInsertable() const {
  return m_opaque_ap->Insertable.c_str();
}

SBCompletionMatch::SBCompletionMatch(
    const lldb_private::CompletionMatch *response)
    : m_opaque_ap(new lldb_private::CompletionMatch(*response)) {}
