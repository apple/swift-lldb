//===-- CPlusPlusLanguage.h -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_CPlusPlusLanguage_h_
#define liblldb_CPlusPlusLanguage_h_

// C Includes
// C++ Includes
#include <set>
#include <vector>

// Other libraries and framework includes
#include "llvm/ADT/StringRef.h"

// Project includes
#include "lldb/Target/Language.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/lldb-private.h"

namespace lldb_private {

class CPlusPlusLanguage : public Language {
public:
  class MethodName {
  public:
    enum Type {
      eTypeInvalid,
      eTypeUnknownMethod,
      eTypeClassMethod,
      eTypeInstanceMethod
    };

    MethodName()
        : m_full(), m_basename(), m_context(), m_arguments(), m_qualifiers(),
          m_type(eTypeInvalid), m_parsed(false), m_parse_error(false) {}

    MethodName(const ConstString &s)
        : m_full(s), m_basename(), m_context(), m_arguments(), m_qualifiers(),
          m_type(eTypeInvalid), m_parsed(false), m_parse_error(false) {}

    void Clear();

    bool IsValid() {
      if (!m_parsed)
        Parse();
      if (m_parse_error)
        return false;
      if (m_type == eTypeInvalid)
        return false;
      return (bool)m_full;
    }

    Type GetType() const { return m_type; }

    const ConstString &GetFullName() const { return m_full; }

    std::string GetScopeQualifiedName();

    llvm::StringRef GetBasename();

    llvm::StringRef GetContext();

    llvm::StringRef GetArguments();

    llvm::StringRef GetQualifiers();

  protected:
    void Parse();

    ConstString m_full; // Full name:
                        // "lldb::SBTarget::GetBreakpointAtIndex(unsigned int)
                        // const"
    llvm::StringRef m_basename;   // Basename:     "GetBreakpointAtIndex"
    llvm::StringRef m_context;    // Decl context: "lldb::SBTarget"
    llvm::StringRef m_arguments;  // Arguments:    "(unsigned int)"
    llvm::StringRef m_qualifiers; // Qualifiers:   "const"
    Type m_type;
    bool m_parsed;
    bool m_parse_error;
  };

  CPlusPlusLanguage() = default;

  ~CPlusPlusLanguage() override = default;

  lldb::LanguageType GetLanguageType() const override {
    return lldb::eLanguageTypeC_plus_plus;
  }

  std::unique_ptr<TypeScavenger> GetTypeScavenger() override;
  lldb::TypeCategoryImplSP GetFormatters() override;

  HardcodedFormatters::HardcodedSummaryFinder GetHardcodedSummaries() override;

  HardcodedFormatters::HardcodedSyntheticFinder
  GetHardcodedSynthetics() override;

  //------------------------------------------------------------------
  // Static Functions
  //------------------------------------------------------------------
  static void Initialize();

  static void Terminate();

  static lldb_private::Language *CreateInstance(lldb::LanguageType language);

  static lldb_private::ConstString GetPluginNameStatic();

  static bool IsCPPMangledName(const char *name);

  // Extract C++ context and identifier from a string using heuristic matching
  // (as opposed to
  // CPlusPlusLanguage::MethodName which has to have a fully qualified C++ name
  // with parens and arguments.
  // If the name is a lone C identifier (e.g. C) or a qualified C identifier
  // (e.g. A::B::C) it will return true,
  // and identifier will be the identifier (C and C respectively) and the
  // context will be "" and "A::B::" respectively.
  // If the name fails the heuristic matching for a qualified or unqualified
  // C/C++ identifier, then it will return false
  // and identifier and context will be unchanged.

  static bool ExtractContextAndIdentifier(const char *name,
                                          llvm::StringRef &context,
                                          llvm::StringRef &identifier);

  // in some cases, compilers will output different names for one same type.
  // when that happens, it might be impossible
  // to construct SBType objects for a valid type, because the name that is
  // available is not the same as the name that
  // can be used as a search key in FindTypes(). the equivalents map here is
  // meant to return possible alternative names
  // for a type through which a search can be conducted. Currently, this is only
  // enabled for C++ but can be extended
  // to ObjC or other languages if necessary
  static uint32_t FindEquivalentNames(ConstString type_name,
                                      std::vector<ConstString> &equivalents);

  // Given a mangled function name, calculates some alternative manglings since
  // the compiler mangling may not line up with the symbol we are expecting
  static uint32_t
  FindAlternateFunctionManglings(const ConstString mangled,
                                 std::set<ConstString> &candidates);

  //------------------------------------------------------------------
  // PluginInterface protocol
  //------------------------------------------------------------------
  ConstString GetPluginName() override;

  uint32_t GetPluginVersion() override;
};

} // namespace lldb_private

#endif // liblldb_CPlusPlusLanguage_h_
