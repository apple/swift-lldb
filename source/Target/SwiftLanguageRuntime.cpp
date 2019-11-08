//===-- SwiftLanguageRuntime.cpp --------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/SwiftLanguageRuntime.h"

#include <string.h>

#include "llvm/Support/raw_ostream.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"

#include "swift/ABI/System.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Module.h"
#include "swift/AST/Types.h"
#include "swift/AST/ASTWalker.h"
#include "swift/Basic/SourceLoc.h"
#include "swift/Demangling/Demangle.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Reflection/ReflectionContext.h"
#include "swift/Reflection/TypeRefBuilder.h"
#include "swift/Remote/MemoryReader.h"
#include "swift/Remote/RemoteAddress.h"
#include "swift/RemoteAST/RemoteAST.h"
#include "swift/Runtime/Metadata.h"

#include "lldb/Breakpoint/StoppointCallbackContext.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Mangled.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/UniqueCStringMap.h"
#include "lldb/Core/Value.h"
#include "lldb/Core/ValueObjectConstResult.h"
#include "lldb/DataFormatters/StringPrinter.h"
#include "lldb/DataFormatters/TypeSynthetic.h"
#include "lldb/DataFormatters/ValueObjectPrinter.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/OptionParser.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/CommandObjectMultiword.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#include "lldb/Interpreter/OptionValueBoolean.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/ClangUtil.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SwiftASTContext.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/ProcessStructReader.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StackFrame.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadPlanRunToAddress.h"
#include "lldb/Target/ThreadPlanStepInRange.h"
#include "lldb/Target/ThreadPlanStepOverRange.h"
#include "lldb/Utility/Status.h"

#include "lldb/Utility/CleanUp.h"
#include "lldb/Utility/DataBuffer.h"
#include "lldb/Utility/LLDBAssert.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/StringLexer.h"

// FIXME: we should not need this
#include "Plugins/Language/Swift/SwiftFormatters.h"

using namespace lldb;
using namespace lldb_private;

char SwiftLanguageRuntime::ID = 0;

static constexpr std::chrono::seconds g_po_function_timeout(15);
static const char *g_dollar_tau_underscore = u8"$\u03C4_";
static ConstString g_self = ConstString("self");
extern "C" unsigned long long _swift_classIsSwiftMask = 0;

namespace lldb_private {
swift::Type GetSwiftType(void *opaque_ptr) {
  return reinterpret_cast<swift::TypeBase *>(opaque_ptr);
}

swift::CanType GetCanonicalSwiftType(void *opaque_ptr) {
  return reinterpret_cast<swift::TypeBase *>(opaque_ptr)->getCanonicalType();
}

swift::CanType GetCanonicalSwiftType(const CompilerType &type) {
  return GetCanonicalSwiftType(
      reinterpret_cast<void *>(type.GetOpaqueQualType()));
}

swift::Type GetSwiftType(const CompilerType &type) {
  return GetSwiftType(reinterpret_cast<void *>(type.GetOpaqueQualType()));
}
} // namespace lldb_private

SwiftLanguageRuntime::~SwiftLanguageRuntime() = default;

static bool HasReflectionInfo(ObjectFile *obj_file) {
  auto findSectionInObject = [&](std::string name) {
    ConstString section_name(name);
    SectionSP section_sp =
        obj_file->GetSectionList()->FindSectionByName(section_name);
    if (section_sp)
      return true;
    return false;
  };

  bool hasReflectionSection = false;
  hasReflectionSection |= findSectionInObject("__swift5_fieldmd");
  hasReflectionSection |= findSectionInObject("__swift5_assocty");
  hasReflectionSection |= findSectionInObject("__swift5_builtin");
  hasReflectionSection |= findSectionInObject("__swift5_capture");
  hasReflectionSection |= findSectionInObject("__swift5_typeref");
  hasReflectionSection |= findSectionInObject("__swift5_reflstr");
  return hasReflectionSection;
}

void SwiftLanguageRuntime::SetupReflection() {
  reflection_ctx.reset(new NativeReflectionContext(this->GetMemoryReader()));

  auto &target = m_process->GetTarget();
  auto exe_module = target.GetExecutableModule();
  if (!exe_module)
    return;
  auto *obj_file = exe_module->GetObjectFile();
  if (!obj_file)
      return;
  if (obj_file->GetPluginName().GetStringRef().equals("elf"))
    return;
  Address start_address = obj_file->GetBaseAddress();
  auto load_ptr = static_cast<uintptr_t>(start_address.GetLoadAddress(&target));

  // Bail out if we can't read the executable instead of crashing.
  if (load_ptr == 0 || load_ptr == LLDB_INVALID_ADDRESS)
    return;

  reflection_ctx.reset(new NativeReflectionContext(this->GetMemoryReader()));
  reflection_ctx->addImage(swift::remote::RemoteAddress(load_ptr));

  auto module_list = GetTargetRef().GetImages();
  module_list.ForEach([&](const ModuleSP &module_sp) -> bool {
    auto *obj_file = module_sp->GetObjectFile();
    if (!obj_file)
        return false;
    if (obj_file->GetPluginName().GetStringRef().equals("elf"))
      return true;
    Address start_address = obj_file->GetBaseAddress();
    auto load_ptr = static_cast<uintptr_t>(
        start_address.GetLoadAddress(&(m_process->GetTarget())));
    if (load_ptr == 0 || load_ptr == LLDB_INVALID_ADDRESS)
      return false;
    if (HasReflectionInfo(obj_file))
      reflection_ctx->addImage(swift::remote::RemoteAddress(load_ptr));
    return true;
  });
}

SwiftLanguageRuntime::SwiftLanguageRuntime(Process *process)
    : LanguageRuntime(process) {
  SetupSwiftError();
  SetupExclusivity();
  SetupReflection();
  SetupABIBit();
}

bool SwiftLanguageRuntime::IsABIStable() {
  return _swift_classIsSwiftMask == 2;
}

static llvm::Optional<lldb::addr_t>
FindSymbolForSwiftObject(Target &target, ConstString object,
                         const SymbolType sym_type) {
  llvm::Optional<lldb::addr_t> retval;

  SymbolContextList sc_list;
  if (target.GetImages().FindSymbolsWithNameAndType(object, sym_type,
                                                    sc_list)) {
    SymbolContext SwiftObject_Class;
    if (sc_list.GetSize() == 1 &&
        sc_list.GetContextAtIndex(0, SwiftObject_Class)) {
      if (SwiftObject_Class.symbol) {
        lldb::addr_t SwiftObject_class_addr =
            SwiftObject_Class.symbol->GetAddress().GetLoadAddress(&target);
        if (SwiftObject_class_addr &&
            SwiftObject_class_addr != LLDB_INVALID_ADDRESS)
          retval = SwiftObject_class_addr;
      }
    }
  }
  return retval;
}

AppleObjCRuntimeV2 *SwiftLanguageRuntime::GetObjCRuntime() {
  if (auto objc_runtime = ObjCLanguageRuntime::Get(*GetProcess())) {
    if (objc_runtime->GetPluginName() ==
        AppleObjCRuntimeV2::GetPluginNameStatic())
      return (AppleObjCRuntimeV2 *)objc_runtime;
  }
  return nullptr;
}

void SwiftLanguageRuntime::SetupSwiftError() {
  Target &target(m_process->GetTarget());

  if (m_SwiftNativeNSErrorISA.hasValue())
    return;

  ConstString g_SwiftNativeNSError("__SwiftNativeNSError");

  m_SwiftNativeNSErrorISA = FindSymbolForSwiftObject(
      target, g_SwiftNativeNSError, eSymbolTypeObjCClass);
}

void SwiftLanguageRuntime::SetupExclusivity() {
  Target &target(m_process->GetTarget());

  ConstString g_disableExclusivityChecking("_swift_disableExclusivityChecking");

  m_dynamic_exclusivity_flag_addr = FindSymbolForSwiftObject(
      target, g_disableExclusivityChecking, eSymbolTypeData);

  Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  if (log)
    log->Printf("SwiftLanguageRuntime: _swift_disableExclusivityChecking = %lu",
                m_dynamic_exclusivity_flag_addr ?
                *m_dynamic_exclusivity_flag_addr : 0);
}

void SwiftLanguageRuntime::SetupABIBit() {
  Target &target(m_process->GetTarget());
  ConstString g_objc_debug_swift_stable_abi_bit("objc_debug_swift_stable_abi_bit");

  if (FindSymbolForSwiftObject(target, g_objc_debug_swift_stable_abi_bit, eSymbolTypeAny))
    _swift_classIsSwiftMask = 2;
  else
    _swift_classIsSwiftMask = 1;
}

void SwiftLanguageRuntime::ModulesDidLoad(const ModuleList &module_list) {
  module_list.ForEach([&](const ModuleSP &module_sp) -> bool {
  auto *obj_file = module_sp->GetObjectFile();
    if (!obj_file)
        return true;
    Address start_address = obj_file->GetBaseAddress();
    auto load_ptr = static_cast<uintptr_t>(
        start_address.GetLoadAddress(&(m_process->GetTarget())));
    if (load_ptr == 0 || load_ptr == LLDB_INVALID_ADDRESS)
      return false;
    if (!reflection_ctx)
      return false;
    if (HasReflectionInfo(obj_file))
      reflection_ctx->addImage(swift::remote::RemoteAddress(load_ptr));
    return true;
  });
}

static const ExecutionContextRef *GetSwiftExeCtx(ValueObject &valobj) {
  return (valobj.GetPreferredDisplayLanguage() == eLanguageTypeSwift)
             ? &valobj.GetExecutionContextRef()
             : nullptr;
}

static bool GetObjectDescription_ObjectCopy(SwiftLanguageRuntime *runtime,
                                            Process *process, Stream &str,
                                            ValueObject &object) {
  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_DATAFORMATTERS));

  ValueObjectSP static_sp(object.GetStaticValue());

  CompilerType static_type(static_sp->GetCompilerType());
  if (auto non_reference_type = static_type.GetNonReferenceType())
    static_type = non_reference_type;

  Status error;

  // If we are in a generic context, here the static type of the object
  // might end up being generic (i.e. <T>). We want to make sure that
  // we correctly map the type into context before asking questions or
  // printing, as IRGen requires a fully realized type to work on.
  auto frame_sp =
      process->GetThreadList().GetSelectedThread()->GetSelectedFrame();
  auto *swift_ast_ctx =
      llvm::dyn_cast_or_null<SwiftASTContext>(static_type.GetTypeSystem());
  if (swift_ast_ctx) {
    SwiftASTContextLock lock(GetSwiftExeCtx(object));
    static_type = runtime->DoArchetypeBindingForType(*frame_sp, static_type);
  }

  auto stride = 0;
  auto opt_stride = static_type.GetByteStride(frame_sp.get());
  if (opt_stride)
    stride = *opt_stride;

  lldb::addr_t copy_location = process->AllocateMemory(
      stride, ePermissionsReadable | ePermissionsWritable, error);
  if (copy_location == LLDB_INVALID_ADDRESS) {
    if (log)
      log->Printf("[GetObjectDescription_ObjectCopy] copy_location invalid");
    return false;
  }
  CleanUp cleanup(
      [process, copy_location] { process->DeallocateMemory(copy_location); });

  DataExtractor data_extractor;
  if (0 == static_sp->GetData(data_extractor, error)) {
    if (log)
      log->Printf("[GetObjectDescription_ObjectCopy] data extraction failed");
    return false;
  }

  if (0 ==
      process->WriteMemory(copy_location, data_extractor.GetDataStart(),
                           data_extractor.GetByteSize(), error)) {
    if (log)
      log->Printf("[GetObjectDescription_ObjectCopy] memory copy failed");
    return false;
  }

  StreamString expr_string;
  expr_string.Printf("Swift._DebuggerSupport.stringForPrintObject(Swift."
                     "UnsafePointer<%s>(bitPattern: 0x%" PRIx64 ")!.pointee)",
                     static_type.GetTypeName().GetCString(), copy_location);

  if (log)
    log->Printf("[GetObjectDescription_ObjectCopy] expression: %s",
                expr_string.GetData());

  ValueObjectSP result_sp;
  EvaluateExpressionOptions eval_options;
  eval_options.SetLanguage(lldb::eLanguageTypeSwift);
  eval_options.SetResultIsInternal(true);
  eval_options.SetGenerateDebugInfo(true);
  eval_options.SetTimeout(g_po_function_timeout);
  auto eval_result = process->GetTarget().EvaluateExpression(
      expr_string.GetData(),
      process->GetThreadList().GetSelectedThread()->GetSelectedFrame().get(),
      result_sp, eval_options);

  if (log) {
    switch (eval_result) {
    case eExpressionCompleted:
      log->Printf("[GetObjectDescription_ObjectCopy] eExpressionCompleted");
      break;
    case eExpressionSetupError:
      log->Printf("[GetObjectDescription_ObjectCopy] eExpressionSetupError");
      break;
    case eExpressionParseError:
      log->Printf("[GetObjectDescription_ObjectCopy] eExpressionParseError");
      break;
    case eExpressionDiscarded:
      log->Printf("[GetObjectDescription_ObjectCopy] eExpressionDiscarded");
      break;
    case eExpressionInterrupted:
      log->Printf("[GetObjectDescription_ObjectCopy] eExpressionInterrupted");
      break;
    case eExpressionHitBreakpoint:
      log->Printf("[GetObjectDescription_ObjectCopy] eExpressionHitBreakpoint");
      break;
    case eExpressionTimedOut:
      log->Printf("[GetObjectDescription_ObjectCopy] eExpressionTimedOut");
      break;
    case eExpressionResultUnavailable:
      log->Printf(
          "[GetObjectDescription_ObjectCopy] eExpressionResultUnavailable");
      break;
    case eExpressionStoppedForDebug:
      log->Printf(
          "[GetObjectDescription_ObjectCopy] eExpressionStoppedForDebug");
      break;
    }
  }

  // sanitize the result of the expression before moving forward
  if (!result_sp) {
    if (log)
      log->Printf(
          "[GetObjectDescription_ObjectCopy] expression generated no result");

    str.Printf("expression produced no result");
    return true;
  }
  if (result_sp->GetError().Fail()) {
    if (log)
      log->Printf(
          "[GetObjectDescription_ObjectCopy] expression generated error: %s",
          result_sp->GetError().AsCString());

    str.Printf("expression produced error: %s",
               result_sp->GetError().AsCString());
    return true;
  }
  if (false == result_sp->GetCompilerType().IsValid()) {
    if (log)
      log->Printf("[GetObjectDescription_ObjectCopy] expression generated "
                  "invalid type");

    str.Printf("expression produced invalid result type");
    return true;
  }

  lldb_private::formatters::StringPrinter::ReadStringAndDumpToStreamOptions
      dump_options;
  dump_options.SetEscapeNonPrintables(false).SetQuote('\0').SetPrefixToken(
      nullptr);
  if (lldb_private::formatters::swift::String_SummaryProvider(
          *result_sp.get(), str, TypeSummaryOptions()
                                     .SetLanguage(lldb::eLanguageTypeSwift)
                                     .SetCapping(eTypeSummaryUncapped),
          dump_options)) {
    if (log)
      log->Printf("[GetObjectDescription_ObjectCopy] expression completed "
                  "successfully");
  } else {
    if (log)
      log->Printf("[GetObjectDescription_ObjectCopy] expression generated "
                  "invalid string data");

    str.Printf("expression produced unprintable string");
  }
  return true;
}

bool SwiftLanguageRuntime::GetObjectDescription(Stream &str,
                                                ValueObject &object) {
  if (object.IsUninitializedReference()) {
    str.Printf("<uninitialized>");
    return true;
  }
  return GetObjectDescription_ObjectCopy(this, m_process, str, object);
}

bool SwiftLanguageRuntime::GetObjectDescription(
    Stream &str, Value &value, ExecutionContextScope *exe_scope) {
  // This is only interesting to do with a ValueObject for Swift
  return false;
}

bool SwiftLanguageRuntime::IsSwiftMangledName(const char *name) {
  return swift::Demangle::isSwiftSymbol(name);
}

void SwiftLanguageRuntime::GetGenericParameterNamesForFunction(
    const SymbolContext &const_sc,
    llvm::DenseMap<SwiftLanguageRuntime::ArchetypePath, StringRef> &dict) {
  // This terrifying cast avoids having too many differences with llvm.org.
  SymbolContext &sc = const_cast<SymbolContext &>(const_sc);

  // While building the Symtab itself the symbol context is incomplete.
  // Note that calling sc.module_sp->FindFunctions() here is too early and
  // would mess up the loading process.
  if (!sc.function && sc.module_sp && sc.symbol)
    return;

  Block *block = sc.GetFunctionBlock();
  if (!block)
    return;

  bool can_create = true;
  VariableListSP var_list = block->GetBlockVariableList(can_create);
  if (!var_list)
    return;

  for (unsigned i = 0; i < var_list->GetSize(); ++i) {
    VariableSP var_sp = var_list->GetVariableAtIndex(i);
    StringRef name = var_sp->GetName().GetStringRef();
    if (!name.consume_front(g_dollar_tau_underscore))
      continue;

    uint64_t depth;
    if (name.consumeInteger(10, depth))
      continue;

    if (!name.consume_front("_"))
      continue;

    uint64_t index;
    if (name.consumeInteger(10, index))
      continue;

    if (!name.empty())
      continue;

    Type *archetype = var_sp->GetType();
    if (!archetype)
      continue;

    dict.insert({{depth, index}, archetype->GetName().GetStringRef()});
  }
}

std::string
SwiftLanguageRuntime::DemangleSymbolAsString(StringRef symbol, bool simplified,
                                             const SymbolContext *sc) {
  bool did_init = false;
  llvm::DenseMap<ArchetypePath, StringRef> dict;
  swift::Demangle::DemangleOptions options;
  if (simplified)
    options = swift::Demangle::DemangleOptions::SimplifiedUIDemangleOptions();

  if (sc) {
    options.GenericParameterName = [&](uint64_t depth, uint64_t index) {
      if (!did_init) {
        GetGenericParameterNamesForFunction(*sc, dict);
        did_init = true;
      }
      auto it = dict.find({depth, index});
      if (it != dict.end())
        return it->second.str();
      return swift::Demangle::genericParameterName(depth, index);
    };
  }
  return swift::Demangle::demangleSymbolAsString(symbol, options);
}

bool SwiftLanguageRuntime::IsSwiftClassName(const char *name)
{
  return swift::Demangle::isClass(name);
}

void SwiftLanguageRuntime::MethodName::Clear() {
  m_full.Clear();
  m_basename = llvm::StringRef();
  m_context = llvm::StringRef();
  m_arguments = llvm::StringRef();
  m_qualifiers = llvm::StringRef();
  m_template_args = llvm::StringRef();
  m_metatype_ref = llvm::StringRef();
  m_return_type = llvm::StringRef();
  m_type = eTypeInvalid;
  m_parsed = false;
  m_parse_error = false;
}

static bool StringHasAllOf(const llvm::StringRef &s, const char *which) {
  for (const char *c = which; *c != 0; c++) {
    if (s.find(*c) == llvm::StringRef::npos)
      return false;
  }
  return true;
}

static bool StringHasAnyOf(const llvm::StringRef &s,
                           std::initializer_list<const char *> which,
                           size_t &where) {
  for (const char *item : which) {
    size_t where_item = s.find(item);
    if (where_item != llvm::StringRef::npos) {
      where = where_item;
      return true;
    }
  }
  where = llvm::StringRef::npos;
  return false;
}

static bool UnpackTerminatedSubstring(const llvm::StringRef &s,
                                      const char start, const char stop,
                                      llvm::StringRef &dest) {
  size_t pos_of_start = s.find(start);
  if (pos_of_start == llvm::StringRef::npos)
    return false;
  size_t pos_of_stop = s.rfind(stop);
  if (pos_of_stop == llvm::StringRef::npos)
    return false;
  size_t token_count = 1;
  size_t idx = pos_of_start + 1;
  while (idx < s.size()) {
    if (s[idx] == start)
      ++token_count;
    if (s[idx] == stop) {
      if (token_count == 1) {
        dest = s.slice(pos_of_start, idx + 1);
        return true;
      }
    }
    idx++;
  }
  return false;
}

static bool UnpackQualifiedName(const llvm::StringRef &s, llvm::StringRef &decl,
                                llvm::StringRef &basename, bool &was_operator) {
  size_t pos_of_dot = s.rfind('.');
  if (pos_of_dot == llvm::StringRef::npos)
    return false;
  decl = s.substr(0, pos_of_dot);
  basename = s.substr(pos_of_dot + 1);
  size_t idx_of_operator;
  was_operator = StringHasAnyOf(basename, {"@infix", "@prefix", "@postfix"},
                                idx_of_operator);
  if (was_operator)
    basename = basename.substr(0, idx_of_operator - 1);
  return !decl.empty() && !basename.empty();
}

static bool ParseLocalDeclName(const swift::Demangle::NodePointer &node,
                               StreamString &identifier,
                               swift::Demangle::Node::Kind &parent_kind,
                               swift::Demangle::Node::Kind &kind) {
  swift::Demangle::Node::iterator end = node->end();
  for (swift::Demangle::Node::iterator pos = node->begin(); pos != end; ++pos) {
    swift::Demangle::NodePointer child = *pos;

    swift::Demangle::Node::Kind child_kind = child->getKind();
    switch (child_kind) {
    case swift::Demangle::Node::Kind::Number:
      break;

    default:
      if (child->hasText()) {
        identifier.PutCString(child->getText());
        return true;
      }
      break;
    }
  }
  return false;
}

static bool ParseFunction(const swift::Demangle::NodePointer &node,
                          StreamString &identifier,
                          swift::Demangle::Node::Kind &parent_kind,
                          swift::Demangle::Node::Kind &kind) {
  swift::Demangle::Node::iterator end = node->end();
  swift::Demangle::Node::iterator pos = node->begin();
  // First child is the function's scope
  parent_kind = (*pos)->getKind();
  ++pos;
  // Second child is either the type (no identifier)
  if (pos != end) {
    switch ((*pos)->getKind()) {
    case swift::Demangle::Node::Kind::Type:
      break;

    case swift::Demangle::Node::Kind::LocalDeclName:
      if (ParseLocalDeclName(*pos, identifier, parent_kind, kind))
        return true;
      else
        return false;
      break;

    default:
    case swift::Demangle::Node::Kind::InfixOperator:
    case swift::Demangle::Node::Kind::PostfixOperator:
    case swift::Demangle::Node::Kind::PrefixOperator:
    case swift::Demangle::Node::Kind::Identifier:
      if ((*pos)->hasText())
        identifier.PutCString((*pos)->getText());
      return true;
    }
  }
  return false;
}

static bool ParseGlobal(const swift::Demangle::NodePointer &node,
                        StreamString &identifier,
                        swift::Demangle::Node::Kind &parent_kind,
                        swift::Demangle::Node::Kind &kind) {
  swift::Demangle::Node::iterator end = node->end();
  for (swift::Demangle::Node::iterator pos = node->begin(); pos != end; ++pos) {
    swift::Demangle::NodePointer child = *pos;
    if (child) {
      kind = child->getKind();
      switch (child->getKind()) {
      case swift::Demangle::Node::Kind::Allocator:
        identifier.PutCString("__allocating_init");
        ParseFunction(child, identifier, parent_kind, kind);
        return true;

      case swift::Demangle::Node::Kind::Constructor:
        identifier.PutCString("init");
        ParseFunction(child, identifier, parent_kind, kind);
        return true;

      case swift::Demangle::Node::Kind::Deallocator:
        identifier.PutCString("__deallocating_deinit");
        ParseFunction(child, identifier, parent_kind, kind);
        return true;

      case swift::Demangle::Node::Kind::Destructor:
        identifier.PutCString("deinit");
        ParseFunction(child, identifier, parent_kind, kind);
        return true;

      case swift::Demangle::Node::Kind::Getter:
      case swift::Demangle::Node::Kind::Setter:
      case swift::Demangle::Node::Kind::Function:
        return ParseFunction(child, identifier, parent_kind, kind);

      // Ignore these, they decorate a function at the same level, but don't
      // contain any text
      case swift::Demangle::Node::Kind::ObjCAttribute:
        break;

      default:
        return false;
      }
    }
  }
  return false;
}

bool SwiftLanguageRuntime::MethodName::ExtractFunctionBasenameFromMangled(
    ConstString mangled, ConstString &basename, bool &is_method) {
  bool success = false;
  swift::Demangle::Node::Kind kind = swift::Demangle::Node::Kind::Global;
  swift::Demangle::Node::Kind parent_kind = swift::Demangle::Node::Kind::Global;
  if (mangled) {
    const char *mangled_cstr = mangled.GetCString();
    const size_t mangled_cstr_len = mangled.GetLength();

    if (mangled_cstr_len > 3) {
      llvm::StringRef mangled_ref(mangled_cstr, mangled_cstr_len);

      // Only demangle swift functions
      // This is a no-op right now for the new mangling, because you
      // have to demangle the whole name to figure this out anyway.
      // I'm leaving the test here in case we actually need to do this
      // only to functions.
      swift::Demangle::Context demangle_ctx;
      swift::Demangle::NodePointer node =
          demangle_ctx.demangleSymbolAsNode(mangled_ref);
      StreamString identifier;
      if (node) {
        switch (node->getKind()) {
        case swift::Demangle::Node::Kind::Global:
          success = ParseGlobal(node, identifier, parent_kind, kind);
          break;

        default:
          break;
        }

        if (!identifier.GetString().empty()) {
          basename = ConstString(identifier.GetString());
        }
      }
    }
  }
  if (success) {
    switch (kind) {
    case swift::Demangle::Node::Kind::Allocator:
    case swift::Demangle::Node::Kind::Constructor:
    case swift::Demangle::Node::Kind::Deallocator:
    case swift::Demangle::Node::Kind::Destructor:
      is_method = true;
      break;

    case swift::Demangle::Node::Kind::Getter:
    case swift::Demangle::Node::Kind::Setter:
      // don't handle getters and setters right now...
      return false;

    case swift::Demangle::Node::Kind::Function:
      switch (parent_kind) {
      case swift::Demangle::Node::Kind::BoundGenericClass:
      case swift::Demangle::Node::Kind::BoundGenericEnum:
      case swift::Demangle::Node::Kind::BoundGenericStructure:
      case swift::Demangle::Node::Kind::Class:
      case swift::Demangle::Node::Kind::Enum:
      case swift::Demangle::Node::Kind::Structure:
        is_method = true;
        break;

      default:
        break;
      }
      break;

    default:
      break;
    }
  }
  return success;
}

void SwiftLanguageRuntime::MethodName::Parse() {
  if (!m_parsed && m_full) {
    m_parse_error = false;
    m_parsed = true;
    llvm::StringRef full(m_full.GetCString());
    bool was_operator = false;

    if (full.find("::") != llvm::StringRef::npos) {
      // :: is not an allowed operator in Swift (func ::(...) { fails to
      // compile)
      // but it's a very legitimate token in C++ - as a defense, reject anything
      // with a :: in it as invalid Swift
      m_parse_error = true;
      return;
    }

    if (StringHasAllOf(full, ".:()")) {
      const size_t open_paren = full.find(" (");
      llvm::StringRef funcname = full.substr(0, open_paren);
      UnpackQualifiedName(funcname, m_context, m_basename, was_operator);
      if (was_operator)
        m_type = eTypeOperator;
      // check for obvious constructor/destructor cases
      else if (m_basename.equals("__deallocating_destructor"))
        m_type = eTypeDeallocator;
      else if (m_basename.equals("__allocating_constructor"))
        m_type = eTypeAllocator;
      else if (m_basename.equals("init"))
        m_type = eTypeConstructor;
      else if (m_basename.equals("destructor"))
        m_type = eTypeDestructor;
      else
        m_type = eTypeUnknownMethod;

      const size_t idx_of_colon =
          full.find(':', open_paren == llvm::StringRef::npos ? 0 : open_paren);
      full = full.substr(idx_of_colon + 2);
      if (full.empty())
        return;
      if (full[0] == '<') {
        if (UnpackTerminatedSubstring(full, '<', '>', m_template_args)) {
          full = full.substr(m_template_args.size());
        } else {
          m_parse_error = true;
          return;
        }
      }
      if (full.empty())
        return;
      if (full[0] == '(') {
        if (UnpackTerminatedSubstring(full, '(', ')', m_metatype_ref)) {
          full = full.substr(m_template_args.size());
          if (full[0] == '<') {
            if (UnpackTerminatedSubstring(full, '<', '>', m_template_args)) {
              full = full.substr(m_template_args.size());
            } else {
              m_parse_error = true;
              return;
            }
          }
        } else {
          m_parse_error = true;
          return;
        }
      }
      if (full.empty())
        return;
      if (full[0] == '(') {
        if (UnpackTerminatedSubstring(full, '(', ')', m_arguments)) {
          full = full.substr(m_template_args.size());
        } else {
          m_parse_error = true;
          return;
        }
      }
      if (full.empty())
        return;
      size_t idx_of_ret = full.find("->");
      if (idx_of_ret == llvm::StringRef::npos) {
        full = full.substr(idx_of_ret);
        if (full.empty()) {
          m_parse_error = true;
          return;
        }
        if (full[0] == ' ')
          full = full.substr(1);
        m_return_type = full;
      }
    } else if (full.find('.') != llvm::StringRef::npos) {
      // this is probably just a full name (module.type.func)
      UnpackQualifiedName(full, m_context, m_basename, was_operator);
      if (was_operator)
        m_type = eTypeOperator;
      else
        m_type = eTypeUnknownMethod;
    } else {
      // this is most probably just a basename
      m_basename = full;
      m_type = eTypeUnknownMethod;
    }
  }
}

llvm::StringRef SwiftLanguageRuntime::MethodName::GetBasename() {
  if (!m_parsed)
    Parse();
  return m_basename;
}

const CompilerType &SwiftLanguageRuntime::GetBoxMetadataType() {
  if (m_box_metadata_type.IsValid())
    return m_box_metadata_type;

  static ConstString g_type_name("__lldb_autogen_boxmetadata");
  const bool is_packed = false;
  if (ClangASTContext *ast_ctx =
          GetProcess()->GetTarget().GetScratchClangASTContext()) {
    CompilerType voidstar =
        ast_ctx->GetBasicType(lldb::eBasicTypeVoid).GetPointerType();
    CompilerType uint32 = ClangASTContext::GetIntTypeFromBitSize(
        ast_ctx->getASTContext(), 32, false);

    m_box_metadata_type = ast_ctx->GetOrCreateStructForIdentifier(
        g_type_name, {{"kind", voidstar}, {"offset", uint32}}, is_packed);
  }

  return m_box_metadata_type;
}

class LLDBMemoryReader : public swift::remote::MemoryReader {
public:
  LLDBMemoryReader(Process *p, size_t max_read_amount = INT32_MAX)
      : m_process(p) {
    lldbassert(m_process && "MemoryReader requires a valid Process");
    m_max_read_amount = max_read_amount;
  }

  virtual ~LLDBMemoryReader() = default;

  bool queryDataLayout(DataLayoutQueryType type, void *inBuffer,
                        void *outBuffer) override {
    switch (type) {
      case DLQ_GetPointerSize: {
        auto result = static_cast<uint8_t *>(outBuffer);
        *result = m_process->GetAddressByteSize();
        return true;
      }
      case DLQ_GetSizeSize: {
        auto result = static_cast<uint8_t *>(outBuffer);
        *result = m_process->GetAddressByteSize();  // FIXME: sizeof(size_t)
        return true;
      }
    }

    return false;
  }

  swift::remote::RemoteAddress
  getSymbolAddress(const std::string &name) override {
    lldbassert(!name.empty());
    if (name.empty())
      return swift::remote::RemoteAddress(nullptr);

    LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
             "[MemoryReader] asked to retrieve the address of symbol {0}",
             name);

    ConstString name_cs(name.c_str(), name.size());
    SymbolContextList sc_list;
    if (!m_process->GetTarget().GetImages().FindSymbolsWithNameAndType(
            name_cs, lldb::eSymbolTypeAny, sc_list)) {
      LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
               "[MemoryReader] symbol resoution failed {0}", name);
      return swift::remote::RemoteAddress(nullptr);
    }

    SymbolContext sym_ctx;
    // Remove undefined symbols from the list.
    size_t num_sc_matches = sc_list.GetSize();
    if (num_sc_matches > 1) {
      SymbolContextList tmp_sc_list(sc_list);
      sc_list.Clear();
      for (size_t idx = 0; idx < num_sc_matches; idx++) {
        tmp_sc_list.GetContextAtIndex(idx, sym_ctx);
        if (sym_ctx.symbol &&
            sym_ctx.symbol->GetType() != lldb::eSymbolTypeUndefined) {
            sc_list.Append(sym_ctx);
        }
      }
    }
    if (sc_list.GetSize() == 1 && sc_list.GetContextAtIndex(0, sym_ctx)) {
      if (sym_ctx.symbol) {
        auto load_addr =
            sym_ctx.symbol->GetLoadAddress(&m_process->GetTarget());
        LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
                 "[MemoryReader] symbol resolved to 0x%" PRIx64, load_addr);
        return swift::remote::RemoteAddress(load_addr);
      }
    }

    // Empty list, resolution failed.
    if (sc_list.GetSize() == 0) {
      LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
               "[MemoryReader] symbol resoution failed {0}", name);
      return swift::remote::RemoteAddress(nullptr);
    }

    // If there's a single symbol, then we're golden. If there's more than
    // a symbol, then just make sure all of them agree on the value.
    Status error;
    auto sym = sc_list.GetContextAtIndex(0, sym_ctx);
    auto load_addr = sym_ctx.symbol->GetLoadAddress(&m_process->GetTarget());
    uint64_t sym_value = m_process->GetTarget().ReadUnsignedIntegerFromMemory(
        load_addr, false, m_process->GetAddressByteSize(), 0, error);
    for (unsigned i = 1; i < sc_list.GetSize(); ++i) {
      auto other_sym = sc_list.GetContextAtIndex(i, sym_ctx);
      auto other_load_addr =
          sym_ctx.symbol->GetLoadAddress(&m_process->GetTarget());
      uint64_t other_sym_value =
           m_process->GetTarget().ReadUnsignedIntegerFromMemory(
               load_addr, false, m_process->GetAddressByteSize(), 0, error);
      if (sym_value != other_sym_value) {
        LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
                 "[MemoryReader] symbol resoution failed {0}", name);
        return swift::remote::RemoteAddress(nullptr);
      }
    }
    LLDB_LOG(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES),
             "[MemoryReader] symbol resolved to {0}", load_addr);
    return swift::remote::RemoteAddress(load_addr);
  }

  bool readBytes(swift::remote::RemoteAddress address, uint8_t *dest,
                 uint64_t size) override {
    if (m_local_buffer) {
      auto addr = address.getAddressData();
      if (addr >= m_local_buffer &&
          addr + size <= m_local_buffer + m_local_buffer_size) {
        // If this crashes, the assumptions stated in
        // GetDynamicTypeAndAddress_Protocol() most likely no longer
        // hold.
        memcpy(dest, (void *) addr, size);
        return true;
      }
    }

    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf("[MemoryReader] asked to read %" PRIu64
                  " bytes at address 0x%" PRIx64,
                  size, address.getAddressData());

    if (size > m_max_read_amount) {
      if (log)
        log->Printf(
            "[MemoryReader] memory read exceeds maximum allowed size");
      return false;
    }

    Target &target(m_process->GetTarget());
    Address addr(address.getAddressData());
    Status error;
    if (size > target.ReadMemory(addr, false, dest, size, error)) {
      if (log)
        log->Printf(
            "[MemoryReader] memory read returned fewer bytes than asked for");
      return false;
    }
    if (error.Fail()) {
      if (log)
        log->Printf("[MemoryReader] memory read returned error: %s",
                    error.AsCString());
      return false;
    }

    if (log && log->GetVerbose()) {
      StreamString stream;
      for (uint64_t i = 0; i < size; i++) {
        stream.PutHex8(dest[i]);
        stream.PutChar(' ');
      }
      log->Printf("[MemoryReader] memory read returned data: %s",
                  stream.GetData());
    }

    return true;
  }

  bool readString(swift::remote::RemoteAddress address,
                  std::string &dest) override {
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

    if (log)
      log->Printf(
          "[MemoryReader] asked to read string data at address 0x%" PRIx64,
          address.getAddressData());

    uint32_t read_size = 50 * 1024;
    std::vector<char> storage(read_size, 0);
    Target &target(m_process->GetTarget());
    Address addr(address.getAddressData());
    Status error;
    target.ReadCStringFromMemory(addr, &storage[0], storage.size(), error);
    if (error.Success()) {
      dest.assign(&storage[0]);
      if (log)
        log->Printf("[MemoryReader] memory read returned data: %s",
                    dest.c_str());
      return true;
    } else {
      if (log)
        log->Printf("[MemoryReader] memory read returned error: %s",
                    error.AsCString());
      return false;
    }
  }

  void pushLocalBuffer(uint64_t local_buffer, uint64_t local_buffer_size) {
    lldbassert(!m_local_buffer);
    m_local_buffer = local_buffer;
    m_local_buffer_size = local_buffer_size;
  }

  void popLocalBuffer() {
    lldbassert(m_local_buffer);
    m_local_buffer = 0;
    m_local_buffer_size = 0;
  }

private:
  Process *m_process;
  size_t m_max_read_amount;

  uint64_t m_local_buffer = 0;
  uint64_t m_local_buffer_size = 0;
};

std::shared_ptr<swift::remote::MemoryReader>
SwiftLanguageRuntime::GetMemoryReader() {
  if (!m_memory_reader_sp)
    m_memory_reader_sp.reset(new LLDBMemoryReader(GetProcess()));

  return m_memory_reader_sp;
}

void SwiftLanguageRuntime::PushLocalBuffer(uint64_t local_buffer,
                                           uint64_t local_buffer_size) {
  ((LLDBMemoryReader *)GetMemoryReader().get())->pushLocalBuffer(
        local_buffer, local_buffer_size);
}

void SwiftLanguageRuntime::PopLocalBuffer() {
  ((LLDBMemoryReader *)GetMemoryReader().get())->popLocalBuffer();
}

SwiftLanguageRuntime::MetadataPromise::MetadataPromise(
    ValueObject &for_object, SwiftLanguageRuntime &runtime,
    lldb::addr_t location)
    : m_for_object_sp(for_object.GetSP()), m_swift_runtime(runtime),
      m_metadata_location(location) {}

CompilerType
SwiftLanguageRuntime::MetadataPromise::FulfillTypePromise(Status *error) {
  if (error)
    error->Clear();

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

  if (log)
    log->Printf("[MetadataPromise] asked to fulfill type promise at location "
                "0x%" PRIx64,
                m_metadata_location);

  if (m_compiler_type.hasValue())
    return m_compiler_type.getValue();

  auto swift_ast_ctx = m_for_object_sp->GetScratchSwiftASTContext();
  if (!swift_ast_ctx) {
    error->SetErrorString("couldn't get Swift scratch context");
    return CompilerType();
  }
  auto &remote_ast = m_swift_runtime.GetRemoteASTContext(*swift_ast_ctx);
  swift::remoteAST::Result<swift::Type> result =
      remote_ast.getTypeForRemoteTypeMetadata(
          swift::remote::RemoteAddress(m_metadata_location));

  if (result) {
    m_compiler_type = {swift_ast_ctx.get(), result.getValue().getPointer()};
    if (log)
      log->Printf("[MetadataPromise] result is type %s",
                  m_compiler_type->GetTypeName().AsCString());
    return m_compiler_type.getValue();
  } else {
    const auto &failure = result.getFailure();
    if (error)
      error->SetErrorStringWithFormat("error in resolving type: %s",
                                      failure.render().c_str());
    if (log)
      log->Printf("[MetadataPromise] failure: %s", failure.render().c_str());
    return (m_compiler_type = CompilerType()).getValue();
  }
}

SwiftLanguageRuntime::MetadataPromiseSP
SwiftLanguageRuntime::GetMetadataPromise(lldb::addr_t addr,
                                         ValueObject &for_object) {
  auto swift_ast_ctx = for_object.GetScratchSwiftASTContext();
  if (!swift_ast_ctx || swift_ast_ctx->HasFatalErrors())
    return nullptr;

  if (addr == 0 || addr == LLDB_INVALID_ADDRESS)
    return nullptr;

  auto key = std::make_pair(swift_ast_ctx->GetASTContext(), addr);
  auto iter = m_promises_map.find(key);
  if (iter != m_promises_map.end())
    return iter->second;

  MetadataPromiseSP promise_sp(new MetadataPromise(for_object, *this, addr));
  m_promises_map.insert({key, promise_sp});
  return promise_sp;
}

swift::remoteAST::RemoteASTContext &
SwiftLanguageRuntime::GetRemoteASTContext(SwiftASTContext &swift_ast_ctx) {
  // If we already have a remote AST context for this AST context,
  // return it.
  auto known = m_remote_ast_contexts.find(swift_ast_ctx.GetASTContext());
  if (known != m_remote_ast_contexts.end())
    return *known->second;

  // Initialize a new remote AST context.
  auto remote_ast_up = std::make_unique<swift::remoteAST::RemoteASTContext>(
      *swift_ast_ctx.GetASTContext(), GetMemoryReader());
  auto &remote_ast = *remote_ast_up;
  m_remote_ast_contexts.insert(
      {swift_ast_ctx.GetASTContext(), std::move(remote_ast_up)});
  return remote_ast;
}

void SwiftLanguageRuntime::ReleaseAssociatedRemoteASTContext(
    swift::ASTContext *ctx) {
  m_remote_ast_contexts.erase(ctx);
}

namespace {
class ASTVerifier : public swift::ASTWalker {
  bool hasMissingPatterns = false;

  bool walkToDeclPre(swift::Decl *D) override {
    if (auto *PBD = llvm::dyn_cast<swift::PatternBindingDecl>(D)) {
      if (PBD->getPatternList().empty()) {
        hasMissingPatterns = true;
        return false;
      }
    }
    return true;
  }

public:
  /// Detect (one form of) incomplete types. These may appear if
  /// member variables have Clang-imported types that couldn't be
  /// resolved.
  static bool Verify(swift::Decl *D) {
    if (!D)
      return false;

    ASTVerifier verifier;
    D->walk(verifier);
    return !verifier.hasMissingPatterns;
  }
};

}

llvm::Optional<uint64_t>
SwiftLanguageRuntime::GetMemberVariableOffset(CompilerType instance_type,
                                              ValueObject *instance,
                                              ConstString member_name,
                                              Status *error) {
  if (!instance_type.IsValid())
    return llvm::None;

  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
  // Using the module context for RemoteAST is cheaper bit only safe
  // when there is no dynamic type resolution involved.
  auto *module_ctx =
      llvm::dyn_cast_or_null<SwiftASTContext>(instance_type.GetTypeSystem());
  if (!module_ctx || module_ctx->HasFatalErrors())
    return llvm::None;

  llvm::Optional<SwiftASTContextReader> scratch_ctx;
  if (instance) {
    scratch_ctx = instance->GetScratchSwiftASTContext();
    if (!scratch_ctx)
      return llvm::None;
  }
  
  auto *remote_ast = &GetRemoteASTContext(*module_ctx);

  if (log)
    log->Printf(
        "[GetMemberVariableOffset] asked to resolve offset for member %s",
        member_name.AsCString());

  // Check whether we've already cached this offset.
  swift::TypeBase *swift_type =
      GetCanonicalSwiftType(instance_type).getPointer();

  // Perform the cache lookup.
  MemberID key{swift_type, member_name.GetCString()};
  auto it = m_member_offsets.find(key);
  if (it != m_member_offsets.end())
    return it->second;

  // Dig out metadata describing the type, if it's easy to find.
  // FIXME: the Remote AST library should make this easier.
  swift::remote::RemoteAddress optmeta(nullptr);
  const swift::TypeKind type_kind = swift_type->getKind();
  switch (type_kind) {
  case swift::TypeKind::Class:
  case swift::TypeKind::BoundGenericClass: {
    if (log)
      log->Printf("[MemberVariableOffsetResolver] type is a class - trying to "
                  "get metadata for valueobject %s",
                  (instance ? instance->GetName().AsCString() : "<null>"));
    if (instance) {
      lldb::addr_t pointer = instance->GetPointerValue();
      if (!pointer || pointer == LLDB_INVALID_ADDRESS)
        break;
      swift::remote::RemoteAddress address(pointer);
      if (auto metadata = remote_ast->getHeapMetadataForObject(address))
        optmeta = metadata.getValue();
    }
    if (log)
      log->Printf("[MemberVariableOffsetResolver] optmeta = 0x%" PRIx64,
                  optmeta.getAddressData());
    break;
  }

  default:
    // Bind generic parameters if necessary.
    if (instance && swift_type->hasTypeParameter())
      if (auto *frame = instance->GetExecutionContextRef().GetFrameSP().get())
        if (auto bound = DoArchetypeBindingForType(*frame, instance_type)) {
          if (log)
            log->Printf(
                "[MemberVariableOffsetResolver] resolved non-class type = %s",
                bound.GetTypeName().AsCString());

          swift_type = GetCanonicalSwiftType(bound).getPointer();
          MemberID key{swift_type, member_name.GetCString()};
          auto it = m_member_offsets.find(key);
          if (it != m_member_offsets.end())
            return it->second;

          assert(bound.GetTypeSystem() == scratch_ctx->get());
          remote_ast = &GetRemoteASTContext(*scratch_ctx->get());
        }
  }

  // Try to determine whether it is safe to use RemoteAST.  RemoteAST
  // is faster than RemoteMirrors, but can't do dynamic types (checked
  // inside RemoteAST) or incomplete types (checked here).
  bool safe_to_use_remote_ast = true;
  if (swift::Decl *type_decl = swift_type->getNominalOrBoundGenericNominal())
    safe_to_use_remote_ast &= ASTVerifier::Verify(type_decl);
 
  // Use RemoteAST to determine the member offset.
  if (safe_to_use_remote_ast) {
    swift::remoteAST::Result<uint64_t> result = remote_ast->getOffsetOfMember(
        swift_type, optmeta, member_name.GetStringRef());
    if (result) {
      if (log)
        log->Printf(
            "[MemberVariableOffsetResolver] offset discovered = %" PRIu64,
            (uint64_t)result.getValue());

      // Cache this result.
      MemberID key{swift_type, member_name.GetCString()};
      m_member_offsets.insert({key, result.getValue()});
      return result.getValue();
    }

    const auto &failure = result.getFailure();
    if (error)
      error->SetErrorStringWithFormat("error in resolving type offset: %s",
                                      failure.render().c_str());
    if (log)
      log->Printf("[MemberVariableOffsetResolver] failure: %s",
                  failure.render().c_str());
  }

  // Try remote mirrors.
  const swift::reflection::TypeInfo *type_info = GetTypeInfo(instance_type);
  if (!type_info)
    return llvm::None;
  auto record_type_info =
      llvm::dyn_cast<swift::reflection::RecordTypeInfo>(type_info);
  if (record_type_info) {
    // Handle tuples.
    if (record_type_info->getRecordKind() ==
        swift::reflection::RecordKind::Tuple) {
      unsigned tuple_idx;
      if (member_name.GetStringRef().getAsInteger(10, tuple_idx) ||
          tuple_idx >= record_type_info->getNumFields()) {
        if (error)
          error->SetErrorString("tuple index out of bounds");
        return llvm::None;
      }
      return record_type_info->getFields()[tuple_idx].Offset;
    }

    // Handle other record types.
    for (auto &field : record_type_info->getFields()) {
      if (ConstString(field.Name) == member_name)
        return field.Offset;
    }
  }

  lldb::addr_t pointer = instance->GetPointerValue();
  auto class_instance_type_info = reflection_ctx->getInstanceTypeInfo(pointer);
  if (class_instance_type_info) {
    auto class_type_info = llvm::dyn_cast<swift::reflection::RecordTypeInfo>(
        class_instance_type_info);
    if (class_type_info) {
      for (auto &field : class_type_info->getFields()) {
        if (ConstString(field.Name) == member_name)
          return field.Offset;
      }
    }
  }
  return llvm::None;
}

bool SwiftLanguageRuntime::IsSelf(Variable &variable) {
  // A variable is self if its name if "self", and it's either a
  // function argument or a local variable and it's scope is a
  // constructor. These checks are sorted from cheap to expensive.
  if (variable.GetUnqualifiedName() != g_self)
    return false;

  if (variable.GetScope() == lldb::eValueTypeVariableArgument)
    return true;

  if (variable.GetScope() != lldb::eValueTypeVariableLocal)
    return false;

  SymbolContextScope *sym_ctx_scope = variable.GetSymbolContextScope();
  if (!sym_ctx_scope)
    return false;
  Function *function = sym_ctx_scope->CalculateSymbolContextFunction();
  if (!function)
    return false;
  StringRef func_name = function->GetMangled().GetMangledName().GetStringRef();
  swift::Demangle::Context demangle_ctx;
  swift::Demangle::NodePointer node_ptr =
      demangle_ctx.demangleSymbolAsNode(func_name);
  if (!node_ptr)
    return false;
  if (node_ptr->getKind() != swift::Demangle::Node::Kind::Global)
    return false;
  if (node_ptr->getNumChildren() != 1)
    return false;
  node_ptr = node_ptr->getFirstChild();
  return node_ptr->getKind() == swift::Demangle::Node::Kind::Constructor;
}

/// Determine whether the scratch SwiftASTContext has been locked.
static bool IsScratchContextLocked(Target &target) {
  if (target.GetSwiftScratchContextLock().try_lock()) {
    target.GetSwiftScratchContextLock().unlock();
    return false;
  }
  return true;
}

/// Determine whether the scratch SwiftASTContext has been locked.
static bool IsScratchContextLocked(TargetSP target) {
    return target ? IsScratchContextLocked(*target) : true;
}

bool SwiftLanguageRuntime::GetDynamicTypeAndAddress_Class(
    ValueObject &in_value, SwiftASTContext &scratch_ctx,
    lldb::DynamicValueType use_dynamic, TypeAndOrName &class_type_or_name,
    Address &address) {
  AddressType address_type;
  lldb::addr_t class_metadata_ptr = in_value.GetPointerValue(&address_type);
  if (class_metadata_ptr == LLDB_INVALID_ADDRESS || class_metadata_ptr == 0)
    return false;
  address.SetRawAddress(class_metadata_ptr);

  // Ask mirrors about the right type.
  auto metadata = reflection_ctx->readMetadataFromInstance(class_metadata_ptr);
  auto tr = reflection_ctx->readTypeFromMetadata(*metadata);
  if (!tr)
    return false;
  std::string mangled;
  if (auto *ntr = llvm::dyn_cast<swift::reflection::NominalTypeRef>(tr))
    mangled = ntr->getMangledName();
  else if (auto *bgtr =
               llvm::dyn_cast<swift::reflection::BoundGenericTypeRef>(tr))
    mangled = bgtr->getMangledName();
  else
    llvm_unreachable("Invalid typeref!");

  Status error;

  // FIXME: Use an API to retrieve the prefix instead of hardcoding it.
  ConstString mangled_with_prefix("$s" + mangled);

  SwiftASTContext *swift_ast_ctx = llvm::dyn_cast_or_null<SwiftASTContext>(
      in_value.GetCompilerType().GetTypeSystem());

  // TypeRef -> swift::Type conversion through type reconstruction.
  auto resolved_type =
      swift_ast_ctx->GetTypeFromMangledTypename(mangled_with_prefix, error);
  class_type_or_name.SetCompilerType(resolved_type);
  return true;
}

bool SwiftLanguageRuntime::IsValidErrorValue(ValueObject &in_value) {
  CompilerType var_type = in_value.GetStaticValue()->GetCompilerType();
  SwiftASTContext::ProtocolInfo protocol_info;
  if (!SwiftASTContext::GetProtocolTypeInfo(var_type, protocol_info))
    return false;
  if (!protocol_info.m_is_errortype)
    return false;

  unsigned index = SwiftASTContext::ProtocolInfo::error_instance_index;
  ValueObjectSP instance_type_sp(
                  in_value.GetStaticValue()->GetChildAtIndex(index, true));
  if (!instance_type_sp)
    return false;
  lldb::addr_t metadata_location = instance_type_sp->GetValueAsUnsigned(0);
  if (metadata_location == 0 || metadata_location == LLDB_INVALID_ADDRESS)
    return false;

  SetupSwiftError();
  if (m_SwiftNativeNSErrorISA.hasValue()) {
    if (auto objc_runtime = GetObjCRuntime()) {
      if (auto descriptor =
              objc_runtime->GetClassDescriptor(*instance_type_sp)) {
        if (descriptor->GetISA() != m_SwiftNativeNSErrorISA.getValue()) {
          // not a __SwiftNativeNSError - but statically typed as ErrorType
          // return true here
          return true;
        }
      }
    }
  }

  if (GetObjCRuntime()) {
    // this is a swift native error but it can be bridged to ObjC
    // so it needs to be layout compatible

    size_t ptr_size = m_process->GetAddressByteSize();
    size_t metadata_offset =
        ptr_size + 4 + (ptr_size == 8 ? 4 : 0);        // CFRuntimeBase
    metadata_offset += ptr_size + ptr_size + ptr_size; // CFIndex + 2*CFRef

    metadata_location += metadata_offset;
    Status error;
    lldb::addr_t metadata_ptr_value =
        m_process->ReadPointerFromMemory(metadata_location, error);
    if (metadata_ptr_value == 0 || metadata_ptr_value == LLDB_INVALID_ADDRESS ||
        error.Fail())
      return false;
  } else {
    // this is a swift native error and it has no way to be bridged to ObjC
    // so it adopts a more compact layout

    Status error;

    size_t ptr_size = m_process->GetAddressByteSize();
    size_t metadata_offset = 2 * ptr_size;
    metadata_location += metadata_offset;
    lldb::addr_t metadata_ptr_value =
        m_process->ReadPointerFromMemory(metadata_location, error);
    if (metadata_ptr_value == 0 || metadata_ptr_value == LLDB_INVALID_ADDRESS ||
        error.Fail())
      return false;
  }

  return true;
}

bool SwiftLanguageRuntime::GetDynamicTypeAndAddress_Protocol(
    ValueObject &in_value, CompilerType protocol_type,
    SwiftASTContext &scratch_ctx,
    lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name,
    Address &address) {
  Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

  auto &target = m_process->GetTarget();
  assert(IsScratchContextLocked(target) &&
          "Swift scratch context not locked ahead");
  auto &remote_ast = GetRemoteASTContext(scratch_ctx);

  lldb::addr_t existential_address;
  bool use_local_buffer = false;

  if (in_value.GetValueType() == eValueTypeConstResult &&
      in_value.GetValue().GetValueType() ==
          lldb_private::Value::eValueTypeHostAddress) {
    if (log)
      log->Printf("existential value is a const result");

    // We have a locally materialized value that is a host address;
    // register it with MemoryReader so it does not treat it as a load
    // address.  Note that this assumes that any address at that host
    // address is also a load address. If this assumption breaks there
    // will be a crash in readBytes().
    existential_address = in_value.GetValue().GetScalar().ULongLong();
    use_local_buffer = true;
  } else {
    existential_address = in_value.GetAddressOf();
  }

  if (log)
    log->Printf("existential address is %llu", existential_address);

  if (!existential_address || existential_address == LLDB_INVALID_ADDRESS)
    return false;

  if (use_local_buffer)
    PushLocalBuffer(existential_address, in_value.GetByteSize());

  swift::remote::RemoteAddress remote_existential(existential_address);
  auto result = remote_ast.getDynamicTypeAndAddressForExistential(
      remote_existential, GetSwiftType(protocol_type));

  if (use_local_buffer)
    PopLocalBuffer();

  if (!result.isSuccess()) {
    if (log)
      log->Printf("RemoteAST failed to get dynamic type of existential");
    return false;
  }

  auto type_and_address = result.getValue();
  class_type_or_name.SetCompilerType(type_and_address.InstanceType);
  address.SetRawAddress(type_and_address.PayloadAddress.getAddressData());
  return true;
}

SwiftLanguageRuntime::MetadataPromiseSP
SwiftLanguageRuntime::GetPromiseForTypeNameAndFrame(const char *type_name,
                                                    StackFrame *frame) {
  if (!frame || !type_name || !type_name[0])
    return nullptr;

  StreamString type_metadata_ptr_var_name;
  type_metadata_ptr_var_name.Printf("$%s", type_name);
  VariableList *var_list = frame->GetVariableList(false);
  if (!var_list)
    return nullptr;

  VariableSP var_sp(var_list->FindVariable(
      ConstString(type_metadata_ptr_var_name.GetData())));
  if (!var_sp)
    return nullptr;

  ValueObjectSP metadata_ptr_var_sp(
      frame->GetValueObjectForFrameVariable(var_sp, lldb::eNoDynamicValues));
  if (!metadata_ptr_var_sp ||
      metadata_ptr_var_sp->UpdateValueIfNeeded() == false)
    return nullptr;

  lldb::addr_t metadata_location(metadata_ptr_var_sp->GetValueAsUnsigned(0));
  if (metadata_location == 0 || metadata_location == LLDB_INVALID_ADDRESS)
    return nullptr;

  return GetMetadataPromise(metadata_location, *metadata_ptr_var_sp);
}

CompilerType
SwiftLanguageRuntime::DoArchetypeBindingForType(StackFrame &stack_frame,
                                                CompilerType base_type) {
  auto sc = stack_frame.GetSymbolContext(lldb::eSymbolContextEverything);
  Status error;
  // A failing Clang import in a module context permanently damages
  // that module context.  Binding archetypes can trigger an import of
  // another module, so switch to a scratch context where such an
  // operation is safe.
  auto &target = m_process->GetTarget();
  assert(IsScratchContextLocked(target) &&
         "Swift scratch context not locked ahead of archetype binding");
  auto scratch_ctx = target.GetScratchSwiftASTContext(error, stack_frame);
  if (!scratch_ctx)
    return base_type;
  base_type = scratch_ctx->ImportType(base_type, error);

  if (base_type.GetTypeInfo() & lldb::eTypeIsSwift) {
    swift::Type target_swift_type(GetSwiftType(base_type));
    if (target_swift_type->hasArchetype())
      target_swift_type = target_swift_type->mapTypeOutOfContext().getPointer();

    // FIXME: This is wrong, but it doesn't actually matter right now since
    // all conformances are always visible
    auto *module_decl = scratch_ctx->GetASTContext()->getStdlibModule();

    // Replace opaque types with their underlying types when possible.
    swift::Mangle::ASTMangler mangler(true);

    while (target_swift_type->hasOpaqueArchetype()) {
      auto old_type = target_swift_type;
      target_swift_type = target_swift_type.subst(
        [&](swift::SubstitutableType *type) -> swift::Type {
          auto opaque_type =
                          llvm::dyn_cast<swift::OpaqueTypeArchetypeType>(type);
          if (!opaque_type)
            return type;
          
          // Try to find the symbol for the opaque type descriptor in the
          // process.
          auto mangled_name = ConstString(
                    mangler.mangleOpaqueTypeDescriptor(opaque_type->getDecl()));
          
          SymbolContextList found;
          target.GetImages().FindSymbolsWithNameAndType(mangled_name,
                                                        eSymbolTypeData, found);
          
          if (found.GetSize() == 0)
            return type;
          
          swift::Type result_type;
          
          for (unsigned i = 0, e = found.GetSize(); i < e; ++i) {
            SymbolContext found_sc;
            if (!found.GetContextAtIndex(i, found_sc))
              continue;
            
            // See if the symbol has an address.
            if (!found_sc.symbol)
              continue;

            auto addr = found_sc.symbol->GetAddress()
              .GetLoadAddress(&target);
            if (!addr || addr == LLDB_INVALID_ADDRESS)
              continue;

            // Ask RemoteAST to get the underlying type out of the descriptor.
            auto &remote_ast = GetRemoteASTContext(*scratch_ctx);
            auto underlying_type_result =
            remote_ast.getUnderlyingTypeForOpaqueType(
                                            swift::remote::RemoteAddress(addr),
                                            opaque_type->getSubstitutions(),
                                            opaque_type->getOrdinal());
            
            if (!underlying_type_result)
              continue;

            // If we haven't yet gotten an underlying type, use this as our
            // possible result.
            if (!result_type) {
              result_type = underlying_type_result.getValue();
            }
            // If we have two possibilities, they should match.
            else if (!result_type->isEqual(underlying_type_result.getValue())) {
              return type;
            }
          }
          
          if (!result_type)
            return type;
          
          return result_type;
        },
        swift::LookUpConformanceInModule(module_decl),
        swift::SubstFlags::DesugarMemberTypes
          | swift::SubstFlags::SubstituteOpaqueArchetypes);
      
      // Stop if we've reached a fixpoint where we can't further resolve opaque
      // types.
      if (old_type->isEqual(target_swift_type))
        break;
    }
    
    target_swift_type = target_swift_type.subst(
        [this, &stack_frame,
         &scratch_ctx](swift::SubstitutableType *type) -> swift::Type {
          StreamString type_name;
          if (!GetAbstractTypeName(type_name, type))
            return type;
          CompilerType concrete_type = this->GetConcreteType(
              &stack_frame, ConstString(type_name.GetString()));
          Status import_error;
          CompilerType target_concrete_type =
              scratch_ctx->ImportType(concrete_type, import_error);

          if (target_concrete_type.IsValid())
            return swift::Type(GetSwiftType(target_concrete_type));

          return type;
        },
        swift::LookUpConformanceInModule(module_decl),
        swift::SubstFlags::DesugarMemberTypes);
    assert(target_swift_type);

    return {target_swift_type.getPointer()};
  }
  return base_type;
}

bool SwiftLanguageRuntime::GetAbstractTypeName(StreamString &name,
                                               swift::Type swift_type) {
  auto *generic_type_param = swift_type->getAs<swift::GenericTypeParamType>();
  if (!generic_type_param)
    return false;

  name.Printf(u8"\u03C4_%d_%d", generic_type_param->getDepth(),
              generic_type_param->getIndex());
  return true;
}

bool SwiftLanguageRuntime::GetDynamicTypeAndAddress_Value(
    ValueObject &in_value, CompilerType &bound_type,
    lldb::DynamicValueType use_dynamic, TypeAndOrName &class_type_or_name,
    Address &address) {
  class_type_or_name.SetCompilerType(bound_type);

  llvm::Optional<uint64_t> size = bound_type.GetByteSize(
      in_value.GetExecutionContextRef().GetFrameSP().get());
  if (!size)
    return false;
  lldb::addr_t val_address = in_value.GetAddressOf(true, nullptr);
  if (*size && (!val_address || val_address == LLDB_INVALID_ADDRESS))
    return false;

  address.SetLoadAddress(val_address, in_value.GetTargetSP().get());
  return true;
}

bool SwiftLanguageRuntime::GetDynamicTypeAndAddress_IndirectEnumCase(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &address) {
  static ConstString g_offset("offset");

  DataExtractor data;
  Status error;
  if (!(in_value.GetParent() && in_value.GetParent()->GetData(data, error) &&
        error.Success()))
    return false;

  bool has_payload;
  bool is_indirect;
  CompilerType payload_type;
  if (!SwiftASTContext::GetSelectedEnumCase(
          in_value.GetParent()->GetCompilerType(), data, nullptr, &has_payload,
          &payload_type, &is_indirect))
    return false;

  if (has_payload && is_indirect && payload_type)
    class_type_or_name.SetCompilerType(payload_type);

  lldb::addr_t box_addr = in_value.GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
  if (box_addr == LLDB_INVALID_ADDRESS)
    return false;

  box_addr = MaskMaybeBridgedPointer(box_addr);
  lldb::addr_t box_location = m_process->ReadPointerFromMemory(box_addr, error);
  if (box_location == LLDB_INVALID_ADDRESS)
    return false;

  box_location = MaskMaybeBridgedPointer(box_location);
  ProcessStructReader reader(m_process, box_location, GetBoxMetadataType());
  uint32_t offset = reader.GetField<uint32_t>(g_offset);
  lldb::addr_t box_value = box_addr + offset;

  // try to read one byte at the box value
  m_process->ReadUnsignedIntegerFromMemory(box_value, 1, 0, error);
  if (error.Fail()) // and if that fails, then we're off in no man's land
    return false;

  Flags type_info(payload_type.GetTypeInfo());
  if (type_info.AllSet(eTypeIsSwift | eTypeIsClass)) {
    lldb::addr_t old_box_value = box_value;
    box_value = m_process->ReadPointerFromMemory(box_value, error);
    if (box_value == LLDB_INVALID_ADDRESS)
      return false;

    DataExtractor data(&box_value, m_process->GetAddressByteSize(),
                       m_process->GetByteOrder(),
                       m_process->GetAddressByteSize());
    ValueObjectSP valobj_sp(ValueObject::CreateValueObjectFromData(
        "_", data, *m_process, payload_type));
    if (!valobj_sp)
      return false;

    Value::ValueType value_type;
    if (!GetDynamicTypeAndAddress(*valobj_sp, use_dynamic, class_type_or_name,
                                  address, value_type))
      return false;

    address.SetRawAddress(old_box_value);
    return true;
  } else if (type_info.AllSet(eTypeIsSwift | eTypeIsProtocol)) {
    SwiftASTContext::ProtocolInfo protocol_info;
    if (!SwiftASTContext::GetProtocolTypeInfo(payload_type, protocol_info))
      return false;
    auto ptr_size = m_process->GetAddressByteSize();
    std::vector<uint8_t> buffer(ptr_size * protocol_info.m_num_storage_words,
                                0);
    for (uint32_t idx = 0; idx < protocol_info.m_num_storage_words; idx++) {
      lldb::addr_t word = m_process->ReadUnsignedIntegerFromMemory(
          box_value + idx * ptr_size, ptr_size, 0, error);
      if (error.Fail())
        return false;
      memcpy(&buffer[idx * ptr_size], &word, ptr_size);
    }
    DataExtractor data(&buffer[0], buffer.size(), m_process->GetByteOrder(),
                       m_process->GetAddressByteSize());
    ValueObjectSP valobj_sp(ValueObject::CreateValueObjectFromData(
        "_", data, *m_process, payload_type));
    if (!valobj_sp)
      return false;

    Value::ValueType value_type;
    if (!GetDynamicTypeAndAddress(*valobj_sp, use_dynamic, class_type_or_name,
                                  address, value_type))
      return false;

    address.SetRawAddress(box_value);
    return true;
  } else {
    // This is most likely a statically known type.
    address.SetLoadAddress(box_value, &m_process->GetTarget());
    return true;
  }
}

// Dynamic type resolution tends to want to generate scalar data - but there are
// caveats
// Per original comment here
// "Our address is the location of the dynamic type stored in memory.  It isn't
// a load address,
//  because we aren't pointing to the LOCATION that stores the pointer to us,
//  we're pointing to us..."
// See inlined comments for exceptions to this general rule.
Value::ValueType SwiftLanguageRuntime::GetValueType(
    Value::ValueType static_value_type, const CompilerType &static_type,
    const CompilerType &dynamic_type, bool is_indirect_enum_case) {
  Flags static_type_flags(static_type.GetTypeInfo());
  Flags dynamic_type_flags(dynamic_type.GetTypeInfo());

  if (dynamic_type_flags.AllSet(eTypeIsSwift)) {
    // for a protocol object where does the dynamic data live if the target
    // object is a struct? (for a class, it's easy)
    if (static_type_flags.AllSet(eTypeIsSwift | eTypeIsProtocol) &&
        dynamic_type_flags.AnySet(eTypeIsStructUnion | eTypeIsEnumeration)) {
      SwiftASTContext *swift_ast_ctx =
          llvm::dyn_cast_or_null<SwiftASTContext>(static_type.GetTypeSystem());

      if (swift_ast_ctx && swift_ast_ctx->IsErrorType(static_type)) {
        // ErrorType values are always a pointer
        return Value::eValueTypeLoadAddress;
      }

      switch (SwiftASTContext::GetAllocationStrategy(dynamic_type)) {
      case SwiftASTContext::TypeAllocationStrategy::eDynamic:
      case SwiftASTContext::TypeAllocationStrategy::eUnknown:
        break;
      case SwiftASTContext::TypeAllocationStrategy::eInline: // inline data;
                                                             // same as the
                                                             // static data
        return static_value_type;
      case SwiftASTContext::TypeAllocationStrategy::ePointer: // pointed-to; in
                                                              // the target
        return Value::eValueTypeLoadAddress;
      }
    }
    if (static_type_flags.AllSet(eTypeIsSwift | eTypeIsGenericTypeParam)) {
      // if I am handling a non-pointer Swift type obtained from an archetype,
      // then the runtime vends the location
      // of the object, not the object per se (since the object is not a pointer
      // itself, this is way easier to achieve)
      // hence, it's a load address, not a scalar containing a pointer as for
      // ObjC classes
      if (dynamic_type_flags.AllClear(eTypeIsPointer | eTypeIsReference |
                                      eTypeInstanceIsPointer))
        return Value::eValueTypeLoadAddress;
    }

    if (static_type_flags.AllSet(eTypeIsSwift | eTypeIsPointer) &&
        static_type_flags.AllClear(eTypeIsGenericTypeParam)) {
      // FIXME: This branch is not covered by any testcases in the test suite.
      if (is_indirect_enum_case || static_type_flags.AllClear(eTypeIsBuiltIn))
        return Value::eValueTypeLoadAddress;
    }
  }

  // Enabling this makes the inout_variables test hang.
  //  return Value::eValueTypeScalar;
  if (static_type_flags.AllSet(eTypeIsSwift) &&
      dynamic_type_flags.AllSet(eTypeIsSwift) &&
      dynamic_type_flags.AllClear(eTypeIsPointer | eTypeInstanceIsPointer))
    return static_value_type;
  else
    return Value::eValueTypeScalar;
}

bool SwiftLanguageRuntime::GetDynamicTypeAndAddress_ClangType(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &address,
    Value::ValueType &value_type) {
  AppleObjCRuntime *objc_runtime = GetObjCRuntime();
  if (!objc_runtime)
    return false;

  // This is a Clang type, which means it must have been an
  // Objective-C protocol. Protocols are not represented in DWARF and
  // LLDB's ObjC runtime implementation doesn't know how to deal with
  // them either.  Use the Objective-C runtime to perform dynamic type
  // resolution first, and then map the dynamic Objective-C type back
  // into Swift.
  TypeAndOrName dyn_class_type_or_name = class_type_or_name;
  if (!objc_runtime->GetDynamicTypeAndAddress(
          in_value, use_dynamic, dyn_class_type_or_name, address, value_type))
    return false;

  StringRef dyn_name = dyn_class_type_or_name.GetName().GetStringRef();
  // If this is an Objective-C runtime value, skip; this is handled elsewhere.
  if (swift::Demangle::isOldFunctionTypeMangling(dyn_name) ||
      dyn_name.startswith("__NS"))
    return false;

  std::string remangled;
  {
    // Create a mangle tree for __C.dyn_name?.
    using namespace swift::Demangle;
    NodeFactory factory;
    NodePointer global = factory.createNode(Node::Kind::Global);
    NodePointer tm = factory.createNode(Node::Kind::TypeMangling);
    global->addChild(tm, factory);
    NodePointer bge = factory.createNode(Node::Kind::BoundGenericEnum);
    tm->addChild(bge, factory);
    NodePointer ety = factory.createNode(Node::Kind::Type);
    bge->addChild(ety, factory);
    NodePointer e = factory.createNode(Node::Kind::Enum);
    e->addChild(factory.createNode(Node::Kind::Module, "Swift"), factory);
    e->addChild(factory.createNode(Node::Kind::Identifier, "Optional"), factory);
    ety->addChild(e, factory);
    NodePointer list = factory.createNode(Node::Kind::TypeList);
    bge->addChild(list, factory);
    NodePointer cty = factory.createNode(Node::Kind::Type);
    list->addChild(cty, factory);
    NodePointer c = factory.createNode(Node::Kind::Class);
    c->addChild(factory.createNode(Node::Kind::Module, "__C"), factory);
    c->addChild(factory.createNode(Node::Kind::Identifier, dyn_name), factory);
    cty->addChild(c, factory);

    remangled = mangleNode(global);
  }

  // Import the remangled dynamic name into the scratch context.
  Status error;
  assert(IsScratchContextLocked(in_value.GetTargetSP()) &&
         "Swift scratch context not locked ahead of dynamic type resolution");
  auto scratch_ctx = in_value.GetScratchSwiftASTContext();
  if (!scratch_ctx)
    return false;
  CompilerType swift_type =
      scratch_ctx->GetTypeFromMangledTypename(ConstString(remangled), error);

  // Roll back the ObjC dynamic type resolution.
  if (!swift_type)
    return false;
  class_type_or_name = dyn_class_type_or_name;
  class_type_or_name.SetCompilerType(swift_type);
  value_type = GetValueType(in_value.GetValue().GetValueType(),
                            in_value.GetCompilerType(),
                            class_type_or_name.GetCompilerType(), false);
  return true;
}

static bool IsIndirectEnumCase(ValueObject &valobj) {
  return (valobj.GetLanguageFlags() &
          SwiftASTContext::LanguageFlags::eIsIndirectEnumCase) ==
         SwiftASTContext::LanguageFlags::eIsIndirectEnumCase;
}

bool SwiftLanguageRuntime::GetDynamicTypeAndAddress(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &address,
    Value::ValueType &value_type) {
  class_type_or_name.Clear();
  if (use_dynamic == lldb::eNoDynamicValues)
    return false;

  // Try to import a Clang type into Swift.
  if (in_value.GetObjectRuntimeLanguage() == eLanguageTypeObjC)
    return GetDynamicTypeAndAddress_ClangType(
        in_value, use_dynamic, class_type_or_name, address, value_type);

  if (!CouldHaveDynamicValue(in_value))
    return false;

  // Dynamic type resolution in RemoteAST might pull in other Swift modules, so
  // use the scratch context where such operations are legal and safe.
  assert(IsScratchContextLocked(in_value.GetTargetSP()) &&
         "Swift scratch context not locked ahead of dynamic type resolution");
  auto scratch_ctx = in_value.GetScratchSwiftASTContext();
  if (!scratch_ctx)
    return false;

  auto retry_once = [&]() {
    // Retry exactly once using the per-module fallback scratch context.
    auto &target = m_process->GetTarget();
    if (!target.UseScratchTypesystemPerModule()) {
      Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
      if (log)
        log->Printf("Dynamic type resolution detected fatal errors in "
                    "shared Swift state. Falling back to per-module "
                    "scratch context.\n");
      target.SetUseScratchTypesystemPerModule(true);
      return GetDynamicTypeAndAddress(in_value, use_dynamic, class_type_or_name,
                                      address, value_type);
    }
    return false;
  };

  if (scratch_ctx->HasFatalErrors())
    return retry_once();

  // Import the type into the scratch context. Any form of dynamic
  // type resolution may trigger a cross-module import.
  CompilerType val_type(in_value.GetCompilerType());
  Flags type_info(val_type.GetTypeInfo());
  if (!type_info.AnySet(eTypeIsSwift))
    return false;

  bool success = false;
  bool is_indirect_enum_case = IsIndirectEnumCase(in_value);
  // Type kinds with metadata don't need archetype binding.
  if (is_indirect_enum_case)
    // ..._IndirectEnumCase() recurses, no need to bind archetypes.
    success = GetDynamicTypeAndAddress_IndirectEnumCase(
        in_value, use_dynamic, class_type_or_name, address);
  else if (type_info.AnySet(eTypeIsClass) ||
           type_info.AllSet(eTypeIsBuiltIn | eTypeIsPointer | eTypeHasValue))
    success = GetDynamicTypeAndAddress_Class(
        in_value, *scratch_ctx, use_dynamic, class_type_or_name, address);
  else if (type_info.AnySet(eTypeIsProtocol))
    success = GetDynamicTypeAndAddress_Protocol(
        in_value, val_type, *scratch_ctx, use_dynamic,
        class_type_or_name, address);
  else {
    // Perform archetype binding in the scratch context.
    auto *frame = in_value.GetExecutionContextRef().GetFrameSP().get();
    if (!frame)
      return false;

    CompilerType bound_type = DoArchetypeBindingForType(*frame, val_type);
    if (!bound_type)
      return false;

    Flags subst_type_info(bound_type.GetTypeInfo());
    if (subst_type_info.AnySet(eTypeIsClass)) {
      success = GetDynamicTypeAndAddress_Class(in_value, *scratch_ctx, use_dynamic,
                                               class_type_or_name, address);
    } else if (subst_type_info.AnySet(eTypeIsProtocol)) {
      success = GetDynamicTypeAndAddress_Protocol(in_value, bound_type,
                                                  *scratch_ctx, use_dynamic,
                                                  class_type_or_name, address);
    } else {
      success = GetDynamicTypeAndAddress_Value(in_value, bound_type,
                                               use_dynamic, class_type_or_name,
                                               address);
    }
  }

  if (success)
    value_type = GetValueType(
        in_value.GetValue().GetValueType(), in_value.GetCompilerType(),
        class_type_or_name.GetCompilerType(), is_indirect_enum_case);
  else if (scratch_ctx->HasFatalErrors())
    return retry_once();
  return success;
}

TypeAndOrName
SwiftLanguageRuntime::FixUpDynamicType(const TypeAndOrName &type_and_or_name,
                                       ValueObject &static_value) {
  CompilerType static_type = static_value.GetCompilerType();
  CompilerType dynamic_type = type_and_or_name.GetCompilerType();
  // The logic in this function only applies to static/dynamic Swift types.
  if (llvm::isa<ClangASTContext>(static_type.GetTypeSystem()))
    return type_and_or_name;

  bool should_be_made_into_ref = false;
  bool should_be_made_into_ptr = false;
  Flags type_flags = static_type.GetTypeInfo();
  Flags type_andor_name_flags = dynamic_type.GetTypeInfo();

  // if the static type is a pointer or reference, so should the
  // dynamic type caveat: if the static type is a Swift class
  // instance, the dynamic type could either be a Swift type (no need
  // to change anything), or an ObjC type in which case it needs to be
  // made into a pointer
  if (type_flags.AnySet(eTypeIsPointer))
    should_be_made_into_ptr =
        (type_flags.AllClear(eTypeIsGenericTypeParam | eTypeIsBuiltIn) &&
         !IsIndirectEnumCase(static_value));
  else if (type_flags.AnySet(eTypeInstanceIsPointer))
    should_be_made_into_ptr = !type_andor_name_flags.AllSet(eTypeIsSwift);
  else if (type_flags.AnySet(eTypeIsReference))
    should_be_made_into_ref = true;
  else if (type_flags.AllSet(eTypeIsSwift | eTypeIsProtocol))
    should_be_made_into_ptr =
        dynamic_type.IsRuntimeGeneratedType() &&
        !dynamic_type.IsPointerType();

  if (type_and_or_name.HasType()) {
    // The type will always be the type of the dynamic object.  If our
    // parent's type was a pointer, then our type should be a pointer
    // to the type of the dynamic object.  If a reference, then the
    // original type should be okay...
    CompilerType corrected_type = dynamic_type;
    if (should_be_made_into_ptr)
      corrected_type = dynamic_type.GetPointerType();
    else if (should_be_made_into_ref)
      corrected_type = dynamic_type.GetLValueReferenceType();
    TypeAndOrName result = type_and_or_name;
    result.SetCompilerType(corrected_type);
    return result;
  }
  return type_and_or_name;
}

bool SwiftLanguageRuntime::IsTaggedPointer(lldb::addr_t addr,
                                           CompilerType type) {
  swift::CanType swift_can_type = GetCanonicalSwiftType(type);
  switch (swift_can_type->getKind()) {
  case swift::TypeKind::UnownedStorage: {
    Target &target = m_process->GetTarget();
    llvm::Triple triple = target.GetArchitecture().GetTriple();
    // On Darwin the Swift runtime stores unowned references to
    // Objective-C objects as a pointer to a struct that has the
    // actual object pointer at offset zero. The least significant bit
    // of the reference pointer indicates whether the reference refers
    // to an Objective-C or Swift object.
    //
    // This is a property of the Swift runtime(!). In the future it
    // may be necessary to check for the version of the Swift runtime
    // (or indirectly by looking at the version of the remote
    // operating system) to determine how to interpret references.
    if (triple.isOSDarwin())
      // Check whether this is a reference to an Objective-C object.
      if ((addr & 1) == 1)
        return true;
  }
  default:
    break;
  }
  return false;
}

std::pair<lldb::addr_t, bool>
SwiftLanguageRuntime::FixupPointerValue(lldb::addr_t addr, CompilerType type) {
  // Check for an unowned Darwin Objective-C reference.
  if (IsTaggedPointer(addr, type)) {
    // Clear the discriminator bit to get at the pointer to Objective-C object.
    bool needs_deref = true;
    return {addr & ~1ULL, needs_deref};
  }

  // Adjust the pointer to strip away the spare bits.
  Target &target = m_process->GetTarget();
  llvm::Triple triple = target.GetArchitecture().GetTriple();
  switch (triple.getArch()) {
  case llvm::Triple::ArchType::aarch64:
    return {addr & ~SWIFT_ABI_ARM64_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::arm:
    return {addr & ~SWIFT_ABI_ARM_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::x86:
    return {addr & ~SWIFT_ABI_I386_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::x86_64:
    return {addr & ~SWIFT_ABI_X86_64_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::systemz:
    return {addr & ~SWIFT_ABI_S390X_SWIFT_SPARE_BITS_MASK, false};
  case llvm::Triple::ArchType::ppc64le:
    return { addr & ~SWIFT_ABI_POWERPC64_SWIFT_SPARE_BITS_MASK, false};
  default:
    break;
  }
  return {addr, false};
}

/// This allows a language runtime to adjust references depending on the type.
lldb::addr_t SwiftLanguageRuntime::FixupAddress(lldb::addr_t addr,
                                                CompilerType type,
                                                Status &error) {
  swift::CanType swift_can_type = GetCanonicalSwiftType(type);
  switch (swift_can_type->getKind()) {
  case swift::TypeKind::UnownedStorage: {
    // Peek into the reference to see whether it needs an extra deref.
    // If yes, return the fixed-up address we just read.
    Target &target = m_process->GetTarget();
    size_t ptr_size = m_process->GetAddressByteSize();
    lldb::addr_t refd_addr = LLDB_INVALID_ADDRESS;
    target.ReadMemory(addr, false, &refd_addr, ptr_size, error);
    if (error.Success()) {
      bool extra_deref;
      std::tie(refd_addr, extra_deref) = FixupPointerValue(refd_addr, type);
      if (extra_deref)
        return refd_addr;
    }
  }
  default:
    break;
  }
  return addr;
}

const swift::reflection::TypeInfo *
SwiftLanguageRuntime::GetTypeInfo(CompilerType type) {
  swift::CanType swift_can_type(GetCanonicalSwiftType(type));
  CompilerType can_type(swift_can_type);
  ConstString mangled_name(can_type.GetMangledTypeName());
  StringRef mangled_no_prefix =
      swift::Demangle::dropSwiftManglingPrefix(mangled_name.GetStringRef());
  swift::Demangle::Demangler Dem;
  auto demangled = Dem.demangleType(mangled_no_prefix);
  auto *type_ref = swift::Demangle::decodeMangledType(
      reflection_ctx->getBuilder(), demangled);
  if (!type_ref)
    return nullptr;
  return reflection_ctx->getBuilder().getTypeConverter().getTypeInfo(type_ref);
}

bool SwiftLanguageRuntime::IsStoredInlineInBuffer(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->isBitwiseTakable() && type_info->getSize() <= 24;
  return true;
}

llvm::Optional<uint64_t> SwiftLanguageRuntime::GetBitSize(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->getSize() * 8;
  return {};
}

llvm::Optional<uint64_t> SwiftLanguageRuntime::GetByteStride(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->getStride();
  return {};
}

llvm::Optional<size_t> SwiftLanguageRuntime::GetBitAlignment(CompilerType type) {
  if (auto *type_info = GetTypeInfo(type))
    return type_info->getAlignment();
  return {};
}

bool SwiftLanguageRuntime::IsWhitelistedRuntimeValue(ConstString name) {
  return name == g_self;
}

bool SwiftLanguageRuntime::CouldHaveDynamicValue(ValueObject &in_value) {
  if (IsIndirectEnumCase(in_value))
    return true;
  CompilerType var_type(in_value.GetCompilerType());
  Flags var_type_flags(var_type.GetTypeInfo());
  if (var_type_flags.AllSet(eTypeIsSwift | eTypeInstanceIsPointer)) {
    // Swift class instances are actually pointers, but base class instances
    // are inlined at offset 0 in the class data. If we just let base classes
    // be dynamic, it would cause an infinite recursion. So we would usually
    // disable it.
    return !in_value.IsBaseClass();
  }
  return var_type.IsPossibleDynamicType(nullptr, false, false, true);
}

CompilerType
SwiftLanguageRuntime::GetConcreteType(ExecutionContextScope *exe_scope,
                                      ConstString abstract_type_name) {
  if (!exe_scope)
    return CompilerType();

  StackFrame *frame(exe_scope->CalculateStackFrame().get());
  if (!frame)
    return CompilerType();

  MetadataPromiseSP promise_sp(
      GetPromiseForTypeNameAndFrame(abstract_type_name.GetCString(), frame));
  if (!promise_sp)
    return CompilerType();

  return promise_sp->FulfillTypePromise();
}

namespace {

enum class ThunkKind
{
  Unknown = 0,
  AllocatingInit,
  PartialApply,
  ObjCAttribute,
  Reabstraction,
  ProtocolConformance,
};

enum class ThunkAction
{
  Unknown = 0,
  GetThunkTarget,
  StepIntoConformance,
  StepThrough
};

}

static ThunkKind
GetThunkKind(llvm::StringRef symbol_name)
{
  swift::Demangle::Node::Kind kind;
  swift::Demangle::Context demangle_ctx;
  if (!demangle_ctx.isThunkSymbol(symbol_name))
    return ThunkKind::Unknown;

  swift::Demangle::NodePointer nodes = demangle_ctx.demangleSymbolAsNode(symbol_name);
  size_t num_global_children = nodes->getNumChildren();
  if (num_global_children == 0)
    return ThunkKind::Unknown;

  if (nodes->getKind() != swift::Demangle::Node::Kind::Global)
    return ThunkKind::Unknown;
  if (nodes->getNumChildren() == 0)
    return ThunkKind::Unknown;

  swift::Demangle::NodePointer node_ptr = nodes->getFirstChild();
  kind = node_ptr->getKind();
  switch (kind)
  {
  case swift::Demangle::Node::Kind::ObjCAttribute:
        return ThunkKind::ObjCAttribute;
    break;
  case swift::Demangle::Node::Kind::ProtocolWitness:
    if (node_ptr->getNumChildren() == 0)
      return ThunkKind::Unknown;
    if (node_ptr->getFirstChild()->getKind() 
           == swift::Demangle::Node::Kind::ProtocolConformance)
      return ThunkKind::ProtocolConformance;
    break;
  case swift::Demangle::Node::Kind::ReabstractionThunkHelper:
    return ThunkKind::Reabstraction;
  case swift::Demangle::Node::Kind::PartialApplyForwarder:
    return ThunkKind::PartialApply;
  case swift::Demangle::Node::Kind::Allocator:
    if (node_ptr->getNumChildren() == 0)
      return ThunkKind::Unknown;
    if (node_ptr->getFirstChild()->getKind() 
           == swift::Demangle::Node::Kind::Class)
      return ThunkKind::AllocatingInit;
    break;
  default:
    break;
  }

  return ThunkKind::Unknown;
}
static const char *GetThunkKindName (ThunkKind kind)
{
  switch (kind)
  {
    case ThunkKind::Unknown:
      return "Unknown";
    case ThunkKind::AllocatingInit:
      return "StepThrough";
    case ThunkKind::PartialApply:
      return "GetThunkTarget";
    case ThunkKind::ObjCAttribute:
      return "GetThunkTarget";
    case ThunkKind::Reabstraction:
      return "GetThunkTarget";
    case ThunkKind::ProtocolConformance:
      return "StepIntoConformance";
  }
}

static ThunkAction
GetThunkAction (ThunkKind kind)
{
    switch (kind)
    {
      case ThunkKind::Unknown:
        return ThunkAction::Unknown;
      case ThunkKind::AllocatingInit:
        return ThunkAction::StepThrough;
      case ThunkKind::PartialApply:
        return ThunkAction::GetThunkTarget;
      case ThunkKind::ObjCAttribute:
        return ThunkAction::GetThunkTarget;
      case ThunkKind::Reabstraction:
        return ThunkAction::StepThrough;
      case ThunkKind::ProtocolConformance:
        return ThunkAction::StepIntoConformance;
    }
}

bool SwiftLanguageRuntime::GetTargetOfPartialApply(SymbolContext &curr_sc,
                                                   ConstString &apply_name,
                                                   SymbolContext &sc) {
  if (!curr_sc.module_sp)
    return false;
    
  SymbolContextList sc_list;
  swift::Demangle::Context demangle_ctx;
  // Make sure this is a partial apply:
  
  std::string apply_target = demangle_ctx.getThunkTarget(apply_name.GetStringRef());
  if (!apply_target.empty()) {
    size_t num_symbols = curr_sc.module_sp->FindFunctions(
        ConstString(apply_target), NULL, eFunctionNameTypeFull, true, false, false, sc_list);
    if (num_symbols == 0)
      return false;
      
    CompileUnit *curr_cu = curr_sc.comp_unit;

    size_t num_found = 0;
    for (size_t i = 0; i < num_symbols; i++) {
      SymbolContext tmp_sc;
      if (sc_list.GetContextAtIndex(i, tmp_sc)) {
        if (tmp_sc.comp_unit && curr_cu && tmp_sc.comp_unit == curr_cu) {
          sc = tmp_sc;
          num_found++;
        } else if (curr_sc.module_sp == tmp_sc.module_sp) {
          sc = tmp_sc;
          num_found++;
        }
      }
    }
    if (num_found == 1)
      return true;
    else {
      sc.Clear(false);
      return false;
    }
  } else {
    return false;
  }
}

bool SwiftLanguageRuntime::IsSymbolARuntimeThunk(const Symbol &symbol) {

  llvm::StringRef symbol_name = symbol.GetMangled().GetMangledName().GetStringRef();
  if (symbol_name.empty())
    return false;

  swift::Demangle::Context demangle_ctx;
  return demangle_ctx.isThunkSymbol(symbol_name);
}

lldb::ThreadPlanSP
SwiftLanguageRuntime::GetStepThroughTrampolinePlan(Thread &thread,
                                                   bool stop_others) {
  // Here are the trampolines we have at present.
  // 1) The thunks from protocol invocations to the call in the actual object
  // implementing the protocol.
  // 2) Thunks for going from Swift ObjC classes to their actual method
  // invocations
  // 3) Thunks that retain captured objects in closure invocations.

  ThreadPlanSP new_thread_plan_sp;

  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_STEP));
  StackFrameSP stack_sp = thread.GetStackFrameAtIndex(0);
  if (!stack_sp)
    return new_thread_plan_sp;

  SymbolContext sc = stack_sp->GetSymbolContext(eSymbolContextEverything);
  Symbol *symbol = sc.symbol;

  // Note, I don't really need to consult IsSymbolARuntimeThunk here, but it
  // is fast to do and
  // keeps this list and the one in IsSymbolARuntimeThunk in sync.
  if (!symbol || !IsSymbolARuntimeThunk(*symbol))
      return new_thread_plan_sp;
      
  // Only do this if you are at the beginning of the thunk function:
  lldb::addr_t cur_addr = thread.GetRegisterContext()->GetPC();
  lldb::addr_t symbol_addr = symbol->GetAddress().GetLoadAddress(
      &thread.GetProcess()->GetTarget());

  if (symbol_addr != cur_addr)
    return new_thread_plan_sp;

  Address target_address;
  ConstString symbol_mangled_name = symbol->GetMangled().GetMangledName();
  const char *symbol_name = symbol_mangled_name.AsCString();
  
  ThunkKind thunk_kind = GetThunkKind(symbol_mangled_name.GetStringRef());
  ThunkAction thunk_action = GetThunkAction(thunk_kind);
  

  switch (thunk_action)
  {
    case ThunkAction::Unknown:
      return new_thread_plan_sp;
    case ThunkAction::GetThunkTarget:
    {
      swift::Demangle::Context demangle_ctx;
      std::string thunk_target = demangle_ctx.getThunkTarget(symbol_name);
      if (thunk_target.empty())
      {
        if (log)
          log->Printf("Stepped to thunk \"%s\" (kind: %s) but could not "
                      "find the thunk target. ",
                      symbol_name,
                      GetThunkKindName(thunk_kind));
        return new_thread_plan_sp;
      }
      if (log)
        log->Printf("Stepped to thunk \"%s\" (kind: %s) stepping to target: \"%s\".",
                    symbol_name, GetThunkKindName(thunk_kind), thunk_target.c_str());

      ModuleList modules = thread.GetProcess()->GetTarget().GetImages();
      SymbolContextList sc_list;
      modules.FindFunctionSymbols(ConstString(thunk_target),
                                    eFunctionNameTypeFull, sc_list);
      if (sc_list.GetSize() == 1) {
        SymbolContext sc;
        sc_list.GetContextAtIndex(0, sc);

        if (sc.symbol)
          target_address = sc.symbol->GetAddress();
      }
    }
    break;
    case ThunkAction::StepIntoConformance:
    {
      // The TTW symbols encode the protocol conformance requirements and it
      // is possible to go to
      // the AST and get it to replay the logic that it used to determine
      // what to dispatch to.
      // But that ties us too closely to the logic of the compiler, and
      // these thunks are quite
      // simple, they just do a little retaining, and then call the correct
      // function.
      // So for simplicity's sake, I'm just going to get the base name of
      // the function
      // this protocol thunk is preparing to call, then step into through
      // the thunk, stopping if I end up
      // in a frame with that function name.

      swift::Demangle::Context demangle_ctx;
      swift::Demangle::NodePointer demangled_nodes =
          demangle_ctx.demangleSymbolAsNode(symbol_mangled_name.GetStringRef());

      // Now find the ProtocolWitness node in the demangled result.

      swift::Demangle::NodePointer witness_node = demangled_nodes;
      bool found_witness_node = false;
      while (witness_node) {
        if (witness_node->getKind() ==
            swift::Demangle::Node::Kind::ProtocolWitness) {
          found_witness_node = true;
          break;
        }
        witness_node = witness_node->getFirstChild();
      }
      if (!found_witness_node) {
        if (log)
          log->Printf("Stepped into witness thunk \"%s\" but could not "
                      "find the ProtocolWitness node in the demangled "
                      "nodes.",
                      symbol_name);
        return new_thread_plan_sp;
      }

      size_t num_children = witness_node->getNumChildren();
      if (num_children < 2) {
        if (log)
          log->Printf("Stepped into witness thunk \"%s\" but the "
                      "ProtocolWitness node doesn't have enough nodes.",
                      symbol_name);
        return new_thread_plan_sp;
      }

      swift::Demangle::NodePointer function_node =
          witness_node->getChild(1);
      if (function_node == nullptr ||
          function_node->getKind() !=
              swift::Demangle::Node::Kind::Function) {
        if (log)
          log->Printf("Stepped into witness thunk \"%s\" but could not "
                      "find the function in the ProtocolWitness node.",
                      symbol_name);
        return new_thread_plan_sp;
      }

      // Okay, now find the name of this function.
      num_children = function_node->getNumChildren();
      swift::Demangle::NodePointer name_node(nullptr);
      for (size_t i = 0; i < num_children; i++) {
        if (function_node->getChild(i)->getKind() ==
            swift::Demangle::Node::Kind::Identifier) {
          name_node = function_node->getChild(i);
          break;
        }
      }

      if (!name_node) {
        if (log)
          log->Printf("Stepped into witness thunk \"%s\" but could not "
                      "find the Function name in the function node.",
                      symbol_name);
        return new_thread_plan_sp;
      }

      std::string function_name(name_node->getText());
      if (function_name.empty()) {
        if (log)
          log->Printf("Stepped into witness thunk \"%s\" but the Function "
                      "name was empty.",
                      symbol_name);
        return new_thread_plan_sp;
      }

      // We have to get the address range of the thunk symbol, and make a
      // "step through range stepping in"
      AddressRange sym_addr_range(sc.symbol->GetAddress(),
                                  sc.symbol->GetByteSize());
      new_thread_plan_sp.reset(new ThreadPlanStepInRange(
          thread, sym_addr_range, sc, function_name.c_str(),
          eOnlyDuringStepping, eLazyBoolNo, eLazyBoolNo));
      return new_thread_plan_sp;

    }
    break;
    case ThunkAction::StepThrough:
    {
      if (log)
        log->Printf("Stepping through thunk: %s kind: %s",
                    symbol_name, GetThunkKindName(thunk_kind));
      AddressRange sym_addr_range(sc.symbol->GetAddress(),
                                  sc.symbol->GetByteSize());
      new_thread_plan_sp.reset(new ThreadPlanStepInRange(
          thread, sym_addr_range, sc, nullptr, eOnlyDuringStepping,
          eLazyBoolNo, eLazyBoolNo));
      return new_thread_plan_sp;
    }
    break;
  }
    
  if (target_address.IsValid()) {
    new_thread_plan_sp.reset(
        new ThreadPlanRunToAddress(thread, target_address, stop_others));
  }

  return new_thread_plan_sp;
}

void SwiftLanguageRuntime::FindFunctionPointersInCall(
    StackFrame &frame, std::vector<Address> &addresses, bool debug_only,
    bool resolve_thunks) {
  // Extract the mangled name from the stack frame, and realize the function
  // type in the Target's SwiftASTContext.
  // Then walk the arguments looking for function pointers.  If we find one in
  // the FIRST argument, we can fetch
  // the pointer value and return that.
  // FIXME: when we can ask swift/llvm for the location of function arguments,
  // then we can do this for all the
  // function pointer arguments we find.

  SymbolContext sc = frame.GetSymbolContext(eSymbolContextSymbol);
  if (sc.symbol) {
    Mangled mangled_name = sc.symbol->GetMangled();
    if (mangled_name.GuessLanguage() == lldb::eLanguageTypeSwift) {
      Status error;
      Target &target = frame.GetThread()->GetProcess()->GetTarget();
      ExecutionContext exe_ctx(frame);
      auto swift_ast = target.GetScratchSwiftASTContext(error, frame);
      if (swift_ast) {
        CompilerType function_type = swift_ast->GetTypeFromMangledTypename(
            mangled_name.GetMangledName(), error);
        if (error.Success()) {
          if (function_type.IsFunctionType()) {
            // FIXME: For now we only check the first argument since we don't
            // know how to find the values
            // of arguments further in the argument list.
            // int num_arguments = function_type.GetFunctionArgumentCount();
            // for (int i = 0; i < num_arguments; i++)

            for (int i = 0; i < 1; i++) {
              CompilerType argument_type =
                  function_type.GetFunctionArgumentTypeAtIndex(i);
              if (argument_type.IsFunctionPointerType()) {
                // We found a function pointer argument.  Try to track down its
                // value.  This is a hack
                // for now, we really should ask swift/llvm how to find the
                // argument(s) given the
                // Swift decl for this function, and then look those up in the
                // frame.

                ABISP abi_sp(frame.GetThread()->GetProcess()->GetABI());
                ValueList argument_values;
                Value input_value;
                CompilerType clang_void_ptr_type =
                    target.GetScratchClangASTContext()
                        ->GetBasicType(eBasicTypeVoid)
                        .GetPointerType();

                input_value.SetValueType(Value::eValueTypeScalar);
                input_value.SetCompilerType(clang_void_ptr_type);
                argument_values.PushValue(input_value);

                bool success = abi_sp->GetArgumentValues(
                    *(frame.GetThread().get()), argument_values);
                if (success) {
                  // Now get a pointer value from the zeroth argument.
                  Status error;
                  DataExtractor data;
                  ExecutionContext exe_ctx;
                  frame.CalculateExecutionContext(exe_ctx);
                  error = argument_values.GetValueAtIndex(0)->GetValueAsData(
                      &exe_ctx, data, NULL);
                  lldb::offset_t offset = 0;
                  lldb::addr_t fn_ptr_addr = data.GetPointer(&offset);
                  Address fn_ptr_address;
                  fn_ptr_address.SetLoadAddress(fn_ptr_addr, &target);
                  // Now check to see if this has debug info:
                  bool add_it = true;

                  if (resolve_thunks) {
                    SymbolContext sc;
                    fn_ptr_address.CalculateSymbolContext(
                        &sc, eSymbolContextEverything);
                    if (sc.comp_unit && sc.symbol) {
                      ConstString symbol_name =
                          sc.symbol->GetMangled().GetMangledName();
                      if (symbol_name) {
                        SymbolContext target_context;
                        if (GetTargetOfPartialApply(sc, symbol_name,
                                                    target_context)) {
                          if (target_context.symbol)
                            fn_ptr_address =
                                target_context.symbol->GetAddress();
                          else if (target_context.function)
                            fn_ptr_address =
                                target_context.function->GetAddressRange()
                                    .GetBaseAddress();
                        }
                      }
                    }
                  }

                  if (debug_only) {
                    LineEntry line_entry;
                    fn_ptr_address.CalculateSymbolContextLineEntry(line_entry);
                    if (!line_entry.IsValid())
                      add_it = false;
                  }
                  if (add_it)
                    addresses.push_back(fn_ptr_address);
                }
              }
            }
          }
        }
      }
    }
  }
}

//------------------------------------------------------------------
// Exception breakpoint Precondition class for Swift:
//------------------------------------------------------------------
void SwiftLanguageRuntime::SwiftExceptionPrecondition::AddTypeName(
    const char *class_name) {
  m_type_names.insert(class_name);
}

void SwiftLanguageRuntime::SwiftExceptionPrecondition::AddEnumSpec(
    const char *enum_name, const char *element_name) {
  std::unordered_map<std::string, std::vector<std::string>>::value_type
      new_value(enum_name, std::vector<std::string>());
  auto result = m_enum_spec.emplace(new_value);
  result.first->second.push_back(element_name);
}

SwiftLanguageRuntime::SwiftExceptionPrecondition::SwiftExceptionPrecondition() {
}

ValueObjectSP
SwiftLanguageRuntime::CalculateErrorValueObjectFromValue(
    Value &value, ConstString name, bool persistent)
{
  ValueObjectSP error_valobj_sp;
  auto type_system_or_err = m_process->GetTarget().GetScratchTypeSystemForLanguage(eLanguageTypeSwift);
  if (!type_system_or_err)
    return error_valobj_sp;

  auto *ast_context = llvm::dyn_cast_or_null<SwiftASTContext>(&*type_system_or_err);
  if (!ast_context)
    return error_valobj_sp;

  CompilerType swift_error_proto_type = ast_context->GetErrorType();
  value.SetCompilerType(swift_error_proto_type);
  
  error_valobj_sp = ValueObjectConstResult::Create(
      m_process, value, name);
  
  if (error_valobj_sp && error_valobj_sp->GetError().Success()) {
    error_valobj_sp = error_valobj_sp->GetQualifiedRepresentationIfAvailable(
        lldb::eDynamicCanRunTarget, true);
    if (!IsValidErrorValue(*(error_valobj_sp.get()))) {
      error_valobj_sp.reset();
    }
  }

  if (persistent && error_valobj_sp) {
    ExecutionContext ctx =
      error_valobj_sp->GetExecutionContextRef().Lock(false);
    auto *exe_scope = ctx.GetBestExecutionContextScope();
    if (!exe_scope)
      return error_valobj_sp;
    Target &target = m_process->GetTarget();
    auto *persistent_state =
        target.GetSwiftPersistentExpressionState(*exe_scope);

    const bool is_error = true;
    auto prefix = persistent_state->GetPersistentVariablePrefix(is_error);
    ConstString persistent_variable_name(
        persistent_state->GetNextPersistentVariableName(target, prefix));

    lldb::ValueObjectSP const_valobj_sp;

    // Check in case our value is already a constant value
    if (error_valobj_sp->GetIsConstant()) {
      const_valobj_sp = error_valobj_sp;
      const_valobj_sp->SetName(persistent_variable_name);
    } else
      const_valobj_sp =
          error_valobj_sp->CreateConstantValue(persistent_variable_name);

    lldb::ValueObjectSP live_valobj_sp = error_valobj_sp;

    error_valobj_sp = const_valobj_sp;

    ExpressionVariableSP clang_expr_variable_sp(
        persistent_state->CreatePersistentVariable(error_valobj_sp));
    clang_expr_variable_sp->m_live_sp = live_valobj_sp;
    clang_expr_variable_sp->m_flags |=
        ClangExpressionVariable::EVIsProgramReference;

    error_valobj_sp = clang_expr_variable_sp->GetValueObject();
  }
  return error_valobj_sp;
}

ValueObjectSP
SwiftLanguageRuntime::CalculateErrorValue(StackFrameSP frame_sp,
                                          ConstString variable_name) {
  ProcessSP process_sp(frame_sp->GetThread()->GetProcess());
  Status error;
  Target *target = frame_sp->CalculateTarget().get();
  ValueObjectSP error_valobj_sp;

  auto *runtime = Get(*process_sp);
  if (!runtime)
    return error_valobj_sp;

  llvm::Optional<Value> arg0 =
      runtime->GetErrorReturnLocationAfterReturn(frame_sp);
  if (!arg0)
    return error_valobj_sp;

  ExecutionContext exe_ctx;
  frame_sp->CalculateExecutionContext(exe_ctx);

  auto *exe_scope = exe_ctx.GetBestExecutionContextScope();
  if (!exe_scope)
    return error_valobj_sp;

  auto ast_context = target->GetScratchSwiftASTContext(error, *frame_sp);
  if (!ast_context || error.Fail())
    return error_valobj_sp;

  lldb::DataBufferSP buffer(new lldb_private::DataBufferHeap(
      arg0->GetScalar().GetBytes(), arg0->GetScalar().GetByteSize()));

  CompilerType swift_error_proto_type = ast_context->GetErrorType();
  if (!swift_error_proto_type.IsValid())
    return error_valobj_sp;

  error_valobj_sp = ValueObjectConstResult::Create(
      exe_scope, swift_error_proto_type,
      variable_name, buffer, endian::InlHostByteOrder(),
      exe_ctx.GetAddressByteSize());
  if (error_valobj_sp->GetError().Fail())
    return error_valobj_sp;

  error_valobj_sp = error_valobj_sp->GetQualifiedRepresentationIfAvailable(
      lldb::eDynamicCanRunTarget, true);
  return error_valobj_sp;
}

void SwiftLanguageRuntime::RegisterGlobalError(Target &target, ConstString name,
                                               lldb::addr_t addr) {
  auto type_system_or_err = target.GetScratchTypeSystemForLanguage(eLanguageTypeSwift);
  if (!type_system_or_err) {
    llvm::consumeError(type_system_or_err.takeError());
    return;
  }

  auto *ast_context = llvm::dyn_cast_or_null<SwiftASTContext>(&*type_system_or_err);
  if (ast_context && !ast_context->HasFatalErrors()) {
    SwiftPersistentExpressionState *persistent_state =
        llvm::cast<SwiftPersistentExpressionState>(
            target.GetPersistentExpressionStateForLanguage(
                lldb::eLanguageTypeSwift));

    std::string module_name = "$__lldb_module_for_";
    module_name.append(&name.GetCString()[1]);
    SourceModule module_info;
    module_info.path.push_back(ConstString(module_name));

    Status module_creation_error;
    swift::ModuleDecl *module_decl =
        ast_context->CreateModule(module_info, module_creation_error);

    if (module_creation_error.Success() && module_decl) {
      const bool is_static = false;
      const auto introducer = swift::VarDecl::Introducer::Let;
      const bool is_capture_list = false;

      swift::VarDecl *var_decl = new (*ast_context->GetASTContext())
          swift::VarDecl(is_static, introducer, is_capture_list, swift::SourceLoc(),
                         ast_context->GetIdentifier(name.GetCString()),
                         module_decl);
      var_decl->setInterfaceType(GetSwiftType(ast_context->GetErrorType()));
      var_decl->setDebuggerVar(true);

      persistent_state->RegisterSwiftPersistentDecl(var_decl);

      ConstString mangled_name;

      {
        swift::Mangle::ASTMangler mangler(true);
        mangled_name = ConstString(mangler.mangleGlobalVariableFull(var_decl));
      }

      lldb::addr_t symbol_addr;

      {
        ProcessSP process_sp(target.GetProcessSP());
        Status alloc_error;

        symbol_addr = process_sp->AllocateMemory(
            process_sp->GetAddressByteSize(),
            lldb::ePermissionsWritable | lldb::ePermissionsReadable,
            alloc_error);

        if (alloc_error.Success() && symbol_addr != LLDB_INVALID_ADDRESS) {
          Status write_error;
          process_sp->WritePointerToMemory(symbol_addr, addr, write_error);

          if (write_error.Success()) {
            persistent_state->RegisterSymbol(mangled_name, symbol_addr);
          }
        }
      }
    }
  }
}

lldb::BreakpointPreconditionSP
SwiftLanguageRuntime::GetBreakpointExceptionPrecondition(LanguageType language,
                                                         bool throw_bp) {
  if (language != eLanguageTypeSwift)
    return lldb::BreakpointPreconditionSP();
  if (!throw_bp)
    return lldb::BreakpointPreconditionSP();
  BreakpointPreconditionSP precondition_sp(
      new SwiftLanguageRuntime::SwiftExceptionPrecondition());
  return precondition_sp;
}

bool SwiftLanguageRuntime::SwiftExceptionPrecondition::EvaluatePrecondition(
    StoppointCallbackContext &context) {
  if (!m_type_names.empty()) {
    StackFrameSP frame_sp = context.exe_ctx_ref.GetFrameSP();
    if (!frame_sp)
      return true;

    ValueObjectSP error_valobj_sp =
        CalculateErrorValue(frame_sp, ConstString("__swift_error_var"));
    if (!error_valobj_sp || error_valobj_sp->GetError().Fail())
      return true;

    // This shouldn't fail, since at worst it will return me the object I just
    // successfully got.
    std::string full_error_name(
        error_valobj_sp->GetCompilerType().GetTypeName().AsCString());
    size_t last_dot_pos = full_error_name.rfind('.');
    std::string type_name_base;
    if (last_dot_pos == std::string::npos)
      type_name_base = full_error_name;
    else {
      if (last_dot_pos + 1 <= full_error_name.size())
        type_name_base =
            full_error_name.substr(last_dot_pos + 1, full_error_name.size());
    }

    // The type name will be the module and then the type.  If the match name
    // has a dot, we require a complete
    // match against the type, if the type name has no dot, we match it against
    // the base.

    for (std::string name : m_type_names) {
      if (name.rfind('.') != std::string::npos) {
        if (name == full_error_name)
          return true;
      } else {
        if (name == type_name_base)
          return true;
      }
    }
    return false;
  }
  return true;
}

void SwiftLanguageRuntime::SwiftExceptionPrecondition::GetDescription(
    Stream &stream, lldb::DescriptionLevel level) {
  if (level == eDescriptionLevelFull || level == eDescriptionLevelVerbose) {
    if (m_type_names.size() > 0) {
      stream.Printf("\nType Filters:");
      for (std::string name : m_type_names) {
        stream.Printf(" %s", name.c_str());
      }
      stream.Printf("\n");
    }
  }
}

Status SwiftLanguageRuntime::SwiftExceptionPrecondition::ConfigurePrecondition(
    Args &args) {
  Status error;
  std::vector<std::string> object_typenames;
  args.GetOptionValuesAsStrings("exception-typename", object_typenames);
  for (auto type_name : object_typenames)
    AddTypeName(type_name.c_str());
  return error;
}

void SwiftLanguageRuntime::AddToLibraryNegativeCache(StringRef library_name) {
  std::lock_guard<std::mutex> locker(m_negative_cache_mutex);
  m_library_negative_cache.insert(library_name);
}

bool SwiftLanguageRuntime::IsInLibraryNegativeCache(StringRef library_name) {
  std::lock_guard<std::mutex> locker(m_negative_cache_mutex);
  return m_library_negative_cache.count(library_name) == 1;
}

lldb::addr_t
SwiftLanguageRuntime::MaskMaybeBridgedPointer(lldb::addr_t addr,
                                              lldb::addr_t *masked_bits) {
  if (!m_process)
    return addr;
  const ArchSpec &arch_spec(m_process->GetTarget().GetArchitecture());
  ArchSpec::Core core_kind = arch_spec.GetCore();
  bool is_arm = false;
  bool is_intel = false;
  bool is_s390x = false;
  bool is_32 = false;
  bool is_64 = false;
  if (core_kind == ArchSpec::Core::eCore_arm_arm64) {
    is_arm = is_64 = true;
  } else if (core_kind >= ArchSpec::Core::kCore_arm_first &&
             core_kind <= ArchSpec::Core::kCore_arm_last) {
    is_arm = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_64_first &&
             core_kind <= ArchSpec::Core::kCore_x86_64_last) {
    is_intel = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_32_first &&
             core_kind <= ArchSpec::Core::kCore_x86_32_last) {
    is_intel = true;
  } else if (core_kind == ArchSpec::Core::eCore_s390x_generic) {
    is_s390x = true;
  } else {
    // this is a really random CPU core to be running on - just get out fast
    return addr;
  }

  switch (arch_spec.GetAddressByteSize()) {
  case 4:
    is_32 = true;
    break;
  case 8:
    is_64 = true;
    break;
  default:
    // this is a really random pointer size to be running on - just get out fast
    return addr;
  }

  lldb::addr_t mask = 0;

  if (is_arm && is_64)
    mask = SWIFT_ABI_ARM64_SWIFT_SPARE_BITS_MASK;

  if (is_arm && is_32)
    mask = SWIFT_ABI_ARM_SWIFT_SPARE_BITS_MASK;

  if (is_intel && is_64)
    mask = SWIFT_ABI_X86_64_SWIFT_SPARE_BITS_MASK;

  if (is_intel && is_32)
    mask = SWIFT_ABI_I386_SWIFT_SPARE_BITS_MASK;

  if (is_s390x && is_64)
    mask = SWIFT_ABI_S390X_SWIFT_SPARE_BITS_MASK;

  if (masked_bits)
    *masked_bits = addr & mask;
  return addr & ~mask;
}

lldb::addr_t
SwiftLanguageRuntime::MaybeMaskNonTrivialReferencePointer(
    lldb::addr_t addr,
    SwiftASTContext::NonTriviallyManagedReferenceStrategy strategy) {

  if (addr == 0)
    return addr;

  AppleObjCRuntime *objc_runtime = GetObjCRuntime();
  
  if (objc_runtime) {
    // tagged pointers don't perform any masking
    if (objc_runtime->IsTaggedPointer(addr))
      return addr;
  }

  if (!m_process)
    return addr;
  const ArchSpec &arch_spec(m_process->GetTarget().GetArchitecture());
  ArchSpec::Core core_kind = arch_spec.GetCore();
  bool is_arm = false;
  bool is_intel = false;
  bool is_32 = false;
  bool is_64 = false;
  if (core_kind == ArchSpec::Core::eCore_arm_arm64) {
    is_arm = is_64 = true;
  } else if (core_kind >= ArchSpec::Core::kCore_arm_first &&
             core_kind <= ArchSpec::Core::kCore_arm_last) {
    is_arm = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_64_first &&
             core_kind <= ArchSpec::Core::kCore_x86_64_last) {
    is_intel = true;
  } else if (core_kind >= ArchSpec::Core::kCore_x86_32_first &&
             core_kind <= ArchSpec::Core::kCore_x86_32_last) {
    is_intel = true;
  } else {
    // this is a really random CPU core to be running on - just get out fast
    return addr;
  }

  switch (arch_spec.GetAddressByteSize()) {
  case 4:
    is_32 = true;
    break;
  case 8:
    is_64 = true;
    break;
  default:
    // this is a really random pointer size to be running on - just get out fast
    return addr;
  }

  lldb::addr_t mask = 0;

  if (strategy == SwiftASTContext::NonTriviallyManagedReferenceStrategy::eWeak) {
    bool is_indirect = true;
    
    // On non-objc platforms, the weak reference pointer always pointed to a
    // runtime structure.
    // For ObjC platforms, the masked value determines whether it is indirect.
    
    uint32_t value = 0;
    
    if (objc_runtime)
    {
    
      if (is_intel) {
        if (is_64) {
          mask = SWIFT_ABI_X86_64_OBJC_WEAK_REFERENCE_MARKER_MASK;
          value = SWIFT_ABI_X86_64_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        } else {
          mask = SWIFT_ABI_I386_OBJC_WEAK_REFERENCE_MARKER_MASK;
          value = SWIFT_ABI_I386_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        }
      } else if (is_arm) {
        if (is_64) {
            mask = SWIFT_ABI_ARM64_OBJC_WEAK_REFERENCE_MARKER_MASK;
            value = SWIFT_ABI_ARM64_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        } else {
            mask = SWIFT_ABI_ARM_OBJC_WEAK_REFERENCE_MARKER_MASK;
            value = SWIFT_ABI_ARM_OBJC_WEAK_REFERENCE_MARKER_VALUE;
        }
      }
    } else {
        // This name is a little confusing. The "DEFAULT" marking in System.h
        // is supposed to mean: the value for non-ObjC platforms.  So
        // DEFAULT_OBJC here actually means "non-ObjC".
        mask = SWIFT_ABI_DEFAULT_OBJC_WEAK_REFERENCE_MARKER_MASK;
        value = SWIFT_ABI_DEFAULT_OBJC_WEAK_REFERENCE_MARKER_VALUE;
    }
    
    is_indirect = ((addr & mask) == value);
    
    if (!is_indirect)
      return addr;
    
    // The masked value of address is a pointer to the runtime structure.
    // The first field of the structure is the actual pointer.
    Process *process = GetProcess();
    Status error;
    
    lldb::addr_t masked_addr = addr & ~mask;
    lldb::addr_t isa_addr = process->ReadPointerFromMemory(masked_addr, error);
    if (error.Fail())
    {
        // FIXME: do some logging here.
        return addr;
    }
    return isa_addr;
    
      
  } else {
    if (is_arm && is_64)
      mask = SWIFT_ABI_ARM64_OBJC_NUM_RESERVED_LOW_BITS;
    else if (is_intel && is_64)
      mask = SWIFT_ABI_X86_64_OBJC_NUM_RESERVED_LOW_BITS;
    else
      mask = SWIFT_ABI_DEFAULT_OBJC_NUM_RESERVED_LOW_BITS;

    mask = (1 << mask) | (1 << (mask + 1));

    return addr & ~mask;
  }
  
  return addr;
}

ConstString SwiftLanguageRuntime::GetErrorBackstopName() {
  return ConstString("swift_errorInMain");
}

ConstString SwiftLanguageRuntime::GetStandardLibraryBaseName() {
  static ConstString g_swiftCore("swiftCore");
  return g_swiftCore;
}

ConstString SwiftLanguageRuntime::GetStandardLibraryName() {
  PlatformSP platform_sp(m_process->GetTarget().GetPlatform());
  if (platform_sp)
    return platform_sp->GetFullNameForDylib(GetStandardLibraryBaseName());
  return GetStandardLibraryBaseName();
}

class ProjectionSyntheticChildren : public SyntheticChildren {
public:
  struct FieldProjection {
    ConstString name;
    CompilerType type;
    int32_t byte_offset;

    FieldProjection(CompilerType parent_type, ExecutionContext *exe_ctx,
                    size_t idx) {
      const bool transparent_pointers = false;
      const bool omit_empty_base_classes = true;
      const bool ignore_array_bounds = false;
      bool child_is_base_class = false;
      bool child_is_deref_of_parent = false;
      std::string child_name;

      uint32_t child_byte_size;
      uint32_t child_bitfield_bit_size;
      uint32_t child_bitfield_bit_offset;
      uint64_t language_flags;

      type = parent_type.GetChildCompilerTypeAtIndex(
          exe_ctx, idx, transparent_pointers, omit_empty_base_classes,
          ignore_array_bounds, child_name, child_byte_size, byte_offset,
          child_bitfield_bit_size, child_bitfield_bit_offset,
          child_is_base_class, child_is_deref_of_parent, nullptr,
          language_flags);

      if (child_is_base_class)
        type.Clear(); // invalidate - base classes are dealt with outside of the
                      // projection
      else
        name.SetCStringWithLength(child_name.c_str(), child_name.size());
    }

    bool IsValid() { return !name.IsEmpty() && type.IsValid(); }

    explicit operator bool() { return IsValid(); }
  };

  struct TypeProjection {
    std::vector<FieldProjection> field_projections;
    ConstString type_name;
  };

  typedef std::unique_ptr<TypeProjection> TypeProjectionUP;

  bool IsScripted() { return false; }

  std::string GetDescription() { return "projection synthetic children"; }

  ProjectionSyntheticChildren(const Flags &flags, TypeProjectionUP &&projection)
      : SyntheticChildren(flags), m_projection(std::move(projection)) {}

protected:
  TypeProjectionUP m_projection;

  class ProjectionFrontEndProvider : public SyntheticChildrenFrontEnd {
  public:
    ProjectionFrontEndProvider(ValueObject &backend,
                               TypeProjectionUP &projection)
        : SyntheticChildrenFrontEnd(backend), m_num_bases(0),
          m_projection(projection.get()) {
      lldbassert(m_projection && "need a valid projection");
      CompilerType type(backend.GetCompilerType());
      m_num_bases = type.GetNumDirectBaseClasses();
    }

    size_t CalculateNumChildren() override {
      return m_projection->field_projections.size() + m_num_bases;
    }

    lldb::ValueObjectSP GetChildAtIndex(size_t idx) override {
      if (idx < m_num_bases) {
        if (ValueObjectSP base_object_sp =
                m_backend.GetChildAtIndex(idx, true)) {
          CompilerType base_type(base_object_sp->GetCompilerType());
          ConstString base_type_name(base_type.GetTypeName());
          if (base_type_name.IsEmpty() ||
              !SwiftLanguageRuntime::IsSwiftClassName(
                  base_type_name.GetCString()))
            return base_object_sp;
          base_object_sp = m_backend.GetSyntheticBase(
              0, base_type, true,
              Mangled(base_type_name, true)
                  .GetDemangledName(lldb::eLanguageTypeSwift));
          return base_object_sp;
        } else
          return nullptr;
      }
      idx -= m_num_bases;
      if (idx < m_projection->field_projections.size()) {
        auto &projection(m_projection->field_projections.at(idx));
        return m_backend.GetSyntheticChildAtOffset(
            projection.byte_offset, projection.type, true, projection.name);
      }
      return nullptr;
    }

    size_t GetIndexOfChildWithName(ConstString name) override {
      for (size_t idx = 0; idx < m_projection->field_projections.size();
           idx++) {
        if (m_projection->field_projections.at(idx).name == name)
          return idx;
      }
      return UINT32_MAX;
    }

    bool Update() override { return false; }

    bool MightHaveChildren() override { return true; }

    ConstString GetSyntheticTypeName() override {
      return m_projection->type_name;
    }

  private:
    size_t m_num_bases;
    TypeProjectionUP::element_type *m_projection;
  };

public:
  SyntheticChildrenFrontEnd::AutoPointer GetFrontEnd(ValueObject &backend) {
    return SyntheticChildrenFrontEnd::AutoPointer(
        new ProjectionFrontEndProvider(backend, m_projection));
  }
};

lldb::SyntheticChildrenSP
SwiftLanguageRuntime::GetBridgedSyntheticChildProvider(ValueObject &valobj) {
  ConstString type_name = valobj.GetCompilerType().GetTypeName();

  if (!type_name.IsEmpty()) {
    auto iter = m_bridged_synthetics_map.find(type_name.AsCString()),
         end = m_bridged_synthetics_map.end();
    if (iter != end)
      return iter->second;
  }

  ProjectionSyntheticChildren::TypeProjectionUP type_projection(
      new ProjectionSyntheticChildren::TypeProjectionUP::element_type());

  if (auto swift_ast_ctx = valobj.GetScratchSwiftASTContext()) {
    Status error;
    CompilerType swift_type =
        swift_ast_ctx->GetTypeFromMangledTypename(type_name, error);

    if (swift_type.IsValid()) {
      ExecutionContext exe_ctx(GetProcess());
      bool any_projected = false;
      for (size_t idx = 0, e = swift_type.GetNumChildren(true, &exe_ctx);
           idx < e; idx++) {
        // if a projection fails, keep going - we have offsets here, so it
        // should be OK to skip some members
        if (auto projection = ProjectionSyntheticChildren::FieldProjection(
                swift_type, &exe_ctx, idx)) {
          any_projected = true;
          type_projection->field_projections.push_back(projection);
        }
      }

      if (any_projected) {
        type_projection->type_name = swift_type.GetDisplayTypeName();
        SyntheticChildrenSP synth_sp =
            SyntheticChildrenSP(new ProjectionSyntheticChildren(
                SyntheticChildren::Flags(), std::move(type_projection)));
        m_bridged_synthetics_map.insert({type_name.AsCString(), synth_sp});
        return synth_sp;
      }
    }
  }

  return nullptr;
}

void SwiftLanguageRuntime::WillStartExecutingUserExpression(
    bool runs_in_playground_or_repl) {
  std::lock_guard<std::mutex> lock(m_active_user_expr_mutex);
  Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  if (m_active_user_expr_count == 0 && m_dynamic_exclusivity_flag_addr &&
      !runs_in_playground_or_repl) {
    // We're executing the first user expression. Toggle the flag.

    auto type_system_or_err = m_process->GetTarget().GetScratchTypeSystemForLanguage(eLanguageTypeC_plus_plus);
    if (!type_system_or_err) {
      LLDB_LOG_ERROR(log, type_system_or_err.takeError(),
                     "SwiftLanguageRuntime: Unable to get pointer to type system");
      return;
    }

    ConstString BoolName("bool");
    llvm::Optional<uint64_t> bool_size =
        type_system_or_err->GetBuiltinTypeByName(BoolName).GetByteSize(nullptr);
    if (!bool_size)
      return;

    Status error;
    Scalar original_value;
    m_process->ReadScalarIntegerFromMemory(*m_dynamic_exclusivity_flag_addr,
                                           *bool_size, false, original_value,
                                           error);

    m_original_dynamic_exclusivity_flag_state = original_value.UInt() != 0;

    if (error.Fail()) {
      if (log)
        log->Printf("SwiftLanguageRuntime: Unable to read "
                    "disableExclusivityChecking flag state: %s",
                    error.AsCString());
    } else {
      Scalar new_value(1U);
      m_process->WriteScalarToMemory(*m_dynamic_exclusivity_flag_addr,
                                     new_value, *bool_size, error);
      if (error.Fail()) {
        if (log)
          log->Printf("SwiftLanguageRuntime: Unable to set "
                      "disableExclusivityChecking flag state: %s",
                      error.AsCString());
      } else {
        if (log)
          log->Printf("SwiftLanguageRuntime: Changed "
                      "disableExclusivityChecking flag state from %u to 1",
                      m_original_dynamic_exclusivity_flag_state);
      }
    }
  }
  ++m_active_user_expr_count;

  if (log)
    log->Printf("SwiftLanguageRuntime: starting user expression. "
                "Number active: %u", m_active_user_expr_count);
}

void SwiftLanguageRuntime::DidFinishExecutingUserExpression(
    bool runs_in_playground_or_repl) {
  std::lock_guard<std::mutex> lock(m_active_user_expr_mutex);
  Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  --m_active_user_expr_count;
  if (log)
    log->Printf("SwiftLanguageRuntime: finished user expression. "
                "Number active: %u", m_active_user_expr_count);

  if (m_active_user_expr_count == 0 && m_dynamic_exclusivity_flag_addr &&
      !runs_in_playground_or_repl) {
    auto type_system_or_err = m_process->GetTarget().GetScratchTypeSystemForLanguage(eLanguageTypeC_plus_plus);
    if (!type_system_or_err) {
      LLDB_LOG_ERROR(log, type_system_or_err.takeError(),
                     "SwiftLanguageRuntime: Unable to get pointer to type system");
      return;
    }

    ConstString BoolName("bool");
    llvm::Optional<uint64_t> bool_size =
        type_system_or_err->GetBuiltinTypeByName(BoolName).GetByteSize(nullptr);
    if (!bool_size)
      return;

    Status error;
    Scalar original_value(m_original_dynamic_exclusivity_flag_state ? 1U : 0U);
    m_process->WriteScalarToMemory(*m_dynamic_exclusivity_flag_addr,
                                   original_value, *bool_size, error);
    if (error.Fail()) {
      if (log)
        log->Printf("SwiftLanguageRuntime: Unable to reset "
                    "disableExclusivityChecking flag state: %s",
                    error.AsCString());
    } else {
      if (log)
        log->Printf("SwiftLanguageRuntime: Changed "
                    "disableExclusivityChecking flag state back to %u",
                    m_original_dynamic_exclusivity_flag_state);
    }
  }
}

llvm::Optional<Value> SwiftLanguageRuntime::GetErrorReturnLocationAfterReturn(
    lldb::StackFrameSP frame_sp)
{
  llvm::Optional<Value> error_val;

  llvm::StringRef error_reg_name;
  ArchSpec arch_spec(GetTargetRef().GetArchitecture());
  switch (arch_spec.GetMachine()) {
    case llvm::Triple::ArchType::arm:
      error_reg_name = "r6";
      break;
    case llvm::Triple::ArchType::aarch64:
      error_reg_name = "x21";
      break;
    case llvm::Triple::ArchType::x86_64:
      error_reg_name = "r12";
      break;
    default:
      break;
  }
  
  
  if (error_reg_name.empty())
      return error_val;
      
  RegisterContextSP reg_ctx = frame_sp->GetRegisterContext();
  const RegisterInfo *reg_info = reg_ctx->GetRegisterInfoByName(error_reg_name);
  lldbassert(reg_info && "didn't get the right register name for swift error register");
  if (!reg_info)
    return error_val;
  
  RegisterValue reg_value;
  if (!reg_ctx->ReadRegister(reg_info, reg_value))
  {
    // Do some logging here.
    return error_val;
  }
  
  lldb::addr_t error_addr = reg_value.GetAsUInt64();
  if (error_addr == 0)
    return error_val;

  Value val;
  if (reg_value.GetScalarValue(val.GetScalar())) {
    val.SetValueType(Value::eValueTypeScalar);
    val.SetContext(Value::eContextTypeRegisterInfo,
                     const_cast<RegisterInfo *>(reg_info));
    error_val = val;
  }
  return error_val;
}

llvm::Optional<Value> SwiftLanguageRuntime::GetErrorReturnLocationBeforeReturn(
    lldb::StackFrameSP frame_sp, bool &need_to_check_after_return) {
  llvm::Optional<Value> error_val;
  
  if (!frame_sp)
  {
    need_to_check_after_return = false;
    return error_val;
  }
  
  // For Architectures where the error isn't returned in a register,
  // there's a magic variable that points to the value.  Check that first:
  
  ConstString error_location_name("$error");
  VariableListSP variables_sp = frame_sp->GetInScopeVariableList(false);
  VariableSP error_loc_var_sp = variables_sp->FindVariable(
      error_location_name, eValueTypeVariableArgument);
  if (error_loc_var_sp) {
    need_to_check_after_return = false;
    
    ValueObjectSP error_loc_val_sp = frame_sp->GetValueObjectForFrameVariable(
        error_loc_var_sp, eNoDynamicValues);
    if (error_loc_val_sp && error_loc_val_sp->GetError().Success())
      error_val = error_loc_val_sp->GetValue();

    return error_val;
  }
  
  // Otherwise, see if we know which register it lives in from the calling convention.
  // This should probably go in the ABI plugin not here, but the Swift ABI can change with
  // swiftlang versions and that would make it awkward in the ABI.
  
  Function *func = frame_sp->GetSymbolContext(eSymbolContextFunction).function;
  if (!func)
  {
    need_to_check_after_return = false;
    return error_val;
  }
  
  need_to_check_after_return = func->CanThrow();
  return error_val;

}

//------------------------------------------------------------------
// Static Functions
//------------------------------------------------------------------
LanguageRuntime *
SwiftLanguageRuntime::CreateInstance(Process *process,
                                     lldb::LanguageType language) {
  if (language == eLanguageTypeSwift)
    return new SwiftLanguageRuntime(process);
  else
    return NULL;
}

lldb::BreakpointResolverSP
SwiftLanguageRuntime::CreateExceptionResolver(Breakpoint *bkpt, bool catch_bp,
                                              bool throw_bp) {
  BreakpointResolverSP resolver_sp;

  if (throw_bp)
    resolver_sp.reset(new BreakpointResolverName(
        bkpt, "swift_willThrow", eFunctionNameTypeBase, eLanguageTypeUnknown,
        Breakpoint::Exact, 0, eLazyBoolNo));
  // FIXME: We don't do catch breakpoints for ObjC yet.
  // Should there be some way for the runtime to specify what it can do in this
  // regard?
  return resolver_sp;
}

static const char *
SwiftDemangleNodeKindToCString(const swift::Demangle::Node::Kind node_kind) {
#define NODE(e)                                                                \
  case swift::Demangle::Node::Kind::e:                                         \
    return #e;

  switch (node_kind) {
#include "swift/Demangling/DemangleNodes.def"
  }
  return "swift::Demangle::Node::Kind::???";
#undef NODE
}

static OptionDefinition g_swift_demangle_options[] = {
  // clang-format off
  {LLDB_OPT_SET_1, false, "expand", 'e', OptionParser::eNoArgument, nullptr, {}, 0, eArgTypeNone, "Whether LLDB should print the demangled tree"},
  // clang-format on
};

class CommandObjectSwift_Demangle : public CommandObjectParsed {
public:
  CommandObjectSwift_Demangle(CommandInterpreter &interpreter)
      : CommandObjectParsed(interpreter, "demangle",
                            "Demangle a Swift mangled name",
                            "language swift demangle"),
        m_options() {}

  ~CommandObjectSwift_Demangle() {}

  virtual Options *GetOptions() { return &m_options; }

  class CommandOptions : public Options {
  public:
    CommandOptions() : Options(), m_expand(false, false) {
      OptionParsingStarting(nullptr);
    }

    virtual ~CommandOptions() {}

    Status SetOptionValue(uint32_t option_idx, llvm::StringRef option_arg,
                                 ExecutionContext *execution_context) override {
      Status error;
      const int short_option = m_getopt_table[option_idx].val;
      switch (short_option) {
      case 'e':
        m_expand.SetCurrentValue(true);
        break;

      default:
        error.SetErrorStringWithFormat("invalid short option character '%c'",
                                       short_option);
        break;
      }

      return error;
    }

    void OptionParsingStarting(ExecutionContext *execution_context) override {
      m_expand.Clear();
    }

    llvm::ArrayRef<OptionDefinition> GetDefinitions() override {
      return llvm::makeArrayRef(g_swift_demangle_options);
    }

    // Options table: Required for subclasses of Options.

    OptionValueBoolean m_expand;
  };

protected:
  void PrintNode(swift::Demangle::NodePointer node_ptr, Stream &stream,
                 int depth = 0) {
    if (!node_ptr)
      return;

    std::string indent(2 * depth, ' ');

    stream.Printf("%s", indent.c_str());

    stream.Printf("kind=%s",
                  SwiftDemangleNodeKindToCString(node_ptr->getKind()));
    if (node_ptr->hasText()) {
      std::string Text = node_ptr->getText();
      stream.Printf(", text=\"%s\"", Text.c_str());
    }
    if (node_ptr->hasIndex())
      stream.Printf(", index=%" PRIu64, node_ptr->getIndex());

    stream.Printf("\n");

    for (auto &&child : *node_ptr) {
      PrintNode(child, stream, depth + 1);
    }
  }

  bool DoExecute(Args &command, CommandReturnObject &result) {
    for (size_t i = 0; i < command.GetArgumentCount(); i++) {
      const char *arg = command.GetArgumentAtIndex(i);
      if (arg && *arg) {
        swift::Demangle::Context demangle_ctx;
        auto node_ptr = demangle_ctx.demangleSymbolAsNode(llvm::StringRef(arg));
        if (node_ptr) {
          if (m_options.m_expand) {
            PrintNode(node_ptr, result.GetOutputStream());
          }
          result.GetOutputStream().Printf(
              "%s ---> %s\n", arg,
              swift::Demangle::nodeToString(node_ptr).c_str());
        }
      }
    }
    result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
    return true;
  }

  CommandOptions m_options;
};

class CommandObjectSwift_RefCount : public CommandObjectRaw {
public:
  CommandObjectSwift_RefCount(CommandInterpreter &interpreter)
      : CommandObjectRaw(interpreter, "refcount",
                         "Inspect the reference count data for a Swift object",
                         "language swift refcount",
                         eCommandProcessMustBePaused | eCommandRequiresFrame) {}

  ~CommandObjectSwift_RefCount() {}

  virtual Options *GetOptions() { return nullptr; }

private:
  enum class ReferenceCountType {
    eReferenceStrong,
    eReferenceUnowned,
    eReferenceWeak,
  };

  llvm::Optional<uint32_t> getReferenceCount(StringRef ObjName,
                                             ReferenceCountType Type,
                                             ExecutionContext &exe_ctx,
                                             StackFrameSP &Frame) {
    std::string Kind;
    switch (Type) {
    case ReferenceCountType::eReferenceStrong:
      Kind = "";
      break;
    case ReferenceCountType::eReferenceUnowned:
      Kind = "Unowned";
      break;
    case ReferenceCountType::eReferenceWeak:
      Kind = "Weak";
      break;
    }

    EvaluateExpressionOptions eval_options;
    eval_options.SetLanguage(lldb::eLanguageTypeSwift);
    eval_options.SetResultIsInternal(true);
    ValueObjectSP result_valobj_sp;
    std::string Expr =
        (llvm::Twine("Swift._get") + Kind + llvm::Twine("RetainCount(") +
         ObjName + llvm::Twine(")"))
            .str();
    bool evalStatus = exe_ctx.GetTargetSP()->EvaluateExpression(
        Expr, Frame.get(), result_valobj_sp, eval_options);
    if (evalStatus != eExpressionCompleted)
      return llvm::None;

    bool success = false;
    uint32_t count = result_valobj_sp->GetSyntheticValue()->GetValueAsUnsigned(
        UINT32_MAX, &success);
    if (!success)
      return llvm::None;
    return count;
  }

protected:
  bool DoExecute(llvm::StringRef command, CommandReturnObject &result) {
    StackFrameSP frame_sp(m_exe_ctx.GetFrameSP());
    EvaluateExpressionOptions options;
    options.SetLanguage(lldb::eLanguageTypeSwift);
    options.SetResultIsInternal(true);
    ValueObjectSP result_valobj_sp;

    // We want to evaluate first the object we're trying to get the
    // refcount of, in order, to, e.g. see whether it's available.
    // So, given `language swift refcount patatino`, we try to
    // evaluate `expr patatino` and fail early in case there is
    // an error.
    bool evalStatus = m_exe_ctx.GetTargetSP()->EvaluateExpression(
        command, frame_sp.get(), result_valobj_sp, options);
    if (evalStatus != eExpressionCompleted) {
      result.SetStatus(lldb::eReturnStatusFailed);
      if (result_valobj_sp && result_valobj_sp->GetError().Fail())
        result.AppendError(result_valobj_sp->GetError().AsCString());
      return false;
    }

    // At this point, we're sure we're grabbing in our hands a valid
    // object and we can ask questions about it. `refcounts` are only
    // defined on class objects, so we throw an error in case we're
    // trying to look at something else.
    result_valobj_sp = result_valobj_sp->GetQualifiedRepresentationIfAvailable(
        lldb::eDynamicCanRunTarget, true);
    CompilerType result_type(result_valobj_sp->GetCompilerType());
    if (!(result_type.GetTypeInfo() & lldb::eTypeInstanceIsPointer)) {
      result.AppendError("refcount only available for class types");
      result.SetStatus(lldb::eReturnStatusFailed);
      return false;
    }

    // Ask swift debugger support in the compiler about the objects
    // reference counts, and return them to the user.
    llvm::Optional<uint32_t> strong = getReferenceCount(
        command, ReferenceCountType::eReferenceStrong, m_exe_ctx, frame_sp);
    llvm::Optional<uint32_t> unowned = getReferenceCount(
        command, ReferenceCountType::eReferenceUnowned, m_exe_ctx, frame_sp);
    llvm::Optional<uint32_t> weak = getReferenceCount(
        command, ReferenceCountType::eReferenceWeak, m_exe_ctx, frame_sp);

    std::string unavailable = "<unavailable>";

    result.AppendMessageWithFormat(
        "refcount data: (strong = %s, unowned = %s, weak = %s)\n",
        strong ? std::to_string(*strong).c_str() : unavailable.c_str(),
        unowned ? std::to_string(*unowned).c_str() : unavailable.c_str(),
        weak ? std::to_string(*weak).c_str() : unavailable.c_str());
    result.SetStatus(lldb::eReturnStatusSuccessFinishResult);
    return true;
  }
};

class CommandObjectMultiwordSwift : public CommandObjectMultiword {
public:
  CommandObjectMultiwordSwift(CommandInterpreter &interpreter)
      : CommandObjectMultiword(
            interpreter, "swift",
            "A set of commands for operating on the Swift Language Runtime.",
            "swift <subcommand> [<subcommand-options>]") {
    LoadSubCommand("demangle", CommandObjectSP(new CommandObjectSwift_Demangle(
                                   interpreter)));
    LoadSubCommand("refcount", CommandObjectSP(new CommandObjectSwift_RefCount(
                                   interpreter)));
  }

  virtual ~CommandObjectMultiwordSwift() {}
};

void SwiftLanguageRuntime::Initialize() {
  PluginManager::RegisterPlugin(
      GetPluginNameStatic(), "Language runtime for the Swift language",
      CreateInstance,
      [](CommandInterpreter &interpreter) -> lldb::CommandObjectSP {
        return CommandObjectSP(new CommandObjectMultiwordSwift(interpreter));
      },
      GetBreakpointExceptionPrecondition);
}

void SwiftLanguageRuntime::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

lldb_private::ConstString SwiftLanguageRuntime::GetPluginNameStatic() {
  static ConstString g_name("swift");
  return g_name;
}

//------------------------------------------------------------------
// PluginInterface protocol
//------------------------------------------------------------------
lldb_private::ConstString SwiftLanguageRuntime::GetPluginName() {
  return GetPluginNameStatic();
}

uint32_t SwiftLanguageRuntime::GetPluginVersion() { return 1; }
