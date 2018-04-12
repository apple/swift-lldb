//===-- SwiftHashedContainer.cpp --------------------------------*- C++ -*-===//
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

#include "SwiftHashedContainer.h"

#include "lldb/Core/ValueObjectConstResult.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/SwiftASTContext.h"
#include "lldb/Target/ObjCLanguageRuntime.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SwiftLanguageRuntime.h"
#include "lldb/Utility/DataBufferHeap.h"

#include "Plugins/Language/ObjC/NSDictionary.h"

#include "swift/AST/ASTContext.h"
#include "llvm/ADT/StringRef.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::swift;

//#define DICTIONARY_IS_BROKEN_AGAIN 1

size_t SwiftHashedContainerSyntheticFrontEndBufferHandler::GetCount() {
  return m_frontend->CalculateNumChildren();
}

lldb_private::CompilerType
SwiftHashedContainerSyntheticFrontEndBufferHandler::GetElementType() {
  // this doesn't make sense here - the synthetic children know best
  return CompilerType();
}

lldb::ValueObjectSP
SwiftHashedContainerSyntheticFrontEndBufferHandler::GetElementAtIndex(
    size_t idx) {
  return m_frontend->GetChildAtIndex(idx);
}

SwiftHashedContainerSyntheticFrontEndBufferHandler::
    SwiftHashedContainerSyntheticFrontEndBufferHandler(
        lldb::ValueObjectSP valobj_sp)
    : m_valobj_sp(valobj_sp),
      m_frontend(NSDictionarySyntheticFrontEndCreator(nullptr, valobj_sp)) {
  // Cocoa frontends must be updated before use
  if (m_frontend)
    m_frontend->Update();
}

bool SwiftHashedContainerSyntheticFrontEndBufferHandler::IsValid() {
  return m_frontend.get() != nullptr;
}

size_t SwiftHashedContainerNativeBufferHandler::GetCount() { return m_count; }

lldb_private::CompilerType
SwiftHashedContainerNativeBufferHandler::GetElementType() {
  return m_element_type;
}

lldb::ValueObjectSP
SwiftHashedContainerNativeBufferHandler::GetElementAtIndex(size_t idx) {
  lldb::ValueObjectSP null_valobj_sp;
  if (idx >= m_count)
    return null_valobj_sp;
  if (!IsValid())
    return null_valobj_sp;
  int64_t found_idx = -1;
  Status error;
  for (Cell cell_idx = 0; cell_idx < m_capacity; cell_idx++) {
    const bool used = ReadBitmaskAtIndex(cell_idx, error);
    if (error.Fail()) {
      Status bitmask_error;
      bitmask_error.SetErrorStringWithFormat(
              "Failed to read bit-mask index from Dictionary: %s",
              error.AsCString());
      return ValueObjectConstResult::Create(m_process, bitmask_error);
    }
    if (!used)
      continue;
    if (++found_idx == idx) {
#ifdef DICTIONARY_IS_BROKEN_AGAIN
      printf("found idx = %zu at cell_idx = %" PRIu64 "\n", idx, cell_idx);
#endif

      // you found it!!!
      DataBufferSP full_buffer_sp(
          new DataBufferHeap(m_key_stride_padded + m_value_stride, 0));
      uint8_t *key_buffer_ptr = full_buffer_sp->GetBytes();
      uint8_t *value_buffer_ptr =
          m_value_stride ? (key_buffer_ptr + m_key_stride_padded) : nullptr;
      if (GetDataForKeyAtCell(cell_idx, key_buffer_ptr) &&
          (value_buffer_ptr == nullptr ||
           GetDataForValueAtCell(cell_idx, value_buffer_ptr))) {
        DataExtractor full_data;
        full_data.SetData(full_buffer_sp);
        StreamString name;
        name.Printf("[%zu]", idx);
        return ValueObjectConstResult::Create(
            m_process, m_element_type, ConstString(name.GetData()), full_data);
      }
    }
  }
  return null_valobj_sp;
}

bool SwiftHashedContainerNativeBufferHandler::ReadBitmaskAtIndex(Index i, 
                                                                 Status &error) {
  if (i >= m_capacity)
    return false;
  const size_t word = i / (8 * m_ptr_size);
  const size_t offset = i % (8 * m_ptr_size);
  const lldb::addr_t effective_ptr = m_bitmask_ptr + (word * m_ptr_size);
#ifdef DICTIONARY_IS_BROKEN_AGAIN
  printf("for idx = %" PRIu64
         ", reading at word = %zu offset = %zu, effective_ptr = 0x%" PRIx64
         "\n",
         i, word, offset, effective_ptr);
#endif

  uint64_t data = 0;

  auto cached = m_bitmask_cache.find(effective_ptr);
  if (cached != m_bitmask_cache.end()) {
    data = cached->second;
  } else {
    data = m_process->ReadUnsignedIntegerFromMemory(effective_ptr, m_ptr_size,
                                                    0, error);
    if (error.Fail())
      return false;
    m_bitmask_cache[effective_ptr] = data;
  }

  const uint64_t mask = static_cast<uint64_t>(1UL << offset);
  const uint64_t value = (data & mask);
#ifdef DICTIONARY_IS_BROKEN_AGAIN
  printf("data = 0x%" PRIx64 ", mask = 0x%" PRIx64 ", value = 0x%" PRIx64 "\n",
         data, mask, value);
#endif
  return (0 != value);
}

lldb::addr_t
SwiftHashedContainerNativeBufferHandler::GetLocationOfKeyAtCell(Cell i) {
  return m_keys_ptr + (i * m_key_stride);
}

lldb::addr_t
SwiftHashedContainerNativeBufferHandler::GetLocationOfValueAtCell(Cell i) {
  return m_value_stride ? m_values_ptr + (i * m_value_stride)
                        : LLDB_INVALID_ADDRESS;
}

// these are sharp tools that assume that the Cell contains valid data and the
// destination buffer
// has enough room to store the data to - use with caution
bool SwiftHashedContainerNativeBufferHandler::GetDataForKeyAtCell(
    Cell i, void *data_ptr) {
  if (!data_ptr)
    return false;

  lldb::addr_t addr = GetLocationOfKeyAtCell(i);
  Status error;
  m_process->ReadMemory(addr, data_ptr, m_key_stride, error);
  if (error.Fail())
    return false;

  return true;
}

bool SwiftHashedContainerNativeBufferHandler::GetDataForValueAtCell(
    Cell i, void *data_ptr) {
  if (!data_ptr || !m_value_stride)
    return false;

  lldb::addr_t addr = GetLocationOfValueAtCell(i);
  Status error;
  m_process->ReadMemory(addr, data_ptr, m_value_stride, error);
  if (error.Fail())
    return false;

  return true;
}

SwiftHashedContainerNativeBufferHandler::
    SwiftHashedContainerNativeBufferHandler(
        lldb::ValueObjectSP nativeStorage_sp, CompilerType key_type,
        CompilerType value_type)
    : m_nativeStorage(nativeStorage_sp.get()), m_process(nullptr),
      m_ptr_size(0), m_count(0), m_capacity(0),
      m_bitmask_ptr(LLDB_INVALID_ADDRESS), m_keys_ptr(LLDB_INVALID_ADDRESS),
      m_values_ptr(LLDB_INVALID_ADDRESS), m_element_type(),
      m_key_stride(key_type.GetByteStride()), m_value_stride(0),
      m_key_stride_padded(m_key_stride), m_bitmask_cache() {
  static ConstString g_initializedEntries("initializedEntries");
  static ConstString g_values("values");
  static ConstString g__rawValue("_rawValue");
  static ConstString g_keys("keys");
  static ConstString g_buffer("buffer");

  static ConstString g_key("key");
  static ConstString g_value("value");
  static ConstString g__value("_value");

  static ConstString g_capacity("bucketCount");
  static ConstString g_count("count");

  if (!m_nativeStorage)
    return;
  if (!key_type)
    return;

  if (value_type) {
    m_value_stride = value_type.GetByteStride();
    if (SwiftASTContext *swift_ast =
            llvm::dyn_cast_or_null<SwiftASTContext>(key_type.GetTypeSystem())) {
      std::vector<SwiftASTContext::TupleElement> tuple_elements{
          {g_key, key_type}, {g_value, value_type}};
      m_element_type = swift_ast->CreateTupleType(tuple_elements);
      m_key_stride_padded = m_element_type.GetByteStride() - m_value_stride;
    }
  } else
    m_element_type = key_type;

  if (!m_element_type)
    return;

  m_process = m_nativeStorage->GetProcessSP().get();
  if (!m_process)
    return;

  m_ptr_size = m_process->GetAddressByteSize();

  auto buffer_sp = m_nativeStorage->GetChildAtNamePath({g_buffer});
  if (buffer_sp) {
    auto buffer_ptr = buffer_sp->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
    if (buffer_ptr == 0 || buffer_ptr == LLDB_INVALID_ADDRESS)
      return;

    Status error;
    m_capacity =
        m_process->ReadPointerFromMemory(buffer_ptr + 2 * m_ptr_size, error);
    if (error.Fail())
      return;
    m_count =
        m_process->ReadPointerFromMemory(buffer_ptr + 3 * m_ptr_size, error);
    if (error.Fail())
      return;
  } else {
    auto capacity_sp =
        m_nativeStorage->GetChildAtNamePath({g_capacity, g__value});
    if (!capacity_sp)
      return;
    m_capacity = capacity_sp->GetValueAsUnsigned(0);
    auto count_sp = m_nativeStorage->GetChildAtNamePath({g_count, g__value});
    if (!count_sp)
      return;
    m_count = count_sp->GetValueAsUnsigned(0);
  }

  m_nativeStorage = nativeStorage_sp.get();
  m_bitmask_ptr =
      m_nativeStorage
          ->GetChildAtNamePath({g_initializedEntries, g_values, g__rawValue})
          ->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);

  if (ValueObjectSP value_child_sp =
          m_nativeStorage->GetChildAtNamePath({g_values, g__rawValue})) {
    // it is fine not to pass a value_type, but if the value child exists, then
    // you have to pass one
    if (!value_type)
      return;
    m_values_ptr = value_child_sp->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
  }
  m_keys_ptr = m_nativeStorage->GetChildAtNamePath({g_keys, g__rawValue})
                   ->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
  // Make sure we can read the bitmask at the ount index.  
  // and this will keep us from trying
  // to reconstruct many bajillions of invalid children.
  // Don't bother if the native buffer handler is invalid already, however. 
  if (IsValid())
  {
    Status error;
    ReadBitmaskAtIndex(m_capacity - 1, error);
    if (error.Fail())
    {
      m_bitmask_ptr = LLDB_INVALID_ADDRESS;
    }
  }
}

bool SwiftHashedContainerNativeBufferHandler::IsValid() {
  return (m_nativeStorage != nullptr) && (m_process != nullptr) &&
         m_element_type.IsValid() && m_bitmask_ptr != LLDB_INVALID_ADDRESS &&
         m_keys_ptr != LLDB_INVALID_ADDRESS &&
         /*m_values_ptr != LLDB_INVALID_ADDRESS && you can't check values
            because some containers have only keys*/
         m_capacity >= m_count;
}

std::unique_ptr<SwiftHashedContainerBufferHandler>
SwiftHashedContainerBufferHandler::CreateBufferHandlerForNativeStorageOwner(
    ValueObject &valobj, lldb::addr_t storage_ptr, bool fail_on_no_children,
    NativeCreatorFunction Native) {

  CompilerType valobj_type(valobj.GetCompilerType());
  CompilerType key_type = valobj_type.GetGenericArgumentType(0);
  CompilerType value_type = valobj_type.GetGenericArgumentType(1);

  static ConstString g_Native("native");
  static ConstString g_nativeStorage("nativeStorage");
  static ConstString g_buffer("buffer");

  Status error;

  ProcessSP process_sp(valobj.GetProcessSP());
  if (!process_sp)
    return nullptr;

  ValueObjectSP native_sp(valobj.GetChildAtNamePath({g_nativeStorage}));
  ValueObjectSP native_buffer_sp(
      valobj.GetChildAtNamePath({g_nativeStorage, g_buffer}));
  if (!native_sp || !native_buffer_sp) {
    if (fail_on_no_children)
      return nullptr;
    else {
      lldb::addr_t native_storage_ptr =
          storage_ptr + (3 * process_sp->GetAddressByteSize());
      native_storage_ptr =
          process_sp->ReadPointerFromMemory(native_storage_ptr, error);
      // (AnyObject,AnyObject)?
      SwiftASTContext *swift_ast_ctx = llvm::dyn_cast_or_null<SwiftASTContext>(
          process_sp->GetTarget().GetScratchTypeSystemForLanguage(
              &error, eLanguageTypeSwift));
      if (swift_ast_ctx) {
        CompilerType element_type(swift_ast_ctx->GetTypeFromMangledTypename(
            SwiftLanguageRuntime::GetCurrentMangledName(
                "_TtGSqTPs9AnyObject_PS____")
                .c_str(),
            error));
        auto handler = std::unique_ptr<SwiftHashedContainerBufferHandler>(
            Native(native_sp, key_type, value_type));
        if (handler && handler->IsValid())
          return handler;
      }
      return nullptr;
    }
    }

    CompilerType child_type(native_sp->GetCompilerType());
    CompilerType element_type(child_type.GetGenericArgumentType(1));
    if (element_type.IsValid() == false ||
        child_type.GetGenericArgumentKind(1) != lldb::eBoundGenericKindType)
      return nullptr;
    lldb::addr_t native_storage_ptr = process_sp->ReadPointerFromMemory(storage_ptr + 2*process_sp->GetAddressByteSize(), error);
    if (error.Fail() || native_storage_ptr == LLDB_INVALID_ADDRESS)
        return nullptr;
    auto handler = std::unique_ptr<SwiftHashedContainerBufferHandler>(Native(native_sp, key_type, value_type));
    if (handler && handler->IsValid())
        return handler;

  return nullptr;
}

std::unique_ptr<SwiftHashedContainerBufferHandler>
SwiftHashedContainerBufferHandler::CreateBufferHandler(
    ValueObject &valobj, NativeCreatorFunction Native,
    SyntheticCreatorFunction Synthetic, ConstString mangled,
    ConstString demangled) {
  static ConstString g__variantStorage("_variantStorage");
  static ConstString g__variantBuffer("_variantBuffer");
  static ConstString g_Native("native");
  static ConstString g_Cocoa("cocoa");
  static ConstString g_nativeStorage("nativeStorage");
  static ConstString g_nativeBuffer("nativeBuffer");
  static ConstString g_buffer("buffer");
  static ConstString g_storage("storage");
  static ConstString g__storage("_storage");
  static ConstString g_Some("some");

  Status error;

  ProcessSP process_sp(valobj.GetProcessSP());
  if (!process_sp)
    return nullptr;

  ConstString type_name_cs(valobj.GetTypeName());
  if (type_name_cs) {
    llvm::StringRef type_name_strref(type_name_cs.GetStringRef());

    if (type_name_strref.startswith(mangled.GetCString()) ||
        type_name_strref.startswith(demangled.GetCString())) {
      return CreateBufferHandlerForNativeStorageOwner(
          valobj, valobj.GetPointerValue(), false, Native);
    }
  }

  ValueObjectSP valobj_sp =
      valobj.GetSP()->GetQualifiedRepresentationIfAvailable(
          lldb::eDynamicCanRunTarget, false);

  ValueObjectSP _variantStorageSP(
      valobj_sp->GetChildMemberWithName(g__variantStorage, true));

  if (!_variantStorageSP)
    _variantStorageSP =
        valobj_sp->GetChildMemberWithName(g__variantBuffer, true);

  if (!_variantStorageSP) {
    static ConstString g__SwiftDeferredNSDictionary(
        "Swift._SwiftDeferredNSDictionary");
    if (type_name_cs.GetStringRef().startswith(
            g__SwiftDeferredNSDictionary.GetStringRef())) {
      ValueObjectSP storage_sp(
          valobj_sp->GetChildAtNamePath({g_nativeBuffer, g__storage}));
      if (storage_sp) {
        CompilerType child_type(valobj_sp->GetCompilerType());
        CompilerType key_type(child_type.GetGenericArgumentType(0));
        CompilerType value_type(child_type.GetGenericArgumentType(1));

        auto handler = std::unique_ptr<SwiftHashedContainerBufferHandler>(
            Native(storage_sp, key_type, value_type));
        if (handler && handler->IsValid())
          return handler;
      }
    }
    return nullptr;
  }

  ConstString storage_kind(_variantStorageSP->GetValueAsCString());

  if (!storage_kind)
    return nullptr;

  if (g_Cocoa == storage_kind) {
    ValueObjectSP child_sp(
        _variantStorageSP->GetChildMemberWithName(g_Native, true));
    if (!child_sp)
      return nullptr;
    // it's an NSDictionary in disguise
    uint64_t cocoa_storage_ptr =
        child_sp->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
    if (cocoa_storage_ptr == LLDB_INVALID_ADDRESS || error.Fail())
      return nullptr;
    cocoa_storage_ptr &= 0x00FFFFFFFFFFFFFF; // for some reason I need to zero
                                             // out the MSB; figure out why
                                             // later
    CompilerType id =
        process_sp->GetTarget().GetScratchClangASTContext()->GetBasicType(
            lldb::eBasicTypeObjCID);
    InferiorSizedWord isw(cocoa_storage_ptr, *process_sp);
    ValueObjectSP cocoarr_sp = ValueObject::CreateValueObjectFromData(
        "cocoarr", isw.GetAsData(process_sp->GetByteOrder()),
        valobj.GetExecutionContextRef(), id);
    if (!cocoarr_sp)
      return nullptr;
    auto objc_runtime = process_sp->GetObjCLanguageRuntime();
    auto descriptor_sp = objc_runtime->GetClassDescriptor(*cocoarr_sp);
    if (!descriptor_sp)
      return nullptr;
    ConstString classname(descriptor_sp->GetClassName());
    if (classname &&
        classname.GetStringRef().startswith(mangled.GetCString())) {
      return CreateBufferHandlerForNativeStorageOwner(
          *_variantStorageSP, cocoa_storage_ptr, true, Native);
    } else {
      auto handler = std::unique_ptr<SwiftHashedContainerBufferHandler>(
          Synthetic(cocoarr_sp));
      if (handler && handler->IsValid())
        return handler;
      return nullptr;
    }
  }
  if (g_Native == storage_kind) {
    ValueObjectSP native_sp(_variantStorageSP->GetChildAtNamePath({g_Native}));
    ValueObjectSP nativeStorage_sp(
        _variantStorageSP->GetChildAtNamePath({g_Native, g_nativeStorage}));
    if (!native_sp)
      return nullptr;
    if (!nativeStorage_sp)
      nativeStorage_sp =
          _variantStorageSP->GetChildAtNamePath({g_Native, g__storage});
    if (!nativeStorage_sp)
      return nullptr;

    CompilerType child_type(valobj.GetCompilerType());
    lldb::TemplateArgumentKind kind;
    CompilerType key_type(child_type.GetGenericArgumentType(0));
    CompilerType value_type(child_type.GetGenericArgumentType(1));

    auto handler = std::unique_ptr<SwiftHashedContainerBufferHandler>(
        Native(nativeStorage_sp, key_type, value_type));
    if (handler && handler->IsValid())
      return handler;
    return nullptr;
  }

  return nullptr;
}

lldb_private::formatters::swift::HashedContainerSyntheticFrontEnd::
    HashedContainerSyntheticFrontEnd(lldb::ValueObjectSP valobj_sp)
    : SyntheticChildrenFrontEnd(*valobj_sp.get()), m_buffer() {}

size_t lldb_private::formatters::swift::HashedContainerSyntheticFrontEnd::
    CalculateNumChildren() {
  return m_buffer ? m_buffer->GetCount() : 0;
}

lldb::ValueObjectSP lldb_private::formatters::swift::
    HashedContainerSyntheticFrontEnd::GetChildAtIndex(size_t idx) {
  if (!m_buffer)
    return ValueObjectSP();

  lldb::ValueObjectSP child_sp = m_buffer->GetElementAtIndex(idx);

  if (child_sp)
    child_sp->SetSyntheticChildrenGenerated(true);

  return child_sp;
}

bool lldb_private::formatters::swift::HashedContainerSyntheticFrontEnd::
    MightHaveChildren() {
  return true;
}

size_t lldb_private::formatters::swift::HashedContainerSyntheticFrontEnd::
    GetIndexOfChildWithName(const ConstString &name) {
  if (!m_buffer)
    return UINT32_MAX;
  const char *item_name = name.GetCString();
  uint32_t idx = ExtractIndexFromString(item_name);
  if (idx < UINT32_MAX && idx >= CalculateNumChildren())
    return UINT32_MAX;
  return idx;
}
