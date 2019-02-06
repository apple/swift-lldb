//===-- SBCompletionResponse.cpp --------------------------------*- C++ -*-===//
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

#include "lldb/API/SBCompletionResponse.h"
#include "lldb/API/SBCompletionMatch.h"

using namespace lldb;
using namespace lldb_private;

SBCompletionResponse::SBCompletionResponse()
    : m_opaque_ap(new CompletionResponse()) {}

SBCompletionResponse::SBCompletionResponse(const SBCompletionResponse &rhs)
    : m_opaque_ap(new CompletionResponse(*rhs.m_opaque_ap)) {}

SBCompletionResponse::SBCompletionResponse(const CompletionResponse *response)
    : m_opaque_ap(new CompletionResponse(*response)) {}

const SBCompletionResponse &SBCompletionResponse::
operator=(const SBCompletionResponse &rhs) {
  m_opaque_ap.reset(new CompletionResponse(*rhs.m_opaque_ap));
  return *this;
}

const char *SBCompletionResponse::GetErrorMessage() const {
  return m_opaque_ap->ErrorMessage.c_str();
}

const char *SBCompletionResponse::GetPrefix() const {
  return m_opaque_ap->Prefix.c_str();
}

uint32_t SBCompletionResponse::GetNumMatches() const {
  return m_opaque_ap->Matches.size();
}

SBCompletionMatch SBCompletionResponse::GetMatchAtIndex(size_t idx) const {
  return SBCompletionMatch(&m_opaque_ap->Matches[idx]);
}
