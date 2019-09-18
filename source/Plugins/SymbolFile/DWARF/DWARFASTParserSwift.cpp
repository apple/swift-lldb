//===-- DWARFASTParserSwift.cpp ---------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "DWARFASTParserSwift.h"

#include "DWARFASTParserClang.h"
#include "DWARFCompileUnit.h"
#include "DWARFDIE.h"
#include "DWARFDebugInfo.h"
#include "DWARFDefines.h"
#include "SymbolFileDWARF.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/Demangling/Demangle.h"

#include "clang/AST/DeclObjC.h"

#include "lldb/Core/Module.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/Function.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SwiftASTContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/TypeMap.h"
#include "lldb/Target/SwiftLanguageRuntime.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Status.h"

using namespace lldb;
using namespace lldb_private;

DWARFASTParserSwift::DWARFASTParserSwift(SwiftASTContext &ast) : m_ast(ast) {}

DWARFASTParserSwift::~DWARFASTParserSwift() {}

static llvm::StringRef GetTypedefName(const DWARFDIE &die) {
  if (die.Tag() != DW_TAG_typedef)
    return {};
  DWARFDIE type_die = die.GetAttributeValueAsReferenceDIE(DW_AT_type);
  if (!type_die.IsValid())
    return {};
  return llvm::StringRef::withNullAsEmpty(type_die.GetName());
}

lldb::TypeSP DWARFASTParserSwift::ParseTypeFromDWARF(const SymbolContext &sc,
                                                     const DWARFDIE &die,
                                                     Log *log,
                                                     bool *type_is_new_ptr) {
  lldb::TypeSP type_sp;
  CompilerType compiler_type;
  Status error;

  Declaration decl;
  ConstString mangled_name;
  ConstString name;
  bool is_clang_type = false;
  llvm::Optional<uint64_t> dwarf_byte_size;

  DWARFAttributes attributes;
  const size_t num_attributes = die.GetAttributes(attributes);
  DWARFFormValue type_attr;

  if (num_attributes > 0) {
    uint32_t i;
    for (i = 0; i < num_attributes; ++i) {
      const dw_attr_t attr = attributes.AttributeAtIndex(i);
      DWARFFormValue form_value;
      if (attributes.ExtractFormValueAtIndex(i, form_value)) {
        switch (attr) {
        case DW_AT_decl_file:
          decl.SetFile(sc.comp_unit->GetSupportFiles().GetFileSpecAtIndex(
              form_value.Unsigned()));
          break;
        case DW_AT_decl_line:
          decl.SetLine(form_value.Unsigned());
          break;
        case DW_AT_decl_column:
          decl.SetColumn(form_value.Unsigned());
          break;
        case DW_AT_name:
          name.SetCString(form_value.AsCString());
          break;
        case DW_AT_linkage_name:
        case DW_AT_MIPS_linkage_name:
          mangled_name.SetCString(form_value.AsCString());
          break;
        case DW_AT_byte_size:
          dwarf_byte_size = form_value.Unsigned();
          break;
        default:
          break;
        }
      }
    }
  }

  if (!mangled_name && name) {
    if (name.GetStringRef().equals("$swift.fixedbuffer")) {
      DWARFDIE type_die =
          die.GetFirstChild().GetAttributeValueAsReferenceDIE(DW_AT_type);
      if (auto wrapped_type =
          ParseTypeFromDWARF(sc, type_die, log, type_is_new_ptr)) {
        // Create a unique pointer for the type + fixed buffer flag.
        type_sp.reset(new Type(*wrapped_type));
        type_sp->SetSwiftFixedValueBuffer(true);
        return type_sp;
      }
    }
    if (SwiftLanguageRuntime::IsSwiftMangledName(name.GetCString()))
      mangled_name = name;
  }

  if (mangled_name) {
    type_sp = m_ast.GetCachedType(mangled_name);
    if (type_sp)
      return type_sp;

    // Because of DWARFImporter, we may search for this type again while
    // resolving the mangled name.
    die.GetDWARF()->GetDIEToType()[die.GetDIE()] = DIE_IS_BEING_PARSED;

    // Try to import the type from one of the loaded Swift modules.
    compiler_type = m_ast.GetTypeFromMangledTypename(mangled_name, error);
  }

  ConstString preferred_name;
  if (!compiler_type &&
      swift::Demangle::isObjCSymbol(mangled_name.GetStringRef())) {
    // When we failed to look up the type because no .swiftmodule is
    // present or it couldn't be read, fall back to presenting objects
    // that look like they might be come from Objective-C (or C) as
    // Clang types. LLDB's Objective-C part is very robust against
    // malformed object pointers, so this isn't very risky.
    auto type_system_or_err = sc.module_sp->GetTypeSystemForLanguage(eLanguageTypeObjC);
    if (!type_system_or_err) {
      llvm::consumeError(type_system_or_err.takeError());
      return nullptr;
    }

    if (auto *clang_ctx = llvm::dyn_cast_or_null<ClangASTContext>(&*type_system_or_err)) {
      DWARFASTParserClang *clang_ast_parser =
          static_cast<DWARFASTParserClang *>(clang_ctx->GetDWARFParser());
      TypeMap clang_types;
      GetClangType(die, mangled_name.GetStringRef(), clang_types);

      // Import the Clang type into the Clang context.
      if (!compiler_type && clang_types.GetSize())
        if (TypeSP clang_type_sp = clang_types.GetTypeAtIndex(0))
          if (clang_type_sp) {
            is_clang_type = true;
            compiler_type = clang_ast_parser->GetClangASTImporter().CopyType(
                *clang_ctx, clang_type_sp->GetForwardCompilerType());
            // Swift doesn't know pointers. Convert top-level
            // Objective-C object types to object pointers for Clang.
            auto clang_type = clang::QualType::getFromOpaquePtr(
                compiler_type.GetOpaqueQualType());
            if (clang_type->isObjCObjectOrInterfaceType())
              compiler_type = compiler_type.GetPointerType();
          }

      // Fall back to (id), which is not necessarily correct.
      if (!compiler_type) {
        is_clang_type = true;
        compiler_type = clang_ctx->GetBasicType(eBasicTypeObjCID);
        // Stash away the mangled name for resolving it through
        // the Objective-C runtime later.
        preferred_name = mangled_name;
      }
    }
  }

  if (!compiler_type && name) {
    // Handle Archetypes, which are typedefs to RawPointerType.
    if (GetTypedefName(die).startswith("$sBp")) {
      swift::ASTContext *swift_ast_ctx = m_ast.GetASTContext();
      if (!swift_ast_ctx) {
        if (log)
          log->Printf("Empty Swift AST context while looking up %s.",
                      name.AsCString());
        return {};
      }
      preferred_name = name;
      compiler_type =
          SwiftASTContext::GetCompilerType(swift_ast_ctx->TheRawPointerType);
    }
  }

  switch (die.Tag()) {
  case DW_TAG_inlined_subroutine:
  case DW_TAG_subprogram:
  case DW_TAG_subroutine_type:
    if (!compiler_type || !compiler_type.IsFunctionType()) {
      // Make sure we at least have some function type. The mangling for
      // the "top_level_code" is returning the empty tuple type "()",
      // which is not a function type.
      compiler_type = m_ast.GetVoidFunctionType();
    }
    break;
  default:
    break;
  }

  if (compiler_type) {
    type_sp = TypeSP(new Type(
        die.GetID(), die.GetDWARF(),
        preferred_name ? preferred_name : compiler_type.GetTypeName(),
        is_clang_type ? dwarf_byte_size
                      : compiler_type.GetByteSize(nullptr),
        NULL, LLDB_INVALID_UID, Type::eEncodingIsUID, &decl, compiler_type,
        is_clang_type ? Type::eResolveStateForward : Type::eResolveStateFull));
    // FIXME: This ought to work lazily, too.
    if (is_clang_type)
      type_sp->GetFullCompilerType();
  }

  // Cache this type.
  if (type_sp && mangled_name &&
      SwiftLanguageRuntime::IsSwiftMangledName(mangled_name.GetCString()))
    m_ast.SetCachedType(mangled_name, type_sp);
  die.GetDWARF()->GetDIEToType()[die.GetDIE()] = type_sp.get();

  return type_sp;
}

void DWARFASTParserSwift::GetClangType(const DWARFDIE &die,
                                       llvm::StringRef mangled_name,
                                       TypeMap &clang_types) const {
  llvm::SmallVector<CompilerContext, 4> decl_context;
  die.GetDeclContext(decl_context);
  if (!decl_context.size())
    return;

  // Typedefs don't have a DW_AT_linkage_name, so their DW_AT_name is the
  // mangled. Get the unmangled name.
  auto fixup_typedef = [&mangled_name, &decl_context]() {
    using namespace swift::Demangle;
    Context Ctx;
    NodePointer node = Ctx.demangleSymbolAsNode(mangled_name);
    if (!node || node->getNumChildren() != 1 ||
        node->getKind() != Node::Kind::Global)
      return;
    node = node->getFirstChild();
    if (node->getNumChildren() != 1 ||
        node->getKind() != Node::Kind::TypeMangling)
      return;
    node = node->getFirstChild();
    if (node->getNumChildren() != 1 || node->getKind() != Node::Kind::Type)
      return;
    node = node->getFirstChild();
    if (node->getKind() != Node::Kind::TypeAlias)
      return;
    for (NodePointer child : *node)
      if (child->getKind() == Node::Kind::Identifier && child->hasText()) {
        decl_context.back().kind = CompilerContextKind::Typedef;
        decl_context.back().name = ConstString(child->getText());
        return;
      }
  };
  fixup_typedef();

  auto &sym_file = die.GetCU()->GetSymbolFileDWARF();
  sym_file.UpdateExternalModuleListIfNeeded();

  // The Swift projection of all Clang type is a struct; search every kind.
  decl_context.back().kind = CompilerContextKind::AnyType;
  LanguageSet clang_languages = ClangASTContext::GetSupportedLanguagesForTypes();
  // Search any modules referenced by DWARF.
  for (const auto &name_module : sym_file.getExternalTypeModules()) {
    if (!name_module.second)
      continue;
    if (name_module.second->GetSymbolFile()->FindTypes(
            decl_context, clang_languages, true, clang_types))
      return;
  }

  // Next search the .dSYM the DIE came from, if applicable.
  if (sym_file.FindTypes(decl_context, clang_languages, true, clang_types))
    return;
}

Function *DWARFASTParserSwift::ParseFunctionFromDWARF(
    lldb_private::CompileUnit &comp_unit, const DWARFDIE &die) {
  DWARFRangeList func_ranges;
  const char *name = NULL;
  const char *mangled = NULL;
  int decl_file = 0;
  int decl_line = 0;
  int decl_column = 0;
  int call_file = 0;
  int call_line = 0;
  int call_column = 0;
  DWARFExpression frame_base;

  if (die.Tag() != DW_TAG_subprogram)
    return NULL;

  if (die.GetDIENamesAndRanges(name, mangled, func_ranges, decl_file, decl_line,
                               decl_column, call_file, call_line, call_column,
                               &frame_base)) {
    // Union of all ranges in the function DIE (if the function is
    // discontiguous)
    SymbolFileDWARF *dwarf = die.GetDWARF();
    AddressRange func_range;
    lldb::addr_t lowest_func_addr = func_ranges.GetMinRangeBase(0);
    lldb::addr_t highest_func_addr = func_ranges.GetMaxRangeEnd(0);
    if (lowest_func_addr != LLDB_INVALID_ADDRESS &&
        lowest_func_addr <= highest_func_addr) {
      ModuleSP module_sp(dwarf->GetObjectFile()->GetModule());
      func_range.GetBaseAddress().ResolveAddressUsingFileSections(
          lowest_func_addr, module_sp->GetSectionList());
      if (func_range.GetBaseAddress().IsValid())
        func_range.SetByteSize(highest_func_addr - lowest_func_addr);
    }

    if (func_range.GetBaseAddress().IsValid()) {
      Mangled func_name;
      if (mangled)
        func_name.SetValue(ConstString(mangled), true);
      else
        func_name.SetValue(ConstString(name), false);

      // See if this function can throw.  We can't get that from the
      // mangled name (even though the information is often there)
      // because Swift reserves the right to omit it from the name
      // if it doesn't need it.  So instead we look for the
      // DW_TAG_thrown_type:

      bool can_throw = false;

      DWARFDebugInfoEntry *child(die.GetFirstChild().GetDIE());
      while (child) {
        if (child->Tag() == DW_TAG_thrown_type) {
          can_throw = true;
          break;
        }
        child = child->GetSibling();
      }

      FunctionSP func_sp;
      std::unique_ptr<Declaration> decl_ap;
      if (decl_file != 0 || decl_line != 0 || decl_column != 0)
        decl_ap.reset(new Declaration(
            comp_unit.GetSupportFiles().GetFileSpecAtIndex(decl_file),
            decl_line, decl_column));

      if (dwarf->FixupAddress(func_range.GetBaseAddress())) {
        const user_id_t func_user_id = die.GetID();
        func_sp.reset(new Function(&comp_unit, func_user_id, func_user_id,
                                   func_name, nullptr, func_range,
                                   can_throw)); // first address range

        if (func_sp.get() != NULL) {
          if (frame_base.IsValid())
            func_sp->GetFrameBaseExpression() = frame_base;
          comp_unit.AddFunction(func_sp);
          return func_sp.get();
        }
      }
    }
  }
  return NULL;
}

lldb_private::CompilerDeclContext
DWARFASTParserSwift::GetDeclContextForUIDFromDWARF(const DWARFDIE &die) {
  return CompilerDeclContext();
}

lldb_private::CompilerDeclContext
DWARFASTParserSwift::GetDeclContextContainingUIDFromDWARF(const DWARFDIE &die) {
  return CompilerDeclContext();
}
