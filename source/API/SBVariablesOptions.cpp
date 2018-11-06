//===-- SBVariablesOptions.cpp --------------------------------------*- C++
//-*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBVariablesOptions.h"

using namespace lldb;
using namespace lldb_private;

class VariablesOptionsImpl {
public:
  VariablesOptionsImpl()
      : m_include_arguments(false), m_include_recognized_arguments(false),
        m_include_locals(false), m_include_statics(false),
        m_in_scope_only(false), m_include_runtime_support_values(false),
        m_use_dynamic(lldb::eNoDynamicValues) {}

  VariablesOptionsImpl(const VariablesOptionsImpl &) = default;

  ~VariablesOptionsImpl() = default;

  VariablesOptionsImpl &operator=(const VariablesOptionsImpl &) = default;

  bool GetIncludeArguments() const { return m_include_arguments; }

  void SetIncludeArguments(bool b) { m_include_arguments = b; }

  bool GetIncludeRecognizedArguments() const {
    return m_include_recognized_arguments;
  }

  void SetIncludeRecognizedArguments(bool b) {
    m_include_recognized_arguments = b;
  }

  bool GetIncludeLocals() const { return m_include_locals; }

  void SetIncludeLocals(bool b) { m_include_locals = b; }

  bool GetIncludeStatics() const { return m_include_statics; }

  void SetIncludeStatics(bool b) { m_include_statics = b; }

  bool GetInScopeOnly() const { return m_in_scope_only; }

  void SetInScopeOnly(bool b) { m_in_scope_only = b; }

  bool GetIncludeRuntimeSupportValues() const {
    return m_include_runtime_support_values;
  }

  void SetIncludeRuntimeSupportValues(bool b) {
    m_include_runtime_support_values = b;
  }

  lldb::DynamicValueType GetUseDynamic() const { return m_use_dynamic; }

  void SetUseDynamic(lldb::DynamicValueType d) { m_use_dynamic = d; }

private:
  bool m_include_arguments : 1;
  bool m_include_recognized_arguments : 1;
  bool m_include_locals : 1;
  bool m_include_statics : 1;
  bool m_in_scope_only : 1;
  bool m_include_runtime_support_values : 1;
  lldb::DynamicValueType m_use_dynamic;
};

SBVariablesOptions::SBVariablesOptions()
    : m_opaque_ap(new VariablesOptionsImpl()) {}

SBVariablesOptions::SBVariablesOptions(const SBVariablesOptions &options)
    : m_opaque_ap(new VariablesOptionsImpl(options.ref())) {}

SBVariablesOptions &SBVariablesOptions::
operator=(const SBVariablesOptions &options) {
  m_opaque_ap.reset(new VariablesOptionsImpl(options.ref()));
  return *this;
}

SBVariablesOptions::~SBVariablesOptions() = default;

bool SBVariablesOptions::IsValid() const {
  return m_opaque_ap.get() != nullptr;
}

bool SBVariablesOptions::GetIncludeArguments() const {
  return m_opaque_ap->GetIncludeArguments();
}

void SBVariablesOptions::SetIncludeArguments(bool arguments) {
  m_opaque_ap->SetIncludeArguments(arguments);
}

bool SBVariablesOptions::GetIncludeRecognizedArguments() const {
  return m_opaque_ap->GetIncludeRecognizedArguments();
}

void SBVariablesOptions::SetIncludeRecognizedArguments(bool arguments) {
  m_opaque_ap->SetIncludeRecognizedArguments(arguments);
}

bool SBVariablesOptions::GetIncludeLocals() const {
  return m_opaque_ap->GetIncludeLocals();
}

void SBVariablesOptions::SetIncludeLocals(bool locals) {
  m_opaque_ap->SetIncludeLocals(locals);
}

bool SBVariablesOptions::GetIncludeStatics() const {
  return m_opaque_ap->GetIncludeStatics();
}

void SBVariablesOptions::SetIncludeStatics(bool statics) {
  m_opaque_ap->SetIncludeStatics(statics);
}

bool SBVariablesOptions::GetInScopeOnly() const {
  return m_opaque_ap->GetInScopeOnly();
}

void SBVariablesOptions::SetInScopeOnly(bool in_scope_only) {
  m_opaque_ap->SetInScopeOnly(in_scope_only);
}

bool SBVariablesOptions::GetIncludeRuntimeSupportValues() const {
  return m_opaque_ap->GetIncludeRuntimeSupportValues();
}

void SBVariablesOptions::SetIncludeRuntimeSupportValues(
    bool runtime_support_values) {
  m_opaque_ap->SetIncludeRuntimeSupportValues(runtime_support_values);
}

lldb::DynamicValueType SBVariablesOptions::GetUseDynamic() const {
  return m_opaque_ap->GetUseDynamic();
}

void SBVariablesOptions::SetUseDynamic(lldb::DynamicValueType dynamic) {
  m_opaque_ap->SetUseDynamic(dynamic);
}

VariablesOptionsImpl *SBVariablesOptions::operator->() {
  return m_opaque_ap.operator->();
}

const VariablesOptionsImpl *SBVariablesOptions::operator->() const {
  return m_opaque_ap.operator->();
}

VariablesOptionsImpl *SBVariablesOptions::get() { return m_opaque_ap.get(); }

VariablesOptionsImpl &SBVariablesOptions::ref() { return *m_opaque_ap; }

const VariablesOptionsImpl &SBVariablesOptions::ref() const {
  return *m_opaque_ap;
}

SBVariablesOptions::SBVariablesOptions(VariablesOptionsImpl *lldb_object_ptr)
    : m_opaque_ap(std::move(lldb_object_ptr)) {}

void SBVariablesOptions::SetOptions(VariablesOptionsImpl *lldb_object_ptr) {
  m_opaque_ap.reset(std::move(lldb_object_ptr));
}
