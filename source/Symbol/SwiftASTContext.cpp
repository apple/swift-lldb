//===-- SwiftASTContext.cpp -------------------------------------*- C++ -*-===//
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

#include "lldb/Symbol/SwiftASTContext.h"

// C++ Includes
#include <mutex> // std::once
#include <queue>
#include <set>
#include <sstream>

#include "swift/ABI/MetadataValues.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/DebuggerClient.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SearchPathOptions.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/ASTSectionImporter/ASTSectionImporter.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/PrimarySpecificPaths.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangImporterOptions.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Driver/Util.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDE/Utils.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Driver/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"

#include "swift/../../lib/IRGen/FixedTypeInfo.h"
#include "swift/../../lib/IRGen/GenEnum.h"
#include "swift/../../lib/IRGen/GenHeap.h"
#include "swift/../../lib/IRGen/IRGenModule.h"
#include "swift/../../lib/IRGen/TypeInfo.h"

#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Strings.h"

#include "Plugins/ExpressionParser/Swift/SwiftDiagnostic.h"
#include "Plugins/ExpressionParser/Swift/SwiftUserExpression.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/DumpDataExtractor.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/ThreadSafeDenseMap.h"
#include "lldb/Expression/DiagnosticManager.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/StringConvert.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SwiftLanguageRuntime.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/Utility/CleanUp.h"
#include "lldb/Utility/FileSpec.h"
#include "lldb/Utility/LLDBAssert.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Status.h"

#include "Plugins/Platform/MacOSX/PlatformDarwin.h"
#include "Plugins/SymbolFile/DWARF/DWARFASTParserSwift.h"

#define VALID_OR_RETURN(value)                                                 \
  do {                                                                         \
    if (HasFatalErrors()) {                                                    \
      return (value);                                                          \
    }                                                                          \
  } while (0)
#define VALID_OR_RETURN_VOID()                                                 \
  do {                                                                         \
    if (HasFatalErrors()) {                                                    \
      return;                                                                  \
    }                                                                          \
  } while (0)

using namespace lldb;
using namespace lldb_private;

typedef lldb_private::ThreadSafeDenseMap<swift::ASTContext *, SwiftASTContext *>
    ThreadSafeSwiftASTMap;

static ThreadSafeSwiftASTMap &GetASTMap() {
  // The global destructor list will tear down all of the modules when the LLDB
  // shared library is being unloaded and this needs to live beyond all of those
  // and not be destructed before they have all gone away. So we will leak this
  // list intentionally so we can avoid global destructor problems.
  static ThreadSafeSwiftASTMap *g_map_ptr = NULL;
  static std::once_flag g_once_flag;
  std::call_once(g_once_flag, []() {
    g_map_ptr = new ThreadSafeSwiftASTMap(); // NOTE: Intentional leak
  });
  return *g_map_ptr;
}

static inline swift::Type GetSwiftType(void *opaque_ptr) {
  return swift::Type((swift::TypeBase *)opaque_ptr);
}

static inline swift::CanType GetCanonicalSwiftType(void *opaque_ptr) {
  return ((swift::TypeBase *)opaque_ptr)->getCanonicalType();
}

static inline swift::Type GetSwiftType(CompilerType type) {
  return swift::Type((swift::TypeBase *)type.GetOpaqueQualType());
}

static inline swift::CanType GetCanonicalSwiftType(CompilerType type) {
  return ((swift::TypeBase *)type.GetOpaqueQualType())->getCanonicalType();
}

struct EnumElementInfo {
  CompilerType clang_type;
  lldb_private::ConstString name;
  uint64_t byte_size;
  uint32_t value;       // The value for this enumeration element
  uint32_t extra_value; // If not UINT32_MAX, then this value is an extra value
  // that appears at offset 0 to tell one or more empty
  // enums apart. This value will only be filled in if there
  // are one ore more enum elements that have a non-zero byte size

  EnumElementInfo()
      : clang_type(), name(), byte_size(0), extra_value(UINT32_MAX) {}

  void Dump(Stream &strm) const {
    strm.Printf("<%2" PRIu64 "> %4u", byte_size, value);
    if (extra_value != UINT32_MAX)
      strm.Printf("%4u: ", extra_value);
    else
      strm.Printf("    : ");
    strm.Printf("case %s", name.GetCString());
    if (clang_type)
      strm.Printf("%s", clang_type.GetTypeName().AsCString("<no type name>"));
    strm.EOL();
  }
};

class SwiftEnumDescriptor;

typedef std::shared_ptr<SwiftEnumDescriptor> SwiftEnumDescriptorSP;
typedef llvm::DenseMap<lldb::opaque_compiler_type_t, SwiftEnumDescriptorSP>
    EnumInfoCache;
typedef std::shared_ptr<EnumInfoCache> EnumInfoCacheSP;
typedef llvm::DenseMap<const swift::ASTContext *, EnumInfoCacheSP>
    ASTEnumInfoCacheMap;

static EnumInfoCache *GetEnumInfoCache(const swift::ASTContext *a) {
  static ASTEnumInfoCacheMap g_cache;
  static std::mutex g_mutex;
  std::lock_guard<std::mutex> locker(g_mutex);
  ASTEnumInfoCacheMap::iterator pos = g_cache.find(a);
  if (pos == g_cache.end()) {
    g_cache.insert(
        std::make_pair(a, std::shared_ptr<EnumInfoCache>(new EnumInfoCache())));
    return g_cache.find(a)->second.get();
  }
  return pos->second.get();
}

namespace {
  bool IsDirectory(const FileSpec &spec) {
    return llvm::sys::fs::is_directory(spec.GetPath());
  }
  bool IsRegularFile(const FileSpec &spec) {
    return llvm::sys::fs::is_regular_file(spec.GetPath());
  }
}

llvm::LLVMContext &SwiftASTContext::GetGlobalLLVMContext() {
  // TODO check with Sean.  Do we really want this to be static across
  // an LLDB managing multiple Swift processes?
  static llvm::LLVMContext s_global_context;
  return s_global_context;
}

llvm::ArrayRef<swift::VarDecl *> SwiftASTContext::GetStoredProperties(
                                             swift::NominalTypeDecl *nominal) {
  VALID_OR_RETURN(llvm::ArrayRef<swift::VarDecl *>());

  // Check whether we already have the stored properties for this
  // nominal type.
  auto known = m_stored_properties.find(nominal);
  if (known != m_stored_properties.end()) return known->second;

  // Collect the stored properties from the AST and put them in the
  // cache.
  auto stored_properties = nominal->getStoredProperties();
  auto &stored = m_stored_properties[nominal];
  stored = std::vector<swift::VarDecl *>(stored_properties.begin(),
                                         stored_properties.end());
  return stored;
}

class SwiftEnumDescriptor {
public:
  enum class Kind {
    Empty,      // no cases in this enum
    CStyle,     // no cases have payloads
    AllPayload, // all cases have payloads
    Mixed       // some cases have payloads
  };

  struct ElementInfo {
    lldb_private::ConstString name;
    CompilerType payload_type;
    bool has_payload : 1;
    bool is_indirect : 1;
  };

  Kind GetKind() const { return m_kind; }

  ConstString GetTypeName() { return m_type_name; }

  virtual ElementInfo *
  GetElementFromData(const lldb_private::DataExtractor &data) = 0;

  virtual size_t GetNumElements() {
    return GetNumElementsWithPayload() + GetNumCStyleElements();
  }

  virtual size_t GetNumElementsWithPayload() = 0;

  virtual size_t GetNumCStyleElements() = 0;

  virtual ElementInfo *GetElementWithPayloadAtIndex(size_t idx) = 0;

  virtual ElementInfo *GetElementWithNoPayloadAtIndex(size_t idx) = 0;

  virtual ~SwiftEnumDescriptor() = default;

  static SwiftEnumDescriptor *CreateDescriptor(swift::ASTContext *ast,
                                               swift::CanType swift_can_type,
                                               swift::EnumDecl *enum_decl);

protected:
  SwiftEnumDescriptor(swift::ASTContext *ast, swift::CanType swift_can_type,
                      swift::EnumDecl *enum_decl, SwiftEnumDescriptor::Kind k)
      : m_kind(k), m_type_name() {
    if (swift_can_type.getPointer()) {
      if (auto nominal = swift_can_type->getAnyNominal()) {
        swift::Identifier name(nominal->getName());
        if (name.get())
          m_type_name.SetCString(name.get());
      }
    }
  }

private:
  Kind m_kind;
  ConstString m_type_name;
};

class SwiftEmptyEnumDescriptor : public SwiftEnumDescriptor {
public:
  SwiftEmptyEnumDescriptor(swift::ASTContext *ast,
                           swift::CanType swift_can_type,
                           swift::EnumDecl *enum_decl)
      : SwiftEnumDescriptor(ast, swift_can_type, enum_decl,
                            SwiftEnumDescriptor::Kind::Empty) {}

  virtual ElementInfo *
  GetElementFromData(const lldb_private::DataExtractor &data) {
    return nullptr;
  }

  virtual size_t GetNumElementsWithPayload() { return 0; }

  virtual size_t GetNumCStyleElements() { return 0; }

  virtual ElementInfo *GetElementWithPayloadAtIndex(size_t idx) {
    return nullptr;
  }

  virtual ElementInfo *GetElementWithNoPayloadAtIndex(size_t idx) {
    return nullptr;
  }

  static bool classof(const SwiftEnumDescriptor *S) {
    return S->GetKind() == SwiftEnumDescriptor::Kind::Empty;
  }

  virtual ~SwiftEmptyEnumDescriptor() = default;
};

namespace std {
template <> struct less<swift::ClusteredBitVector> {
  bool operator()(const swift::ClusteredBitVector &lhs,
                  const swift::ClusteredBitVector &rhs) const {
    int iL = lhs.size() - 1;
    int iR = rhs.size() - 1;
    for (; iL >= 0 && iR >= 0; --iL, --iR) {
      bool bL = lhs[iL];
      bool bR = rhs[iR];
      if (bL and not bR)
        return false;
      if (bR and not bL)
        return true;
    }
    return false;
  }
};
}

static std::string Dump(const swift::ClusteredBitVector &bit_vector) {
  std::string buffer;
  llvm::raw_string_ostream ostream(buffer);
  for (size_t i = 0; i < bit_vector.size(); i++) {
    if (bit_vector[i])
      ostream << '1';
    else
      ostream << '0';
    if ((i % 4) == 3)
      ostream << ' ';
  }
  ostream.flush();
  return buffer;
}

class SwiftCStyleEnumDescriptor : public SwiftEnumDescriptor {
public:
  SwiftCStyleEnumDescriptor(swift::ASTContext *ast,
                            swift::CanType swift_can_type,
                            swift::EnumDecl *enum_decl)
      : SwiftEnumDescriptor(ast, swift_can_type, enum_decl,
                            SwiftEnumDescriptor::Kind::CStyle),
        m_nopayload_elems_bitmask(), m_elements(), m_element_indexes() {
    Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf("doing C-style enum layout for %s",
                  GetTypeName().AsCString());

    SwiftASTContext *swift_ast_ctx = SwiftASTContext::GetSwiftASTContext(ast);
    swift::irgen::IRGenModule &irgen_module = swift_ast_ctx->GetIRGenModule();
    const swift::irgen::EnumImplStrategy &enum_impl_strategy =
        swift::irgen::getEnumImplStrategy(irgen_module, swift_can_type);
    llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element>
        elements_with_no_payload =
            enum_impl_strategy.getElementsWithNoPayload();
    const bool has_payload = false;
    const bool is_indirect = false;
    uint64_t case_counter = 0;
    m_nopayload_elems_bitmask =
        enum_impl_strategy.getBitMaskForNoPayloadElements();

    if (log)
      log->Printf("m_nopayload_elems_bitmask = %s",
                  Dump(m_nopayload_elems_bitmask).c_str());

    for (auto enum_case : elements_with_no_payload) {
      ConstString case_name(enum_case.decl->getName().str().data());
      swift::ClusteredBitVector case_value =
          enum_impl_strategy.getBitPatternForNoPayloadElement(enum_case.decl);

      if (log)
        log->Printf("case_name = %s, unmasked value = %s",
                    case_name.AsCString(), Dump(case_value).c_str());

      case_value &= m_nopayload_elems_bitmask;

      if (log)
        log->Printf("case_name = %s, masked value = %s", case_name.AsCString(),
                    Dump(case_value).c_str());

      std::unique_ptr<ElementInfo> elem_info(
          new ElementInfo{case_name, CompilerType(), has_payload, is_indirect});
      m_element_indexes.emplace(case_counter, elem_info.get());
      case_counter++;
      m_elements.emplace(case_value, std::move(elem_info));
    }
  }

  virtual ElementInfo *
  GetElementFromData(const lldb_private::DataExtractor &data) {
    Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf(
          "C-style enum - inspecting data to find enum case for type %s",
          GetTypeName().AsCString());

    swift::ClusteredBitVector current_payload;
    lldb::offset_t offset = 0;
    for (size_t idx = 0; idx < data.GetByteSize(); idx++) {
      uint64_t byte = data.GetU8(&offset);
      current_payload.add(8, byte);
    }

    if (log) {
      log->Printf("m_nopayload_elems_bitmask        = %s",
                  Dump(m_nopayload_elems_bitmask).c_str());
      log->Printf("current_payload                  = %s",
                  Dump(current_payload).c_str());
    }

    if (current_payload.size() != m_nopayload_elems_bitmask.size()) {
      if (log)
        log->Printf("sizes don't match; getting out with an error");
      return nullptr;
    }

    current_payload &= m_nopayload_elems_bitmask;

    if (log)
      log->Printf("masked current_payload           = %s",
                  Dump(current_payload).c_str());

    auto iter = m_elements.find(current_payload), end = m_elements.end();
    if (iter == end) {
      if (log)
        log->Printf("bitmask search failed");
      return nullptr;
    }
    if (log)
      log->Printf("bitmask search success - found case %s",
                  iter->second.get()->name.AsCString());
    return iter->second.get();
  }

  virtual size_t GetNumElementsWithPayload() { return 0; }

  virtual size_t GetNumCStyleElements() { return m_elements.size(); }

  virtual ElementInfo *GetElementWithPayloadAtIndex(size_t idx) {
    return nullptr;
  }

  virtual ElementInfo *GetElementWithNoPayloadAtIndex(size_t idx) {
    if (idx >= m_element_indexes.size())
      return nullptr;
    return m_element_indexes[idx];
  }

  static bool classof(const SwiftEnumDescriptor *S) {
    return S->GetKind() == SwiftEnumDescriptor::Kind::CStyle;
  }

  virtual ~SwiftCStyleEnumDescriptor() = default;

private:
  swift::ClusteredBitVector m_nopayload_elems_bitmask;
  std::map<swift::ClusteredBitVector, std::unique_ptr<ElementInfo>> m_elements;
  std::map<uint64_t, ElementInfo *> m_element_indexes;
};

static CompilerType
GetFunctionArgumentTuple(const CompilerType &compiler_type) {
  if (compiler_type.IsValid() &&
      llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem())) {
    swift::CanType swift_can_type(
        GetCanonicalSwiftType(compiler_type.GetOpaqueQualType()));
    auto func =
        swift::dyn_cast_or_null<swift::AnyFunctionType>(
            swift_can_type);
    if (func) {
      auto input = func.getInput();
      // See comment in swift::AnyFunctionType for rationale here:
      // A function can take either a tuple or a parentype, but if a parentype
      // (i.e. (Foo)), then it will be reduced down to just Foo, so if the input
      // is not a tuple, that must mean there is only 1 input.
      auto tuple = swift::dyn_cast<swift::TupleType>(input);
      if (tuple)
        return CompilerType(compiler_type.GetTypeSystem(), tuple);
      else
        return CompilerType(compiler_type.GetTypeSystem(), input.getPointer());
    }
  }
  return CompilerType();
}

class SwiftAllPayloadEnumDescriptor : public SwiftEnumDescriptor {
public:
  SwiftAllPayloadEnumDescriptor(swift::ASTContext *ast,
                                swift::CanType swift_can_type,
                                swift::EnumDecl *enum_decl)
      : SwiftEnumDescriptor(ast, swift_can_type, enum_decl,
                            SwiftEnumDescriptor::Kind::AllPayload),
        m_tag_bits(), m_elements() {
    Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf("doing ADT-style enum layout for %s",
                  GetTypeName().AsCString());

    SwiftASTContext *swift_ast_ctx = SwiftASTContext::GetSwiftASTContext(ast);
    swift::irgen::IRGenModule &irgen_module = swift_ast_ctx->GetIRGenModule();
    const swift::irgen::EnumImplStrategy &enum_impl_strategy =
        swift::irgen::getEnumImplStrategy(irgen_module, swift_can_type);
    llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element>
        elements_with_payload = enum_impl_strategy.getElementsWithPayload();
    m_tag_bits = enum_impl_strategy.getTagBitsForPayloads();

    if (log)
      log->Printf("tag_bits = %s", Dump(m_tag_bits).c_str());

    auto module_ctx = enum_decl->getModuleContext();
    const bool has_payload = true;
    for (auto enum_case : elements_with_payload) {
      ConstString case_name(enum_case.decl->getName().str().data());

      swift::EnumElementDecl *case_decl = enum_case.decl;
      assert(case_decl);
      CompilerType case_type(
          ast, swift_can_type->getTypeOfMember(module_ctx, case_decl, nullptr)
                   .getPointer());
      case_type = GetFunctionArgumentTuple(case_type.GetFunctionReturnType());

      const bool is_indirect = case_decl->isIndirect() 
          || case_decl->getParentEnum()->isIndirect();

      if (log)
        log->Printf("case_name = %s, type = %s, is_indirect = %s",
                    case_name.AsCString(), case_type.GetTypeName().AsCString(),
                    is_indirect ? "yes" : "no");

      std::unique_ptr<ElementInfo> elem_info(
          new ElementInfo{case_name, case_type, has_payload, is_indirect});
      m_elements.push_back(std::move(elem_info));
    }
  }

  virtual ElementInfo *
  GetElementFromData(const lldb_private::DataExtractor &data) {
    Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf(
          "ADT-style enum - inspecting data to find enum case for type %s",
          GetTypeName().AsCString());

    if (m_elements.size() == 0) // no elements, just fail
    {
      if (log)
        log->Printf("enum with no cases. getting out");
      return nullptr;
    }
    if (m_elements.size() == 1) // one element, so it's gotta be it
    {
      if (log)
        log->Printf("enum with one case. getting out easy with %s",
                    m_elements.front().get()->name.AsCString());

      return m_elements.front().get();
    }

    swift::ClusteredBitVector current_payload;
    lldb::offset_t offset = 0;
    for (size_t idx = 0; idx < data.GetByteSize(); idx++) {
      uint64_t byte = data.GetU8(&offset);
      current_payload.add(8, byte);
    }
    if (log) {
      log->Printf("tag_bits        = %s", Dump(m_tag_bits).c_str());
      log->Printf("current_payload = %s", Dump(current_payload).c_str());
    }

    if (current_payload.size() != m_tag_bits.size()) {
      if (log)
        log->Printf("sizes don't match; getting out with an error");
      return nullptr;
    }

    size_t discriminator = 0;
    size_t power_of_2 = 1;
    auto enumerator = m_tag_bits.enumerateSetBits();
    for (llvm::Optional<size_t> next = enumerator.findNext(); next.hasValue();
         next = enumerator.findNext()) {
      discriminator =
          discriminator + (current_payload[next.getValue()] ? power_of_2 : 0);
      power_of_2 <<= 1;
    }

    if (discriminator >= m_elements.size()) // discriminator too large, get out
    {
      if (log)
        log->Printf("discriminator value of %" PRIu64 " too large, getting out",
                    (uint64_t)discriminator);
      return nullptr;
    } else {
      auto ptr = m_elements[discriminator].get();
      if (log) {
        if (!ptr)
          log->Printf("discriminator value of %" PRIu64
                      " acceptable, but null case matched - that's bad",
                      (uint64_t)discriminator);
        else
          log->Printf("discriminator value of %" PRIu64
                      " acceptable, case %s matched",
                      (uint64_t)discriminator, ptr->name.AsCString());
      }
      return ptr;
    }
  }

  virtual size_t GetNumElementsWithPayload() { return m_elements.size(); }

  virtual size_t GetNumCStyleElements() { return 0; }

  virtual ElementInfo *GetElementWithPayloadAtIndex(size_t idx) {
    if (idx >= m_elements.size())
      return nullptr;
    return m_elements[idx].get();
  }

  virtual ElementInfo *GetElementWithNoPayloadAtIndex(size_t idx) {
    return nullptr;
  }

  static bool classof(const SwiftEnumDescriptor *S) {
    return S->GetKind() == SwiftEnumDescriptor::Kind::AllPayload;
  }

  virtual ~SwiftAllPayloadEnumDescriptor() = default;

private:
  swift::ClusteredBitVector m_tag_bits;
  std::vector<std::unique_ptr<ElementInfo>> m_elements;
};

class SwiftMixedEnumDescriptor : public SwiftEnumDescriptor {
public:
  SwiftMixedEnumDescriptor(swift::ASTContext *ast,
                           swift::CanType swift_can_type,
                           swift::EnumDecl *enum_decl)
      : SwiftEnumDescriptor(ast, swift_can_type, enum_decl,
                            SwiftEnumDescriptor::Kind::Mixed),
        m_non_payload_cases(ast, swift_can_type, enum_decl),
        m_payload_cases(ast, swift_can_type, enum_decl) {}

  virtual ElementInfo *
  GetElementFromData(const lldb_private::DataExtractor &data) {
    ElementInfo *elem_info = m_non_payload_cases.GetElementFromData(data);
    return elem_info ? elem_info : m_payload_cases.GetElementFromData(data);
  }

  static bool classof(const SwiftEnumDescriptor *S) {
    return S->GetKind() == SwiftEnumDescriptor::Kind::Mixed;
  }

  virtual size_t GetNumElementsWithPayload() {
    return m_payload_cases.GetNumElementsWithPayload();
  }

  virtual size_t GetNumCStyleElements() {
    return m_non_payload_cases.GetNumCStyleElements();
  }

  virtual ElementInfo *GetElementWithPayloadAtIndex(size_t idx) {
    return m_payload_cases.GetElementWithPayloadAtIndex(idx);
  }

  virtual ElementInfo *GetElementWithNoPayloadAtIndex(size_t idx) {
    return m_non_payload_cases.GetElementWithNoPayloadAtIndex(idx);
  }

  virtual ~SwiftMixedEnumDescriptor() = default;

private:
  SwiftCStyleEnumDescriptor m_non_payload_cases;
  SwiftAllPayloadEnumDescriptor m_payload_cases;
};

SwiftEnumDescriptor *
SwiftEnumDescriptor::CreateDescriptor(swift::ASTContext *ast,
                                      swift::CanType swift_can_type,
                                      swift::EnumDecl *enum_decl) {
  assert(ast);
  assert(enum_decl);
  assert(swift_can_type.getPointer());
  SwiftASTContext *swift_ast_ctx = SwiftASTContext::GetSwiftASTContext(ast);
  assert(swift_ast_ctx);
  swift::irgen::IRGenModule &irgen_module = swift_ast_ctx->GetIRGenModule();
  const swift::irgen::EnumImplStrategy &enum_impl_strategy =
      swift::irgen::getEnumImplStrategy(irgen_module, swift_can_type);
  llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element>
      elements_with_payload = enum_impl_strategy.getElementsWithPayload();
  llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element>
      elements_with_no_payload = enum_impl_strategy.getElementsWithNoPayload();
  if (elements_with_no_payload.size() == 0) {
    // nothing with no payload.. empty or all payloads?
    if (elements_with_payload.size() == 0)
      return new SwiftEmptyEnumDescriptor(ast, swift_can_type, enum_decl);
    else
      return new SwiftAllPayloadEnumDescriptor(ast, swift_can_type, enum_decl);
  } else {
    // something with no payload.. mixed or C-style?
    if (elements_with_payload.size() == 0)
      return new SwiftCStyleEnumDescriptor(ast, swift_can_type, enum_decl);
    else
      return new SwiftMixedEnumDescriptor(ast, swift_can_type, enum_decl);
  }
}

static SwiftEnumDescriptor *
GetEnumInfoFromEnumDecl(swift::ASTContext *ast, swift::CanType swift_can_type,
                        swift::EnumDecl *enum_decl) {
  return SwiftEnumDescriptor::CreateDescriptor(ast, swift_can_type, enum_decl);
}

SwiftEnumDescriptor *SwiftASTContext::GetCachedEnumInfo(void *type) {
  VALID_OR_RETURN(nullptr);

  if (type) {
    EnumInfoCache *enum_info_cache = GetEnumInfoCache(GetASTContext());
    EnumInfoCache::const_iterator pos = enum_info_cache->find(type);
    if (pos != enum_info_cache->end())
      return pos->second.get();

    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    if (!SwiftASTContext::IsFullyRealized(
            CompilerType(GetASTContext(), swift_can_type)))
      return nullptr;

    SwiftEnumDescriptorSP enum_info_sp;

    if (auto *enum_type = swift_can_type->getAs<swift::EnumType>()) {
      enum_info_sp.reset(GetEnumInfoFromEnumDecl(
          GetASTContext(), swift_can_type, enum_type->getDecl()));
    } else if (auto *bound_enum_type =
          swift_can_type->getAs<swift::BoundGenericEnumType>()) {
      enum_info_sp.reset(GetEnumInfoFromEnumDecl(
          GetASTContext(), swift_can_type, bound_enum_type->getDecl()));
    }

    if (enum_info_sp.get())
      enum_info_cache->insert(std::make_pair(type, enum_info_sp));
    return enum_info_sp.get();
  }
  return nullptr;
}

namespace {
static inline bool
SwiftASTContextSupportsLanguage(lldb::LanguageType language) {
  return language == eLanguageTypeSwift;
}

static bool IsDeviceSupport(const char *path) {
  // The old-style check, which we preserve for safety.
  if (path && strstr(path, "iOS DeviceSupport"))
    return true;

  // The new-style check, which should cover more devices.
  if (path)
    if (const char *Developer_Xcode = strstr(path, "Developer"))
      if (const char *DeviceSupport = strstr(Developer_Xcode, "DeviceSupport"))
        if (strstr(DeviceSupport, "Symbols"))
          return true;

  // Don't look in the simulator runtime frameworks either.  They either 
  // duplicate what the SDK has, or for older simulators conflict with them.
  if (path && strstr(path, ".simruntime/Contents/Resources/"))
    return true;

  return false;
}
}

SwiftASTContext::SwiftASTContext(const char *triple, Target *target)
    : TypeSystem(TypeSystem::eKindSwift), m_source_manager_ap(),
      m_diagnostic_engine_ap(), m_ast_context_ap(), m_ir_gen_module_ap(),
      m_compiler_invocation_ap(new swift::CompilerInvocation()),
      m_dwarf_ast_parser_ap(), m_scratch_module(NULL), m_sil_module_ap(),
      m_serialized_module_loader(NULL), m_clang_importer(NULL),
      m_swift_module_cache(), m_mangled_name_to_type_map(),
      m_type_to_mangled_name_map(), m_pointer_byte_size(0),
      m_pointer_bit_align(0), m_void_function_type(), m_target_wp(),
      m_process(NULL), m_platform_sdk_path(), m_resource_dir(),
      m_ast_file_data_map(), m_initialized_language_options(false),
      m_initialized_search_path_options(false),
      m_initialized_clang_importer_options(false),
      m_reported_fatal_error(false), m_fatal_errors(), m_negative_type_cache(),
      m_extra_type_info_cache(), m_swift_type_map() {
  // Set the clang modules cache path.
  llvm::SmallString<128> path;
  auto props = ModuleList::GetGlobalModuleListProperties();
  props.GetClangModulesCachePath().GetPath(path);
  m_compiler_invocation_ap->setClangModuleCachePath(path);

  if (target)
    m_target_wp = target->shared_from_this();

  if (triple)
    SetTriple(triple);
  swift::IRGenOptions &ir_gen_opts =
      m_compiler_invocation_ap->getIRGenOptions();
  ir_gen_opts.OutputKind = swift::IRGenOutputKind::Module;
  ir_gen_opts.UseJIT = true;
  ir_gen_opts.DWARFVersion = swift::DWARFVersion;

  // FIXME: lldb does not support resilience yet.
  ir_gen_opts.EnableResilienceBypass = true;
}

SwiftASTContext::SwiftASTContext(const SwiftASTContext &rhs)
    : TypeSystem(rhs.getKind()), m_source_manager_ap(),
      m_diagnostic_engine_ap(), m_ast_context_ap(), m_ir_gen_module_ap(),
      m_compiler_invocation_ap(new swift::CompilerInvocation()),
      m_dwarf_ast_parser_ap(), m_scratch_module(NULL), m_sil_module_ap(),
      m_serialized_module_loader(NULL), m_clang_importer(NULL),
      m_swift_module_cache(), m_mangled_name_to_type_map(),
      m_type_to_mangled_name_map(), m_pointer_byte_size(0),
      m_pointer_bit_align(0), m_void_function_type(), m_target_wp(),
      m_process(NULL), m_platform_sdk_path(), m_resource_dir(),
      m_ast_file_data_map(), m_initialized_language_options(false),
      m_initialized_search_path_options(false),
      m_initialized_clang_importer_options(false),
      m_reported_fatal_error(false), m_fatal_errors(), m_negative_type_cache(),
      m_extra_type_info_cache(), m_swift_type_map() {
  if (rhs.m_compiler_invocation_ap) {
    std::string rhs_triple = rhs.GetTriple();
    if (!rhs_triple.empty()) {
      SetTriple(rhs_triple.c_str());
    }
    llvm::StringRef module_cache_path =
        rhs.m_compiler_invocation_ap->getClangModuleCachePath();
    m_compiler_invocation_ap->setClangModuleCachePath(module_cache_path);
  }

  swift::IRGenOptions &ir_gen_opts =
      m_compiler_invocation_ap->getIRGenOptions();
  ir_gen_opts.OutputKind = swift::IRGenOutputKind::Module;
  ir_gen_opts.UseJIT = true;

  TargetSP target_sp = rhs.m_target_wp.lock();
  if (target_sp)
    m_target_wp = target_sp;

  m_platform_sdk_path = rhs.m_platform_sdk_path;
  m_resource_dir = rhs.m_resource_dir;

  swift::ASTContext *lhs_ast = GetASTContext();
  swift::ASTContext *rhs_ast =
      const_cast<SwiftASTContext &>(rhs).GetASTContext();

  if (lhs_ast && rhs_ast) {
    lhs_ast->SearchPathOpts = rhs_ast->SearchPathOpts;
  }
  GetClangImporter();
}

SwiftASTContext::~SwiftASTContext() {
  if (m_ast_context_ap.get()) {
    GetASTMap().Erase(m_ast_context_ap.get());
  }
}

ConstString SwiftASTContext::GetPluginNameStatic() {
  return ConstString("swift");
}

ConstString SwiftASTContext::GetPluginName() {
  return ClangASTContext::GetPluginNameStatic();
}

uint32_t SwiftASTContext::GetPluginVersion() { return 1; }

static std::string &GetDefaultResourceDir() {
  static std::string s_resource_dir;
  return s_resource_dir;
}


/// Retrieve the first serialized AST data blob and initialize
/// the compiler invocation with it. The premise is that the
/// ClangImporter flags will be the same for all AST blobs in
/// this library, and search paths can be concatenated later.
static bool DeserializeCompilerFlags(SwiftASTContext &swift_ast, Module &module,
                                     std::string &error, bool &got_serialized_options) {
  got_serialized_options = false;
  SymbolVendor *sym_vendor = module.GetSymbolVendor();
  if (!sym_vendor)
    return false;

  auto ast_file_datas = sym_vendor->GetASTData(eLanguageTypeSwift);
  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  if (log)
    log->Printf("Found %d AST file data entries for library: %s.",
                (int)ast_file_datas.size(),
                module.GetSpecificationDescription().c_str());

  // If no N_AST symbols exist, this is not an error.
  if (ast_file_datas.empty())
    return false;

  auto ast_file_data_sp = ast_file_datas.front();
  llvm::StringRef section_data_ref((const char *)ast_file_data_sp->GetBytes(),
                                   ast_file_data_sp->GetByteSize());
  auto result =
      swift_ast.GetCompilerInvocation().loadFromSerializedAST(section_data_ref);

  switch (result) {
  case swift::serialization::Status::Valid:
    got_serialized_options = true;
    return false;

  case swift::serialization::Status::FormatTooOld:
    error = "the swift module file format is too old to be used by the "
            "version of the swift compiler in LLDB";
    return true;

  case swift::serialization::Status::FormatTooNew:
    error = "the swift module file format is too new to be used by this "
            "version of the swift compiler in LLDB";
    return true;

  case swift::serialization::Status::MissingDependency:
    error = "the swift module file depends on another module that can't be "
            "loaded";
    return true;

  case swift::serialization::Status::MissingShadowedModule:
    error = "the swift module file is an overlay for a clang module, which "
            "can't be found";
    return true;

  case swift::serialization::Status::FailedToLoadBridgingHeader:
    error = "the swift module file depends on a bridging header that can't "
            "be loaded";
    return true;

  case swift::serialization::Status::Malformed:
    error = "the swift module file is malformed";
    return true;

  case swift::serialization::Status::MalformedDocumentation:
    error = "the swift module documentation file is malformed in some way";
    return true;

  case swift::serialization::Status::NameMismatch:
    error = "the swift module file's name does not match the module it is "
            "being loaded into";
    return true;

  case swift::serialization::Status::TargetIncompatible:
    error = "the swift module file was built for a different target "
            "platform";
    return true;

  case swift::serialization::Status::TargetTooNew:
    error = "the swift module file was built for a target newer than the "
            "current target";
    return true;
  }
}

void SwiftASTContext::RemapClangImporterOptions(
    const PathMappingList &path_map) {
  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  auto &options = GetClangImporterOptions();
  std::string remapped;
  if (path_map.RemapPath(options.BridgingHeader, remapped)) {
    if (log)
      log->Printf("remapped %s -> %s", options.BridgingHeader.c_str(),
                  remapped.c_str());
    options.BridgingHeader = remapped;
  }
  for (auto &arg_string : options.ExtraArgs) {
    StringRef prefix;
    StringRef arg = arg_string;
    if (arg.consume_front("-I"))
      prefix = "-I";
    if (path_map.RemapPath(arg, remapped)) {
      if (log)
        log->Printf("remapped %s -> %s%s", arg.str().c_str(),
                    prefix.str().c_str(), remapped.c_str());
      arg_string = prefix.str()+remapped;
    }
  }
}

lldb::TypeSystemSP SwiftASTContext::CreateInstance(lldb::LanguageType language,
                                                   Module &module,
                                                   Target *target) {
  if (!SwiftASTContextSupportsLanguage(language))
    return lldb::TypeSystemSP();

  ArchSpec arch = module.GetArchitecture();

  ObjectFile *objfile = module.GetObjectFile();
  ArchSpec object_arch;

  if (!objfile || !objfile->GetArchitecture(object_arch))
    return TypeSystemSP();

  lldb::CompUnitSP main_compile_unit_sp = module.GetCompileUnitAtIndex(0);

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

  if (main_compile_unit_sp && !main_compile_unit_sp->Exists()) {
    if (log) {
      StreamString ss;
      module.GetDescription(&ss);

      log->Printf("Corresponding source not found for %s, loading module "
                  "%s is unlikely to succeed",
                  main_compile_unit_sp->GetCString(), ss.GetData());
    }
  }

  std::shared_ptr<SwiftASTContext> swift_ast_sp(
      target ? (new SwiftASTContextForExpressions(*target))
             : new SwiftASTContext());

  swift_ast_sp->GetLanguageOptions().DebuggerSupport = true;
  swift_ast_sp->GetLanguageOptions().EnableAccessControl = false;

  if (!arch.IsValid())
    return TypeSystemSP();

  llvm::Triple triple = arch.GetTriple();

  if (triple.getOS() == llvm::Triple::UnknownOS) {
    // cl_kernels are the only binaries that don't have an LC_MIN_VERSION_xxx
    // load command. This avoids a Swift assertion.

#if defined(__APPLE__)
    switch (triple.getArch()) {
    default:
      triple.setOS(llvm::Triple::MacOSX);
      break;
    case llvm::Triple::arm:
    case llvm::Triple::armeb:
    case llvm::Triple::aarch64:
    case llvm::Triple::aarch64_be:
      triple.setOS(llvm::Triple::IOS);
      break;
    }

#else
    // Not an elegant hack on OS X, not an elegant hack elsewhere.
    // But we shouldn't be claiming things are Mac binaries when they are
    // not.
    triple.setOS(HostInfo::GetArchitecture().GetTriple().getOS());
#endif
  }

  swift_ast_sp->SetTriple(triple.getTriple().c_str(), &module);

  bool set_triple = false;

  SymbolVendor *sym_vendor = module.GetSymbolVendor();

  std::string resource_dir;
  std::string target_triple;

  if (sym_vendor) {
    bool got_serialized_options;
    std::string error;
    if (DeserializeCompilerFlags(*swift_ast_sp, module, error,
                                 got_serialized_options)) {
      swift_ast_sp->m_fatal_errors.SetErrorString(error);
      return swift_ast_sp;
    }

    // Some of the bits in the compiler options we keep separately, so we
    // need to populate them from the serialized options:
    llvm::StringRef serialized_triple =
        swift_ast_sp->GetCompilerInvocation().getTargetTriple();
    if (serialized_triple.empty()) {
      if (log)
        log->Printf("\tSerialized triple for %s was empty.",
                    module.GetSpecificationDescription().c_str());
    } else {
      if (log)
        log->Printf("\tFound serialized triple for %s: %s.",
                    module.GetSpecificationDescription().c_str(),
                    serialized_triple.data());
      swift_ast_sp->SetTriple(serialized_triple.data(), &module);
      set_triple = true;
    }

    llvm::StringRef serialized_sdk_path =
        swift_ast_sp->GetCompilerInvocation().getSDKPath();
    if (serialized_sdk_path.empty()) {
      if (log)
        log->Printf("\tNo serialized SDK path.");
    } else {
      if (log)
        log->Printf("\tGot serialized SDK path %s.",
                    serialized_sdk_path.data());
      FileSpec sdk_spec(serialized_sdk_path.data(), false);
      if (sdk_spec.Exists()) {
        swift_ast_sp->SetPlatformSDKPath(serialized_sdk_path.data());
      }
    }

    if (!got_serialized_options || !swift_ast_sp->GetPlatformSDKPath()) {
      std::string platform_sdk_path;
      if (sym_vendor->GetCompileOption("-sdk", platform_sdk_path)) {
        FileSpec sdk_spec(platform_sdk_path.c_str(), false);
        if (sdk_spec.Exists()) {
          swift_ast_sp->SetPlatformSDKPath(platform_sdk_path.c_str());
        }

        if (sym_vendor->GetCompileOption("-target", target_triple)) {
          llvm::StringRef parsed_triple(target_triple);

          swift_ast_sp->SetTriple(target_triple.c_str(), &module);
          set_triple = true;
        }
      }
    }

    if (sym_vendor->GetCompileOption("-resource-dir", resource_dir)) {
      swift_ast_sp->SetResourceDir(resource_dir.c_str());
    } else if (!GetDefaultResourceDir().empty()) {
      // Use the first resource dir we found when setting up a target.
      swift_ast_sp->SetResourceDir(GetDefaultResourceDir().c_str());
    } else {
      if (log)
        log->Printf("No resource dir available for module's SwiftASTContext.");
    }

    if (!got_serialized_options) {

      std::vector<std::string> framework_search_paths;

      if (sym_vendor->GetCompileOptions("-F", framework_search_paths)) {
        for (std::string &search_path : framework_search_paths) {
          swift_ast_sp->AddFrameworkSearchPath(search_path.c_str());
        }
      }

      std::vector<std::string> include_paths;

      if (sym_vendor->GetCompileOptions("-I", include_paths)) {
        for (std::string &search_path : include_paths) {
          const FileSpec path_spec(search_path.c_str(), false);

          if (path_spec.Exists()) {
            static const ConstString s_hmap_extension("hmap");

            if (IsDirectory(path_spec)) {
              swift_ast_sp->AddModuleSearchPath(search_path.c_str());
            } else if (IsRegularFile(path_spec) &&
                       path_spec.GetFileNameExtension() == s_hmap_extension) {
              std::string argument("-I");
              argument.append(search_path);
              swift_ast_sp->AddClangArgument(argument.c_str());
            }
          }
        }
      }

      std::vector<std::string> cc_options;

      if (sym_vendor->GetCompileOptions("-Xcc", cc_options)) {
        for (int i = 0; i < cc_options.size(); ++i) {
          if (!cc_options[i].compare("-iquote") && i + 1 < cc_options.size()) {
            swift_ast_sp->AddClangArgumentPair("-iquote",
                                               cc_options[i + 1].c_str());
          }
        }
      }
    }

    FileSpecList loaded_modules;

    sym_vendor->GetLoadedModules(lldb::eLanguageTypeSwift, loaded_modules);

    for (size_t mi = 0, me = loaded_modules.GetSize(); mi != me; ++mi) {
      const FileSpec &loaded_module = loaded_modules.GetFileSpecAtIndex(mi);

      if (loaded_module.Exists())
        swift_ast_sp->AddModuleSearchPath(
            loaded_module.GetDirectory().GetCString());
    }
  }

  if (!set_triple) {
    llvm::Triple llvm_triple(swift_ast_sp->GetTriple());

    // LLVM wants this to be set to iOS or MacOSX; if we're working on
    // a bare-boards type image, change the triple for LLVM's benefit.
    if (llvm_triple.getVendor() == llvm::Triple::Apple &&
        llvm_triple.getOS() == llvm::Triple::UnknownOS) {
      if (llvm_triple.getArch() == llvm::Triple::arm ||
          llvm_triple.getArch() == llvm::Triple::thumb) {
        llvm_triple.setOS(llvm::Triple::IOS);
      } else {
        llvm_triple.setOS(llvm::Triple::MacOSX);
      }
      swift_ast_sp->SetTriple(llvm_triple.str().c_str(), &module);
    }
  }

  // Apply source path remappings ofund in the module's dSYM.
  swift_ast_sp->RemapClangImporterOptions(module.GetSourceMappingList());
  
  if (!swift_ast_sp->GetClangImporter()) {
    if (log) {
      log->Printf("((Module*)%p) [%s]->GetSwiftASTContext() returning NULL "
                  "- couldn't create a ClangImporter",
                  &module,
                  module.GetFileSpec().GetFilename().AsCString("<anonymous>"));
    }

    return TypeSystemSP();
  }

  std::vector<std::string> module_names;
  swift_ast_sp->RegisterSectionModules(module, module_names);
  swift_ast_sp->ValidateSectionModules(module, module_names);

  if (log) {
    log->Printf("((Module*)%p) [%s]->GetSwiftASTContext() = %p", &module,
                module.GetFileSpec().GetFilename().AsCString("<anonymous>"),
                swift_ast_sp.get());
    swift_ast_sp->DumpConfiguration(log);
  }
  return swift_ast_sp;
}

lldb::TypeSystemSP SwiftASTContext::CreateInstance(lldb::LanguageType language,
                                                   Target &target,
                                                   const char *extra_options) {
  if (!SwiftASTContextSupportsLanguage(language))
    return lldb::TypeSystemSP();

  ArchSpec arch = target.GetArchitecture();

  // Make an AST but don't set the triple yet. We need to try and detect
  // if we have a iOS simulator...
  std::shared_ptr<SwiftASTContextForExpressions> swift_ast_sp(
      new SwiftASTContextForExpressions(target));

  if (!arch.IsValid())
    return TypeSystemSP();

  bool handled_sdk_path = false;
  bool handled_resource_dir = false;
  const size_t num_images = target.GetImages().GetSize();
  // Set the SDK path and resource dir prior to doing search paths.
  // Otherwise when we create search path options we put in the wrong SDK
  // path.

  FileSpec &target_sdk_spec = target.GetSDKPath();
  if (target_sdk_spec && target_sdk_spec.Exists()) {
    std::string platform_sdk_path(target_sdk_spec.GetPath());
    swift_ast_sp->SetPlatformSDKPath(std::move(platform_sdk_path));
    handled_sdk_path = true;
  }

  Status module_error;
  for (size_t mi = 0; mi != num_images; ++mi) {
    ModuleSP module_sp = target.GetImages().GetModuleAtIndex(mi);

    SwiftASTContext *module_swift_ast = llvm::dyn_cast_or_null<SwiftASTContext>(
        module_sp->GetTypeSystemForLanguage(lldb::eLanguageTypeSwift));

    if (!module_swift_ast || module_swift_ast->HasFatalErrors() ||
        !module_swift_ast->GetClangImporter()) {
      // Make sure we warn about this module load failure, the one that
      // comes from loading types often gets swallowed up and not seen,
      // this is the only reliable point where we can show this.
      // But only do it once per UUID so we don't overwhelm the user with
      // warnings...
      std::unordered_set<std::string> m_swift_warnings_issued;

      UUID module_uuid(module_sp->GetUUID());
      std::pair<std::unordered_set<std::string>::iterator, bool> result(
          m_swift_warnings_issued.insert(module_uuid.GetAsString()));
      if (result.second) {
        StreamString ss;
        module_sp->GetDescription(&ss, eDescriptionLevelBrief);
        target.GetDebugger().GetErrorFile()->Printf(
            "warning: Swift error in module %s" /*": \n    %s\n"*/
            ".\nDebug info from this module will be unavailable in the "
            "debugger.\n\n",
            ss.GetData());
      }

      continue;
    }

    if (!handled_sdk_path) {
      const char *platform_sdk_path = module_swift_ast->GetPlatformSDKPath();

      if (platform_sdk_path) {
        handled_sdk_path = true;
        swift_ast_sp->SetPlatformSDKPath(platform_sdk_path);
      }
    }

    if (!handled_resource_dir) {
      const char *resource_dir = module_swift_ast->GetResourceDir();
      if (resource_dir) {
        handled_resource_dir = true;
        swift_ast_sp->SetResourceDir(resource_dir);
        if (GetDefaultResourceDir().empty()) {
          // Tuck this away as a reasonable default resource dir
          // for contexts that don't have one. The Swift parser
          // will assert without one.
          GetDefaultResourceDir() = resource_dir;
        }
      }
    }

    if (handled_sdk_path && handled_resource_dir)
      break;
  }

  // First, prime the compiler with the options from the main executable:
  bool got_serialized_options = false;
  ModuleSP exe_module_sp(target.GetExecutableModule());

  // If we're debugging a testsuite, then treat the main test bundle as the
  // executable.
  if (exe_module_sp && PlatformDarwin::IsUnitTestExecutable(*exe_module_sp)) {
    ModuleSP unit_test_module =
        PlatformDarwin::GetUnitTestModule(target.GetImages());

    if (unit_test_module) {
      exe_module_sp = unit_test_module;
    }
  }

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

  // Attempt to deserialize the compiler flags from the AST.
  if (exe_module_sp) {
    std::string error;
    if (DeserializeCompilerFlags(*swift_ast_sp, *exe_module_sp, error,
                                 got_serialized_options) && log)
      log->Printf(
          "Attempt to load compiler options from serialized AST failed: %s",
          error.c_str());
  }

  // Now if the user fully specified the triple, let that override the one
  // we got from executable's options:
  if (target.GetArchitecture().IsFullySpecifiedTriple()) {
    swift_ast_sp->SetTriple(
        target.GetArchitecture().GetTriple().str().c_str());
  } else {
    // Always run using the Host OS triple...
    bool set_triple = false;
    PlatformSP platform_sp(target.GetPlatform());
    uint32_t major, minor, update;
    if (platform_sp &&
        !target.GetArchitecture().GetTriple().hasEnvironment() &&
        platform_sp->GetOSVersion(major, minor, update,
                                  target.GetProcessSP().get())) {
      StreamString full_triple_name;
      full_triple_name.PutCString(target.GetArchitecture().GetTriple().str());
      if (major != UINT32_MAX) {
        full_triple_name.Printf("%u", major);
        if (minor != UINT32_MAX) {
          full_triple_name.Printf(".%u", minor);
          if (update != UINT32_MAX)
            full_triple_name.Printf(".%u", update);
        }
      }
      swift_ast_sp->SetTriple(full_triple_name.GetString().data());
      set_triple = true;
    }

    if (!set_triple) {
      ModuleSP exe_module_sp(target.GetExecutableModule());
      if (exe_module_sp) {
        Status exe_error;
        SwiftASTContext *exe_swift_ctx =
            llvm::dyn_cast_or_null<SwiftASTContext>(
                exe_module_sp->GetTypeSystemForLanguage(
                    lldb::eLanguageTypeSwift));
        if (exe_swift_ctx) {
          swift_ast_sp->SetTriple(
              exe_swift_ctx->GetLanguageOptions().Target.str().c_str());
        }
      }
    }
  }

  const bool use_all_compiler_flags =
      !got_serialized_options || target.GetUseAllCompilerFlags();

  std::function<void(ModuleSP &&)> process_one_module =
      [&target, &swift_ast_sp, use_all_compiler_flags](ModuleSP &&module_sp) {
        const FileSpec &module_file = module_sp->GetFileSpec();

        std::string module_path = module_file.GetPath();

        // Add the containing framework to the framework search path.  Don't
        // do that if this is the executable module, since it might be
        // buried in some framework that we don't care about.
        if (use_all_compiler_flags &&
            target.GetExecutableModulePointer() != module_sp.get()) {
          size_t framework_offset = module_path.rfind(".framework/");

          if (framework_offset != std::string::npos) {
            // Sometimes the version of the framework that got loaded has been
            // stripped and in that case, adding it to the framework search
            // path will just short-cut a clang search that might otherwise
            // find the needed headers. So don't add these paths.
            std::string framework_path =
                module_path.substr(0, framework_offset);
            framework_path.append(".framework");
            FileSpec path_spec(framework_path, true);
            FileSpec headers_spec =
                path_spec.CopyByAppendingPathComponent("Headers");
            bool add_it = false;
            if (headers_spec.Exists())
              add_it = true;
            if (!add_it) {
              FileSpec module_spec =
                  path_spec.CopyByAppendingPathComponent("Modules");
              if (module_spec.Exists())
                add_it = true;
            }

            if (!add_it) {
              Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
              if (log)
                log->Printf("process_one_module rejecting framework path"
                            " \"%s\" as it has no Headers "
                            "or Modules subdirectories.",
                            framework_path.c_str());
            }

            if (add_it) {
              while (framework_offset && (module_path[framework_offset] != '/'))
                framework_offset--;

              if (module_path[framework_offset] == '/') {
                // framework_offset now points to the '/';

                std::string parent_path =
                    module_path.substr(0, framework_offset);

                if (strncmp(parent_path.c_str(), "/System/Library",
                            strlen("/System/Library")) &&
                    !IsDeviceSupport(parent_path.c_str())) {
                  swift_ast_sp->AddFrameworkSearchPath(parent_path.c_str());
                }
              }
            }
          }
        }

        SymbolVendor *sym_vendor = module_sp->GetSymbolVendor();

        if (sym_vendor) {
          std::vector<std::string> module_names;

          SymbolFile *sym_file = sym_vendor->GetSymbolFile();
          if (sym_file) {
            Status sym_file_error;
            SwiftASTContext *ast_context =
                llvm::dyn_cast_or_null<SwiftASTContext>(
                    sym_file->GetTypeSystemForLanguage(
                        lldb::eLanguageTypeSwift));
            if (ast_context && !ast_context->HasErrors()) {
              if (use_all_compiler_flags ||
                  target.GetExecutableModulePointer() == module_sp.get()) {
                for (size_t msi = 0,
                            mse = ast_context->GetNumModuleSearchPaths();
                     msi < mse; ++msi) {
                  const char *search_path =
                      ast_context->GetModuleSearchPathAtIndex(msi);
                  swift_ast_sp->AddModuleSearchPath(search_path);
                }

                for (size_t fsi = 0,
                            fse = ast_context->GetNumFrameworkSearchPaths();
                     fsi < fse; ++fsi) {
                  const char *search_path =
                      ast_context->GetFrameworkSearchPathAtIndex(fsi);
                  swift_ast_sp->AddFrameworkSearchPath(search_path);
                }

                for (size_t osi = 0, ose = ast_context->GetNumClangArguments();
                     osi < ose; ++osi) {
                  const char *clang_argument =
                      ast_context->GetClangArgumentAtIndex(osi);
                  swift_ast_sp->AddClangArgument(clang_argument, true);
                }
              }

              swift_ast_sp->RegisterSectionModules(*module_sp, module_names);
            }
          }
        }
      };

  for (size_t mi = 0; mi != num_images; ++mi) {
    process_one_module(target.GetImages().GetModuleAtIndex(mi));
  }

  FileSpecList &framework_search_paths = target.GetSwiftFrameworkSearchPaths();
  FileSpecList &module_search_paths = target.GetSwiftModuleSearchPaths();

  for (size_t fi = 0, fe = framework_search_paths.GetSize(); fi != fe; ++fi) {
    swift_ast_sp->AddFrameworkSearchPath(
        framework_search_paths.GetFileSpecAtIndex(fi).GetPath().c_str());
  }

  for (size_t mi = 0, me = module_search_paths.GetSize(); mi != me; ++mi) {
    swift_ast_sp->AddModuleSearchPath(
        module_search_paths.GetFileSpecAtIndex(mi).GetPath().c_str());
  }

  // Now fold any extra options we were passed. This has to be done BEFORE
  // the ClangImporter is made by calling GetClangImporter or these options
  // will be ignored.

  if (extra_options) {
    swift::CompilerInvocation &compiler_invocation =
        swift_ast_sp->GetCompilerInvocation();
    Args extra_args(extra_options);
    llvm::ArrayRef<const char *> extra_args_ref(extra_args.GetArgumentVector(),
                                                extra_args.GetArgumentCount());
    compiler_invocation.parseArgs(extra_args_ref,
                                  swift_ast_sp->GetDiagnosticEngine());
  }

  // Apply source path remappings ofund in the target settings.
  swift_ast_sp->RemapClangImporterOptions(target.GetSourcePathMap());

  // This needs to happen once all the import paths are set, or otherwise no
  // modules will be found.
  if (!swift_ast_sp->GetClangImporter()) {
    if (log) {
      log->Printf("((Target*)%p)->GetSwiftASTContext() returning NULL - "
                  "couldn't create a ClangImporter",
                  &target);
    }

    return TypeSystemSP();
  }

  if (log) {
    log->Printf("((Target*)%p)->GetSwiftASTContext() = %p", &target,
                swift_ast_sp.get());
    swift_ast_sp->DumpConfiguration(log);
  }

  if (swift_ast_sp->HasFatalErrors()) {
    swift_ast_sp->m_error.SetErrorStringWithFormat(
        "Error creating target Swift AST context: %s",
        swift_ast_sp->GetFatalErrors().AsCString());
    return lldb::TypeSystemSP();
  }

  {
    const bool can_create = true;
    if (!swift_ast_sp->m_ast_context_ap->getStdlibModule(can_create)) {
      // We need to be able to load the standard library!
      return lldb::TypeSystemSP();
    }
  }

  return swift_ast_sp;
}

void SwiftASTContext::EnumerateSupportedLanguages(
    std::set<lldb::LanguageType> &languages_for_types,
    std::set<lldb::LanguageType> &languages_for_expressions) {
  static std::vector<lldb::LanguageType> s_supported_languages_for_types(
      {lldb::eLanguageTypeSwift});

  static std::vector<lldb::LanguageType> s_supported_languages_for_expressions(
      {lldb::eLanguageTypeSwift});

  languages_for_types.insert(s_supported_languages_for_types.begin(),
                             s_supported_languages_for_types.end());
  languages_for_expressions.insert(
      s_supported_languages_for_expressions.begin(),
      s_supported_languages_for_expressions.end());
}

static lldb::TypeSystemSP CreateTypeSystemInstance(lldb::LanguageType language,
                                                   Module *module,
                                                   Target *target,
                                                   const char *extra_options) {
  // This should be called with either a target or a module.
  if (module) {
    assert(!target);
    assert(StringRef(extra_options).empty());
    return SwiftASTContext::CreateInstance(language, *module);
  } else if (target) {
    assert(!module);
    return SwiftASTContext::CreateInstance(language, *target, extra_options);
  }
}

void SwiftASTContext::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(), "swift AST context plug-in",
      CreateTypeSystemInstance, EnumerateSupportedLanguages);
}

void SwiftASTContext::Terminate() {
  PluginManager::UnregisterPlugin(CreateTypeSystemInstance);
}

bool SwiftASTContext::SupportsLanguage(lldb::LanguageType language) {
  return SwiftASTContextSupportsLanguage(language);
}

Status SwiftASTContext::IsCompatible() { return GetFatalErrors(); }

Status SwiftASTContext::GetFatalErrors() {
  Status error;
  if (HasFatalErrors()) {
    error = m_fatal_errors;
    if (error.Success())
      error.SetErrorString("unknown fatal error in swift AST context");
  }
  return error;
}

swift::IRGenOptions &SwiftASTContext::GetIRGenOptions() {
  return m_compiler_invocation_ap->getIRGenOptions();
}

std::string SwiftASTContext::GetTriple() const {
  return m_compiler_invocation_ap->getTargetTriple();
}

// Conditions a triple string to be safe for use with Swift.
// Right now this just strips the Haswell marker off the CPU name.
// TODO make Swift more robust
static std::string GetSwiftFriendlyTriple(const std::string &triple) {
  static std::string s_x86_64h("x86_64h");
  static std::string::size_type s_x86_64h_size = s_x86_64h.size();

  if (0 == triple.compare(0, s_x86_64h_size, s_x86_64h)) {
    std::string fixed_triple("x86_64");
    fixed_triple.append(
        triple.substr(s_x86_64h_size, triple.size() - s_x86_64h_size));
    return fixed_triple;
  }
  return triple;
}

bool SwiftASTContext::SetTriple(const char *triple_cstr, Module *module) {
  if (triple_cstr && triple_cstr[0]) {
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    // We can change our triple up until we create the swift::irgen::IRGenModule
    if (m_ir_gen_module_ap.get() == NULL) {
      std::string raw_triple(triple_cstr);
      std::string triple = GetSwiftFriendlyTriple(raw_triple);

      llvm::Triple llvm_triple(triple);
      const unsigned unspecified = 0;
      // If the OS version is unspecified, do fancy things
      if (llvm_triple.getOSMajorVersion() == unspecified) {
        // If a triple is "<arch>-apple-darwin" change it to be
        // "<arch>-apple-macosx" otherwise the major and minor OS version we
        // append below would be wrong.
        if (llvm_triple.getVendor() == llvm::Triple::VendorType::Apple &&
            llvm_triple.getOS() == llvm::Triple::OSType::Darwin) {
          llvm_triple.setOS(llvm::Triple::OSType::MacOSX);
          triple = llvm_triple.str();
        }

        // Append the min OS to the triple if we have a target
        ModuleSP module_sp;
        if (module == NULL) {
          TargetSP target_sp(m_target_wp.lock());
          if (target_sp) {
            module_sp = target_sp->GetExecutableModule();
            if (module_sp)
              module = module_sp.get();
          }
        }

        if (module) {
          ObjectFile *objfile = module->GetObjectFile();
          uint32_t versions[3];
          if (objfile) {
            uint32_t num_versions = objfile->GetMinimumOSVersion(versions, 3);
            StreamString strm;
            if (num_versions) {
              for (uint32_t v = 0; v < 3; ++v) {
                if (v < num_versions) {
                  if (versions[v] == UINT32_MAX)
                    versions[v] = 0;
                } else
                  versions[v] = 0;
              }
              strm.Printf("%s%u.%u.%u", llvm_triple.getOSName().str().c_str(),
                          versions[0], versions[1], versions[2]);
              llvm_triple.setOSName(strm.GetString());
              triple = llvm_triple.str();
            }
          }
        }
      }
      if (log)
        log->Printf("%p: SwiftASTContext::SetTriple('%s') setting to '%s'%s",
                    this, triple_cstr, triple.c_str(),
                    m_target_wp.lock() ? " (target)" : "");
      m_compiler_invocation_ap->setTargetTriple(triple);

      // Every time the triple is changed the LangOpts must be
      // updated too, because Swift default-initializes the
      // EnableObjCInterop flag based on the triple.
      GetLanguageOptions().EnableObjCInterop = llvm_triple.isOSDarwin();

      return true;
    } else {
      if (log)
        log->Printf("%p: SwiftASTContext::SetTriple('%s') ignoring triple "
                    "since the IRGenModule has already been created",
                    this, triple_cstr);
    }
  }
  return false;
}

static std::string GetXcodeContentsPath() {
  const char substr[] = ".app/Contents/";

  // First, try based on the current shlib's location

  {
    FileSpec fspec;

    if (HostInfo::GetLLDBPath(ePathTypeLLDBShlibDir, fspec)) {
      std::string path_to_shlib = fspec.GetPath();
      size_t pos = path_to_shlib.rfind(substr);
      if (pos != std::string::npos) {
        path_to_shlib.erase(pos + strlen(substr));
        return path_to_shlib;
      }
    }
  }

  // Fall back to using xcrun

  {
    int status = 0;
    int signo = 0;
    std::string output;
    const char *command = "xcrun -sdk macosx --show-sdk-path";
    lldb_private::Status error = Host::RunShellCommand(
        command, // shell command to run
        NULL,    // current working directory
        &status, // Put the exit status of the process in here
        &signo,  // Put the signal that caused the process to exit in here
        &output, // Get the output from the command and place it in this string
        3);      // Timeout in seconds to wait for shell program to finish
    if (status == 0 && !output.empty()) {
      size_t first_non_newline = output.find_last_not_of("\r\n");
      if (first_non_newline != std::string::npos) {
        output.erase(first_non_newline + 1);
      }

      size_t pos = output.rfind(substr);
      if (pos != std::string::npos) {
        output.erase(pos + strlen(substr));
        return output;
      }
    }
  }

  return std::string();
}

static std::string GetCurrentToolchainPath() {
  const char substr[] = ".xctoolchain/";

  {
    FileSpec fspec;

    if (HostInfo::GetLLDBPath(ePathTypeLLDBShlibDir, fspec)) {
      std::string path_to_shlib = fspec.GetPath();
      size_t pos = path_to_shlib.rfind(substr);
      if (pos != std::string::npos) {
        path_to_shlib.erase(pos + strlen(substr));
        return path_to_shlib;
      }
    }
  }

  return std::string();
}

static std::string GetCurrentCLToolsPath() {
  const char substr[] = "/CommandLineTools/";

  {
    FileSpec fspec;

    if (HostInfo::GetLLDBPath(ePathTypeLLDBShlibDir, fspec)) {
      std::string path_to_shlib = fspec.GetPath();
      size_t pos = path_to_shlib.rfind(substr);
      if (pos != std::string::npos) {
        path_to_shlib.erase(pos + strlen(substr));
        return path_to_shlib;
      }
    }
  }

  return std::string();
}

namespace {

enum class SDKType {
  MacOSX = 0,
  iPhoneSimulator,
  iPhoneOS,
  AppleTVSimulator,
  AppleTVOS,
  WatchSimulator,
  watchOS,
  numSDKTypes,
  unknown = -1
};

const char *const sdk_strings[] = {
    "macosx",    "iphonesimulator", "iphoneos", "appletvsimulator",
    "appletvos", "watchsimulator",  "watchos",
};

struct SDKEnumeratorInfo {
  FileSpec found_path;
  SDKType sdk_type;
  uint32_t least_major;
  uint32_t least_minor;
};

static bool SDKSupportsSwift(const FileSpec &sdk_path, SDKType desired_type) {
  ConstString last_path_component = sdk_path.GetLastPathComponent();

  if (last_path_component) {
    const llvm::StringRef sdk_name_raw = last_path_component.GetStringRef();
    std::string sdk_name_lower = sdk_name_raw.lower();
    const llvm::StringRef sdk_name(sdk_name_lower);

    llvm::StringRef version_part;

    SDKType sdk_type = SDKType::unknown;

    if (desired_type == SDKType::unknown) {
      for (int i = (int)SDKType::MacOSX; i < (int)SDKType::numSDKTypes; ++i) {
        if (sdk_name.startswith(sdk_strings[i])) {
          version_part = sdk_name.drop_front(strlen(sdk_strings[i]));
          sdk_type = (SDKType)i;
          break;
        }
      }

      // For non-Darwin SDKs assume Swift is supported
      if (sdk_type == SDKType::unknown)
        return true;
    } else {
      if (sdk_name.startswith(sdk_strings[(int)desired_type])) {
        version_part =
            sdk_name.drop_front(strlen(sdk_strings[(int)desired_type]));
        sdk_type = desired_type;
      } else {
        return false;
      }
    }

    const size_t major_dot_offset = version_part.find('.');
    if (major_dot_offset == llvm::StringRef::npos)
      return false;

    const llvm::StringRef major_version =
        version_part.slice(0, major_dot_offset);
    const llvm::StringRef minor_part =
        version_part.drop_front(major_dot_offset + 1);

    const size_t minor_dot_offset = minor_part.find('.');
    if (minor_dot_offset == llvm::StringRef::npos)
      return false;

    const llvm::StringRef minor_version = minor_part.slice(0, minor_dot_offset);

    unsigned int major = 0;
    unsigned int minor = 0;

    if (major_version.getAsInteger(10, major))
      return false;

    if (minor_version.getAsInteger(10, minor))
      return false;

    switch (sdk_type) {
    case SDKType::MacOSX:
      if (major > 10 || (major == 10 && minor >= 10))
        return true;
      break;
    case SDKType::iPhoneOS:
    case SDKType::iPhoneSimulator:
      if (major >= 8)
        return true;
      break;
    case SDKType::AppleTVSimulator:
    case SDKType::AppleTVOS:
      if (major >= 9)
        return true;
      break;
    case SDKType::WatchSimulator:
    case SDKType::watchOS:
      if (major >= 2)
        return true;
      break;
    default:
      return false;
    }
  }

  return false;
}

FileSpec::EnumerateDirectoryResult
DirectoryEnumerator(void *baton, llvm::sys::fs::file_type file_type,
                    const FileSpec &spec) {
  SDKEnumeratorInfo *enumerator_info = static_cast<SDKEnumeratorInfo *>(baton);

  if (SDKSupportsSwift(spec, enumerator_info->sdk_type)) {
    enumerator_info->found_path = spec;
    return FileSpec::EnumerateDirectoryResult::eEnumerateDirectoryResultNext;
  }

  return FileSpec::EnumerateDirectoryResult::eEnumerateDirectoryResultNext;
};

static ConstString EnumerateSDKsForVersion(FileSpec sdks_spec, SDKType sdk_type,
                                           uint32_t least_major,
                                           uint32_t least_minor) {
  if (!IsDirectory(sdks_spec))
    return ConstString();

  const bool find_directories = true;
  const bool find_files = false;
  const bool find_other = true; // include symlinks

  SDKEnumeratorInfo enumerator_info;

  enumerator_info.sdk_type = sdk_type;
  enumerator_info.least_major = least_major;
  enumerator_info.least_minor = least_minor;

  FileSpec::EnumerateDirectory(sdks_spec.GetPath().c_str(), find_directories,
                               find_files, find_other, DirectoryEnumerator,
                               &enumerator_info);

  if (IsDirectory(enumerator_info.found_path))
    return ConstString(enumerator_info.found_path.GetPath());
  else
    return ConstString();
}

static ConstString GetSDKDirectory(SDKType sdk_type, uint32_t least_major,
                                   uint32_t least_minor) {
  if (sdk_type != SDKType::MacOSX) {
    // Look inside Xcode for the required installed iOS SDK version

    std::string sdks_path = GetXcodeContentsPath();
    sdks_path.append("Developer/Platforms");

    if (sdk_type == SDKType::iPhoneSimulator) {
      sdks_path.append("/iPhoneSimulator.platform/");
    } else if (sdk_type == SDKType::AppleTVSimulator) {
      sdks_path.append("/AppleTVSimulator.platform/");
    } else if (sdk_type == SDKType::AppleTVOS) {
      sdks_path.append("/AppleTVOS.platform/");
    } else if (sdk_type == SDKType::WatchSimulator) {
      sdks_path.append("/WatchSimulator.platform/");
    } else if (sdk_type == SDKType::watchOS) {
      // For now, we need to be prepared to handle either capitalization of this
      // path.

      std::string WatchOS_candidate_path = sdks_path + "/WatchOS.platform/";
      if (IsDirectory(FileSpec(WatchOS_candidate_path.c_str(), false))) {
        sdks_path = WatchOS_candidate_path;
      } else {
        std::string watchOS_candidate_path = sdks_path + "/watchOS.platform/";
        if (IsDirectory(FileSpec(watchOS_candidate_path.c_str(), false))) {
          sdks_path = watchOS_candidate_path;
        } else {
          return ConstString();
        }
      }
    } else {
      sdks_path.append("/iPhoneOS.platform/");
    }

    sdks_path.append("Developer/SDKs/");

    FileSpec sdks_spec(sdks_path.c_str(), false);

    return EnumerateSDKsForVersion(sdks_spec, sdk_type, least_major,
                                   least_major);
  }

  // The SDK type is Mac OS X

  uint32_t major = 0;
  uint32_t minor = 0;
  uint32_t update = 0;

  if (!HostInfo::GetOSVersion(major, minor, update))
    return ConstString();

  // If there are minimum requirements that exceed the current OS, apply those

  if (least_major > major) {
    major = least_major;
    minor = least_minor;
  } else if (least_major == major) {
    if (least_minor > minor)
      minor = least_minor;
  }

  typedef std::map<uint64_t, ConstString> SDKDirectoryCache;
  static std::mutex g_mutex;
  static SDKDirectoryCache g_sdk_cache;
  std::lock_guard<std::mutex> locker(g_mutex);
  const uint64_t major_minor = (uint64_t)major << 32 | (uint64_t)minor;
  SDKDirectoryCache::iterator pos = g_sdk_cache.find(major_minor);
  if (pos != g_sdk_cache.end())
    return pos->second;

  FileSpec fspec;
  std::string xcode_contents_path;

  if (xcode_contents_path.empty())
    xcode_contents_path = GetXcodeContentsPath();

  if (!xcode_contents_path.empty()) {
    StreamString sdk_path;
    sdk_path.Printf(
        "%sDeveloper/Platforms/MacOSX.platform/Developer/SDKs/MacOSX%u.%u.sdk",
        xcode_contents_path.c_str(), major, minor);
    fspec.SetFile(sdk_path.GetString(), false);
    if (fspec.Exists()) {
      ConstString path(sdk_path.GetString());
      // Cache results
      g_sdk_cache[major_minor] = path;
      return path;
    } else if ((least_major != major) || (least_minor != minor)) {
      // Try the required SDK
      sdk_path.Clear();
      sdk_path.Printf("%sDeveloper/Platforms/MacOSX.platform/Developer/SDKs/"
                      "MacOSX%u.%u.sdk",
                      xcode_contents_path.c_str(), least_major, least_minor);
      fspec.SetFile(sdk_path.GetString(), false);
      if (fspec.Exists()) {
        ConstString path(sdk_path.GetString());
        // Cache results
        g_sdk_cache[major_minor] = path;
        return path;
      } else {
        // Okay, we're going to do an exhaustive search for *any* SDK that has
        // an adequate version.

        std::string sdks_path = GetXcodeContentsPath();
        sdks_path.append("Developer/Platforms/MacOSX.platform/Developer/SDKs");

        FileSpec sdks_spec(sdks_path.c_str(), false);

        ConstString sdk_path = EnumerateSDKsForVersion(
            sdks_spec, sdk_type, least_major, least_major);

        if (sdk_path) {
          g_sdk_cache[major_minor] = sdk_path;
          return sdk_path;
        }
      }
    }
  }

  // Cache results
  g_sdk_cache[major_minor] = ConstString();
  return ConstString();
}

static ConstString GetResourceDir() {
  static ConstString g_cached_resource_dir;
  static std::once_flag g_once_flag;
  std::call_once(g_once_flag, []() {
    Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    // First, check if there's something in our bundle
    {
      FileSpec swift_dir_spec;
      if (HostInfo::GetLLDBPath(ePathTypeSwiftDir, swift_dir_spec)) {
        if (log)
          log->Printf("%s: trying ePathTypeSwiftDir: %s", __FUNCTION__,
                      swift_dir_spec.GetCString());
        // We can't just check for the Swift directory, because that
        // always exists.  We have to look for "clang" inside that.
        FileSpec swift_clang_dir_spec = swift_dir_spec;
        swift_clang_dir_spec.AppendPathComponent("clang");

        if (IsDirectory(swift_clang_dir_spec)) {
          g_cached_resource_dir = ConstString(swift_dir_spec.GetPath());
          if (log)
            log->Printf("%s: found Swift resource dir via "
                        "ePathTypeSwiftDir': %s",
                        __FUNCTION__, g_cached_resource_dir.AsCString());
          return;
        }
      }
    }

    // Nothing in our bundle. Are we in a toolchain that has its own Swift
    // compiler resource dir?

    {
      std::string xcode_toolchain_path = GetCurrentToolchainPath();
      if (log)
        log->Printf("%s: trying toolchain path: %s", __FUNCTION__,
                    xcode_toolchain_path.c_str());

      if (!xcode_toolchain_path.empty()) {
        xcode_toolchain_path.append("usr/lib/swift");
        if (log)
          log->Printf("%s: trying toolchain-based lib path: %s", __FUNCTION__,
                      xcode_toolchain_path.c_str());

        if (IsDirectory(FileSpec(xcode_toolchain_path, false))) {
          g_cached_resource_dir = ConstString(xcode_toolchain_path);
          if (log)
            log->Printf("%s: found Swift resource dir via "
                        "toolchain path + 'usr/lib/swift': %s",
                        __FUNCTION__, g_cached_resource_dir.AsCString());
          return;
        }
      }
    }

    // We're not in a toolchain that has one. Use the Xcode default toolchain.

    {
      std::string xcode_contents_path = GetXcodeContentsPath();
      if (log)
        log->Printf("%s: trying Xcode path: %s", __FUNCTION__,
                    xcode_contents_path.c_str());

      if (!xcode_contents_path.empty()) {
        xcode_contents_path.append("Developer/Toolchains/"
                                   "XcodeDefault.xctoolchain"
                                   "/usr/lib/swift");
        if (log)
          log->Printf("%s: trying Xcode-based lib path: %s", __FUNCTION__,
                      xcode_contents_path.c_str());

        if (IsDirectory(FileSpec(xcode_contents_path, false))) {
          g_cached_resource_dir = ConstString(xcode_contents_path);
          if (log)
            log->Printf("%s: found Swift resource dir via "
                        "Xcode contents path + default toolchain "
                        "relative dir: %s",
                        __FUNCTION__, g_cached_resource_dir.AsCString());
          return;
        }
      }
    }

    // We're not in Xcode. We might be in the command-line tools.

    {
      std::string cl_tools_path = GetCurrentCLToolsPath();
      if (log)
        log->Printf("%s: trying command-line tools path: %s", __FUNCTION__,
                    cl_tools_path.c_str());

      if (!cl_tools_path.empty()) {
        cl_tools_path.append("usr/lib/swift");
        if (log)
          log->Printf("%s: trying command-line tools-based lib "
                      "path: %s",
                      __FUNCTION__, cl_tools_path.c_str());

        if (IsDirectory(FileSpec(cl_tools_path, false))) {
          g_cached_resource_dir = ConstString(cl_tools_path);
          if (log)
            log->Printf("%s: found Swift resource dir via "
                        "command-line tools path + "
                        "usr/lib/swift: %s",
                        __FUNCTION__, g_cached_resource_dir.AsCString());
          return;
        }
      }
    }

    // We might be in the build-dir configuration for a build-script-driven
    // LLDB build, which has the Swift build dir as a sibling directory
    // to the lldb build dir.  This looks much different than the install-
    // dir layout that the previous checks would try.
    {
      FileSpec faux_swift_dir_spec;
      if (HostInfo::GetLLDBPath(ePathTypeSwiftDir, faux_swift_dir_spec)) {
// We can't use a C++11 stdlib regex feature here because it
// doesn't work on Ubuntu 14.04 x86_64.  Once we don't care
// about supporting that anymore, let's pull the code below
// back in since it is a simpler implementation using
// std::regex.
#if 0
                // Let's try to regex this.
                // We're looking for /some/path/lldb-{os}-{arch}, and want to
                // build the following:
                //    /some/path/swift-{os}-{arch}/lib/swift/{os}/{arch}
                // In a match, these are the following assignments for
                // backrefs:
                //   $1 - first part of path before swift build dir
                //   $2 - the host OS path separator character
                //   $3 - all the stuff that should come after changing
                //        lldb to swift for the lib dir.
                auto match_regex =
                    std::regex("^(.+([/\\\\]))lldb-(.+)$");
                const std::string replace_format = "$1swift-$3";
                const std::string faux_swift_dir =
                    faux_swift_dir_spec.GetCString();
                const std::string build_tree_resource_dir =
                    std::regex_replace(faux_swift_dir, match_regex,
                                       replace_format);
#else
                std::string build_tree_resource_dir;
                const std::string faux_swift_dir =
                    faux_swift_dir_spec.GetCString();

                // Find something that matches lldb- (particularly,
                // the last one).
                const std::string lldb_dash("lldb-");
                auto lldb_pos = faux_swift_dir.rfind(lldb_dash);
                if ((lldb_pos != std::string::npos) &&
                    (lldb_pos > 0) &&
                    ((faux_swift_dir[lldb_pos - 1] == '\\') ||
                     (faux_swift_dir[lldb_pos - 1] == '/')))
                {
                    // We found something that matches ^.+[/\\]lldb-.+$
                    std::ostringstream stream;
                    // Take everything before lldb- (the path leading up to
                    // the lldb dir).
                    stream << faux_swift_dir.substr(0, lldb_pos);

                    // replace lldb- with swift-.
                    stream << "swift-";

                    // and now tack on the same components from after
                    // the lldb- part.
                    stream << faux_swift_dir.substr(lldb_pos +
                                                    lldb_dash.length());
                    const std::string build_tree_resource_dir = stream.str();
                    if (log)
                        log->Printf("%s: trying ePathTypeSwiftDir regex-based "
                                    "build dir: %s",
                                    __FUNCTION__,
                                    build_tree_resource_dir.c_str());
                    FileSpec swift_resource_dir_spec(
                        build_tree_resource_dir.c_str(), false);
                    if (IsDirectory(swift_resource_dir_spec))
                    {
                        g_cached_resource_dir =
                            ConstString(swift_resource_dir_spec.GetPath());
                        if (log)
                            log->Printf("%s: found Swift resource dir via "
                                        "ePathTypeSwiftDir + inferred "
                                        "build-tree dir: %s", __FUNCTION__,
                                        g_cached_resource_dir.AsCString());
                        return;
                    }
                }
#endif
      }
    }

    // We failed to find a reasonable Swift resource dir.
    if (log)
      log->Printf("%s: failed to find a Swift resource dir", __FUNCTION__);
  });

  return g_cached_resource_dir;
}

} // anonymous namespace

swift::CompilerInvocation &SwiftASTContext::GetCompilerInvocation() {
  return *m_compiler_invocation_ap;
}

swift::SourceManager &SwiftASTContext::GetSourceManager() {
  if (m_source_manager_ap.get() == NULL)
    m_source_manager_ap.reset(new swift::SourceManager());
  return *m_source_manager_ap;
}

swift::LangOptions &SwiftASTContext::GetLanguageOptions() {
  return GetCompilerInvocation().getLangOptions();
}

swift::DiagnosticEngine &SwiftASTContext::GetDiagnosticEngine() {
  if (m_diagnostic_engine_ap.get() == NULL)
    m_diagnostic_engine_ap.reset(
        new swift::DiagnosticEngine(GetSourceManager()));
  return *m_diagnostic_engine_ap;
}

// This code comes from CompilerInvocation.cpp (setRuntimeResourcePath)

static void ConfigureResourceDirs(swift::CompilerInvocation &invocation,
                                  FileSpec resource_dir, llvm::Triple triple) {
  // Make sure the triple is right:
  invocation.setTargetTriple(triple.str());
  invocation.setRuntimeResourcePath(resource_dir.GetPath().c_str());
}

swift::SILOptions &SwiftASTContext::GetSILOptions() {
  return GetCompilerInvocation().getSILOptions();
}

bool SwiftASTContext::TargetHasNoSDK() {
  llvm::Triple triple(GetTriple());

  switch (triple.getOS()) {
  case llvm::Triple::OSType::MacOSX:
  case llvm::Triple::OSType::Darwin:
  case llvm::Triple::OSType::IOS:
    return false;
  default:
    return true;
  }
}

swift::ClangImporterOptions &SwiftASTContext::GetClangImporterOptions() {
  swift::ClangImporterOptions &clang_importer_options =
      GetCompilerInvocation().getClangImporterOptions();
  if (!m_initialized_clang_importer_options) {
    m_initialized_clang_importer_options = true;

    // Set the Clang module search path.
    llvm::SmallString<128> path;
    auto props = ModuleList::GetGlobalModuleListProperties();
    props.GetClangModulesCachePath().GetPath(path);
    clang_importer_options.ModuleCachePath = path.str();

    FileSpec clang_dir_spec;
    if (HostInfo::GetLLDBPath(ePathTypeClangDir, clang_dir_spec))
      clang_importer_options.OverrideResourceDir =
          std::move(clang_dir_spec.GetPath());
    clang_importer_options.DebuggerSupport = true;
  }
  return clang_importer_options;
}

swift::SearchPathOptions &SwiftASTContext::GetSearchPathOptions() {
  swift::SearchPathOptions &search_path_opts =
      GetCompilerInvocation().getSearchPathOptions();

  if (!m_initialized_search_path_options) {
    m_initialized_search_path_options = true;

    bool set_sdk = false;
    bool set_resource_dir = false;

    if (!search_path_opts.SDKPath.empty()) {
      FileSpec provided_sdk_path(search_path_opts.SDKPath, false);
      if (provided_sdk_path.Exists()) {
        // We don't check whether the SDK supports swift because we figure if
        // someone is passing this to us on the command line (e.g., for the
        // REPL), they probably know what they're doing.

        set_sdk = true;
      }
    } else if (!m_platform_sdk_path.empty()) {
      FileSpec platform_sdk(m_platform_sdk_path.c_str(), false);

      if (platform_sdk.Exists() &&
          SDKSupportsSwift(platform_sdk, SDKType::unknown)) {
        search_path_opts.SDKPath = m_platform_sdk_path.c_str();
        set_sdk = true;
      }
    }

    llvm::Triple triple(GetTriple());

    if (!m_resource_dir.empty()) {
      FileSpec resource_dir(m_resource_dir.c_str(), false);

      if (resource_dir.Exists()) {
        ConfigureResourceDirs(GetCompilerInvocation(), resource_dir, triple);
        set_resource_dir = true;
      }
    }

    if (!set_sdk) {
      if (triple.getOS() == llvm::Triple::OSType::MacOSX ||
          triple.getOS() == llvm::Triple::OSType::Darwin) {
        search_path_opts.SDKPath = GetSDKDirectory(SDKType::MacOSX, 10, 10)
                                       .AsCString(""); // we need the 10.10 SDK
      } else if (triple.getOS() == llvm::Triple::OSType::IOS) {
        if (triple.getArchName().startswith("arm")) {
          search_path_opts.SDKPath =
              GetSDKDirectory(SDKType::iPhoneOS, 8, 0).AsCString("");
        } else {
          search_path_opts.SDKPath =
              GetSDKDirectory(SDKType::iPhoneSimulator, 8, 0).AsCString("");
        }
      }
      // explicitly leave the SDKPath blank on other platforms
    }

    if (!set_resource_dir) {
      FileSpec resource_dir(::GetResourceDir().AsCString(""), false);
      if (resource_dir.Exists())
        ConfigureResourceDirs(GetCompilerInvocation(), resource_dir, triple);
    }
  }

  return search_path_opts;
}

namespace lldb_private {

class ANSIColorStringStream : public llvm::raw_string_ostream {
public:
  ANSIColorStringStream(bool colorize)
      : llvm::raw_string_ostream(m_buffer), m_colorize(colorize) {}
  /// Changes the foreground color of text that will be output from this point
  /// forward.
  /// @param Color ANSI color to use, the special SAVEDCOLOR can be used to
  /// change only the bold attribute, and keep colors untouched
  /// @param Bold bold/brighter text, default false
  /// @param BG if true change the background, default: change foreground
  /// @returns itself so it can be used within << invocations
  virtual raw_ostream &changeColor(enum Colors colors, bool bold = false,
                                   bool bg = false) {
    if (llvm::sys::Process::ColorNeedsFlush())
      flush();
    const char *colorcode;
    if (colors == SAVEDCOLOR)
      colorcode = llvm::sys::Process::OutputBold(bg);
    else
      colorcode = llvm::sys::Process::OutputColor(colors, bold, bg);
    if (colorcode) {
      size_t len = strlen(colorcode);
      write(colorcode, len);
    }
    return *this;
  }

  /// Resets the colors to terminal defaults. Call this when you are done
  /// outputting colored text, or before program exit.
  virtual raw_ostream &resetColor() {
    if (llvm::sys::Process::ColorNeedsFlush())
      flush();
    const char *colorcode = llvm::sys::Process::ResetColor();
    if (colorcode) {
      size_t len = strlen(colorcode);
      write(colorcode, len);
    }
    return *this;
  }

  /// Reverses the forground and background colors.
  virtual raw_ostream &reverseColor() {
    if (llvm::sys::Process::ColorNeedsFlush())
      flush();
    const char *colorcode = llvm::sys::Process::OutputReverse();
    if (colorcode) {
      size_t len = strlen(colorcode);
      write(colorcode, len);
    }
    return *this;
  }

  /// This function determines if this stream is connected to a "tty" or
  /// "console" window. That is, the output would be displayed to the user
  /// rather than being put on a pipe or stored in a file.
  virtual bool is_displayed() const { return m_colorize; }

  /// This function determines if this stream is displayed and supports colors.
  virtual bool has_colors() const { return m_colorize; }

protected:
  std::string m_buffer;
  bool m_colorize;
};

class StoringDiagnosticConsumer : public swift::DiagnosticConsumer {
public:
  StoringDiagnosticConsumer(SwiftASTContext &ast_context)
      : m_ast_context(ast_context), m_diagnostics(), m_num_errors(0),
        m_colorize(false) {
    m_ast_context.GetDiagnosticEngine().resetHadAnyError();
    m_ast_context.GetDiagnosticEngine().addConsumer(*this);
  }

  ~StoringDiagnosticConsumer() {
    m_ast_context.GetDiagnosticEngine().takeConsumers();
  }

  virtual void handleDiagnostic(swift::SourceManager &source_mgr,
                                swift::SourceLoc source_loc,
                                swift::DiagnosticKind kind,
                                llvm::StringRef formatString,
                                llvm::ArrayRef<swift::DiagnosticArgument> formatArgs,
                                const swift::DiagnosticInfo &info) {
    llvm::StringRef bufferName = "<anonymous>";
    unsigned bufferID = 0;
    std::pair<unsigned, unsigned> line_col = {0, 0};

    llvm::SmallString<256> text;
    {
      llvm::raw_svector_ostream out(text);
      swift::DiagnosticEngine::formatDiagnosticText(out, 
                                                    formatString, 
                                                    formatArgs);
    }

    if (source_loc.isValid()) {
      bufferID = source_mgr.findBufferContainingLoc(source_loc);
      bufferName = source_mgr.getBufferIdentifierForLoc(source_loc);
      line_col = source_mgr.getLineAndColumn(source_loc);
    }

    if (line_col.first != 0) {
      ANSIColorStringStream os(m_colorize);

      // Determine what kind of diagnostic we're emitting, and whether we want
      // to use its fixits:
      bool use_fixits = false;
      llvm::SourceMgr::DiagKind source_mgr_kind;
      switch (kind) {
      default:
      case swift::DiagnosticKind::Error:
        source_mgr_kind = llvm::SourceMgr::DK_Error;
        use_fixits = true;
        break;
      case swift::DiagnosticKind::Warning:
        source_mgr_kind = llvm::SourceMgr::DK_Warning;
        break;

      case swift::DiagnosticKind::Note:
        source_mgr_kind = llvm::SourceMgr::DK_Note;
        break;
      }

      // Translate ranges.
      llvm::SmallVector<llvm::SMRange, 2> ranges;
      for (auto R : info.Ranges)
        ranges.push_back(getRawRange(source_mgr, R));

      // Translate fix-its.
      llvm::SmallVector<llvm::SMFixIt, 2> fix_its;
      for (swift::DiagnosticInfo::FixIt F : info.FixIts)
        fix_its.push_back(getRawFixIt(source_mgr, F));

      // Display the diagnostic.

      auto message = source_mgr.GetMessage(source_loc, source_mgr_kind, text,
                                           ranges, fix_its);
      source_mgr.getLLVMSourceMgr().PrintMessage(os, message);

      // Use the llvm::raw_string_ostream::str() accessor as it will flush
      // the stream into our "message" and return us a reference to "message".
      std::string &message_ref = os.str();

      if (message_ref.empty())
        m_diagnostics.push_back(RawDiagnostic(
            text.str(), kind, bufferName, bufferID, line_col.first,
            line_col.second,
            use_fixits ? info.FixIts
                       : llvm::ArrayRef<swift::Diagnostic::FixIt>()));
      else
        m_diagnostics.push_back(RawDiagnostic(
            message_ref, kind, bufferName, bufferID, line_col.first,
            line_col.second,
            use_fixits ? info.FixIts
                       : llvm::ArrayRef<swift::Diagnostic::FixIt>()));
    } else {
      m_diagnostics.push_back(RawDiagnostic(
          text.str(), kind, bufferName, bufferID, line_col.first,
          line_col.second, llvm::ArrayRef<swift::Diagnostic::FixIt>()));
    }

    if (kind == swift::DiagnosticKind::Error)
      m_num_errors++;
  }

  void Clear() {
    m_ast_context.GetDiagnosticEngine().resetHadAnyError();
    m_diagnostics.clear();
    m_num_errors = 0;
  }

  unsigned NumErrors() {
    if (m_num_errors)
      return m_num_errors;
    else if (m_ast_context.GetASTContext()->hadError())
      return 1;
    else
      return 0;
  }

  static DiagnosticSeverity SeverityForKind(swift::DiagnosticKind kind) {
    switch (kind) {
    case swift::DiagnosticKind::Error:
      return eDiagnosticSeverityError;
    case swift::DiagnosticKind::Warning:
      return eDiagnosticSeverityWarning;
    case swift::DiagnosticKind::Note:
      return eDiagnosticSeverityRemark;
    }

    llvm_unreachable("Unhandled DiagnosticKind in switch.");
  }

  void PrintDiagnostics(DiagnosticManager &diagnostic_manager,
                        uint32_t bufferID = UINT32_MAX, uint32_t first_line = 0,
                        uint32_t last_line = UINT32_MAX,
                        uint32_t line_offset = 0) {
    bool added_one_diagnostic = false;
    for (const RawDiagnostic &diagnostic : m_diagnostics) {
      // We often make expressions and wrap them in some code.
      // When we see errors we want the line numbers to be correct so
      // we correct them below. LLVM stores in SourceLoc objects as character
      // offsets so there is no way to get LLVM to move its error line numbers
      // around by adjusting the source location, we must do it manually. We
      // also want to use the same error formatting as LLVM and Clang, so we
      // must muck with the string.

      const DiagnosticSeverity severity = SeverityForKind(diagnostic.kind);
      const DiagnosticOrigin origin = eDiagnosticOriginSwift;

      if (first_line > 0 && bufferID != UINT32_MAX &&
          diagnostic.bufferID == bufferID && !diagnostic.bufferName.empty()) {
        // Make sure the error line is in range
        if (diagnostic.line >= first_line && diagnostic.line <= last_line) {
          // Need to remap the error/warning to a different line
          StreamString match;
          match.Printf("%s:%u:", diagnostic.bufferName.str().c_str(),
                       diagnostic.line);
          const size_t match_len = match.GetString().size();
          size_t match_pos = diagnostic.description.find(match.GetString());
          if (match_pos != std::string::npos) {
            // We have some <file>:<line>:" instances that need to be updated
            StreamString fixed_description;
            size_t start_pos = 0;
            do {
              if (match_pos > start_pos)
                fixed_description.Printf(
                    "%s", diagnostic.description.substr(start_pos, match_pos)
                              .c_str());
              fixed_description.Printf("%s:%u:",
                                       diagnostic.bufferName.str().c_str(),
                                       diagnostic.line - first_line +
                                           line_offset + 1);
              start_pos = match_pos + match_len;
              match_pos =
                  diagnostic.description.find(match.GetString(), start_pos);
            } while (match_pos != std::string::npos);

            // Append any last remainging text
            if (start_pos < diagnostic.description.size())
              fixed_description.Printf(
                  "%s",
                  diagnostic.description.substr(start_pos,
                                                diagnostic.description.size() -
                                                    start_pos)
                      .c_str());

            SwiftDiagnostic *new_diagnostic =
                new SwiftDiagnostic(fixed_description.GetString().data(),
                                    severity, origin, bufferID);
            for (auto fixit : diagnostic.fixits)
              new_diagnostic->AddFixIt(fixit);

            diagnostic_manager.AddDiagnostic(new_diagnostic);
            added_one_diagnostic = true;

            continue;
          }
        }
      }
    }

    // In general, we don't want to see diagnostics from outside of the source
    // text range of the actual user expression. But if we didn't find any
    // diagnostics in the text range, it's probably because the source range was
    // not specified correctly, and we don't want to lose legit errors because
    // of that. So in that case we'll add them all here:

    if (!added_one_diagnostic) {
      // This will report diagnostic errors from outside the expression's source
      // range. Those are not interesting to users, so we only emit them in
      // debug builds.
      for (const RawDiagnostic &diagnostic : m_diagnostics) {
        const DiagnosticSeverity severity = SeverityForKind(diagnostic.kind);
        const DiagnosticOrigin origin = eDiagnosticOriginSwift;
        diagnostic_manager.AddDiagnostic(diagnostic.description.c_str(),
                                         severity, origin);
      }
    }
  }

  bool GetColorize() const { return m_colorize; }

  bool SetColorize(bool b) {
    const bool old = m_colorize;
    m_colorize = b;
    return old;
  }

private:
  // We don't currently use lldb_private::Diagostic or any of the lldb
  // DiagnosticManager machinery to store diagnostics as they occur. Instead,
  // we store them in raw form using this struct, then transcode them to
  // SwiftDiagnostics in PrintDiagnostic.
  struct RawDiagnostic {
    RawDiagnostic(std::string in_desc, swift::DiagnosticKind in_kind,
                  llvm::StringRef in_bufferName, unsigned in_bufferID,
                  uint32_t in_line, uint32_t in_column,
                  llvm::ArrayRef<swift::Diagnostic::FixIt> in_fixits)
        : description(in_desc), kind(in_kind), bufferName(in_bufferName),
          bufferID(in_bufferID), line(in_line), column(in_column) {
      for (auto fixit : in_fixits) {
        fixits.push_back(fixit);
      }
    }
    std::string description;
    swift::DiagnosticKind kind;
    const llvm::StringRef bufferName;
    unsigned bufferID;
    uint32_t line;
    uint32_t column;
    std::vector<swift::DiagnosticInfo::FixIt> fixits;
  };
  typedef std::vector<RawDiagnostic> RawDiagnosticBuffer;

  SwiftASTContext &m_ast_context;
  RawDiagnosticBuffer m_diagnostics;
  unsigned m_num_errors = 0;
  bool m_colorize;
};
}

swift::ASTContext *SwiftASTContext::GetASTContext() {
  if (m_ast_context_ap.get() == NULL) {
    m_ast_context_ap.reset(
        new swift::ASTContext(GetLanguageOptions(), GetSearchPathOptions(),
                              GetSourceManager(), GetDiagnosticEngine()));
    m_diagnostic_consumer_ap.reset(new StoringDiagnosticConsumer(*this));

    if (getenv("LLDB_SWIFT_DUMP_DIAGS")) {
      // NOTE: leaking a swift::PrintingDiagnosticConsumer() here, but this only
      // gets enabled when the above environment variable is set.
      GetDiagnosticEngine().addConsumer(
          *new swift::PrintingDiagnosticConsumer());
    }
    // Install the serialized module loader

    std::unique_ptr<swift::ModuleLoader> serialized_module_loader_ap(
        swift::SerializedModuleLoader::create(*m_ast_context_ap));

    if (serialized_module_loader_ap) {
      m_serialized_module_loader =
          (swift::SerializedModuleLoader *)serialized_module_loader_ap.get();
      m_ast_context_ap->addModuleLoader(std::move(serialized_module_loader_ap));
    }

    GetASTMap().Insert(m_ast_context_ap.get(), this);
  }

  VALID_OR_RETURN(nullptr);

  return m_ast_context_ap.get();
}

swift::SerializedModuleLoader *SwiftASTContext::GetSerializeModuleLoader() {
  VALID_OR_RETURN(nullptr);

  GetASTContext();
  return m_serialized_module_loader;
}

swift::ClangImporter *SwiftASTContext::GetClangImporter() {
  VALID_OR_RETURN(nullptr);

  if (m_clang_importer == NULL) {
    swift::ASTContext *ast_ctx = GetASTContext();

    if (!ast_ctx) {
      return nullptr;
    }

    // Install the Clang module loader
    TargetSP target_sp(m_target_wp.lock());
    if (true /*target_sp*/) {
      // PlatformSP platform_sp = target_sp->GetPlatform();
      if (true /*platform_sp*/) {
        if (!ast_ctx->SearchPathOpts.SDKPath.empty() || TargetHasNoSDK()) {
          swift::ClangImporterOptions &clang_importer_options =
              GetClangImporterOptions();
          if (!clang_importer_options.OverrideResourceDir.empty()) {
            std::unique_ptr<swift::ModuleLoader> clang_importer_ap(
                swift::ClangImporter::create(*m_ast_context_ap,
                                             clang_importer_options));

            if (clang_importer_ap) {
              const bool isClang = true;
              m_clang_importer =
                  (swift::ClangImporter *)clang_importer_ap.get();
              m_ast_context_ap->addModuleLoader(std::move(clang_importer_ap),
                                                isClang);
            }
          }
        }
      }
    }
  }
  return m_clang_importer;
}

bool SwiftASTContext::AddModuleSearchPath(const char *path) {
  VALID_OR_RETURN(false);

  if (path && path[0]) {
    swift::ASTContext *ast = GetASTContext();
    std::string path_str(path);
    bool add_search_path = true;
    for (auto path : ast->SearchPathOpts.ImportSearchPaths) {
      if (path == path_str) {
        add_search_path = false;
        break;
      }
    }

    if (add_search_path) {
      ast->SearchPathOpts.ImportSearchPaths.push_back(path);
      return true;
    }
  }
  return false;
}

bool SwiftASTContext::AddFrameworkSearchPath(const char *path) {
  VALID_OR_RETURN(false);

  if (path && path[0]) {
    swift::ASTContext *ast = GetASTContext();
    std::string path_str(path);
    bool add_search_path = true;
    for (const auto &swift_path : ast->SearchPathOpts.FrameworkSearchPaths) {
      if (swift_path.Path == path_str) {
        add_search_path = false;
        break;
      }
    }

    if (add_search_path) {
      ast->SearchPathOpts.FrameworkSearchPaths.push_back({path, /*isSystem=*/false});
      return true;
    }
  }
  return false;
}

bool SwiftASTContext::AddClangArgument(const char *clang_arg, bool force) {
  if (clang_arg && clang_arg[0]) {
    swift::ClangImporterOptions &importer_options = GetClangImporterOptions();

    bool add_hmap = true;

    if (!force) {
      for (std::string &arg : importer_options.ExtraArgs) {
        if (!arg.compare(clang_arg)) {
          add_hmap = false;
          break;
        }
      }
    }

    if (add_hmap) {
      importer_options.ExtraArgs.push_back(clang_arg);
      return true;
    }
  }
  return false;
}

bool SwiftASTContext::AddClangArgumentPair(const char *clang_arg_1,
                                           const char *clang_arg_2) {
  if (clang_arg_1 && clang_arg_2 && clang_arg_1[0] && clang_arg_2[0]) {
    swift::ClangImporterOptions &importer_options = GetClangImporterOptions();

    bool add_hmap = true;

    for (ssize_t ai = 0, ae = importer_options.ExtraArgs.size() -
                              1; // -1 because we look at the next one too
         ai < ae;
         ++ai) {
      if (!importer_options.ExtraArgs[ai].compare(clang_arg_1) &&
          !importer_options.ExtraArgs[ai + 1].compare(clang_arg_2)) {
        add_hmap = false;
        break;
      }
    }

    if (add_hmap) {
      importer_options.ExtraArgs.push_back(clang_arg_1);
      importer_options.ExtraArgs.push_back(clang_arg_2);
      return true;
    }
  }
  return false;
}

size_t SwiftASTContext::GetNumModuleSearchPaths() const {
  VALID_OR_RETURN(0);

  if (m_ast_context_ap.get())
    return m_ast_context_ap->SearchPathOpts.ImportSearchPaths.size();
  return 0;
}

const char *SwiftASTContext::GetModuleSearchPathAtIndex(size_t idx) const {
  VALID_OR_RETURN(nullptr);

  if (m_ast_context_ap.get()) {
    if (idx < m_ast_context_ap->SearchPathOpts.ImportSearchPaths.size())
      return m_ast_context_ap->SearchPathOpts.ImportSearchPaths[idx].c_str();
  }
  return NULL;
}

size_t SwiftASTContext::GetNumFrameworkSearchPaths() const {
  VALID_OR_RETURN(0);

  if (m_ast_context_ap.get())
    return m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths.size();
  return 0;
}

const char *SwiftASTContext::GetFrameworkSearchPathAtIndex(size_t idx) const {
  VALID_OR_RETURN(nullptr);

  if (m_ast_context_ap.get()) {
    if (idx < m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths.size())
      return m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths[idx].Path.c_str();
  }
  return NULL;
}

size_t SwiftASTContext::GetNumClangArguments() {
  swift::ClangImporterOptions &importer_options = GetClangImporterOptions();

  return importer_options.ExtraArgs.size();
}

const char *SwiftASTContext::GetClangArgumentAtIndex(size_t idx) {
  swift::ClangImporterOptions &importer_options = GetClangImporterOptions();

  if (idx < importer_options.ExtraArgs.size())
    return importer_options.ExtraArgs[idx].c_str();

  return NULL;
}

swift::ModuleDecl *
SwiftASTContext::GetCachedModule(const ConstString &module_name) {
  VALID_OR_RETURN(nullptr);

  SwiftModuleMap::const_iterator iter =
      m_swift_module_cache.find(module_name.GetCString());

  if (iter != m_swift_module_cache.end())
    return iter->second;
  return NULL;
}

swift::ModuleDecl *
SwiftASTContext::CreateModule(const ConstString &module_basename,
                              Status &error) {
  VALID_OR_RETURN(nullptr);

  if (module_basename) {
    swift::ModuleDecl *module = GetCachedModule(module_basename);
    if (module) {
      error.SetErrorStringWithFormat("module already exists for '%s'",
                                     module_basename.GetCString());
      return NULL;
    }

    swift::ASTContext *ast = GetASTContext();
    if (ast) {
      swift::Identifier module_id(
          ast->getIdentifier(module_basename.GetCString()));
      module = swift::ModuleDecl::create(module_id, *ast);
      if (module) {
        m_swift_module_cache[module_basename.GetCString()] = module;
        return module;
      } else {
        error.SetErrorStringWithFormat("invalid swift AST (NULL)");
      }
    } else {
      error.SetErrorStringWithFormat("invalid swift AST (NULL)");
    }
  } else {
    error.SetErrorStringWithFormat("invalid module name (empty)");
  }
  return NULL;
}

void SwiftASTContext::CacheModule(swift::ModuleDecl *module) {
  VALID_OR_RETURN_VOID();

  if (!module)
    return;
  auto ID = module->getName().get();
  if (nullptr == ID || 0 == ID[0])
    return;
  if (m_swift_module_cache.find(ID) != m_swift_module_cache.end())
    return;
  m_swift_module_cache.insert({ID, module});
}

swift::ModuleDecl *
SwiftASTContext::GetModule(const ConstString &module_basename, Status &error) {
  VALID_OR_RETURN(nullptr);

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  if (log)
    log->Printf("((SwiftASTContext*)%p)->GetModule('%s')", this,
                module_basename.AsCString("<no name>"));

  if (module_basename) {
    swift::ModuleDecl *module = GetCachedModule(module_basename);
    if (module)
      return module;
    if (swift::ASTContext *ast = GetASTContext()) {
      typedef std::pair<swift::Identifier, swift::SourceLoc> ModuleNameSpec;
      llvm::StringRef module_basename_sref(module_basename.GetCString());
      ModuleNameSpec name_pair(ast->getIdentifier(module_basename_sref),
                               swift::SourceLoc());

      if (HasFatalErrors()) {
        error.SetErrorStringWithFormat("failed to get module '%s' from AST "
                                       "context:\nAST context is in a fatal "
                                       "error state",
                                       module_basename.GetCString());
        printf("error in SwiftASTContext::GetModule(%s): AST context is in a "
               "fatal error stat",
               module_basename.GetCString());
        return nullptr;
      }

      ClearDiagnostics();

      module = ast->getModuleByName(module_basename_sref);

      if (HasErrors()) {
        DiagnosticManager diagnostic_manager;
        PrintDiagnostics(diagnostic_manager);
        error.SetErrorStringWithFormat(
            "failed to get module '%s' from AST context:\n%s",
            module_basename.GetCString(),
            diagnostic_manager.GetString().data());
#ifdef LLDB_CONFIGURATION_DEBUG
        printf("error in SwiftASTContext::GetModule(%s): '%s'",
               module_basename.GetCString(),
               diagnostic_manager.GetString().data());
#endif
        if (log)
          log->Printf("((SwiftASTContext*)%p)->GetModule('%s') -- error: %s",
                      this, module_basename.GetCString(),
                      diagnostic_manager.GetString().data());
      } else if (module) {
        if (log)
          log->Printf("((SwiftASTContext*)%p)->GetModule('%s') -- found %s",
                      this, module_basename.GetCString(),
                      module->getName().str().str().c_str());

        m_swift_module_cache[module_basename.GetCString()] = module;
        return module;
      } else {
        if (log)
          log->Printf(
              "((SwiftASTContext*)%p)->GetModule('%s') -- failed with no error",
              this, module_basename.GetCString());

        error.SetErrorStringWithFormat(
            "failed to get module '%s' from AST context",
            module_basename.GetCString());
      }
    } else {
      if (log)
        log->Printf(
            "((SwiftASTContext*)%p)->GetModule('%s') -- invalid ASTContext",
            this, module_basename.GetCString());

      error.SetErrorString("invalid swift::ASTContext");
    }
  } else {
    if (log)
      log->Printf(
          "((SwiftASTContext*)%p)->GetModule('%s') -- empty module name", this,
          module_basename.GetCString());

    error.SetErrorString("invalid module name (empty)");
  }
  return NULL;
}

swift::ModuleDecl *SwiftASTContext::GetModule(const FileSpec &module_spec,
                                              Status &error) {
  VALID_OR_RETURN(nullptr);

  ConstString module_basename(module_spec.GetFileNameStrippingExtension());

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  if (log)
    log->Printf("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s')", this,
                module_spec.GetPath().c_str());

  if (module_basename) {
    SwiftModuleMap::const_iterator iter =
        m_swift_module_cache.find(module_basename.GetCString());

    if (iter != m_swift_module_cache.end())
      return iter->second;

    if (module_spec.Exists()) {
      swift::ASTContext *ast = GetASTContext();
      if (!GetClangImporter()) {
        if (log)
          log->Printf("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- no "
                      "ClangImporter so giving up",
                      this, module_spec.GetPath().c_str());
        error.SetErrorStringWithFormat("couldn't get a ClangImporter");
        return nullptr;
      }

      std::string module_directory(module_spec.GetDirectory().GetCString());
      bool add_search_path = true;
      for (auto path : ast->SearchPathOpts.ImportSearchPaths) {
        if (path == module_directory) {
          add_search_path = false;
          break;
        }
      }
      // Add the search path if needed so we can find the module by basename
      if (add_search_path)
        ast->SearchPathOpts.ImportSearchPaths.push_back(
            std::move(module_directory));

      typedef std::pair<swift::Identifier, swift::SourceLoc> ModuleNameSpec;
      llvm::StringRef module_basename_sref(module_basename.GetCString());
      ModuleNameSpec name_pair(ast->getIdentifier(module_basename_sref),
                               swift::SourceLoc());
      swift::ModuleDecl *module =
          ast->getModule(llvm::ArrayRef<ModuleNameSpec>(name_pair));
      if (module) {
        if (log)
          log->Printf(
              "((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- found %s",
              this, module_spec.GetPath().c_str(),
              module->getName().str().str().c_str());

        m_swift_module_cache[module_basename.GetCString()] = module;
        return module;
      } else {
        if (log)
          log->Printf("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- "
                      "couldn't get from AST context",
                      this, module_spec.GetPath().c_str());

        error.SetErrorStringWithFormat(
            "failed to get module '%s' from AST context",
            module_basename.GetCString());
      }
    } else {
      if (log)
        log->Printf("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- "
                    "doesn't exist",
                    this, module_spec.GetPath().c_str());

      error.SetErrorStringWithFormat("module '%s' doesn't exist",
                                     module_spec.GetPath().c_str());
    }
  } else {
    if (log)
      log->Printf(
          "((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- no basename",
          this, module_spec.GetPath().c_str());

    error.SetErrorStringWithFormat("no module basename in '%s'",
                                   module_spec.GetPath().c_str());
  }
  return NULL;
}

swift::ModuleDecl *
SwiftASTContext::FindAndLoadModule(const ConstString &module_basename,
                                   Process &process, Status &error) {
  VALID_OR_RETURN(nullptr);

  swift::ModuleDecl *swift_module = GetModule(module_basename, error);
  if (!swift_module)
    return nullptr;
  LoadModule(swift_module, process, error);
  return swift_module;
}

swift::ModuleDecl *
SwiftASTContext::FindAndLoadModule(const FileSpec &module_spec,
                                   Process &process, Status &error) {
  VALID_OR_RETURN(nullptr);

  swift::ModuleDecl *swift_module = GetModule(module_spec, error);
  if (!swift_module)
    return nullptr;
  LoadModule(swift_module, process, error);
  return swift_module;
}

bool SwiftASTContext::LoadOneImage(Process &process, FileSpec &link_lib_spec,
                                   Status &error) {
  VALID_OR_RETURN(false);

  error.Clear();

  PlatformSP platform_sp = process.GetTarget().GetPlatform();
  if (platform_sp)
    return platform_sp->LoadImage(&process, FileSpec(), link_lib_spec, error) !=
           LLDB_INVALID_IMAGE_TOKEN;
  else
    return false;
}

static void
GetLibrarySearchPaths(std::vector<std::string> &paths,
                      const swift::SearchPathOptions &search_path_opts) {
  paths.clear();
  paths.resize(search_path_opts.LibrarySearchPaths.size() + 1);
  std::copy(search_path_opts.LibrarySearchPaths.begin(),
            search_path_opts.LibrarySearchPaths.end(), paths.begin());
  paths.push_back(search_path_opts.RuntimeLibraryPath);
}

void SwiftASTContext::LoadModule(swift::ModuleDecl *swift_module,
                                 Process &process, Status &error) {
  VALID_OR_RETURN_VOID();

  Status current_error;
  auto addLinkLibrary = [&](swift::LinkLibrary link_lib) {
    Status load_image_error;
    StreamString all_dlopen_errors;
    const char *library_name = link_lib.getName().data();

    if (library_name == NULL || library_name[0] == '\0') {
      error.SetErrorString("Empty library name passed to addLinkLibrary");
      return;
    }

    SwiftLanguageRuntime *runtime = process.GetSwiftLanguageRuntime();

    if (runtime && runtime->IsInLibraryNegativeCache(library_name))
      return;

    swift::LibraryKind library_kind = link_lib.getKind();

    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_TYPES));
    if (log)
      log->Printf("\nLoading link library \"%s\" of kind: %d.", library_name,
                  library_kind);

    switch (library_kind) {
    case swift::LibraryKind::Framework: {

      // First make sure the library isn't already loaded. Since this is a
      // framework, we make sure the file name and the framework name are the
      // same, and that we are contained in FileName.framework with no other
      // intervening frameworks.  We can get more restrictive if this gives
      // false positives.

      ConstString library_cstr(library_name);

      std::string framework_name(library_name);
      framework_name.append(".framework");

      // Lookup the module by file basename and make sure that basename has
      // "<basename>.framework" in the path.

      ModuleSpec module_spec;
      module_spec.GetFileSpec().GetFilename() = library_cstr;
      lldb_private::ModuleList matching_module_list;
      bool module_already_loaded = false;
      if (process.GetTarget().GetImages().FindModules(module_spec,
                                                      matching_module_list)) {
        matching_module_list.ForEach(
            [&module_already_loaded, &module_spec,
             &framework_name](const ModuleSP &module_sp) -> bool {
              module_already_loaded = module_spec.GetFileSpec().GetPath().find(
                                          framework_name) != std::string::npos;
              return module_already_loaded ==
                     false; // Keep iterating if we didn't find the right module
            });
      }
      // If we already have this library loaded, don't try and load it again.
      if (module_already_loaded) {
        if (log)
          log->Printf("Skipping load of %s as it is already loaded.",
                      framework_name.c_str());
        return;
      }

      for (auto module : process.GetTarget().GetImages().Modules()) {
        FileSpec module_file = module->GetFileSpec();
        if (module_file.GetFilename() == library_cstr) {
          std::string module_path = module_file.GetPath();

          size_t framework_offset = module_path.rfind(framework_name);

          if (framework_offset != std::string::npos) {
            // The Framework is already loaded, so we don't need to try to load
            // it again.
            if (log)
              log->Printf("Skipping load of %s as it is already loaded.",
                          framework_name.c_str());
            return;
          }
        }
      }

      std::string framework_path("@rpath/");
      framework_path.append(library_name);
      framework_path.append(".framework/");
      framework_path.append(library_name);
      FileSpec framework_spec(framework_path.c_str(), false);

      if (LoadOneImage(process, framework_spec, load_image_error)) {
        if (log)
          log->Printf("Found framework at: %s.", framework_path.c_str());

        return;
      } else
        all_dlopen_errors.Printf("Looking for \"%s\", error: %s\n",
                                 framework_path.c_str(),
                                 load_image_error.AsCString());

      // And then in the various framework search paths.
      std::unordered_set<std::string> seen_paths;
      for (const auto &framework_search_dir :
           swift_module->getASTContext().SearchPathOpts.FrameworkSearchPaths) {
        // The framework search dir as it comes from the AST context often has
        // duplicate entries, don't try to load along the same path twice.

        std::pair<std::unordered_set<std::string>::iterator, bool>
            insert_result = seen_paths.insert(framework_search_dir.Path);
        if (!insert_result.second)
          continue;

        framework_path = framework_search_dir.Path;
        framework_path.append("/");
        framework_path.append(library_name);
        framework_path.append(".framework/");
        framework_path.append(library_name);
        framework_spec.SetFile(framework_path.c_str(), false);

        if (LoadOneImage(process, framework_spec, load_image_error)) {
          if (log)
            log->Printf("Found framework at: %s.", framework_path.c_str());

          return;
        } else
          all_dlopen_errors.Printf("Looking for \"%s\"\n,    error: %s\n",
                                   framework_path.c_str(),
                                   load_image_error.AsCString());
      }

      // Maybe we were told to add a link library that exists in the system.  I
      // tried just specifying Foo.framework/Foo and letting the system search
      // figure that out, but if DYLD_FRAMEWORK_FALLBACK_PATH is set
      // (e.g. in Xcode's test scheme) then these aren't found. So for now I
      // dial them in explicitly:

      std::string system_path("/System/Library/Frameworks/");
      system_path.append(library_name);
      system_path.append(".framework/");
      system_path.append(library_name);
      framework_spec.SetFile(system_path.c_str(), true);
      if (LoadOneImage(process, framework_spec, load_image_error))
        return;
      else
        all_dlopen_errors.Printf("Looking for \"%s\"\n,    error: %s\n",
                                 framework_path.c_str(),
                                 load_image_error.AsCString());
    } break;
    case swift::LibraryKind::Library: {
      std::vector<std::string> search_paths;

      GetLibrarySearchPaths(search_paths,
                            swift_module->getASTContext().SearchPathOpts);

      if (LoadLibraryUsingPaths(process, library_name, search_paths, true,
                                all_dlopen_errors))
        return;
    } break;
    }

    // If we get here, we aren't going to find this image, so add it to a
    // negative cache:
    if (runtime)
      runtime->AddToLibraryNegativeCache(library_name);

    current_error.SetErrorStringWithFormat(
        "Failed to load linked library %s of module %s - errors:\n%s\n",
        library_name, swift_module->getName().str().str().c_str(),
        all_dlopen_errors.GetData());
  };

  swift_module->collectLinkLibraries(addLinkLibrary);
  error = current_error;
}

bool SwiftASTContext::LoadLibraryUsingPaths(
    Process &process, llvm::StringRef library_name,
    std::vector<std::string> &search_paths, bool check_rpath,
    StreamString &all_dlopen_errors) {
  VALID_OR_RETURN(false);

  Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_TYPES));

  SwiftLanguageRuntime *runtime = process.GetSwiftLanguageRuntime();
  if (!runtime) {
    all_dlopen_errors.PutCString(
        "Can't load Swift libraries without a language runtime.");
    return false;
  }

  if (ConstString::Equals(runtime->GetStandardLibraryBaseName(),
                          ConstString(library_name))) {
    // Never dlopen the standard library. Some binaries statically link to the
    // Swift standard library and dlopening it here will cause ObjC runtime
    // conflicts.
    // If you want to run Swift expressions you have to arrange to load the
    // Swift standard library by hand before doing so.
    if (log)
      log->Printf("Skipping swift standard library \"%s\" - we don't hand load "
                  "that one.",
                  runtime->GetStandardLibraryBaseName().AsCString());
    return true;
  }

  PlatformSP platform_sp(process.GetTarget().GetPlatform());

  std::string library_fullname;

  if (platform_sp) {
    library_fullname =
        platform_sp->GetFullNameForDylib(ConstString(library_name)).AsCString();
  } else // This is the old way, and we shouldn't use it except on Mac OS
  {
#ifdef __APPLE__
    library_fullname = "lib";
    library_fullname.append(library_name);
    library_fullname.append(".dylib");
#else
    return false;
#endif
  }

  ModuleSpec module_spec;
  module_spec.GetFileSpec().GetFilename().SetCString(library_fullname.c_str());
  lldb_private::ModuleList matching_module_list;

  if (process.GetTarget().GetImages().FindModules(module_spec,
                                                  matching_module_list) > 0) {
    if (log)
      log->Printf("Skipping module %s as it is already loaded.",
                  library_fullname.c_str());
    return true;
  }

  FileSpec library_spec;
  std::string library_path;
  std::unordered_set<std::string> seen_paths;
  Status load_image_error;

  for (const std::string &library_search_dir : search_paths) {
    // The library search dir as it comes from the AST context often has
    // duplicate entries, don't try to load along the same path twice.

    std::pair<std::unordered_set<std::string>::iterator, bool> insert_result =
        seen_paths.insert(library_search_dir);
    if (!insert_result.second)
      continue;

    library_path = library_search_dir;
    library_path.append("/");
    library_path.append(library_fullname);
    library_spec.SetFile(library_path.c_str(), false);
    if (LoadOneImage(process, library_spec, load_image_error)) {
      if (log)
        log->Printf("Found library at: %s.", library_path.c_str());
      return true;
    } else
      all_dlopen_errors.Printf("Looking for \"%s\"\n,    error: %s\n",
                               library_path.c_str(),
                               load_image_error.AsCString());
  }

  if (check_rpath) {
    // Let our RPATH help us out when finding the right library
    library_path = "@rpath/";
    library_path += library_fullname;

    FileSpec link_lib_spec(library_path.c_str(), false);

    if (LoadOneImage(process, link_lib_spec, load_image_error)) {
      if (log)
        log->Printf("Found library at: %s.", library_path.c_str());
      return true;
    } else
      all_dlopen_errors.Printf("Looking for \"%s\", error: %s\n",
                               library_path.c_str(),
                               load_image_error.AsCString());
  }
  return false;
}

void SwiftASTContext::LoadExtraDylibs(Process &process, Status &error) {
  VALID_OR_RETURN_VOID();

  error.Clear();
  swift::IRGenOptions &irgen_options = GetIRGenOptions();
  for (const swift::LinkLibrary &link_lib : irgen_options.LinkLibraries) {
    // We don't have to do frameworks here, they actually record their link
    // libraries properly.
    if (link_lib.getKind() == swift::LibraryKind::Library) {
      const char *library_name = link_lib.getName().data();
      StreamString errors;

      std::vector<std::string> search_paths;

      GetLibrarySearchPaths(search_paths,
                            m_compiler_invocation_ap->getSearchPathOptions());

      bool success = LoadLibraryUsingPaths(process, library_name, search_paths,
                                           false, errors);
      if (!success) {
        error.SetErrorString(errors.GetData());
      }
    }
  }
}

bool SwiftASTContext::RegisterSectionModules(
    Module &module, std::vector<std::string> &module_names) {
  VALID_OR_RETURN(false);

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

  swift::SerializedModuleLoader *sml = GetSerializeModuleLoader();
  if (sml) {
    SectionList *section_list = module.GetSectionList();
    if (section_list) {
      SectionSP section_sp(
          section_list->FindSectionByType(eSectionTypeSwiftModules, true));
      if (section_sp) {
        DataExtractor section_data;

        if (section_sp->GetSectionData(section_data)) {
          llvm::StringRef section_data_ref(
              (const char *)section_data.GetDataStart(),
              section_data.GetByteSize());
          llvm::SmallVector<std::string, 4> llvm_modules;
          if (swift::parseASTSection(sml, section_data_ref, llvm_modules)) {
            for (auto module_name : llvm_modules)
              module_names.push_back(module_name);
            return true;
          }
        }
      } else {
        if (m_ast_file_data_map.find(&module) != m_ast_file_data_map.end())
          return true;

        SymbolVendor *sym_vendor = module.GetSymbolVendor();
        if (sym_vendor) {
          // Grab all the AST blobs from the symbol vendor.
          auto ast_file_datas = sym_vendor->GetASTData(eLanguageTypeSwift);
          if (log)
            log->Printf("SwiftASTContext::%s() retrieved %zu AST Data blobs "
                        "from the symbol vendor.",
                        __FUNCTION__, ast_file_datas.size());

          // Add each of the AST blobs to the vector of AST blobs for the
          // module.
          auto &ast_vector = GetASTVectorForModule(&module);
          ast_vector.insert(ast_vector.end(), ast_file_datas.begin(),
                            ast_file_datas.end());

          // Retrieve the module names from the AST blobs retrieved from the
          // symbol vendor.
          size_t parse_fail_count = 0;
          size_t ast_number = 0;
          for (auto ast_file_data_sp : ast_file_datas) {
            // Parse the AST section info from the AST blob.
            ++ast_number;
            llvm::StringRef section_data_ref(
                (const char *)ast_file_data_sp->GetBytes(),
                ast_file_data_sp->GetByteSize());
            llvm::SmallVector<std::string, 4> llvm_modules;
            if (swift::parseASTSection(sml, section_data_ref, llvm_modules)) {
              // Collect the LLVM module names referenced by the AST.
              for (auto module_name : llvm_modules)
                module_names.push_back(module_name);
              if (log)
                log->Printf("SwiftASTContext::%s() - parsed %zu llvm modules "
                            "from Swift AST section %zu of %zu.",
                            __FUNCTION__, llvm_modules.size(), ast_number,
                            ast_file_datas.size());
            } else {
              // Keep track of the fact that we failed to parse the AST
              // section info.
              if (log)
                log->Printf("SwiftASTContext::%s() - failed to parse AST "
                            "section %zu of %zu.",
                            __FUNCTION__, ast_number, ast_file_datas.size());
              ++parse_fail_count;
            }
          }
          if (!ast_file_datas.empty() && (parse_fail_count == 0)) {
            // We found AST data entries and we successfully parsed all of
            // them.
            return true;
          }
        }
      }
    }
  }
  return false;
}

void SwiftASTContext::ValidateSectionModules(
    Module &module, const std::vector<std::string> &module_names) {
  VALID_OR_RETURN_VOID();

  Status error;

  for (const std::string &module_name : module_names)
    if (!GetModule(ConstString(module_name.c_str()), error))
      module.ReportWarning("unable to load swift module '%s' (%s)",
                           module_name.c_str(), error.AsCString());
}

swift::Identifier SwiftASTContext::GetIdentifier(const char *name) {
  VALID_OR_RETURN(swift::Identifier());

  return GetASTContext()->getIdentifier(llvm::StringRef(name));
}

swift::Identifier SwiftASTContext::GetIdentifier(const llvm::StringRef &name) {
  VALID_OR_RETURN(swift::Identifier());

  return GetASTContext()->getIdentifier(name);
}

ConstString SwiftASTContext::GetMangledTypeName(swift::TypeBase *type_base) {
  VALID_OR_RETURN(ConstString());

  auto iter = m_type_to_mangled_name_map.find(type_base),
       end = m_type_to_mangled_name_map.end();
  if (iter != end)
    return ConstString(iter->second);

  swift::Type swift_type(type_base);

  bool has_archetypes = swift_type->hasArchetype();

  if (!has_archetypes) {
    swift::Mangle::ASTMangler mangler(true);
    std::string s = mangler.mangleTypeForDebugger(swift_type, nullptr, nullptr);

    if (!s.empty()) {
      ConstString mangled_cs(s.c_str());
      CacheDemangledType(mangled_cs.AsCString(), type_base);
      return mangled_cs;
    }
  }

  return ConstString();
}

void SwiftASTContext::CacheDemangledType(const char *name,
                                         swift::TypeBase *found_type) {
  VALID_OR_RETURN_VOID();

  m_type_to_mangled_name_map.insert(std::make_pair(found_type, name));
  m_mangled_name_to_type_map.insert(std::make_pair(name, found_type));
}

void SwiftASTContext::CacheDemangledTypeFailure(const char *name) {
  VALID_OR_RETURN_VOID();

  m_negative_type_cache.Insert(name);
}

CompilerType
SwiftASTContext::GetTypeFromMangledTypename(const char *mangled_typename,
                                            Status &error) {
  VALID_OR_RETURN(CompilerType());

  if (mangled_typename 
      && SwiftLanguageRuntime::IsSwiftMangledName(mangled_typename)) {
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
    if (log)
      log->Printf("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s')",
                  this, mangled_typename);

    swift::ASTContext *ast_ctx = GetASTContext();
    if (!ast_ctx) {
      if (log)
        log->Printf("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') "
                    "-- null Swift AST Context",
                    this, mangled_typename);
      error.SetErrorString("null Swift AST Context");
      return CompilerType();
    }

    error.Clear();

    // If we were to crash doing this, remember what type caused it
    llvm::PrettyStackTraceFormat PST("error finding type for %s",
                                        mangled_typename);
    ConstString mangled_name(mangled_typename);
    swift::TypeBase *found_type =
        m_mangled_name_to_type_map.lookup(mangled_name.GetCString());
    if (found_type) {
      if (log)
        log->Printf("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') "
                    "-- found in the positive cache",
                    this, mangled_typename);
      return CompilerType(ast_ctx, found_type);
    }

    if (m_negative_type_cache.Lookup(mangled_name.GetCString())) {
      if (log)
        log->Printf("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') "
                    "-- found in the negative cache",
                    this, mangled_typename);
      return CompilerType();
    }

    if (log)
      log->Printf("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') -- "
                  "not cached, searching",
                  this, mangled_typename);

    std::string swift_error;
    found_type = swift::ide::getTypeFromMangledSymbolname(
                     *ast_ctx, mangled_typename, swift_error)
                     .getPointer();

    if (found_type) {
      CacheDemangledType(mangled_name.GetCString(), found_type);
      CompilerType result_type(ast_ctx, found_type);
      if (log)
        log->Printf("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') "
                    "-- found %s",
                    this, mangled_typename,
                    result_type.GetTypeName().GetCString());
      return result_type;
    } else {
      if (log)
        log->Printf("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') "
                    "-- error: %s",
                    this, mangled_typename, swift_error.c_str());

      error.SetErrorStringWithFormat("type for typename '%s' was not found",
                                     mangled_typename);
      CacheDemangledTypeFailure(mangled_name.GetCString());
      return CompilerType();
    }
  }
  error.SetErrorStringWithFormat("typename '%s' is not a valid Swift mangled "
                                 "typename, it should begin with _T",
                                 mangled_typename);
  return CompilerType();
}

CompilerType SwiftASTContext::GetVoidFunctionType() {
  VALID_OR_RETURN(CompilerType());

  if (!m_void_function_type) {
    swift::ASTContext *ast = GetASTContext();
    swift::Type empty_tuple_type(swift::TupleType::getEmpty(*ast));
    m_void_function_type = CompilerType(
        ast, swift::FunctionType::get(empty_tuple_type, empty_tuple_type));
  }
  return m_void_function_type;
}

static CompilerType ValueDeclToType(swift::ValueDecl *decl,
                                    swift::ASTContext *ast) {
  if (decl) {
    switch (decl->getKind()) {
    case swift::DeclKind::TypeAlias: {
      swift::TypeAliasDecl *alias_decl = swift::cast<swift::TypeAliasDecl>(decl);
      if (alias_decl->hasInterfaceType()) {
        swift::Type swift_type =
          swift::NameAliasType::get(
                                  alias_decl, swift::Type(),
                                  swift::SubstitutionMap(),
                                  alias_decl->getUnderlyingTypeLoc().getType());
        return CompilerType(ast, swift_type.getPointer());
      }
      break;
    }

    case swift::DeclKind::Enum:
    case swift::DeclKind::Struct:
    case swift::DeclKind::Protocol:
    case swift::DeclKind::Class: {
      swift::NominalTypeDecl *nominal_decl = swift::cast<swift::NominalTypeDecl>(decl);
      if (nominal_decl->hasInterfaceType()) {
        swift::Type swift_type = nominal_decl->getDeclaredType();
        return CompilerType(ast, swift_type.getPointer());
      }
    } break;

    default:
      break;
    }
  }
  return CompilerType();
}

CompilerType SwiftASTContext::FindQualifiedType(const char *qualified_name) {
  VALID_OR_RETURN(CompilerType());

  if (qualified_name && qualified_name[0]) {
    const char *dot_pos = strchr(qualified_name, '.');
    if (dot_pos) {
      ConstString module_name(qualified_name, dot_pos - qualified_name);
      swift::ModuleDecl *swift_module = GetCachedModule(module_name);
      if (swift_module) {
        swift::ModuleDecl::AccessPathTy access_path;
        llvm::SmallVector<swift::ValueDecl *, 4> decls;
        const char *module_type_name = dot_pos + 1;
        swift_module->lookupValue(access_path, GetIdentifier(module_type_name),
                                  swift::NLKind::UnqualifiedLookup, decls);
        for (auto decl : decls) {
          CompilerType type = ValueDeclToType(decl, GetASTContext());
          if (type)
            return type;
        }
      }
    }
  }
  return CompilerType();
}

static CompilerType DeclToType(swift::Decl *decl, swift::ASTContext *ast) {
  if (swift::ValueDecl *value_decl =
          swift::dyn_cast_or_null<swift::ValueDecl>(decl))
    return ValueDeclToType(value_decl, ast);
  return CompilerType();
}

static SwiftASTContext::TypeOrDecl DeclToTypeOrDecl(swift::ASTContext *ast,
                                                    swift::Decl *decl) {
  if (decl) {
    switch (decl->getKind()) {
    case swift::DeclKind::Import:
    case swift::DeclKind::Extension:
    case swift::DeclKind::PatternBinding:
    case swift::DeclKind::TopLevelCode:
    case swift::DeclKind::GenericTypeParam:
    case swift::DeclKind::AssociatedType:
    case swift::DeclKind::EnumElement:
    case swift::DeclKind::EnumCase:
    case swift::DeclKind::IfConfig:
    case swift::DeclKind::Param:
    case swift::DeclKind::Module:
    case swift::DeclKind::MissingMember:
      break;

    case swift::DeclKind::InfixOperator:
    case swift::DeclKind::PrefixOperator:
    case swift::DeclKind::PostfixOperator:
    case swift::DeclKind::PrecedenceGroup:
      return decl;

    case swift::DeclKind::TypeAlias: {
      swift::TypeAliasDecl *alias_decl =
          swift::cast<swift::TypeAliasDecl>(decl);
      if (alias_decl->hasInterfaceType()) {
        swift::Type swift_type =
          swift::NameAliasType::get(
                                  alias_decl, swift::Type(),
                                  swift::SubstitutionMap(),
                                  alias_decl->getUnderlyingTypeLoc().getType());
        return CompilerType(ast, swift_type.getPointer());
      }
    } break;
    case swift::DeclKind::Enum:
    case swift::DeclKind::Struct:
    case swift::DeclKind::Class:
    case swift::DeclKind::Protocol: {
      swift::NominalTypeDecl *nominal_decl =
          swift::cast<swift::NominalTypeDecl>(decl);
      if (nominal_decl->hasInterfaceType()) {
        swift::Type swift_type = nominal_decl->getDeclaredType();
        return CompilerType(ast, swift_type.getPointer());
      }
    } break;

    case swift::DeclKind::Func:
    case swift::DeclKind::Var:
      return decl;

    case swift::DeclKind::Subscript:
    case swift::DeclKind::Constructor:
    case swift::DeclKind::Destructor:
      break;
    }
  }
  return CompilerType();
}

size_t
SwiftASTContext::FindContainedTypeOrDecl(llvm::StringRef name,
                                         TypeOrDecl container_type_or_decl,
                                         TypesOrDecls &results, bool append) {
  VALID_OR_RETURN(0);

  if (!append)
    results.clear();
  size_t size_before = results.size();

  CompilerType container_type = container_type_or_decl.Apply<CompilerType>(
      [](CompilerType type) -> CompilerType { return type; },
      [this](swift::Decl *decl) -> CompilerType {
        return DeclToType(decl, GetASTContext());
      });

  if (false == name.empty() &&
      llvm::dyn_cast_or_null<SwiftASTContext>(container_type.GetTypeSystem())) {
    swift::Type swift_type(GetSwiftType(container_type));
    if (!swift_type)
      return 0;
    swift::CanType swift_can_type(swift_type->getCanonicalType());
    swift::NominalType *nominal_type =
        swift_can_type->getAs<swift::NominalType>();
    if (!nominal_type)
      return 0;
    swift::NominalTypeDecl *nominal_decl = nominal_type->getDecl();
    llvm::ArrayRef<swift::ValueDecl *> decls =
        nominal_decl->lookupDirect(
            swift::DeclName(m_ast_context_ap->getIdentifier(name)));
    for (auto decl : decls)
      results.emplace(DeclToTypeOrDecl(GetASTContext(), decl));
  }
  return results.size() - size_before;
}

CompilerType SwiftASTContext::FindType(const char *name,
                                       swift::ModuleDecl *swift_module) {
  VALID_OR_RETURN(CompilerType());

  std::set<CompilerType> search_results;

  FindTypes(name, swift_module, search_results, false);

  if (search_results.empty())
    return CompilerType();
  else
    return *search_results.begin();
}

llvm::Optional<SwiftASTContext::TypeOrDecl>
SwiftASTContext::FindTypeOrDecl(const char *name,
                                swift::ModuleDecl *swift_module) {
  VALID_OR_RETURN(llvm::Optional<SwiftASTContext::TypeOrDecl>());

  TypesOrDecls search_results;

  FindTypesOrDecls(name, swift_module, search_results, false);

  if (search_results.empty())
    return llvm::Optional<SwiftASTContext::TypeOrDecl>();
  else
    return *search_results.begin();
}

size_t SwiftASTContext::FindTypes(const char *name,
                                  swift::ModuleDecl *swift_module,
                                  std::set<CompilerType> &results,
                                  bool append) {
  VALID_OR_RETURN(0);

  if (!append)
    results.clear();

  size_t before = results.size();

  TypesOrDecls types_or_decls_results;
  FindTypesOrDecls(name, swift_module, types_or_decls_results);

  for (const auto &result : types_or_decls_results) {
    CompilerType type = result.Apply<CompilerType>(
        [](CompilerType type) -> CompilerType { return type; },
        [this](swift::Decl *decl) -> CompilerType {
          if (swift::ValueDecl *value_decl =
                  swift::dyn_cast_or_null<swift::ValueDecl>(decl)) {
            if (value_decl->hasInterfaceType()) {
              swift::Type swift_type = value_decl->getInterfaceType();
              swift::MetatypeType *meta_type =
                  swift_type->getAs<swift::MetatypeType>();
              swift::ASTContext *ast = GetASTContext();
              if (meta_type)
                return CompilerType(ast,
                                    meta_type->getInstanceType().getPointer());
              else
                return CompilerType(ast, swift_type.getPointer());
            }
          }
          return CompilerType();
        });
    results.emplace(type);
  }

  return results.size() - before;
}

size_t SwiftASTContext::FindTypesOrDecls(const char *name,
                                         swift::ModuleDecl *swift_module,
                                         TypesOrDecls &results, bool append) {
  VALID_OR_RETURN(0);

  if (!append)
    results.clear();

  size_t before = results.size();

  if (name && name[0] && swift_module) {
    swift::ModuleDecl::AccessPathTy access_path;
    llvm::SmallVector<swift::ValueDecl *, 4> value_decls;
    swift::Identifier identifier(GetIdentifier(name));
    if (strchr(name, '.'))
      swift_module->lookupValue(access_path, identifier,
                                swift::NLKind::QualifiedLookup, value_decls);
    else
      swift_module->lookupValue(access_path, identifier,
                                swift::NLKind::UnqualifiedLookup, value_decls);
    if (identifier.isOperator()) {
      swift::OperatorDecl *op_decl =
          swift_module->lookupPrefixOperator(identifier);
      if (op_decl)
        results.emplace(DeclToTypeOrDecl(GetASTContext(), op_decl));
      if ((op_decl = swift_module->lookupInfixOperator(identifier)))
        results.emplace(DeclToTypeOrDecl(GetASTContext(), op_decl));
      if ((op_decl = swift_module->lookupPostfixOperator(identifier)))
        results.emplace(DeclToTypeOrDecl(GetASTContext(), op_decl));
    }
    if (swift::PrecedenceGroupDecl *pg_decl =
            swift_module->lookupPrecedenceGroup(identifier))
      results.emplace(DeclToTypeOrDecl(GetASTContext(), pg_decl));

    for (auto decl : value_decls)
      results.emplace(DeclToTypeOrDecl(GetASTContext(), decl));
  }

  return results.size() - before;
}

size_t SwiftASTContext::FindType(const char *name,
                                 std::set<CompilerType> &results, bool append) {
  VALID_OR_RETURN(0);

  if (!append)
    results.clear();
  auto iter = m_swift_module_cache.begin(), end = m_swift_module_cache.end();

  size_t count = 0;

  std::function<void(swift::ModuleDecl *)> lookup_func =
      [this, name, &results, &count](swift::ModuleDecl *module) -> void {
    CompilerType candidate(this->FindType(name, module));
    if (candidate) {
      ++count;
      results.insert(candidate);
    }
  };

  for (; iter != end; iter++)
    lookup_func(iter->second);

  if (m_scratch_module)
    lookup_func(m_scratch_module);

  return count;
}

CompilerType SwiftASTContext::FindFirstType(const char *name,
                                            const ConstString &module_name) {
  VALID_OR_RETURN(CompilerType());

  if (name && name[0]) {
    if (module_name) {
      return FindType(name, GetCachedModule(module_name));
    } else {
      std::set<CompilerType> types;
      FindType(name, types);
      if (!types.empty())
        return *types.begin();
    }
  }
  return CompilerType();
}

CompilerType SwiftASTContext::ImportType(CompilerType &type, Status &error) {
  VALID_OR_RETURN(CompilerType());

  if (m_ast_context_ap.get() == NULL)
    return CompilerType();

  SwiftASTContext *swift_ast_ctx =
      llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem());

  if (swift_ast_ctx == nullptr) {
    error.SetErrorString("Can't import clang type into a Swift ASTContext.");
    return CompilerType();
  } else if (swift_ast_ctx == this) {
    // This is the same AST context, so the type is already imported...
    return type;
  }

  // For now we're going to do this all using mangled names.  If we find that is
  // too slow, we can use the TypeBase * in the CompilerType to match this to
  // the version of the type we got from the mangled name in the original
  // swift::ASTContext.

  ConstString mangled_name(type.GetMangledTypeName());
  if (mangled_name) {
    swift::TypeBase *our_type_base =
        m_mangled_name_to_type_map.lookup(mangled_name.GetCString());
    if (our_type_base)
      return CompilerType(m_ast_context_ap.get(), our_type_base);
    else {
      Status error;

      CompilerType our_type(
          GetTypeFromMangledTypename(mangled_name.GetCString(), error));
      if (error.Success())
        return our_type;
    }
  }
  return CompilerType();
}

swift::IRGenDebugInfoKind SwiftASTContext::GetGenerateDebugInfo() {
  return GetIRGenOptions().DebugInfoKind;
}

swift::PrintOptions SwiftASTContext::GetUserVisibleTypePrintingOptions(
    bool print_help_if_available) {
  swift::PrintOptions print_options;
  print_options.SynthesizeSugarOnTypes = true;
  print_options.VarInitializers = true;
  print_options.TypeDefinitions = true;
  print_options.PrintGetSetOnRWProperties = true;
  print_options.SkipImplicit = false;
  print_options.PreferTypeRepr = true;
  print_options.FunctionDefinitions = true;
  print_options.FullyQualifiedTypesIfAmbiguous = true;
  print_options.FullyQualifiedTypes = true;
  print_options.ExplodePatternBindingDecls = false;
  print_options.PrintDocumentationComments =
      print_options.PrintRegularClangComments = print_help_if_available;
  return print_options;
}

void SwiftASTContext::SetGenerateDebugInfo(swift::IRGenDebugInfoKind b) {
  GetIRGenOptions().DebugInfoKind = b;
}

llvm::TargetOptions *SwiftASTContext::getTargetOptions() {
  if (m_target_options_ap.get() == NULL) {
    m_target_options_ap.reset(new llvm::TargetOptions());
  }
  return m_target_options_ap.get();
}

swift::ModuleDecl *SwiftASTContext::GetScratchModule() {
  VALID_OR_RETURN(nullptr);

  if (m_scratch_module == nullptr)
    m_scratch_module = swift::ModuleDecl::create(
        GetASTContext()->getIdentifier("__lldb_scratch_module"),
        *GetASTContext());
  return m_scratch_module;
}

swift::SILModule *SwiftASTContext::GetSILModule() {
  VALID_OR_RETURN(nullptr);

  if (m_sil_module_ap.get() == NULL)
    m_sil_module_ap = swift::SILModule::createEmptyModule(GetScratchModule(),
                                                          GetSILOptions());
  return m_sil_module_ap.get();
}

swift::irgen::IRGenerator &
SwiftASTContext::GetIRGenerator(swift::IRGenOptions &opts,
                                swift::SILModule &module) {
  if (m_ir_generator_ap.get() == nullptr) {
    m_ir_generator_ap.reset(new swift::irgen::IRGenerator(opts, module));
  }

  return *m_ir_generator_ap.get();
}

swift::irgen::IRGenModule &SwiftASTContext::GetIRGenModule() {
  VALID_OR_RETURN(*m_ir_gen_module_ap);

  if (m_ir_gen_module_ap.get() == NULL) {
    // Make sure we have a good ClangImporter.
    GetClangImporter();

    swift::IRGenOptions &ir_gen_opts = GetIRGenOptions();

    std::string error_str;
    std::string triple = GetTriple();
    const llvm::Target *llvm_target =
        llvm::TargetRegistry::lookupTarget(triple, error_str);

    llvm::CodeGenOpt::Level optimization_level = llvm::CodeGenOpt::Level::None;

    // Create a target machine.
    llvm::TargetMachine *target_machine = llvm_target->createTargetMachine(
        triple,
        "generic", // cpu
        "",        // features
        *getTargetOptions(),
        llvm::Reloc::Static, // TODO verify with Sean, Default went away
        llvm::None, optimization_level);
    if (target_machine) {
      // Set the module's string representation.
      const llvm::DataLayout data_layout = target_machine->createDataLayout();

      llvm::Triple llvm_triple(triple);
      swift::SILModule *sil_module = GetSILModule();
      if (sil_module != nullptr) {
        swift::irgen::IRGenerator &ir_generator =
            GetIRGenerator(ir_gen_opts, *sil_module);
        swift::PrimarySpecificPaths PSPs =
            GetCompilerInvocation()
                .getFrontendOptions()
                .InputsAndOutputs.getPrimarySpecificPathsForAtMostOnePrimary();
        m_ir_gen_module_ap.reset(new swift::irgen::IRGenModule(
            ir_generator, ir_generator.createTargetMachine(), nullptr,
            GetGlobalLLVMContext(), ir_gen_opts.ModuleName, PSPs.OutputFilename,
            PSPs.MainInputFilenameForDebugInfo));
        llvm::Module *llvm_module = m_ir_gen_module_ap->getModule();
        llvm_module->setDataLayout(data_layout.getStringRepresentation());
        llvm_module->setTargetTriple(triple);
      }
    }
  }
  return *m_ir_gen_module_ap;
}

CompilerType
SwiftASTContext::CreateTupleType(const std::vector<CompilerType> &elements) {
  VALID_OR_RETURN(CompilerType());

  Status error;
  if (elements.size() == 0)
    return CompilerType(GetASTContext(), GetASTContext()->TheEmptyTupleType);
  else {
    std::vector<swift::TupleTypeElt> tuple_elems;
    for (const CompilerType &type : elements) {
      if (auto swift_type = GetSwiftType(type))
        tuple_elems.push_back(swift::TupleTypeElt(swift_type));
      else
        return CompilerType();
    }
    llvm::ArrayRef<swift::TupleTypeElt> fields(tuple_elems);
    return CompilerType(
        GetASTContext(),
        swift::TupleType::get(fields, *GetASTContext()).getPointer());
  }
}

CompilerType
SwiftASTContext::CreateTupleType(const std::vector<TupleElement> &elements) {
  VALID_OR_RETURN(CompilerType());

  Status error;
  if (elements.size() == 0)
    return CompilerType(GetASTContext(), GetASTContext()->TheEmptyTupleType);
  else {
    std::vector<swift::TupleTypeElt> tuple_elems;
    for (const TupleElement &element : elements) {
      if (auto swift_type = GetSwiftType(element.element_type)) {
        if (element.element_name.IsEmpty())
          tuple_elems.push_back(swift::TupleTypeElt(swift_type));
        else
          tuple_elems.push_back(swift::TupleTypeElt(
              swift_type, m_ast_context_ap->getIdentifier(
                              element.element_name.GetCString())));
      } else
        return CompilerType();
    }
    llvm::ArrayRef<swift::TupleTypeElt> fields(tuple_elems);
    return CompilerType(
        GetASTContext(),
        swift::TupleType::get(fields, *GetASTContext()).getPointer());
  }
}

CompilerType SwiftASTContext::CreateFunctionType(CompilerType arg_type,
                                                 CompilerType ret_type,
                                                 bool throws) {
  VALID_OR_RETURN(CompilerType());

  if (!llvm::dyn_cast_or_null<SwiftASTContext>(arg_type.GetTypeSystem()) ||
      !llvm::dyn_cast_or_null<SwiftASTContext>(ret_type.GetTypeSystem()))
    return CompilerType();
  swift::FunctionType::ExtInfo ext_info;
  if (throws)
    ext_info = ext_info.withThrows();
  return CompilerType(GetASTContext(), swift::FunctionType::get(
                                           GetSwiftType(arg_type),
                                           GetSwiftType(ret_type), ext_info));
}

CompilerType SwiftASTContext::GetErrorType() {
  VALID_OR_RETURN(CompilerType());

  swift::ASTContext *swift_ctx = GetASTContext();
  if (swift_ctx) {
    // Getting the error type requires the Stdlib module be loaded, but doesn't
    // cause it to be loaded.
    // Do that here:
    swift_ctx->getStdlibModule(true);
    swift::NominalTypeDecl *error_type_decl = GetASTContext()->getErrorDecl();
    if (error_type_decl) {
      auto error_type = error_type_decl->getDeclaredType().getPointer();
      return CompilerType(GetASTContext(), error_type);
    }
  }
  return CompilerType();
}

CompilerType SwiftASTContext::GetNSErrorType(Status &error) {
  VALID_OR_RETURN(CompilerType());

  return GetTypeFromMangledTypename(SwiftLanguageRuntime::GetCurrentMangledName("_TtC10Foundation7NSError").c_str(), error);
}

CompilerType SwiftASTContext::CreateMetatypeType(CompilerType instance_type) {
  VALID_OR_RETURN(CompilerType());

  if (llvm::dyn_cast_or_null<SwiftASTContext>(instance_type.GetTypeSystem()))
    return CompilerType(GetASTContext(),
                        swift::MetatypeType::get(GetSwiftType(instance_type),
                                                 *GetASTContext()));
  return CompilerType();
}

SwiftASTContext *SwiftASTContext::GetSwiftASTContext(swift::ASTContext *ast) {
  SwiftASTContext *swift_ast = GetASTMap().Lookup(ast);
  return swift_ast;
}

uint32_t SwiftASTContext::GetPointerByteSize() {
  VALID_OR_RETURN(0);

  if (m_pointer_byte_size == 0) {
    swift::ASTContext *ast = GetASTContext();
    m_pointer_byte_size = CompilerType(ast, ast->TheRawPointerType.getPointer())
                              .GetByteSize(nullptr);
  }
  return m_pointer_byte_size;
}

uint32_t SwiftASTContext::GetPointerBitAlignment() {
  VALID_OR_RETURN(0);

  if (m_pointer_bit_align == 0) {
    swift::ASTContext *ast = GetASTContext();
    m_pointer_bit_align = CompilerType(ast, ast->TheRawPointerType.getPointer())
                              .GetAlignedBitSize();
  }
  return m_pointer_bit_align;
}

bool SwiftASTContext::HasErrors() {
  if (m_diagnostic_consumer_ap.get())
    return (
        static_cast<StoringDiagnosticConsumer *>(m_diagnostic_consumer_ap.get())
            ->NumErrors() != 0);
  else
    return false;
}

bool SwiftASTContext::HasFatalErrors(swift::ASTContext *ast_context) {
  return (ast_context && ast_context->Diags.hasFatalErrorOccurred());
}

void SwiftASTContext::ClearDiagnostics() {
  assert(!HasFatalErrors() && "Never clear a fatal diagnostic!");
  if (m_diagnostic_consumer_ap.get())
    static_cast<StoringDiagnosticConsumer *>(m_diagnostic_consumer_ap.get())
        ->Clear();
}

bool SwiftASTContext::SetColorizeDiagnostics(bool b) {
  if (m_diagnostic_consumer_ap.get())
    return static_cast<StoringDiagnosticConsumer *>(
               m_diagnostic_consumer_ap.get())
        ->SetColorize(b);
  return false;
}

void SwiftASTContext::PrintDiagnostics(DiagnosticManager &diagnostic_manager,
                                       uint32_t bufferID, uint32_t first_line,
                                       uint32_t last_line,
                                       uint32_t line_offset) {
  // If this is a fatal error, copy the error into the AST context's fatal error
  // field, and then put it to the stream, otherwise just dump the diagnostics
  // to the stream.


  // N.B. you cannot use VALID_OR_RETURN_VOID here since that exits if you have
  // fatal errors, which are what we are trying to print here.
  if (!m_ast_context_ap.get()) {
    SymbolFile *sym_file = GetSymbolFile();
    if (sym_file) {
      ConstString name 
              = sym_file->GetObjectFile()->GetModule()->GetObjectName();
      m_fatal_errors.SetErrorStringWithFormat(
                  "Null context for %s.", name.AsCString());
    } else {
      m_fatal_errors.SetErrorString("Unknown fatal error occurred.");
    }
    return;
  }

  if (m_ast_context_ap->Diags.hasFatalErrorOccurred() &&
      !m_reported_fatal_error) {
    DiagnosticManager fatal_diagnostics;

    if (m_diagnostic_consumer_ap.get())
      static_cast<StoringDiagnosticConsumer *>(m_diagnostic_consumer_ap.get())
          ->PrintDiagnostics(fatal_diagnostics, bufferID, first_line, last_line,
                             line_offset);
    if (fatal_diagnostics.Diagnostics().size())
      m_fatal_errors.SetErrorString(fatal_diagnostics.GetString().data());
    else
      m_fatal_errors.SetErrorString("Unknown fatal error occurred.");

    m_reported_fatal_error = true;

    for (const DiagnosticList::value_type &fatal_diagnostic :
         fatal_diagnostics.Diagnostics()) {
      // FIXME: need to add a CopyDiagnostic operation for copying diagnostics
      // from one manager to another.
      diagnostic_manager.AddDiagnostic(
          fatal_diagnostic->GetMessage(), fatal_diagnostic->GetSeverity(),
          fatal_diagnostic->getKind(), fatal_diagnostic->GetCompilerID());
    }
  } else {
    if (m_diagnostic_consumer_ap.get())
      static_cast<StoringDiagnosticConsumer *>(m_diagnostic_consumer_ap.get())
          ->PrintDiagnostics(diagnostic_manager, bufferID, first_line,
                             last_line, line_offset);
  }
}

void SwiftASTContext::ModulesDidLoad(ModuleList &module_list) {
  ClearModuleDependentCaches();
}

void SwiftASTContext::ClearModuleDependentCaches() {
  m_negative_type_cache.Clear();
  m_extra_type_info_cache.Clear();
}

void SwiftASTContext::DumpConfiguration(Log *log) {
  VALID_OR_RETURN_VOID();

  if (!log)
    return;

  log->Printf("(SwiftASTContext*)%p:", this);

  if (!m_ast_context_ap)
    log->Printf("  (no AST context)");

  log->Printf("  Architecture                 : %s",
              m_ast_context_ap->LangOpts.Target.getTriple().c_str());

  log->Printf("  SDK path                     : %s",
              m_ast_context_ap->SearchPathOpts.SDKPath.c_str());
  log->Printf("  Runtime resource path        : %s",
              m_ast_context_ap->SearchPathOpts.RuntimeResourcePath.c_str());
  log->Printf("  Runtime library path         : %s",
              m_ast_context_ap->SearchPathOpts.RuntimeLibraryPath.c_str());
  log->Printf(
      "  Runtime library import path  : %s",
      m_ast_context_ap->SearchPathOpts.RuntimeLibraryImportPath.c_str());

  log->Printf("  Framework search paths       : (%llu items)",
              (unsigned long long)
                  m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths.size());
  for (const auto &framework_search_path :
       m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths) {
    log->Printf("    %s", framework_search_path.Path.c_str());
  }

  log->Printf("  Import search paths          : (%llu items)",
              (unsigned long long)
                  m_ast_context_ap->SearchPathOpts.ImportSearchPaths.size());
  for (std::string &import_search_path :
       m_ast_context_ap->SearchPathOpts.ImportSearchPaths) {
    log->Printf("    %s", import_search_path.c_str());
  }

  swift::ClangImporterOptions &clang_importer_options =
      GetClangImporterOptions();

  log->Printf("  Extra clang arguments        : (%llu items)",
              (unsigned long long)clang_importer_options.ExtraArgs.size());
  for (std::string &extra_arg : clang_importer_options.ExtraArgs) {
    log->Printf("    %s", extra_arg.c_str());
  }
}

bool SwiftASTContext::HasTarget() const {
  lldb::TargetWP empty_wp;

  // If either call to "std::weak_ptr::owner_before(...) value returns true,
  // this indicates that m_section_wp once contained (possibly still does) a
  // reference to a valid shared pointer. This helps us know if we had a valid
  // reference to a target which is now invalid because the target was deleted.
  return empty_wp.owner_before(m_target_wp) ||
         m_target_wp.owner_before(empty_wp);
}

bool SwiftASTContext::CheckProcessChanged() {
  if (HasTarget()) {
    TargetSP target_sp(m_target_wp.lock());
    if (target_sp) {
      Process *process = target_sp->GetProcessSP().get();
      if (m_process == NULL) {
        if (process)
          m_process = process;
      } else {
        if (m_process != process)
          return true;
      }
    }
  }
  return false;
}

void SwiftASTContext::AddDebuggerClient(
    swift::DebuggerClient *debugger_client) {
  m_debugger_clients.push_back(
      std::unique_ptr<swift::DebuggerClient>(debugger_client));
}

SwiftASTContext::ExtraTypeInformation::ExtraTypeInformation()
    : m_flags(false, false) {}

SwiftASTContext::ExtraTypeInformation::ExtraTypeInformation(
    swift::CanType swift_can_type)
    : m_flags(false, false) {
  static ConstString g_rawValue("rawValue");

  swift::ASTContext &ast_ctx = swift_can_type->getASTContext();
  SwiftASTContext *swift_ast = SwiftASTContext::GetSwiftASTContext(&ast_ctx);
  if (swift_ast) {
    swift::ProtocolDecl *option_set =
        ast_ctx.getProtocol(swift::KnownProtocolKind::OptionSet);
    if (option_set) {
      if (auto nominal_decl =
              swift_can_type.getNominalOrBoundGenericNominal()) {
        for (swift::ProtocolDecl *protocol_decl :
             nominal_decl->getAllProtocols()) {
          if (protocol_decl == option_set) {
            for (swift::VarDecl *stored_property :
                 nominal_decl->getStoredProperties()) {
              swift::Identifier name = stored_property->getName();
              if (name.str() == g_rawValue.GetStringRef()) {
                m_flags.m_is_trivial_option_set = true;
                break;
              }
            }
          }
        }
      }
    }
  }

  if (auto metatype_type = swift::dyn_cast_or_null<swift::MetatypeType>(
          swift_can_type)) {
    if (!metatype_type->hasRepresentation() ||
        (swift::MetatypeRepresentation::Thin ==
         metatype_type->getRepresentation()))
      m_flags.m_is_zero_size = true;
  } else if (auto enum_decl = swift_can_type->getEnumOrBoundGenericEnum()) {
    size_t num_nopayload = 0, num_payload = 0;
    for (auto the_case : enum_decl->getAllElements()) {
      if (the_case->getArgumentInterfaceType()) {
        num_payload = 1;
        break;
      } else {
        if (++num_nopayload > 1)
          break;
      }
    }
    if (num_nopayload == 1 && num_payload == 0)
      m_flags.m_is_zero_size = true;
  } else if (auto struct_decl =
                 swift_can_type->getStructOrBoundGenericStruct()) {
    bool has_storage = false;
    auto members = struct_decl->getMembers();
    for (const auto &member : members) {
      if (swift::VarDecl *var_decl =
              swift::dyn_cast<swift::VarDecl>(member)) {
        if (!var_decl->isStatic() && var_decl->hasStorage()) {
          has_storage = true;
          break;
        }
      }
    }
    m_flags.m_is_zero_size = !has_storage;
  } else if (auto tuple_type = swift::dyn_cast_or_null<swift::TupleType>(
                 swift_can_type)) {
    m_flags.m_is_zero_size = (tuple_type->getNumElements() == 0);
  }
}

SwiftASTContext::ExtraTypeInformation
SwiftASTContext::GetExtraTypeInformation(void *type) {
  if (!type)
    return ExtraTypeInformation();

  swift::CanType swift_can_type;
  void *swift_can_type_ptr = nullptr;
  if (auto swift_type = GetSwiftType(type)) {
    swift_can_type = swift_type->getCanonicalType();
    swift_can_type_ptr = swift_can_type.getPointer();
  }
  if (!swift_can_type_ptr)
    return ExtraTypeInformation();

  ExtraTypeInformation eti;
  if (!m_extra_type_info_cache.Lookup(swift_can_type_ptr, eti)) {
    ExtraTypeInformation extra_info(swift_can_type);
    m_extra_type_info_cache.Insert(swift_can_type_ptr, extra_info);
    return extra_info;
  } else {
    return eti;
  }
}

bool SwiftASTContext::DeclContextIsStructUnionOrClass(void *opaque_decl_ctx) {
  return false;
}

ConstString SwiftASTContext::DeclContextGetName(void *opaque_decl_ctx) {
  return ConstString();
}

ConstString
SwiftASTContext::DeclContextGetScopeQualifiedName(void *opaque_decl_ctx) {
  return ConstString();
}

bool SwiftASTContext::DeclContextIsClassMethod(
    void *opaque_decl_ctx, lldb::LanguageType *language_ptr,
    bool *is_instance_method_ptr, ConstString *language_object_name_ptr) {
  return false;
}

///////////
////////////////////
///////////

bool SwiftASTContext::IsArrayType(void *type, CompilerType *element_type_ptr,
                                  uint64_t *size, bool *is_incomplete) {
  VALID_OR_RETURN(false);

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  swift::BoundGenericStructType *struct_type =
      swift_can_type->getAs<swift::BoundGenericStructType>();
  if (struct_type) {
    swift::StructDecl *struct_decl = struct_type->getDecl();
    if (strcmp(struct_decl->getName().get(), "Array") != 0)
      return false;
    if (!struct_decl->getModuleContext()->isStdlibModule())
      return false;
    const llvm::ArrayRef<swift::Type> &args = struct_type->getGenericArgs();
    if (args.size() != 1)
      return false;
    if (is_incomplete)
      *is_incomplete = true;
    if (size)
      *size = 0;
    if (element_type_ptr)
      *element_type_ptr =
          CompilerType(GetASTContext(), args[0].getPointer());
    return true;
  }

  return false;
}

bool SwiftASTContext::IsAggregateType(void *type) {
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    auto referent_type = swift_can_type->getReferenceStorageReferent();
    return (referent_type->is<swift::TupleType>() ||
            referent_type->is<swift::BuiltinVectorType>() ||
            referent_type->getAnyNominal());
  }

  return false;
}

bool SwiftASTContext::IsVectorType(void *type, CompilerType *element_type,
                                   uint64_t *size) {
  return false;
}

bool SwiftASTContext::IsRuntimeGeneratedType(void *type) { return false; }

bool SwiftASTContext::IsCharType(void *type) { return false; }

bool SwiftASTContext::IsCompleteType(void *type) { return true; }

bool SwiftASTContext::IsConst(void *type) { return false; }

bool SwiftASTContext::IsCStringType(void *type, uint32_t &length) {
  return false;
}

bool SwiftASTContext::IsFunctionType(void *type, bool *is_variadic_ptr) {
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind) {
    case swift::TypeKind::Function:
    case swift::TypeKind::GenericFunction:
      return true;
    case swift::TypeKind::SILFunction:
      return false; // TODO: is this correct?
    default:
      return false;
    }
  }
  return false;
}

// Used to detect "Homogeneous Floating-point Aggregates"
uint32_t SwiftASTContext::IsHomogeneousAggregate(void *type,
                                                 CompilerType *base_type_ptr) {
  return 0;
}

size_t SwiftASTContext::GetNumberOfFunctionArguments(void *type) {
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    auto func =
        swift::dyn_cast_or_null<swift::AnyFunctionType>(
            swift_can_type);
    if (func) {
      auto input = func.getInput();
      // See comment in swift::AnyFunctionType for rationale here:
      // A function can take either a tuple or a parentype, but if a parentype
      // (i.e. (Foo)), then it will be reduced down to just Foo, so if the input
      // is not a tuple, that must mean there is only 1 input.
      auto tuple = swift::dyn_cast<swift::TupleType>(input);
      if (tuple)
        return tuple->getNumElements();
      else
        return 1;
    }
  }
  return 0;
}

CompilerType SwiftASTContext::GetFunctionArgumentAtIndex(void *type,
                                                         const size_t index) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    auto func =
        swift::dyn_cast<swift::AnyFunctionType>(
            swift_can_type);
    if (func) {
      auto input = func.getInput();
      // See comment in swift::AnyFunctionType for rationale here:
      // A function can take either a tuple or a parentype, but if a parentype
      // (i.e. (Foo)), then it will be reduced down to just Foo, so if the input
      // is not a tuple, that must mean there is only 1 input.
      auto tuple = swift::dyn_cast<swift::TupleType>(input);
      if (tuple) {
        if (index < tuple->getNumElements())
          return CompilerType(GetASTContext(),
                              tuple->getElementType(index));
      } else
        return CompilerType(GetASTContext(), input);
    }
  }
  return CompilerType();
}

bool SwiftASTContext::IsFunctionPointerType(void *type) {
  return IsFunctionType(type, nullptr); // FIXME: think about this
}

bool SwiftASTContext::IsBlockPointerType(
    void *type, CompilerType *function_pointer_type_ptr) {
  return false;
}

bool SwiftASTContext::IsIntegerType(void *type, bool &is_signed) {
  return (GetTypeInfo(type, nullptr) & eTypeIsInteger);
}

bool SwiftASTContext::IsPointerType(void *type, CompilerType *pointee_type) {
  VALID_OR_RETURN(false);

  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    auto referent_type = swift_can_type->getReferenceStorageReferent();
    return (referent_type->is<swift::BuiltinRawPointerType>() ||
            referent_type->is<swift::BuiltinNativeObjectType>() ||
            referent_type->is<swift::BuiltinUnsafeValueBufferType>() ||
            referent_type->is<swift::BuiltinUnknownObjectType>() ||
            referent_type->is<swift::BuiltinBridgeObjectType>());
  }

  if (pointee_type)
    pointee_type->Clear();
  return false;
}

bool SwiftASTContext::IsPointerOrReferenceType(void *type,
                                               CompilerType *pointee_type) {
  return IsPointerType(type, pointee_type) ||
         IsReferenceType(type, pointee_type, nullptr);
}

bool SwiftASTContext::ShouldTreatScalarValueAsAddress(
    lldb::opaque_compiler_type_t type) {
  return Flags(GetTypeInfo(type, nullptr))
      .AnySet(eTypeInstanceIsPointer | eTypeIsReference);
}

bool SwiftASTContext::IsReferenceType(void *type, CompilerType *pointee_type,
                                      bool *is_rvalue) {
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind) {
    case swift::TypeKind::InOut:
    case swift::TypeKind::LValue:
      if (pointee_type)
        *pointee_type = GetNonReferenceType(type);
      return true;
    default:
      break;
    }
  }
  if (pointee_type)
    pointee_type->Clear();
  return false;
}

bool SwiftASTContext::IsFloatingPointType(void *type, uint32_t &count,
                                          bool &is_complex) {
  if (type) {
    if (GetTypeInfo(type, nullptr) & eTypeIsFloat) {
      count = 1;
      is_complex = false;
      return true;
    }
  }
  count = 0;
  is_complex = false;
  return false;
}

bool SwiftASTContext::IsDefined(void *type) {
  if (!type)
    return false;

  return true;
}

bool SwiftASTContext::IsPolymorphicClass(void *type) { return false; }

bool SwiftASTContext::IsPossibleDynamicType(void *type,
                                            CompilerType *dynamic_pointee_type,
                                            bool check_cplusplus,
                                            bool check_objc, bool check_swift) {
  VALID_OR_RETURN(false);

  if (type && check_swift) {
    // FIXME: use the dynamic_pointee_type
    Flags type_flags(GetTypeInfo(type, nullptr));

    if (type_flags.AnySet(eTypeIsArchetype | eTypeIsClass | eTypeIsProtocol))
      return true;

    if (type_flags.AnySet(eTypeIsStructUnion | eTypeIsEnumeration |
                          eTypeIsTuple)) {
      CompilerType compiler_type(GetASTContext(), GetCanonicalSwiftType(type));
      return !SwiftASTContext::IsFullyRealized(compiler_type);
    }

    auto can_type = GetCanonicalSwiftType(type).getPointer();
    if (can_type == GetASTContext()->TheRawPointerType.getPointer())
      return true;
    if (can_type == GetASTContext()->TheUnknownObjectType.getPointer())
      return true;
    if (can_type == GetASTContext()->TheNativeObjectType.getPointer())
      return true;
    if (can_type == GetASTContext()->TheBridgeObjectType.getPointer())
      return true;
  }

  if (dynamic_pointee_type)
    dynamic_pointee_type->Clear();
  return false;
}

bool SwiftASTContext::IsScalarType(void *type) {
  if (!type)
    return false;

  return (GetTypeInfo(type, nullptr) & eTypeIsScalar) != 0;
}

bool SwiftASTContext::IsTypedefType(void *type) {
  if (!type)
    return false;
  swift::Type swift_type(GetSwiftType(type));
  return swift::isa<swift::NameAliasType>(swift_type.getPointer());
}

bool SwiftASTContext::IsVoidType(void *type) {
  VALID_OR_RETURN(false);

  if (!type)
    return false;
  return type == GetASTContext()->TheEmptyTupleType.getPointer();
}

bool SwiftASTContext::IsArchetypeType(const CompilerType &compiler_type) {
  if (!compiler_type.IsValid())
    return false;

  if (llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem())) {
    swift::Type swift_type(GetSwiftType(compiler_type));
    return swift_type->is<swift::ArchetypeType>();
  }
  return false;
}

bool SwiftASTContext::IsSelfArchetypeType(const CompilerType &compiler_type) {
  if (!compiler_type.IsValid())
    return false;

  if (llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem())) {
    if (swift::isa<swift::ArchetypeType>(
        (swift::TypeBase *)compiler_type.GetOpaqueQualType())) {
      // Hack: Just assume if we have an archetype as the type of 'self',
      // it's going to be a protocol 'Self' type.
      return true;
    }
  }
  return false;
}

bool SwiftASTContext::IsPossibleZeroSizeType(
    const CompilerType &compiler_type) {
  if (!compiler_type.IsValid())
    return false;

  if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(
          compiler_type.GetTypeSystem()))
    return ast
        ->GetExtraTypeInformation(
            GetCanonicalSwiftType(compiler_type).getPointer())
        .m_flags.m_is_zero_size;
  return false;
}

bool SwiftASTContext::IsErrorType(const CompilerType &compiler_type) {
  if (compiler_type.IsValid() &&
      llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem())) {
    ProtocolInfo protocol_info;

    if (GetProtocolTypeInfo(compiler_type, protocol_info))
      return protocol_info.m_is_errortype;
    return false;
  }
  return false;
}

CompilerType
SwiftASTContext::GetReferentType(const CompilerType &compiler_type) {
  VALID_OR_RETURN(CompilerType());

  if (compiler_type.IsValid() &&
      llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem())) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(compiler_type));
    swift::TypeBase *swift_type = swift_can_type.getPointer();
    if (swift_type && llvm::isa<swift::WeakStorageType>(swift_type))
      return compiler_type;

    auto ref_type = swift_can_type->getReferenceStorageReferent();
    return CompilerType(GetASTContext(), ref_type);
  }

  return CompilerType();
}

bool SwiftASTContext::IsTrivialOptionSetType(
    const CompilerType &compiler_type) {
  if (compiler_type.IsValid() &&
      llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
    return GetExtraTypeInformation(compiler_type.GetOpaqueQualType())
        .m_flags.m_is_trivial_option_set;
  return false;
}

bool SwiftASTContext::IsFullyRealized(const CompilerType &compiler_type) {
  if (!compiler_type.IsValid())
    return false;

  if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(
          compiler_type.GetTypeSystem())) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(compiler_type));
    if (swift::isa<swift::MetatypeType>(swift_can_type))
      return true;
    return !swift_can_type->hasArchetype();
  }

  return false;
}

bool SwiftASTContext::GetProtocolTypeInfo(const CompilerType &type,
                                          ProtocolInfo &protocol_info) {
  if (auto ast =
          llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem())) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    if (!swift_can_type.isExistentialType())
      return false;

    swift::ExistentialLayout layout = swift_can_type.getExistentialLayout();
    protocol_info.m_is_class_only = layout.requiresClass();
    protocol_info.m_num_protocols = layout.getProtocols().size();
    protocol_info.m_is_objc = layout.isObjC();
    protocol_info.m_is_anyobject = layout.isAnyObject();
    protocol_info.m_is_errortype = layout.isErrorExistential();

    if (layout.superclass) {
      protocol_info.m_superclass =
        CompilerType(ast->GetASTContext(), layout.superclass.getPointer());
    }

    unsigned num_witness_tables = 0;
    for (auto protoTy : layout.getProtocols()) {
      if (!protoTy->getDecl()->isObjC())
        num_witness_tables++;
    }

    if (layout.isErrorExistential()) {
      // Error existential -- instance pointer only
      protocol_info.m_num_payload_words = 0;
      protocol_info.m_num_storage_words = 1;
    } else if (layout.requiresClass()) {
      // Class-constrained existential -- instance pointer plus witness tables
      protocol_info.m_num_payload_words = 0;
      protocol_info.m_num_storage_words = 1 + num_witness_tables;
    } else {
      // Opaque existential -- three words of inline storage, metadata and
      // witness tables
      protocol_info.m_num_payload_words = swift::NumWords_ValueBuffer;
      protocol_info.m_num_storage_words =
        swift::NumWords_ValueBuffer + 1 + num_witness_tables;
    }

    return true;
  }

  return false;
}

SwiftASTContext::TypeAllocationStrategy
SwiftASTContext::GetAllocationStrategy(const CompilerType &type) {
  if (auto ast =
          llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem())) {
    const swift::irgen::TypeInfo *type_info =
        ast->GetSwiftTypeInfo(type.GetOpaqueQualType());
    if (!type_info)
      return TypeAllocationStrategy::eUnknown;
    switch (type_info->getFixedPacking(ast->GetIRGenModule())) {
    case swift::irgen::FixedPacking::OffsetZero:
      return TypeAllocationStrategy::eInline;
    case swift::irgen::FixedPacking::Allocate:
      return TypeAllocationStrategy::ePointer;
    case swift::irgen::FixedPacking::Dynamic:
      return TypeAllocationStrategy::eDynamic;
    default:
      break;
    }
  }

  return TypeAllocationStrategy::eUnknown;
}

bool SwiftASTContext::IsBeingDefined(void *type) { return false; }

bool SwiftASTContext::IsObjCObjectPointerType(const CompilerType &type,
                                              CompilerType *class_type_ptr) {
  if (!type)
    return false;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  const swift::TypeKind type_kind = swift_can_type->getKind();
  if (type_kind == swift::TypeKind::BuiltinNativeObject ||
      type_kind == swift::TypeKind::BuiltinUnknownObject)
    return true;

  if (class_type_ptr)
    class_type_ptr->Clear();
  return false;
}

//----------------------------------------------------------------------
// Type Completion
//----------------------------------------------------------------------

bool SwiftASTContext::GetCompleteType(void *type) { return true; }

ConstString SwiftASTContext::GetTypeName(void *type) {
  std::string type_name;
  if (type) {
    swift::Type swift_type(GetSwiftType(type));

    swift::Type normalized_type =
        swift_type.transform([](swift::Type type) -> swift::Type {
          if (swift::SyntaxSugarType *syntax_sugar_type =
                  swift::dyn_cast<swift::SyntaxSugarType>(type.getPointer())) {
            return syntax_sugar_type->getSinglyDesugaredType();
          }
          if (swift::DictionaryType *dictionary_type =
                  swift::dyn_cast<swift::DictionaryType>(type.getPointer())) {
            return dictionary_type->getSinglyDesugaredType();
          }
          return type;
        });

    swift::PrintOptions print_options;
    print_options.FullyQualifiedTypes = true;
    print_options.SynthesizeSugarOnTypes = false;
    type_name = normalized_type.getString(print_options);
  }
  return ConstString(type_name);
}

ConstString SwiftASTContext::GetDisplayTypeName(void *type) {
  std::string type_name(GetTypeName(type).AsCString(""));

  if (type) {
    swift::Type swift_type(GetSwiftType(type));

    swift::PrintOptions print_options;
    print_options.FullyQualifiedTypes = false;
    print_options.SynthesizeSugarOnTypes = true;
    print_options.FullyQualifiedTypesIfAmbiguous = true;
    type_name = swift_type.getString(print_options);
  }

  return ConstString(type_name);
}

ConstString SwiftASTContext::GetTypeSymbolName(void *type) {
  swift::Type swift_type(GetSwiftType(type));
  return GetTypeName(swift_type->getWithoutParens().getPointer());
}

ConstString SwiftASTContext::GetMangledTypeName(void *type) {
  return GetMangledTypeName(GetSwiftType(type).getPointer());
}

uint32_t
SwiftASTContext::GetTypeInfo(void *type,
                             CompilerType *pointee_or_element_clang_type) {
  VALID_OR_RETURN(0);

  if (!type)
    return 0;

  if (pointee_or_element_clang_type)
    pointee_or_element_clang_type->Clear();

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  const swift::TypeKind type_kind = swift_can_type->getKind();
  uint32_t swift_flags = eTypeIsSwift;
  switch (type_kind) {
  case swift::TypeKind::DependentMember:
  case swift::TypeKind::Error:
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::Module:
  case swift::TypeKind::TypeVariable:
    break;
  case swift::TypeKind::UnboundGeneric:
    swift_flags |= eTypeIsGeneric;
    break;

  case swift::TypeKind::GenericFunction:
    swift_flags |= eTypeIsGeneric;
  case swift::TypeKind::Function:
    swift_flags |=
        eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeInstanceIsPointer;
    break;
  case swift::TypeKind::BuiltinInteger:
    swift_flags |=
        eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeIsInteger;
    break;
  case swift::TypeKind::BuiltinFloat:
    swift_flags |=
        eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeIsFloat;
    break;
  case swift::TypeKind::BuiltinRawPointer:
    swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer |
                   eTypeIsScalar | eTypeHasValue;
    break;
  case swift::TypeKind::BuiltinNativeObject:
    swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer |
                   eTypeIsScalar | eTypeHasValue;
    break;
  case swift::TypeKind::BuiltinUnknownObject:
    swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer |
                   eTypeIsScalar | eTypeHasValue | eTypeIsObjC;
    break;
  case swift::TypeKind::BuiltinBridgeObject:
    swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer |
                   eTypeIsScalar | eTypeHasValue | eTypeIsObjC;
    break;
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
    swift_flags |=
        eTypeIsBuiltIn | eTypeIsPointer | eTypeIsScalar | eTypeHasValue;
    break;
  case swift::TypeKind::BuiltinVector:
    // TODO: OR in eTypeIsFloat or eTypeIsInteger as needed
    return eTypeIsBuiltIn | eTypeHasChildren | eTypeIsVector;
    break;

  case swift::TypeKind::Tuple:
    swift_flags |= eTypeHasChildren | eTypeIsTuple;
    break;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    swift_flags |=
        CompilerType(GetASTContext(),
                     swift_can_type->getReferenceStorageReferent())
            .GetTypeInfo(pointee_or_element_clang_type);
    break;
  case swift::TypeKind::BoundGenericEnum:
    swift_flags |= eTypeIsGeneric | eTypeIsBound;
  case swift::TypeKind::Enum: {
    SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo(type);
    if (cached_enum_info) {
      if (cached_enum_info->GetNumElementsWithPayload() == 0)
        swift_flags |= eTypeHasValue | eTypeIsEnumeration;
      else
        swift_flags |= eTypeHasValue | eTypeIsEnumeration | eTypeHasChildren;
    } else
      swift_flags |= eTypeIsEnumeration;
  } break;

  case swift::TypeKind::BoundGenericStruct:
    swift_flags |= eTypeIsGeneric | eTypeIsBound;
  case swift::TypeKind::Struct:
    swift_flags |= eTypeHasChildren | eTypeIsStructUnion;
    break;

  case swift::TypeKind::BoundGenericClass:
    swift_flags |= eTypeIsGeneric | eTypeIsBound;
  case swift::TypeKind::Class:
    swift_flags |= eTypeHasChildren | eTypeIsClass | eTypeHasValue |
                   eTypeInstanceIsPointer;
    break;

  case swift::TypeKind::Protocol:
  case swift::TypeKind::ProtocolComposition:
    swift_flags |= eTypeHasChildren | eTypeIsStructUnion | eTypeIsProtocol;
    break;
  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype:
    swift_flags |= eTypeIsMetatype | eTypeHasValue;
    break;

  case swift::TypeKind::Archetype:
    swift_flags |=
        eTypeHasValue | eTypeIsScalar | eTypeIsPointer | eTypeIsArchetype;
    break;

  case swift::TypeKind::LValue:
    if (pointee_or_element_clang_type)
      *pointee_or_element_clang_type = GetNonReferenceType(type);
    swift_flags |= eTypeHasChildren | eTypeIsReference | eTypeHasValue;
    break;
  case swift::TypeKind::InOut:
  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }
  return swift_flags;
}

lldb::LanguageType SwiftASTContext::GetMinimumLanguage(void *type) {
  if (!type)
    return lldb::eLanguageTypeC;

  return lldb::eLanguageTypeSwift;
}

lldb::TypeClass SwiftASTContext::GetTypeClass(void *type) {
  VALID_OR_RETURN(lldb::eTypeClassInvalid);

  if (!type)
    return lldb::eTypeClassInvalid;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
    return lldb::eTypeClassOther;
  case swift::TypeKind::BuiltinInteger:
    return lldb::eTypeClassBuiltin;
  case swift::TypeKind::BuiltinFloat:
    return lldb::eTypeClassBuiltin;
  case swift::TypeKind::BuiltinRawPointer:
    return lldb::eTypeClassBuiltin;
  case swift::TypeKind::BuiltinNativeObject:
    return lldb::eTypeClassBuiltin;
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
    return lldb::eTypeClassBuiltin;
  case swift::TypeKind::BuiltinUnknownObject:
    return lldb::eTypeClassBuiltin;
  case swift::TypeKind::BuiltinBridgeObject:
    return lldb::eTypeClassBuiltin;
  case swift::TypeKind::BuiltinVector:
    return lldb::eTypeClassVector;
  case swift::TypeKind::Tuple:
    return lldb::eTypeClassArray;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .GetTypeClass();
  case swift::TypeKind::GenericTypeParam:
    return lldb::eTypeClassOther;
  case swift::TypeKind::DependentMember:
    return lldb::eTypeClassOther;
  case swift::TypeKind::Enum:
    return lldb::eTypeClassUnion;
  case swift::TypeKind::Struct:
    return lldb::eTypeClassStruct;
  case swift::TypeKind::Class:
    return lldb::eTypeClassClass;
  case swift::TypeKind::Protocol:
    return lldb::eTypeClassOther;
  case swift::TypeKind::Metatype:
    return lldb::eTypeClassOther;
  case swift::TypeKind::Module:
    return lldb::eTypeClassOther;
  case swift::TypeKind::Archetype:
    return lldb::eTypeClassOther;
  case swift::TypeKind::Function:
    return lldb::eTypeClassFunction;
  case swift::TypeKind::GenericFunction:
    return lldb::eTypeClassFunction;
  case swift::TypeKind::ProtocolComposition:
    return lldb::eTypeClassOther;
  case swift::TypeKind::LValue:
    return lldb::eTypeClassReference;
  case swift::TypeKind::UnboundGeneric:
    return lldb::eTypeClassOther;
  case swift::TypeKind::BoundGenericClass:
    return lldb::eTypeClassClass;
  case swift::TypeKind::BoundGenericEnum:
    return lldb::eTypeClassUnion;
  case swift::TypeKind::BoundGenericStruct:
    return lldb::eTypeClassStruct;
  case swift::TypeKind::TypeVariable:
    return lldb::eTypeClassOther;
  case swift::TypeKind::ExistentialMetatype:
    return lldb::eTypeClassOther;
  case swift::TypeKind::DynamicSelf:
    return lldb::eTypeClassOther;
  case swift::TypeKind::SILBox:
    return lldb::eTypeClassOther;
  case swift::TypeKind::SILFunction:
    return lldb::eTypeClassFunction;
  case swift::TypeKind::SILBlockStorage:
    return lldb::eTypeClassOther;
  case swift::TypeKind::InOut:
    return lldb::eTypeClassOther;
  case swift::TypeKind::Unresolved:
    return lldb::eTypeClassOther;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }

  return lldb::eTypeClassOther;
}

unsigned SwiftASTContext::GetTypeQualifiers(void *type) { return 0; }

//----------------------------------------------------------------------
// Creating related types
//----------------------------------------------------------------------

CompilerType SwiftASTContext::GetArrayElementType(void *type,
                                                  uint64_t *stride) {
  VALID_OR_RETURN(CompilerType());

  CompilerType element_type;
  if (type) {
    swift::CanType swift_type(GetCanonicalSwiftType(type));
    // There are a couple of structs that mean "Array" in Swift:
    // Array<T>
    // NativeArray<T>
    // Slice<T>
    // Treat them as arrays for convenience sake.
    swift::BoundGenericStructType *boundGenericStructType(
        swift_type->getAs<swift::BoundGenericStructType>());
    if (boundGenericStructType) {
      auto args = boundGenericStructType->getGenericArgs();
      swift::StructDecl *decl = boundGenericStructType->getDecl();
      if (args.size() == 1 &&
          decl->getModuleContext()->isStdlibModule()) {
        const char *declname = decl->getName().get();
        if (0 == strcmp(declname, "NativeArray") ||
            0 == strcmp(declname, "Array") || 0 == strcmp(declname, "ArraySlice"))
          element_type = CompilerType(GetASTContext(), args[0].getPointer());
      }
    }
  }
  return element_type;
}

CompilerType SwiftASTContext::GetCanonicalType(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (type)
    return CompilerType(GetASTContext(),
                        GetCanonicalSwiftType(type).getPointer());
  return CompilerType();
}

CompilerType SwiftASTContext::GetInstanceType(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (!type)
    return CompilerType();

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  switch (swift_can_type->getKind()) {
  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype: {
    auto metatype_type =
        swift::dyn_cast<swift::AnyMetatypeType>(swift_can_type);
    if (metatype_type)
      return CompilerType(GetASTContext(),
                          metatype_type.getInstanceType().getPointer());
    return CompilerType();
  }
  default:
    break;
  }

  return CompilerType(GetASTContext(), GetSwiftType(type));
}

CompilerType SwiftASTContext::GetFullyUnqualifiedType(void *type) {
  VALID_OR_RETURN(CompilerType());

  return CompilerType(GetASTContext(), GetSwiftType(type));
}

int SwiftASTContext::GetFunctionArgumentCount(void *type) {
  return GetNumberOfFunctionArguments(type);
}

CompilerType SwiftASTContext::GetFunctionArgumentTypeAtIndex(void *type,
                                                             size_t idx) {
  return GetFunctionArgumentAtIndex(type, idx);
}

CompilerType SwiftASTContext::GetFunctionReturnType(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    auto func = swift::dyn_cast<swift::AnyFunctionType>(
        GetCanonicalSwiftType(type));
    if (func)
      return CompilerType(GetASTContext(), func.getResult().getPointer());
  }
  return CompilerType();
}

size_t SwiftASTContext::GetNumMemberFunctions(void *type) {
  size_t num_functions = 0;
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));

    auto nominal_decl = swift_can_type.getAnyNominal();
    if (nominal_decl) {
      auto iter = nominal_decl->getMembers().begin();
      auto end = nominal_decl->getMembers().end();
      for (; iter != end; iter++) {
        switch (iter->getKind()) {
        case swift::DeclKind::Constructor:
        case swift::DeclKind::Destructor:
        case swift::DeclKind::Func:
          num_functions += 1;
          break;
        default:
          break;
        }
      }
    }
  }
  return num_functions;
}

TypeMemberFunctionImpl SwiftASTContext::GetMemberFunctionAtIndex(void *type,
                                                                 size_t idx) {
  VALID_OR_RETURN(TypeMemberFunctionImpl());

  std::string name("");
  CompilerType result_type;
  MemberFunctionKind kind(MemberFunctionKind::eMemberFunctionKindUnknown);
  swift::AbstractFunctionDecl *the_decl_we_care_about = nullptr;
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));

    auto nominal_decl = swift_can_type.getAnyNominal();
    if (nominal_decl) {
      auto iter = nominal_decl->getMembers().begin();
      auto end = nominal_decl->getMembers().end();
      for (; iter != end; iter++) {
        auto decl_kind = iter->getKind();
        switch (decl_kind) {
        case swift::DeclKind::Constructor:
        case swift::DeclKind::Destructor:
        case swift::DeclKind::Func: {
          if (idx == 0) {
            swift::AbstractFunctionDecl *abstract_func_decl =
                llvm::dyn_cast_or_null<swift::AbstractFunctionDecl>(*iter);
            if (abstract_func_decl) {
              switch (decl_kind) {
              case swift::DeclKind::Constructor:
                name.clear();
                kind = lldb::eMemberFunctionKindConstructor;
                the_decl_we_care_about = abstract_func_decl;
                break;
              case swift::DeclKind::Destructor:
                name.clear();
                kind = lldb::eMemberFunctionKindDestructor;
                the_decl_we_care_about = abstract_func_decl;
                break;
              case swift::DeclKind::Func:
              default: // I know that this can only be one of three kinds
                       // since I am here..
              {
                swift::FuncDecl *func_decl =
                    llvm::dyn_cast<swift::FuncDecl>(*iter);
                if (func_decl) {
                  if (func_decl->getName().empty())
                    name.clear();
                  else
                    name.assign(func_decl->getName().get());
                  if (func_decl->isStatic())
                    kind = lldb::eMemberFunctionKindStaticMethod;
                  else
                    kind = lldb::eMemberFunctionKindInstanceMethod;
                  the_decl_we_care_about = func_decl;
                }
              }
              }
              result_type =
                  CompilerType(GetASTContext(),
                               abstract_func_decl->getInterfaceType().getPointer());
            }
          } else
            --idx;
        } break;
        default:
          break;
        }
      }
    }
  }

  if (type && the_decl_we_care_about && (kind != eMemberFunctionKindUnknown))
    return TypeMemberFunctionImpl(
        result_type, CompilerDecl(this, the_decl_we_care_about), name, kind);

  return TypeMemberFunctionImpl();
}

CompilerType SwiftASTContext::GetLValueReferenceType(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (type)
    return CompilerType(GetASTContext(),
                        swift::LValueType::get(GetSwiftType(type)));
  return CompilerType();
}

CompilerType SwiftASTContext::GetRValueReferenceType(void *type) {
  return CompilerType();
}

CompilerType SwiftASTContext::GetNonReferenceType(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));

    swift::LValueType *lvalue = swift_can_type->getAs<swift::LValueType>();
    if (lvalue)
      return CompilerType(GetASTContext(),
                          lvalue->getObjectType().getPointer());
    swift::InOutType *inout = swift_can_type->getAs<swift::InOutType>();
    if (inout)
        return CompilerType(GetASTContext(),
                            inout->getObjectType().getPointer());
  }
  return CompilerType();
}

CompilerType SwiftASTContext::GetPointeeType(void *type) {
  return CompilerType();
}

CompilerType SwiftASTContext::GetPointerType(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::Type swift_type(::GetSwiftType(type));
    const swift::TypeKind type_kind = swift_type->getKind();
    if (type_kind == swift::TypeKind::BuiltinRawPointer)
      return CompilerType(GetASTContext(), swift_type);
  }
  return CompilerType();
}

CompilerType SwiftASTContext::GetTypedefedType(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::Type swift_type(::GetSwiftType(type));
    swift::NameAliasType *name_alias_type =
        swift::dyn_cast<swift::NameAliasType>(swift_type.getPointer());
    if (name_alias_type) {
      return CompilerType(GetASTContext(),
                          name_alias_type->getSinglyDesugaredType());
    }
  }

  return CompilerType();
}

CompilerType
SwiftASTContext::GetUnboundType(lldb::opaque_compiler_type_t type) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    swift::BoundGenericType *bound_generic_type =
        swift_can_type->getAs<swift::BoundGenericType>();
    if (bound_generic_type) {
      swift::NominalTypeDecl *nominal_type_decl = bound_generic_type->getDecl();
      if (nominal_type_decl)
        return CompilerType(GetASTContext(),
                            nominal_type_decl->getDeclaredType());
    }
  }

  return CompilerType(GetASTContext(), GetSwiftType(type));
}

//----------------------------------------------------------------------
// Create related types using the current type's AST
//----------------------------------------------------------------------

CompilerType SwiftASTContext::GetBasicTypeFromAST(lldb::BasicType basic_type) {
  return CompilerType();
}

CompilerType SwiftASTContext::GetIntTypeFromBitSize(size_t bit_size,
                                                    bool is_signed) {
  return CompilerType();
}

CompilerType SwiftASTContext::GetFloatTypeFromBitSize(size_t bit_size) {
  return CompilerType();
}

//----------------------------------------------------------------------
// Exploring the type
//----------------------------------------------------------------------

const swift::irgen::TypeInfo *
SwiftASTContext::GetSwiftTypeInfo(swift::Type container_type,
                                  swift::VarDecl *item_decl) {
  VALID_OR_RETURN(nullptr);

  if (container_type && item_decl) {
    auto &irgen_module = GetIRGenModule();
    swift::CanType container_can_type(
        GetCanonicalSwiftType(container_type.getPointer()));
    swift::SILType lowered_container_type =
        irgen_module.getLoweredType(container_can_type);
    swift::SILType lowered_field_type =
        lowered_container_type.getFieldType(item_decl, *GetSILModule());
    return &irgen_module.getTypeInfo(lowered_field_type);
  }

  return nullptr;
}

const swift::irgen::TypeInfo *SwiftASTContext::GetSwiftTypeInfo(void *type) {
  VALID_OR_RETURN(nullptr);

  if (type) {
    auto &irgen_module = GetIRGenModule();
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    swift::SILType swift_sil_type = irgen_module.getLoweredType(
        swift_can_type);
    return &irgen_module.getTypeInfo(swift_sil_type);
  }
  return nullptr;
}

const swift::irgen::FixedTypeInfo *
SwiftASTContext::GetSwiftFixedTypeInfo(void *type) {
  VALID_OR_RETURN(nullptr);

  const swift::irgen::TypeInfo *type_info = GetSwiftTypeInfo(type);
  if (type_info) {
    if (type_info->isFixedSize())
      return swift::cast<const swift::irgen::FixedTypeInfo>(type_info);
  }
  return nullptr;
}

uint64_t SwiftASTContext::GetBitSize(lldb::opaque_compiler_type_t type,
                                     ExecutionContextScope *exe_scope) {
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind) {
    case swift::TypeKind::Archetype:
    case swift::TypeKind::LValue:
    case swift::TypeKind::UnboundGeneric:
    case swift::TypeKind::GenericFunction:
    case swift::TypeKind::Function:
      return GetPointerByteSize() * 8;
    default:
      break;
    }
    const swift::irgen::FixedTypeInfo *fixed_type_info =
        GetSwiftFixedTypeInfo(type);
    if (fixed_type_info)
      return fixed_type_info->getFixedSize().getValue() * 8;
  }
  return 0;
}

uint64_t SwiftASTContext::GetByteStride(lldb::opaque_compiler_type_t type) {
  if (type) {
    const swift::irgen::FixedTypeInfo *fixed_type_info =
        GetSwiftFixedTypeInfo(type);
    if (fixed_type_info)
      return fixed_type_info->getFixedStride().getValue();
  }
  return 0;
}

size_t SwiftASTContext::GetTypeBitAlign(void *type) {
  if (type) {
    const swift::irgen::FixedTypeInfo *fixed_type_info =
        GetSwiftFixedTypeInfo(type);
    if (fixed_type_info)
      return fixed_type_info->getFixedAlignment().getValue();
  }
  return 0;
}

lldb::Encoding SwiftASTContext::GetEncoding(void *type, uint64_t &count) {
  VALID_OR_RETURN(lldb::eEncodingInvalid);

  if (!type)
    return lldb::eEncodingInvalid;

  count = 1;
  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
    break;
  case swift::TypeKind::BuiltinInteger:
    return lldb::eEncodingSint; // TODO: detect if an integer is unsigned
  case swift::TypeKind::BuiltinFloat:
    return lldb::eEncodingIEEE754; // TODO: detect if an integer is unsigned

  case swift::TypeKind::Archetype:
  case swift::TypeKind::BuiltinRawPointer:
  case swift::TypeKind::BuiltinNativeObject:
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
  case swift::TypeKind::BuiltinUnknownObject:
  case swift::TypeKind::BuiltinBridgeObject:
  case swift::TypeKind::Class: // Classes are pointers in swift...
  case swift::TypeKind::BoundGenericClass:
    return lldb::eEncodingUint;

  case swift::TypeKind::BuiltinVector:
    break;
  case swift::TypeKind::Tuple:
    break;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .GetEncoding(count);
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::DependentMember:
    break;

  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype:
    return lldb::eEncodingUint;

  case swift::TypeKind::GenericFunction:
  case swift::TypeKind::Function:
    return lldb::eEncodingUint;

  case swift::TypeKind::Enum:
  case swift::TypeKind::BoundGenericEnum:
    break;

  case swift::TypeKind::Struct:
  case swift::TypeKind::Protocol:
  case swift::TypeKind::Module:
  case swift::TypeKind::ProtocolComposition:
    break;
  case swift::TypeKind::LValue:
    return lldb::eEncodingUint;
  case swift::TypeKind::UnboundGeneric:
  case swift::TypeKind::BoundGenericStruct:
  case swift::TypeKind::TypeVariable:
  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::InOut:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }
  count = 0;
  return lldb::eEncodingInvalid;
}

lldb::Format SwiftASTContext::GetFormat(void *type) {
  VALID_OR_RETURN(lldb::eFormatInvalid);

  if (!type)
    return lldb::eFormatDefault;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
    break;
  case swift::TypeKind::BuiltinInteger:
    return eFormatDecimal; // TODO: detect if an integer is unsigned
  case swift::TypeKind::BuiltinFloat:
    return eFormatFloat; // TODO: detect if an integer is unsigned

  case swift::TypeKind::BuiltinRawPointer:
  case swift::TypeKind::BuiltinNativeObject:
  case swift::TypeKind::BuiltinUnknownObject:
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
  case swift::TypeKind::BuiltinBridgeObject:
  case swift::TypeKind::Archetype:
    return eFormatAddressInfo;

  // Classes are always pointers in swift...
  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass:
    return eFormatHex;

  case swift::TypeKind::BuiltinVector:
    break;
  case swift::TypeKind::Tuple:
    break;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .GetFormat();
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::DependentMember:
    break;

  case swift::TypeKind::Enum:
  case swift::TypeKind::BoundGenericEnum:
    return eFormatUnsigned;

  case swift::TypeKind::GenericFunction:
  case swift::TypeKind::Function:
    return lldb::eFormatAddressInfo;

  case swift::TypeKind::Struct:
  case swift::TypeKind::Protocol:
  case swift::TypeKind::Metatype:
  case swift::TypeKind::Module:
  case swift::TypeKind::ProtocolComposition:
    break;
  case swift::TypeKind::LValue:
    return lldb::eFormatHex;
  case swift::TypeKind::UnboundGeneric:
  case swift::TypeKind::BoundGenericStruct:
  case swift::TypeKind::TypeVariable:
  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::InOut:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }
  // We don't know hot to display this type...
  return lldb::eFormatBytes;
}

uint32_t SwiftASTContext::GetNumChildren(void *type,
                                         bool omit_empty_base_classes) {
  VALID_OR_RETURN(0);

  if (!type)
    return 0;

  uint32_t num_children = 0;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
  case swift::TypeKind::BuiltinInteger:
  case swift::TypeKind::BuiltinFloat:
  case swift::TypeKind::BuiltinRawPointer:
  case swift::TypeKind::BuiltinNativeObject:
  case swift::TypeKind::BuiltinUnknownObject:
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
  case swift::TypeKind::BuiltinBridgeObject:
  case swift::TypeKind::BuiltinVector:
  case swift::TypeKind::Module:
  case swift::TypeKind::Function:
  case swift::TypeKind::GenericFunction:
  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::InOut:
    break;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .GetNumChildren(omit_empty_base_classes);
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::DependentMember:
    break;

  case swift::TypeKind::Enum:
  case swift::TypeKind::BoundGenericEnum: {
    SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo(type);
    if (cached_enum_info)
      return cached_enum_info->GetNumElementsWithPayload();
  } break;

  case swift::TypeKind::Tuple:
  case swift::TypeKind::Struct:
  case swift::TypeKind::BoundGenericStruct:
    return GetNumFields(type);

  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass: {
    auto class_decl = swift_can_type->getClassOrBoundGenericClass();
    return (class_decl->hasSuperclass() ? 1 : 0) + GetNumFields(type);
  }

  case swift::TypeKind::Protocol:
  case swift::TypeKind::ProtocolComposition: {
    ProtocolInfo protocol_info;
    if (!GetProtocolTypeInfo(
            CompilerType(GetASTContext(), GetSwiftType(type)), protocol_info))
      break;

    return protocol_info.m_num_storage_words;
  }

  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype:
  case swift::TypeKind::Archetype:
    return 0;

  case swift::TypeKind::LValue: {
    swift::LValueType *lvalue_type = swift_can_type->castTo<swift::LValueType>();
    swift::TypeBase *deref_type = lvalue_type->getObjectType().getPointer();

    uint32_t num_pointee_children =
        CompilerType(GetASTContext(), deref_type)
            .GetNumChildren(omit_empty_base_classes);
    // If this type points to a simple type (or to a class), then it has 1 child
    if (num_pointee_children == 0 || deref_type->getClassOrBoundGenericClass())
      num_children = 1;
    else
      num_children = num_pointee_children;
  } break;

  case swift::TypeKind::UnboundGeneric:
    break;
  case swift::TypeKind::TypeVariable:
    break;

  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }

  return num_children;
}

lldb::BasicType SwiftASTContext::GetBasicTypeEnumeration(void *type) {
  return eBasicTypeInvalid;
}

#pragma mark Aggregate Types

uint32_t SwiftASTContext::GetNumDirectBaseClasses(void *opaque_type) {
  if (!opaque_type)
    return 0;

  swift::CanType swift_can_type(GetCanonicalSwiftType(opaque_type));
  swift::ClassDecl *class_decl = swift_can_type->getClassOrBoundGenericClass();
  if (class_decl) {
    if (class_decl->hasSuperclass())
      return 1;
  }

  return 0;
}

uint32_t SwiftASTContext::GetNumVirtualBaseClasses(void *opaque_type) {
  return 0;
}

uint32_t SwiftASTContext::GetNumFields(void *type) {
  VALID_OR_RETURN(0);

  if (!type)
    return 0;

  uint32_t count = 0;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
  case swift::TypeKind::BuiltinInteger:
  case swift::TypeKind::BuiltinFloat:
  case swift::TypeKind::BuiltinRawPointer:
  case swift::TypeKind::BuiltinNativeObject:
  case swift::TypeKind::BuiltinUnknownObject:
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
  case swift::TypeKind::BuiltinBridgeObject:
  case swift::TypeKind::BuiltinVector:
    break;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .GetNumFields();
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::DependentMember:
    break;

  case swift::TypeKind::Enum:
  case swift::TypeKind::BoundGenericEnum: {
    SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo(type);
    if (cached_enum_info)
      return cached_enum_info->GetNumElementsWithPayload();
  } break;

  case swift::TypeKind::Tuple:
    return cast<swift::TupleType>(swift_can_type)->getNumElements();

  case swift::TypeKind::Struct:
  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass:
  case swift::TypeKind::BoundGenericStruct: {
    auto nominal = swift_can_type->getAnyNominal();
    return GetStoredProperties(nominal).size();
  }

  case swift::TypeKind::Protocol:
  case swift::TypeKind::ProtocolComposition:
    return GetNumChildren(type, /*omit_empty_base_classes=*/false);

  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype:
    return 0;

  case swift::TypeKind::Module:
  case swift::TypeKind::Archetype:
  case swift::TypeKind::Function:
  case swift::TypeKind::GenericFunction:
  case swift::TypeKind::LValue:
  case swift::TypeKind::UnboundGeneric:
  case swift::TypeKind::TypeVariable:
  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::InOut:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }

  return count;
}

CompilerType
SwiftASTContext::GetDirectBaseClassAtIndex(void *opaque_type, size_t idx,
                                           uint32_t *bit_offset_ptr) {
  VALID_OR_RETURN(CompilerType());

  if (opaque_type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(opaque_type));
    swift::ClassDecl *class_decl =
        swift_can_type->getClassOrBoundGenericClass();
    if (class_decl) {
      swift::Type base_class_type = class_decl->getSuperclass();
      if (base_class_type)
        return CompilerType(GetASTContext(), base_class_type.getPointer());
    }
  }
  return CompilerType();
}

CompilerType
SwiftASTContext::GetVirtualBaseClassAtIndex(void *opaque_type, size_t idx,
                                            uint32_t *bit_offset_ptr) {
  return CompilerType();
}

/// Retrieve the printable name of a tuple element.
static std::string GetTupleElementName(const swift::TupleType *tuple_type,
                                       unsigned index,
                                       llvm::StringRef printed_index = "") {
  const auto &element = tuple_type->getElement(index);

  // Use the element name if there is one.
  if (!element.getName().empty()) return element.getName().str();

  // If we know the printed index already, use that.
  if (!printed_index.empty()) return printed_index;

  // Print the index and return that.
  std::string str;
  llvm::raw_string_ostream(str) << index;
  return str;
}

/// Retrieve the printable name of a type referenced as a superclass.
static std::string GetSuperclassName(const CompilerType &superclass_type) {
  return superclass_type.GetUnboundType().GetTypeName()
    .AsCString("<no type name>");
}

/// Retrieve the type and name of a child of an existential type.
static std::pair<CompilerType, std::string>
GetExistentialTypeChild(swift::ASTContext *swift_ast_ctx,
                        CompilerType type,
                        const SwiftASTContext::ProtocolInfo &protocol_info,
                        unsigned idx) {
  assert(idx < protocol_info.m_num_storage_words &&
         "caller is responsible for validating index");

  // A payload word for a non-class, non-error existential.
  if (idx < protocol_info.m_num_payload_words) {
    std::string name;
    llvm::raw_string_ostream(name) << "payload_data_" << idx;

    auto raw_pointer = swift_ast_ctx->TheRawPointerType;
    return { CompilerType(swift_ast_ctx, raw_pointer.getPointer()),
             std::move(name) };
  }

   // The instance for a class-bound existential.
  if (idx == 0 && protocol_info.m_is_class_only) {
    CompilerType class_type;
    if (protocol_info.m_superclass) {
      class_type = protocol_info.m_superclass;
    } else {
      auto raw_pointer = swift_ast_ctx->TheRawPointerType;
      class_type = CompilerType(swift_ast_ctx, raw_pointer.getPointer());
    }

    return { class_type, "instance" };
  }

  // The instance for an error existential.
  if (idx == 0 && protocol_info.m_is_errortype) {
    auto raw_pointer = swift_ast_ctx->TheRawPointerType;
    return { CompilerType(swift_ast_ctx, raw_pointer.getPointer()),
             "error_instance" };
  }

  // The metatype for a non-class, non-error existential.
  if (idx && idx == protocol_info.m_num_payload_words) {
    // The metatype for a non-class, non-error existential.
    auto any_metatype =
      swift::ExistentialMetatypeType::get(swift_ast_ctx->TheAnyType);
    return { CompilerType(swift_ast_ctx, any_metatype), "instance_type" };
  }

  // A witness table. Figure out which protocol it corresponds to.
  unsigned witness_table_idx = idx - protocol_info.m_num_payload_words - 1;
  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  swift::ExistentialLayout layout = swift_can_type.getExistentialLayout();

  std::string name;
  for (auto protoType : layout.getProtocols()) {
    auto proto = protoType->getDecl();
    if (proto->isObjC()) continue;

    if (witness_table_idx == 0) {
      llvm::raw_string_ostream(name) << "witness_table_"
                                     << proto->getBaseName().userFacingName();
      break;
    }
    --witness_table_idx;
  }

  auto raw_pointer = swift_ast_ctx->TheRawPointerType;
  return { CompilerType(swift_ast_ctx, raw_pointer.getPointer()),
           std::move(name) };
}

CompilerType SwiftASTContext::GetFieldAtIndex(void *type, size_t idx,
                                              std::string &name,
                                              uint64_t *bit_offset_ptr,
                                              uint32_t *bitfield_bit_size_ptr,
                                              bool *is_bitfield_ptr) {
  VALID_OR_RETURN(CompilerType());

  if (!type)
    return CompilerType();

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
  case swift::TypeKind::BuiltinInteger:
  case swift::TypeKind::BuiltinFloat:
  case swift::TypeKind::BuiltinRawPointer:
  case swift::TypeKind::BuiltinNativeObject:
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
  case swift::TypeKind::BuiltinUnknownObject:
  case swift::TypeKind::BuiltinBridgeObject:
  case swift::TypeKind::BuiltinVector:
    break;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .GetFieldAtIndex(idx, name, bit_offset_ptr, bitfield_bit_size_ptr,
                         is_bitfield_ptr);
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::DependentMember:
    break;

  case swift::TypeKind::Enum:
  case swift::TypeKind::BoundGenericEnum: {
    SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo(type);
    if (cached_enum_info &&
        idx < cached_enum_info->GetNumElementsWithPayload()) {
      const SwiftEnumDescriptor::ElementInfo *enum_element_info =
          cached_enum_info->GetElementWithPayloadAtIndex(idx);
      name.assign(enum_element_info->name.GetCString());
      if (bit_offset_ptr)
        *bit_offset_ptr = 0;
      if (bitfield_bit_size_ptr)
        *bitfield_bit_size_ptr = 0;
      if (is_bitfield_ptr)
        *is_bitfield_ptr = false;
      return enum_element_info->payload_type;
    }
  } break;

  case swift::TypeKind::Tuple: {
    auto tuple_type = cast<swift::TupleType>(swift_can_type);
    if (idx >= tuple_type->getNumElements()) break;

    // We cannot reliably get layout information without an execution
    // context.
    if (bit_offset_ptr)
      *bit_offset_ptr = LLDB_INVALID_IVAR_OFFSET;
    if (bitfield_bit_size_ptr)
      *bitfield_bit_size_ptr = 0;
    if (is_bitfield_ptr)
      *is_bitfield_ptr = false;

    name = GetTupleElementName(tuple_type, idx);

    const auto &child = tuple_type->getElement(idx);
    return CompilerType(GetASTContext(), child.getType().getPointer());
  }

  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass: {
    auto class_decl = swift_can_type->getClassOrBoundGenericClass();
    if (class_decl->hasSuperclass()) {
      if (idx == 0) {
        swift::Type superclass_swift_type = swift_can_type->getSuperclass();
        CompilerType superclass_type(GetASTContext(),
                                     superclass_swift_type.getPointer());

        name = GetSuperclassName(superclass_type);

        // We cannot reliably get layout information without an execution
        // context.
        if (bit_offset_ptr)
          *bit_offset_ptr = LLDB_INVALID_IVAR_OFFSET;
        if (bitfield_bit_size_ptr)
          *bitfield_bit_size_ptr = 0;
        if (is_bitfield_ptr)
          *is_bitfield_ptr = false;

        return superclass_type;
      }

      // Adjust the index to refer into the stored properties.
      --idx;
    }

    LLVM_FALLTHROUGH;
  }

  case swift::TypeKind::Struct:
  case swift::TypeKind::BoundGenericStruct: {
    auto nominal = swift_can_type->getAnyNominal();
    auto stored_properties = GetStoredProperties(nominal);
    if (idx >= stored_properties.size()) break;

    auto property = stored_properties[idx];
    name = property->getBaseName().userFacingName();

    // We cannot reliably get layout information without an execution
    // context.
    if (bit_offset_ptr)
      *bit_offset_ptr = LLDB_INVALID_IVAR_OFFSET;
    if (bitfield_bit_size_ptr)
      *bitfield_bit_size_ptr = 0;
    if (is_bitfield_ptr)
      *is_bitfield_ptr = false;

    swift::Type child_swift_type = swift_can_type->getTypeOfMember(
        nominal->getModuleContext(), property, nullptr);
    return CompilerType(GetASTContext(), child_swift_type.getPointer());
  }

  case swift::TypeKind::Protocol:
  case swift::TypeKind::ProtocolComposition: {
    ProtocolInfo protocol_info;
    if (!GetProtocolTypeInfo(
            CompilerType(GetASTContext(), GetSwiftType(type)), protocol_info))
      break;

    if (idx >= protocol_info.m_num_storage_words) break;

    CompilerType compiler_type(GetASTContext(), GetSwiftType(type));
    CompilerType child_type;
    std::tie(child_type, name) =
      GetExistentialTypeChild(GetASTContext(), compiler_type, protocol_info,
                              idx);

    uint64_t child_size = child_type.GetByteSize(nullptr);
    if (bit_offset_ptr)
      *bit_offset_ptr = idx * child_size * 8;
    if (bitfield_bit_size_ptr)
      *bitfield_bit_size_ptr = 0;
    if (is_bitfield_ptr)
      *is_bitfield_ptr = false;

    return child_type;
  }

  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype:
    break;

  case swift::TypeKind::Module:
  case swift::TypeKind::Archetype:
  case swift::TypeKind::Function:
  case swift::TypeKind::GenericFunction:
  case swift::TypeKind::LValue:
  case swift::TypeKind::UnboundGeneric:
  case swift::TypeKind::TypeVariable:
  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::InOut:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }

  return CompilerType();
}

// If a pointer to a pointee type (the clang_type arg) says that it has no
// children, then we either need to trust it, or override it and return a
// different result. For example, an "int *" has one child that is an integer,
// but a function pointer doesn't have any children. Likewise if a Record type
// claims it has no children, then there really is nothing to show.
uint32_t SwiftASTContext::GetNumPointeeChildren(void *type) {
  if (!type)
    return 0;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
    return 0;
  case swift::TypeKind::BuiltinInteger:
    return 1;
  case swift::TypeKind::BuiltinFloat:
    return 1;
  case swift::TypeKind::BuiltinRawPointer:
    return 1;
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
    return 1;
  case swift::TypeKind::BuiltinNativeObject:
    return 1;
  case swift::TypeKind::BuiltinUnknownObject:
    return 1;
  case swift::TypeKind::BuiltinBridgeObject:
    return 1;
  case swift::TypeKind::BuiltinVector:
    return 0;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return GetNumPointeeChildren(
        swift::cast<swift::ReferenceStorageType>(swift_can_type)
            .getPointer());
  case swift::TypeKind::Tuple:
    return 0;
  case swift::TypeKind::GenericTypeParam:
    return 0;
  case swift::TypeKind::DependentMember:
    return 0;
  case swift::TypeKind::Enum:
    return 0;
  case swift::TypeKind::Struct:
    return 0;
  case swift::TypeKind::Class:
    return 0;
  case swift::TypeKind::Protocol:
    return 0;
  case swift::TypeKind::Metatype:
    return 0;
  case swift::TypeKind::Module:
    return 0;
  case swift::TypeKind::Archetype:
    return 0;
  case swift::TypeKind::Function:
    return 0;
  case swift::TypeKind::GenericFunction:
    return 0;
  case swift::TypeKind::ProtocolComposition:
    return 0;
  case swift::TypeKind::LValue:
    return 1;
  case swift::TypeKind::UnboundGeneric:
    return 0;
  case swift::TypeKind::BoundGenericClass:
    return 0;
  case swift::TypeKind::BoundGenericEnum:
    return 0;
  case swift::TypeKind::BoundGenericStruct:
    return 0;
  case swift::TypeKind::TypeVariable:
    return 0;
  case swift::TypeKind::ExistentialMetatype:
    return 0;
  case swift::TypeKind::DynamicSelf:
    return 0;
  case swift::TypeKind::SILBox:
    return 0;
  case swift::TypeKind::SILFunction:
    return 0;
  case swift::TypeKind::SILBlockStorage:
    return 0;
  case swift::TypeKind::InOut:
    return 0;
  case swift::TypeKind::Unresolved:
    return 0;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }

  return 0;
}

static int64_t GetInstanceVariableOffset_Metadata(
    ValueObject *valobj, ExecutionContext *exe_ctx, const CompilerType &type,
    ConstString ivar_name, const CompilerType &ivar_type) {
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  if (log)
    log->Printf(
        "[GetInstanceVariableOffset_Metadata] ivar_name = %s, type = %s",
        ivar_name.AsCString(), type.GetTypeName().AsCString());

  Process *process = exe_ctx->GetProcessPtr();
  if (process) {
    SwiftLanguageRuntime *runtime = process->GetSwiftLanguageRuntime();
    if (runtime) {
      Status error;
      if (auto offset =
            runtime->GetMemberVariableOffset(type, valobj, ivar_name, &error)) {
        if (log)
          log->Printf("[GetInstanceVariableOffset_Metadata] for %s: %llu",
                      ivar_name.AsCString(), *offset);
        return *offset;
      }
      else if (log) {
        log->Printf("[GetInstanceVariableOffset_Metadata] resolver failure: %s",
                    error.AsCString());
      }
    } else if (log)
      log->Printf("[GetInstanceVariableOffset_Metadata] no runtime");
  } else if (log)
    log->Printf("[GetInstanceVariableOffset_Metadata] no process");
  return LLDB_INVALID_IVAR_OFFSET;
}

static int64_t GetInstanceVariableOffset(ValueObject *valobj,
                                         ExecutionContext *exe_ctx,
                                         const CompilerType &class_type,
                                         const char *ivar_name,
                                         const CompilerType &ivar_type) {
  int64_t offset = LLDB_INVALID_IVAR_OFFSET;

  if (ivar_name && ivar_name[0]) {
    if (exe_ctx) {
      Target *target = exe_ctx->GetTargetPtr();
      if (target) {
        offset = GetInstanceVariableOffset_Metadata(
            valobj, exe_ctx, class_type, ConstString(ivar_name), ivar_type);
      }
    }
  }
  return offset;
}

bool SwiftASTContext::IsNonTriviallyManagedReferenceType(
    const CompilerType &type, NonTriviallyManagedReferenceStrategy &strategy,
    CompilerType *underlying_type) {
  if (auto ast =
          llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem())) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));

    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind) {
    default:
      break;
    case swift::TypeKind::UnmanagedStorage: {
      strategy = NonTriviallyManagedReferenceStrategy::eUnmanaged;
      if (underlying_type)
        *underlying_type = CompilerType(
            ast, swift_can_type->getReferenceStorageReferent()
              .getPointer());
    }
      return true;
    case swift::TypeKind::UnownedStorage: {
      strategy = NonTriviallyManagedReferenceStrategy::eUnowned;
      if (underlying_type)
        *underlying_type = CompilerType(
            ast, swift_can_type->getReferenceStorageReferent()
              .getPointer());
    }
      return true;
    case swift::TypeKind::WeakStorage: {
      strategy = NonTriviallyManagedReferenceStrategy::eWeak;
      if (underlying_type)
        *underlying_type = CompilerType(
            ast, swift_can_type->getReferenceStorageReferent()
              .getPointer());
    }
      return true;
    }
  }
  return false;
}

CompilerType SwiftASTContext::GetChildCompilerTypeAtIndex(
    void *type, ExecutionContext *exe_ctx, size_t idx,
    bool transparent_pointers, bool omit_empty_base_classes,
    bool ignore_array_bounds, std::string &child_name,
    uint32_t &child_byte_size, int32_t &child_byte_offset,
    uint32_t &child_bitfield_bit_size, uint32_t &child_bitfield_bit_offset,
    bool &child_is_base_class, bool &child_is_deref_of_parent,
    ValueObject *valobj, uint64_t &language_flags) {
  VALID_OR_RETURN(CompilerType());

  if (!type)
    return CompilerType();

  language_flags = 0;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
  case swift::TypeKind::BuiltinInteger:
  case swift::TypeKind::BuiltinFloat:
  case swift::TypeKind::BuiltinRawPointer:
  case swift::TypeKind::BuiltinNativeObject:
  case swift::TypeKind::BuiltinUnknownObject:
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
  case swift::TypeKind::BuiltinBridgeObject:
  case swift::TypeKind::BuiltinVector:
    break;
  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .GetChildCompilerTypeAtIndex(
            exe_ctx, idx, transparent_pointers, omit_empty_base_classes,
            ignore_array_bounds, child_name, child_byte_size, child_byte_offset,
            child_bitfield_bit_size, child_bitfield_bit_offset,
            child_is_base_class, child_is_deref_of_parent, valobj,
            language_flags);
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::DependentMember:
    break;

  case swift::TypeKind::Enum:
  case swift::TypeKind::BoundGenericEnum: {
    SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo(type);
    if (cached_enum_info &&
        idx < cached_enum_info->GetNumElementsWithPayload()) {
      const SwiftEnumDescriptor::ElementInfo *element_info =
          cached_enum_info->GetElementWithPayloadAtIndex(idx);
      child_name.assign(element_info->name.GetCString());
      child_byte_size = element_info->payload_type.GetByteSize(
          exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
      child_byte_offset = 0;
      child_bitfield_bit_size = 0;
      child_bitfield_bit_offset = 0;
      child_is_base_class = false;
      child_is_deref_of_parent = false;
      if (element_info->is_indirect) {
        language_flags |= LanguageFlags::eIsIndirectEnumCase;
        return CompilerType(GetASTContext(),
                            GetASTContext()->TheRawPointerType.getPointer());
      } else
        return element_info->payload_type;
    }
  } break;

  case swift::TypeKind::Tuple: {
    auto tuple_type = cast<swift::TupleType>(swift_can_type);
    if (idx >= tuple_type->getNumElements()) break;

    const auto &child = tuple_type->getElement(idx);

    // Format the integer.
    llvm::SmallString<16> printed_idx;
    llvm::raw_svector_ostream(printed_idx) << idx;

    CompilerType child_type(GetASTContext(), child.getType().getPointer());

    auto exe_ctx_scope =
      exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL;
    child_name = GetTupleElementName(tuple_type, idx, printed_idx);
    child_byte_size = child_type.GetByteSize(exe_ctx_scope);
      child_is_base_class = false;
    child_is_deref_of_parent = false;

    CompilerType compiler_type(GetASTContext(), GetSwiftType(type));
    child_byte_offset =
      GetInstanceVariableOffset(valobj, exe_ctx, compiler_type,
                                printed_idx.c_str(), child_type);
    child_bitfield_bit_size = 0;
    child_bitfield_bit_offset = 0;

    return child_type;
  }

  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass: {
    auto class_decl = swift_can_type->getClassOrBoundGenericClass();
    // Child 0 is the superclass, if there is one.
    if (class_decl->hasSuperclass()) {
      if (idx == 0) {
        swift::Type superclass_swift_type = swift_can_type->getSuperclass();
        CompilerType superclass_type(GetASTContext(),
                                     superclass_swift_type.getPointer());

        child_name = GetSuperclassName(superclass_type);

        auto exe_ctx_scope =
          exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL;
        child_byte_size = superclass_type.GetByteSize(exe_ctx_scope);
        child_is_base_class = true;
        child_is_deref_of_parent = false;

        child_byte_offset = 0;
        child_bitfield_bit_size = 0;
        child_bitfield_bit_offset = 0;

        language_flags |= LanguageFlags::eIgnoreInstancePointerness;
        return superclass_type;
      }

      // Adjust the index to refer into the stored properties.
      --idx;
    }
    LLVM_FALLTHROUGH;
  }

  case swift::TypeKind::Struct:
  case swift::TypeKind::BoundGenericStruct: {
    auto nominal = swift_can_type->getAnyNominal();
    auto stored_properties = GetStoredProperties(nominal);
    if (idx >= stored_properties.size()) break;

    // Find the stored property with this index.
    auto property = stored_properties[idx];
    swift::Type child_swift_type = swift_can_type->getTypeOfMember(
        nominal->getModuleContext(), property, nullptr);

    CompilerType child_type(GetASTContext(), child_swift_type.getPointer());

    auto exe_ctx_scope =
      exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL;

    child_name = property->getBaseName().userFacingName();
    child_byte_size = child_type.GetByteSize(exe_ctx_scope);
    child_is_base_class = false;
    child_is_deref_of_parent = false;

    CompilerType compiler_type(GetASTContext(), GetSwiftType(type));
    child_byte_offset =
      GetInstanceVariableOffset(valobj, exe_ctx, compiler_type,
                                child_name.c_str(), child_type);
    child_bitfield_bit_size = 0;
    child_bitfield_bit_offset = 0;
    return child_type;
  }

  case swift::TypeKind::Protocol:
  case swift::TypeKind::ProtocolComposition: {
    ProtocolInfo protocol_info;
    if (!GetProtocolTypeInfo(
            CompilerType(GetASTContext(), GetSwiftType(type)), protocol_info))
      break;

    if (idx >= protocol_info.m_num_storage_words) break;

    CompilerType compiler_type(GetASTContext(), GetSwiftType(type));
    CompilerType child_type;
    std::tie(child_type, child_name) =
      GetExistentialTypeChild(GetASTContext(), compiler_type, protocol_info,
                              idx);

    auto exe_ctx_scope =
      exe_ctx ? exe_ctx->GetBestExecutionContextScope() : nullptr;

    child_byte_size = child_type.GetByteSize(exe_ctx_scope);
    child_byte_offset = idx * child_byte_size;
    child_bitfield_bit_size = 0;
    child_bitfield_bit_offset = 0;
    child_is_base_class = false;
    child_is_deref_of_parent = false;

    return child_type;
  }

  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype:
    break;

  case swift::TypeKind::Module:
  case swift::TypeKind::Archetype:
  case swift::TypeKind::Function:
  case swift::TypeKind::GenericFunction:
    break;

  case swift::TypeKind::LValue:
    if (idx < GetNumChildren(type, omit_empty_base_classes)) {
      CompilerType pointee_clang_type(GetNonReferenceType(type));
      Flags pointee_clang_type_flags(pointee_clang_type.GetTypeInfo());
      const char *parent_name = valobj ? valobj->GetName().GetCString() : NULL;
      if (parent_name) {
        child_name.assign(1, '&');
        child_name += parent_name;
      }

      // We have a pointer to a simple type
      if (idx == 0) {
        child_byte_size = pointee_clang_type.GetByteSize(
            exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
        child_byte_offset = 0;
        return pointee_clang_type;
      }
    }
    break;
  case swift::TypeKind::UnboundGeneric:
    break;
  case swift::TypeKind::TypeVariable:
    break;

  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::InOut:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }
  return CompilerType();
}

// Look for a child member (doesn't include base classes, but it does include
// their members) in the type hierarchy. Returns an index path into "clang_type"
// on how to reach the appropriate member.
//
//    class A
//    {
//    public:
//        int m_a;
//        int m_b;
//    };
//
//    class B
//    {
//    };
//
//    class C :
//        public B,
//        public A
//    {
//    };
//
// If we have a clang type that describes "class C", and we wanted to look for
// "m_b" in it:
//
// With omit_empty_base_classes == false we would get an integer array back
// with:
// { 1,  1 }
// The first index 1 is the child index for "class A" within class C.
// The second index 1 is the child index for "m_b" within class A.
//
// With omit_empty_base_classes == true we would get an integer array back with:
// { 0,  1 }
// The first index 0 is the child index for "class A" within class C (since
// class B doesn't have any members it doesn't count).
// The second index 1 is the child index for "m_b" within class A.

size_t SwiftASTContext::GetIndexOfChildMemberWithName(
    void *type, const char *name, bool omit_empty_base_classes,
    std::vector<uint32_t> &child_indexes) {
  VALID_OR_RETURN(0);

  if (type && name && name[0]) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));

    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind) {
    case swift::TypeKind::Error:
    case swift::TypeKind::BuiltinInteger:
    case swift::TypeKind::BuiltinFloat:
    case swift::TypeKind::BuiltinRawPointer:
    case swift::TypeKind::BuiltinNativeObject:
    case swift::TypeKind::BuiltinUnknownObject:
    case swift::TypeKind::BuiltinUnsafeValueBuffer:
    case swift::TypeKind::BuiltinBridgeObject:
    case swift::TypeKind::BuiltinVector:
      break;

    case swift::TypeKind::UnmanagedStorage:
    case swift::TypeKind::UnownedStorage:
    case swift::TypeKind::WeakStorage:
      return CompilerType(GetASTContext(),
                          swift_can_type->getReferenceStorageReferent())
          .GetIndexOfChildMemberWithName(name, omit_empty_base_classes,
                                         child_indexes);
    case swift::TypeKind::GenericTypeParam:
    case swift::TypeKind::DependentMember:
      break;

    case swift::TypeKind::Enum:
    case swift::TypeKind::BoundGenericEnum: {
      SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo(type);
      if (cached_enum_info) {
        ConstString const_name(name);
        const size_t num_sized_elements =
            cached_enum_info->GetNumElementsWithPayload();
        for (size_t i = 0; i < num_sized_elements; ++i) {
          if (cached_enum_info->GetElementWithPayloadAtIndex(i)->name ==
              const_name) {
            child_indexes.push_back(i);
            return child_indexes.size();
          }
        }
      }
    } break;

    case swift::TypeKind::Tuple: {
      // For tuples only always look for the member by number first as a tuple
      // element can be named, yet still be accessed by the number...
      swift::TupleType *tuple_type = swift_can_type->castTo<swift::TupleType>();
      uint32_t tuple_idx = StringConvert::ToUInt32(name, UINT32_MAX);
      if (tuple_idx != UINT32_MAX) {
        if (tuple_idx < tuple_type->getNumElements()) {
          child_indexes.push_back(tuple_idx);
          return child_indexes.size();
        } else
          return 0;
      }

      // Otherwise, perform lookup by name.
      for (uint32_t tuple_idx : swift::range(tuple_type->getNumElements())) {
        if (tuple_type->getElement(tuple_idx).getName().str() == name) {
          child_indexes.push_back(tuple_idx);
          return child_indexes.size();
        }
      }

      return 0;
    }

    case swift::TypeKind::Struct:
    case swift::TypeKind::Class:
    case swift::TypeKind::BoundGenericClass:
    case swift::TypeKind::BoundGenericStruct: {
      auto nominal = swift_can_type->getAnyNominal();
      auto stored_properties = GetStoredProperties(nominal);
      auto class_decl = llvm::dyn_cast<swift::ClassDecl>(nominal);

      // Search the stored properties.
      for (unsigned idx : indices(stored_properties)) {
        auto property = stored_properties[idx];
        if (property->getBaseName().userFacingName() == name) {
          // We found it!

          // If we have a superclass, adjust the index accordingly.
          if (class_decl && class_decl->hasSuperclass())
            ++idx;

          child_indexes.push_back(idx);
          return child_indexes.size();
        }
      }

      // Search the superclass, if there is one.
      if (class_decl && class_decl->hasSuperclass()) {
        // Push index zero for the base class
        child_indexes.push_back(0);

        // Look in the superclass.
        swift::Type superclass_swift_type = swift_can_type->getSuperclass();
        CompilerType superclass_type(GetASTContext(),
                                     superclass_swift_type.getPointer());
        if (superclass_type.GetIndexOfChildMemberWithName(
                name, omit_empty_base_classes, child_indexes))
          return child_indexes.size();

        // We didn't find a stored property matching "name" in our
        // superclass, pop the superclass zero index that
        // we pushed on above.
        child_indexes.pop_back();
      }
    } break;

    case swift::TypeKind::Protocol:
    case swift::TypeKind::ProtocolComposition: {
      ProtocolInfo protocol_info;
      if (!GetProtocolTypeInfo(CompilerType(GetASTContext(),
                                            GetSwiftType(type)), protocol_info))
        break;

      CompilerType compiler_type(GetASTContext(), GetSwiftType(type));
      for (unsigned idx : swift::range(protocol_info.m_num_storage_words)) {
        CompilerType child_type;
        std::string child_name;
        std::tie(child_type, child_name) =
          GetExistentialTypeChild(GetASTContext(), compiler_type,
                                  protocol_info, idx);
        if (name == child_name) {
          child_indexes.push_back(idx);
          return child_indexes.size();
        }
      }
    } break;

    case swift::TypeKind::ExistentialMetatype:
    case swift::TypeKind::Metatype:
      break;

    case swift::TypeKind::Module:
    case swift::TypeKind::Archetype:
    case swift::TypeKind::Function:
    case swift::TypeKind::GenericFunction:
      break;
    case swift::TypeKind::InOut:
    case swift::TypeKind::LValue: {
      CompilerType pointee_clang_type(GetNonReferenceType(type));

      if (pointee_clang_type.IsAggregateType()) {
        return pointee_clang_type.GetIndexOfChildMemberWithName(
            name, omit_empty_base_classes, child_indexes);
      }
    } break;
    case swift::TypeKind::UnboundGeneric:
      break;
    case swift::TypeKind::TypeVariable:
      break;

    case swift::TypeKind::DynamicSelf:
    case swift::TypeKind::SILBox:
    case swift::TypeKind::SILFunction:
    case swift::TypeKind::SILBlockStorage:
    case swift::TypeKind::Unresolved:
      break;

    case swift::TypeKind::Optional:
    case swift::TypeKind::NameAlias:
    case swift::TypeKind::Paren:
    case swift::TypeKind::Dictionary:
    case swift::TypeKind::ArraySlice:
      assert(false && "Not a canonical type");
      break;

    }
  }
  return 0;
}

// Get the index of the child of "clang_type" whose name matches. This function
// doesn't descend into the children, but only looks one level deep and name
// matches can include base class names.

uint32_t
SwiftASTContext::GetIndexOfChildWithName(void *type, const char *name,
                                         bool omit_empty_base_classes) {
  VALID_OR_RETURN(UINT32_MAX);

  std::vector<uint32_t> child_indexes;
  size_t num_child_indexes =
    GetIndexOfChildMemberWithName(type, name, omit_empty_base_classes,
                                  child_indexes);
  return num_child_indexes == 1 ? child_indexes.front() : UINT32_MAX;
}

size_t SwiftASTContext::GetNumTemplateArguments(void *type) {
  if (!type)
    return 0;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::UnboundGeneric: {
    swift::UnboundGenericType *unbound_generic_type =
        swift_can_type->castTo<swift::UnboundGenericType>();
    auto *nominal_type_decl = unbound_generic_type->getDecl();
    swift::GenericParamList *generic_param_list =
        nominal_type_decl->getGenericParams();
    return generic_param_list->getParams().size();
  } break;
  case swift::TypeKind::BoundGenericClass:
  case swift::TypeKind::BoundGenericStruct:
  case swift::TypeKind::BoundGenericEnum: {
    swift::BoundGenericType *bound_generic_type =
        swift_can_type->castTo<swift::BoundGenericType>();
    return bound_generic_type->getGenericArgs().size();
  }
  default:
    break;
  }

  return 0;
}

bool SwiftASTContext::GetSelectedEnumCase(const CompilerType &type,
                                          const DataExtractor &data,
                                          ConstString *name, bool *has_payload,
                                          CompilerType *payload,
                                          bool *is_indirect) {
  if (auto ast =
          llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem())) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));

    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind) {
    default:
      break;
    case swift::TypeKind::Enum:
    case swift::TypeKind::BoundGenericEnum: {
      SwiftEnumDescriptor *cached_enum_info =
          ast->GetCachedEnumInfo(swift_can_type.getPointer());
      if (cached_enum_info) {
        auto enum_elem_info = cached_enum_info->GetElementFromData(data);
        if (enum_elem_info) {
          if (name)
            *name = enum_elem_info->name;
          if (has_payload)
            *has_payload = enum_elem_info->has_payload;
          if (payload)
            *payload = enum_elem_info->payload_type;
          if (is_indirect)
            *is_indirect = enum_elem_info->is_indirect;
          return true;
        }
      }
    } break;
    }
  }

  return false;
}

lldb::GenericKind SwiftASTContext::GetGenericArgumentKind(void *type,
                                                          size_t idx) {
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    if (auto *unbound_generic_type =
            swift_can_type->getAs<swift::UnboundGenericType>())
      return eUnboundGenericKindType;
    if (auto *bound_generic_type =
            swift_can_type->getAs<swift::BoundGenericType>())
      if (idx < bound_generic_type->getGenericArgs().size())
        return eBoundGenericKindType;
  }
  return eNullGenericKindType;
}

CompilerType SwiftASTContext::GetBoundGenericType(void *type, size_t idx) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    if (auto *bound_generic_type =
            swift_can_type->getAs<swift::BoundGenericType>())
      if (idx < bound_generic_type->getGenericArgs().size())
        return CompilerType(
            GetASTContext(),
            bound_generic_type->getGenericArgs()[idx].getPointer());
  }
  return CompilerType();
}

CompilerType SwiftASTContext::GetUnboundGenericType(void *type, size_t idx) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    if (auto *unbound_generic_type =
          swift_can_type->getAs<swift::UnboundGenericType>()) {
      auto *nominal_type_decl = unbound_generic_type->getDecl();
      swift::GenericSignature *generic_sig =
          nominal_type_decl->getGenericSignature();
      auto depTy = generic_sig->getGenericParams()[idx];
      return CompilerType(GetASTContext(),
                          nominal_type_decl->mapTypeIntoContext(depTy)
                              ->castTo<swift::ArchetypeType>());
    }
  }
  return CompilerType();
}

CompilerType SwiftASTContext::GetGenericArgumentType(void *type, size_t idx) {
  VALID_OR_RETURN(CompilerType());

  switch (GetGenericArgumentKind(type, idx)) {
  case eBoundGenericKindType:
    return GetBoundGenericType(type, idx);
  case eUnboundGenericKindType:
    return GetUnboundGenericType(type, idx);
  default:
    break;
  }
  return CompilerType();
}

CompilerType SwiftASTContext::GetTypeForFormatters(void *type) {
  VALID_OR_RETURN(CompilerType());

  if (type) {
    swift::Type swift_type(GetSwiftType(type));
    return CompilerType(GetASTContext(), swift_type);
  }
  return CompilerType();
}

LazyBool SwiftASTContext::ShouldPrintAsOneLiner(void *type,
                                                ValueObject *valobj) {
  if (type) {
    CompilerType can_compiler_type(GetCanonicalType(type));
    if (IsImportedType(can_compiler_type, nullptr))
      return eLazyBoolNo;
  }
  if (valobj) {
    if (valobj->IsBaseClass())
      return eLazyBoolNo;
    if ((valobj->GetLanguageFlags() & LanguageFlags::eIsIndirectEnumCase) ==
        LanguageFlags::eIsIndirectEnumCase)
      return eLazyBoolNo;
  }

  return eLazyBoolCalculate;
}

bool SwiftASTContext::IsMeaninglessWithoutDynamicResolution(void *type) {
  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));

    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind) {
    case swift::TypeKind::Archetype:
      return true;
    default:
      return false;
    }
  }

  return false;
}

//----------------------------------------------------------------------
// Dumping types
//----------------------------------------------------------------------
#define DEPTH_INCREMENT 2

void SwiftASTContext::DumpValue(
    void *type, ExecutionContext *exe_ctx, Stream *s, lldb::Format format,
    const lldb_private::DataExtractor &data, lldb::offset_t data_byte_offset,
    size_t data_byte_size, uint32_t bitfield_bit_size,
    uint32_t bitfield_bit_offset, bool show_types, bool show_summary,
    bool verbose, uint32_t depth) {}

bool SwiftASTContext::DumpTypeValue(
    void *type, Stream *s, lldb::Format format,
    const lldb_private::DataExtractor &data, lldb::offset_t byte_offset,
    size_t byte_size, uint32_t bitfield_bit_size, uint32_t bitfield_bit_offset,
    ExecutionContextScope *exe_scope, bool is_base_class) {
  VALID_OR_RETURN(false);

  if (!type)
    return false;

  swift::CanType swift_can_type(GetCanonicalSwiftType(type));

  const swift::TypeKind type_kind = swift_can_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Error:
    break;

  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass:
    // If we have a class that is in a variable then it is a pointer,
    // else if it is a base class, it has no value.
    if (is_base_class)
      break;
  // Fall through to case below
  case swift::TypeKind::BuiltinInteger:
  case swift::TypeKind::BuiltinFloat:
  case swift::TypeKind::BuiltinRawPointer:
  case swift::TypeKind::BuiltinNativeObject:
  case swift::TypeKind::BuiltinUnsafeValueBuffer:
  case swift::TypeKind::BuiltinUnknownObject:
  case swift::TypeKind::BuiltinBridgeObject:
  case swift::TypeKind::Archetype:
  case swift::TypeKind::Function:
  case swift::TypeKind::GenericFunction:
  case swift::TypeKind::LValue: {
    uint32_t item_count = 1;
    // A few formats, we might need to modify our size and count for depending
    // on how we are trying to display the value...
    switch (format) {
    default:
    case eFormatBoolean:
    case eFormatBinary:
    case eFormatComplex:
    case eFormatCString: // NULL terminated C strings
    case eFormatDecimal:
    case eFormatEnum:
    case eFormatHex:
    case eFormatHexUppercase:
    case eFormatFloat:
    case eFormatOctal:
    case eFormatOSType:
    case eFormatUnsigned:
    case eFormatPointer:
    case eFormatVectorOfChar:
    case eFormatVectorOfSInt8:
    case eFormatVectorOfUInt8:
    case eFormatVectorOfSInt16:
    case eFormatVectorOfUInt16:
    case eFormatVectorOfSInt32:
    case eFormatVectorOfUInt32:
    case eFormatVectorOfSInt64:
    case eFormatVectorOfUInt64:
    case eFormatVectorOfFloat32:
    case eFormatVectorOfFloat64:
    case eFormatVectorOfUInt128:
      break;

    case eFormatAddressInfo:
      if (byte_size == 0) {
        byte_size = exe_scope->CalculateTarget()
                        ->GetArchitecture()
                        .GetAddressByteSize();
        item_count = 1;
      }
      break;

    case eFormatChar:
    case eFormatCharPrintable:
    case eFormatCharArray:
    case eFormatBytes:
    case eFormatBytesWithASCII:
      item_count = byte_size;
      byte_size = 1;
      break;

    case eFormatUnicode16:
      item_count = byte_size / 2;
      byte_size = 2;
      break;

    case eFormatUnicode32:
      item_count = byte_size / 4;
      byte_size = 4;
      break;
    }
    return DumpDataExtractor(data, s, byte_offset, format, byte_size, item_count, UINT32_MAX,
                     LLDB_INVALID_ADDRESS, bitfield_bit_size,
                     bitfield_bit_offset, exe_scope);
  } break;
  case swift::TypeKind::BuiltinVector:
    break;

  case swift::TypeKind::Tuple:
    break;

  case swift::TypeKind::UnmanagedStorage:
  case swift::TypeKind::UnownedStorage:
  case swift::TypeKind::WeakStorage:
    return CompilerType(GetASTContext(),
                        swift_can_type->getReferenceStorageReferent())
        .DumpTypeValue(s, format, data, byte_offset, byte_size,
                       bitfield_bit_size, bitfield_bit_offset, exe_scope,
                       is_base_class);
  case swift::TypeKind::Enum:
  case swift::TypeKind::BoundGenericEnum: {
    SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo(type);
    if (cached_enum_info) {
      auto enum_elem_info = cached_enum_info->GetElementFromData(data);
      if (enum_elem_info)
        s->Printf("%s", enum_elem_info->name.GetCString());
      else {
        lldb::offset_t ptr = 0;
        if (data.GetByteSize())
          s->Printf("<invalid> (0x%" PRIx8 ")", data.GetU8(&ptr));
        else
          s->Printf("<empty>");
      }
      return true;
    } else
      s->Printf("<unknown type>");
  } break;

  case swift::TypeKind::Struct:
  case swift::TypeKind::Protocol:
  case swift::TypeKind::GenericTypeParam:
  case swift::TypeKind::DependentMember:
    return false;

  case swift::TypeKind::ExistentialMetatype:
  case swift::TypeKind::Metatype: {
    return DumpDataExtractor(data, s, byte_offset, eFormatPointer, byte_size, 1, UINT32_MAX,
                     LLDB_INVALID_ADDRESS, bitfield_bit_size,
                     bitfield_bit_offset, exe_scope);
  } break;

  case swift::TypeKind::Module:
  case swift::TypeKind::ProtocolComposition:
  case swift::TypeKind::UnboundGeneric:
  case swift::TypeKind::BoundGenericStruct:
  case swift::TypeKind::TypeVariable:
  case swift::TypeKind::DynamicSelf:
  case swift::TypeKind::SILBox:
  case swift::TypeKind::SILFunction:
  case swift::TypeKind::SILBlockStorage:
  case swift::TypeKind::InOut:
  case swift::TypeKind::Unresolved:
    break;

  case swift::TypeKind::Optional:
  case swift::TypeKind::NameAlias:
  case swift::TypeKind::Paren:
  case swift::TypeKind::Dictionary:
  case swift::TypeKind::ArraySlice:
    assert(false && "Not a canonical type");
    break;
  }

  return 0;
}

bool SwiftASTContext::IsImportedType(const CompilerType &type,
                                     CompilerType *original_type) {
  bool success = false;

  if (llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem())) {
    do {
      swift::CanType swift_can_type(GetCanonicalSwiftType(type));
      swift::NominalType *nominal_type =
          swift_can_type->getAs<swift::NominalType>();
      if (!nominal_type)
        break;
      swift::NominalTypeDecl *nominal_type_decl = nominal_type->getDecl();
      if (nominal_type_decl && nominal_type_decl->hasClangNode()) {
        const clang::Decl *clang_decl = nominal_type_decl->getClangDecl();
        if (!clang_decl)
          break;
        success = true;
        if (!original_type)
          break;

        if (const clang::ObjCInterfaceDecl *objc_interface_decl =
                llvm::dyn_cast<clang::ObjCInterfaceDecl>(
                    clang_decl)) // ObjCInterfaceDecl is not a TypeDecl
        {
          *original_type =
              CompilerType(&objc_interface_decl->getASTContext(),
                           clang::QualType::getFromOpaquePtr(
                               objc_interface_decl->getTypeForDecl()));
        } else if (const clang::TypeDecl *type_decl =
                       llvm::dyn_cast<clang::TypeDecl>(clang_decl)) {
          *original_type = CompilerType(
              &type_decl->getASTContext(),
              clang::QualType::getFromOpaquePtr(type_decl->getTypeForDecl()));
        } else // TODO: any more cases that we care about?
        {
          *original_type = CompilerType();
        }
      }
    } while (0);
  }

  return success;
}

bool SwiftASTContext::IsImportedObjectiveCType(const CompilerType &type,
                                               CompilerType *original_type) {
  bool success = false;

  if (llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem())) {
    CompilerType local_original_type;

    if (IsImportedType(type, &local_original_type)) {
      if (local_original_type.IsValid()) {
        ClangASTContext *clang_ast = llvm::dyn_cast_or_null<ClangASTContext>(
            local_original_type.GetTypeSystem());
        if (clang_ast &&
            clang_ast->IsObjCObjectOrInterfaceType(local_original_type)) {
          if (original_type)
            *original_type = local_original_type;
          success = true;
        }
      }
    }
  }

  return success;
}

void SwiftASTContext::DumpSummary(void *type, ExecutionContext *exe_ctx,
                                  Stream *s,
                                  const lldb_private::DataExtractor &data,
                                  lldb::offset_t data_byte_offset,
                                  size_t data_byte_size) {}

size_t SwiftASTContext::ConvertStringToFloatValue(void *type, const char *s,
                                                  uint8_t *dst,
                                                  size_t dst_size) {
  return 0;
}

void SwiftASTContext::DumpTypeDescription(void *type) {
  StreamFile s(stdout, false);
  DumpTypeDescription(type, &s);
}

void SwiftASTContext::DumpTypeDescription(void *type, Stream *s) {
  DumpTypeDescription(type, s, false, true);
}

void SwiftASTContext::DumpTypeDescription(void *type,
                                          bool print_help_if_available,
                                          bool print_extensions_if_available) {
  StreamFile s(stdout, false);
  DumpTypeDescription(type, &s, print_help_if_available,
                      print_extensions_if_available);
}

static void PrintSwiftNominalType(swift::NominalTypeDecl *nominal_type_decl,
                                  Stream *s, bool print_help_if_available,
                                  bool print_extensions_if_available) {
  if (nominal_type_decl && s) {
    std::string buffer;
    llvm::raw_string_ostream ostream(buffer);
    const swift::PrintOptions &print_options(
        SwiftASTContext::GetUserVisibleTypePrintingOptions(
            print_help_if_available));
    nominal_type_decl->print(ostream, print_options);
    ostream.flush();
    if (buffer.empty() == false)
      s->Printf("%s\n", buffer.c_str());
    if (print_extensions_if_available) {
      for (auto ext : nominal_type_decl->getExtensions()) {
        if (ext) {
          buffer.clear();
          llvm::raw_string_ostream ext_ostream(buffer);
          ext->print(ext_ostream, print_options);
          ext_ostream.flush();
          if (buffer.empty() == false)
            s->Printf("%s\n", buffer.c_str());
        }
      }
    }
  }
}

void SwiftASTContext::DumpTypeDescription(void *type, Stream *s,
                                          bool print_help_if_available,
                                          bool print_extensions_if_available) {
  llvm::SmallVector<char, 1024> buf;
  llvm::raw_svector_ostream llvm_ostrm(buf);

  if (type) {
    swift::CanType swift_can_type(GetCanonicalSwiftType(type));
    switch (swift_can_type->getKind()) {
    case swift::TypeKind::Module: {
      swift::ModuleType *module_type =
          swift_can_type->castTo<swift::ModuleType>();
      swift::ModuleDecl *module = module_type->getModule();
      llvm::SmallVector<swift::Decl *, 10> decls;
      module->getDisplayDecls(decls);
      for (swift::Decl *decl : decls) {
        swift::DeclKind kind = decl->getKind();
        if (kind >= swift::DeclKind::First_TypeDecl &&
            kind <= swift::DeclKind::Last_TypeDecl) {
          swift::TypeDecl *type_decl =
              llvm::dyn_cast_or_null<swift::TypeDecl>(decl);
          if (type_decl) {
            CompilerType clang_type(&module->getASTContext(),
                                    type_decl->getDeclaredInterfaceType().getPointer());
            if (clang_type) {
              Flags clang_type_flags(clang_type.GetTypeInfo());
              DumpTypeDescription(clang_type.GetOpaqueQualType(), s,
                                  print_help_if_available,
                                  print_extensions_if_available);
            }
          }
        } else if (kind == swift::DeclKind::Func ||
                   kind == swift::DeclKind::Var) {
          std::string buffer;
          llvm::raw_string_ostream stream(buffer);
          decl->print(stream,
                      SwiftASTContext::GetUserVisibleTypePrintingOptions(
                          print_help_if_available));
          stream.flush();
          s->Printf("%s\n", buffer.c_str());
        } else if (kind == swift::DeclKind::Import) {
          swift::ImportDecl *import_decl =
              llvm::dyn_cast_or_null<swift::ImportDecl>(decl);
          if (import_decl) {
            switch (import_decl->getImportKind()) {
            case swift::ImportKind::Module: {
              swift::ModuleDecl *imported_module = import_decl->getModule();
              if (imported_module) {
                s->Printf("import %s\n", imported_module->getName().get());
              }
            } break;
            default: {
              for (swift::Decl *imported_decl : import_decl->getDecls()) {
                // all of the non-module things you can import should be a
                // ValueDecl
                if (swift::ValueDecl *imported_value_decl =
                        llvm::dyn_cast_or_null<swift::ValueDecl>(
                            imported_decl)) {
                  if (swift::TypeBase *decl_type =
                          imported_value_decl->getInterfaceType().getPointer()) {
                    DumpTypeDescription(decl_type, s,
                                        print_help_if_available,
                                        print_extensions_if_available);
                  }
                }
              }
            } break;
            }
          }
        }
      }
      break;
    }
    case swift::TypeKind::Metatype: {
      s->PutCString("metatype ");
      swift::MetatypeType *metatype_type =
          swift_can_type->castTo<swift::MetatypeType>();
      DumpTypeDescription(metatype_type->getInstanceType().getPointer(),
                          print_help_if_available,
                          print_extensions_if_available);
    } break;
    case swift::TypeKind::UnboundGeneric: {
      swift::UnboundGenericType *unbound_generic_type =
          swift_can_type->castTo<swift::UnboundGenericType>();
      auto nominal_type_decl = llvm::dyn_cast<swift::NominalTypeDecl>(
          unbound_generic_type->getDecl());
      if (nominal_type_decl) {
        PrintSwiftNominalType(nominal_type_decl, s, print_help_if_available,
                              print_extensions_if_available);
      }
    } break;
    case swift::TypeKind::GenericFunction:
    case swift::TypeKind::Function: {
      swift::AnyFunctionType *any_function_type =
          swift_can_type->castTo<swift::AnyFunctionType>();
      std::string buffer;
      llvm::raw_string_ostream ostream(buffer);
      const swift::PrintOptions &print_options(
          SwiftASTContext::GetUserVisibleTypePrintingOptions(
              print_help_if_available));

      any_function_type->print(ostream, print_options);
      ostream.flush();
      if (buffer.empty() == false)
        s->Printf("%s\n", buffer.c_str());
    } break;
    case swift::TypeKind::Tuple: {
      swift::TupleType *tuple_type = swift_can_type->castTo<swift::TupleType>();
      std::string buffer;
      llvm::raw_string_ostream ostream(buffer);
      const swift::PrintOptions &print_options(
          SwiftASTContext::GetUserVisibleTypePrintingOptions(
              print_help_if_available));

      tuple_type->print(ostream, print_options);
      ostream.flush();
      if (buffer.empty() == false)
        s->Printf("%s\n", buffer.c_str());
    } break;
    case swift::TypeKind::BoundGenericClass:
    case swift::TypeKind::BoundGenericEnum:
    case swift::TypeKind::BoundGenericStruct: {
      swift::BoundGenericType *bound_generic_type =
          swift_can_type->castTo<swift::BoundGenericType>();
      swift::NominalTypeDecl *nominal_type_decl =
          bound_generic_type->getDecl();
      PrintSwiftNominalType(nominal_type_decl, s, print_help_if_available,
                            print_extensions_if_available);
    } break;
    case swift::TypeKind::BuiltinInteger: {
      swift::BuiltinIntegerType *builtin_integer_type =
          swift_can_type->castTo<swift::BuiltinIntegerType>();
      s->Printf("builtin integer type of width %u bits\n",
                builtin_integer_type->getWidth().getGreatestWidth());
      break;
    }
    case swift::TypeKind::BuiltinFloat: {
      swift::BuiltinFloatType *builtin_float_type =
          swift_can_type->castTo<swift::BuiltinFloatType>();
      s->Printf("builtin floating-point type of width %u bits\n",
                builtin_float_type->getBitWidth());
      break;
    }
    case swift::TypeKind::ProtocolComposition: {
      swift::ProtocolCompositionType *protocol_composition_type =
          swift_can_type->castTo<swift::ProtocolCompositionType>();
      std::string buffer;
      llvm::raw_string_ostream ostream(buffer);
      const swift::PrintOptions &print_options(
          SwiftASTContext::GetUserVisibleTypePrintingOptions(
              print_help_if_available));

      protocol_composition_type->print(ostream, print_options);
      ostream.flush();
      if (buffer.empty() == false)
        s->Printf("%s\n", buffer.c_str());
      break;
    }
    default: {
      swift::NominalType *nominal_type =
          llvm::dyn_cast_or_null<swift::NominalType>(
              swift_can_type.getPointer());
      if (nominal_type) {
        swift::NominalTypeDecl *nominal_type_decl = nominal_type->getDecl();
        PrintSwiftNominalType(nominal_type_decl, s, print_help_if_available,
                              print_extensions_if_available);
      }
    } break;
    }

    if (buf.size() > 0) {
      s->Write(buf.data(), buf.size());
    }
  }
}

TypeSP SwiftASTContext::GetCachedType(const ConstString &mangled) {
  TypeSP type_sp;
  if (m_swift_type_map.Lookup(mangled.GetCString(), type_sp))
    return type_sp;
  else
    return TypeSP();
}

void SwiftASTContext::SetCachedType(const ConstString &mangled,
                                    const TypeSP &type_sp) {
  m_swift_type_map.Insert(mangled.GetCString(), type_sp);
}

DWARFASTParser *SwiftASTContext::GetDWARFParser() {
  if (!m_dwarf_ast_parser_ap)
    m_dwarf_ast_parser_ap.reset(new DWARFASTParserSwift(*this));
  return m_dwarf_ast_parser_ap.get();
}

std::vector<lldb::DataBufferSP> &
SwiftASTContext::GetASTVectorForModule(const Module *module) {
  return m_ast_file_data_map[const_cast<Module *>(module)];
}

SwiftASTContextForExpressions::SwiftASTContextForExpressions(Target &target)
    : SwiftASTContext(target.GetArchitecture().GetTriple().getTriple().c_str(),
                      &target),
      m_persistent_state_up(new SwiftPersistentExpressionState) {}

UserExpression *SwiftASTContextForExpressions::GetUserExpression(
    llvm::StringRef expr, llvm::StringRef prefix, lldb::LanguageType language,
    Expression::ResultType desired_type,
    const EvaluateExpressionOptions &options) {
  TargetSP target_sp = m_target_wp.lock();
  if (!target_sp)
    return nullptr;

  return new SwiftUserExpression(*target_sp.get(), expr, prefix, language,
                                 desired_type, options);
}

PersistentExpressionState *
SwiftASTContextForExpressions::GetPersistentExpressionState() {
  return m_persistent_state_up.get();
}
