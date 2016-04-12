//===-- SwiftFormatters.cpp -------------------------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "lldb/Core/Error.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "SwiftFormatters.h"
#include "lldb/DataFormatters/StringPrinter.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SwiftLanguageRuntime.h"

// FIXME: we should not need this
#include "Plugins/Language/CPlusPlus/CxxStringTypes.h"
#include "Plugins/Language/ObjC/Cocoa.h"
#include "Plugins/Language/ObjC/NSString.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;
using namespace lldb_private::formatters::swift;

bool
lldb_private::formatters::swift::Character_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g_Small("Small");
    static ConstString g_Large("Large");
    
    static ConstString g__representation("_representation");

    static ConstString g__storage("_storage");
    static ConstString g_storage("storage");
    static ConstString g_Some("Some");
    
    ProcessSP process_sp(valobj.GetProcessSP());
    if (!process_sp)
        return false;
    
    ValueObjectSP representation_sp = valobj.GetChildMemberWithName(g__representation, true);
    
    if (!representation_sp)
        return false;
    
    ConstString value(representation_sp->GetValueAsCString());
    
    if (value == g_Large)
    {
        ValueObjectSP largeBuffer_sp(representation_sp->GetChildAtNamePath( {g_Large,g__storage,g_storage,g_Some} ));
        if (!largeBuffer_sp)
            return false;
        
        lldb::addr_t buffer_ptr = largeBuffer_sp->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
        if (LLDB_INVALID_ADDRESS == buffer_ptr || 0 == buffer_ptr)
            return false;
        
        buffer_ptr += 2*process_sp->GetAddressByteSize();
        Error error;
        buffer_ptr = process_sp->ReadPointerFromMemory(buffer_ptr, error);
        if (LLDB_INVALID_ADDRESS == buffer_ptr || 0 == buffer_ptr)
            return false;

        StringPrinter::ReadStringAndDumpToStreamOptions options;
        options.SetLocation(buffer_ptr)
        .SetEscapeNonPrintables(true)
        .SetNeedsZeroTermination(true)
        .SetPrefixToken(0)
        .SetProcessSP(valobj.GetProcessSP())
        .SetQuote('"')
        .SetStream(&stream)
        .SetLanguage(lldb::eLanguageTypeSwift);

        return StringPrinter::ReadStringAndDumpToStream<StringPrinter::StringElementType::UTF16>(options);
    }
    else if (value == g_Small)
    {
        const uint64_t invalidRepr = 0xFFFFFFFFFFFFFFFF;
        
        ValueObjectSP smallBuffer_sp(representation_sp->GetChildAtNamePath( {g_Small} ));
        if (!smallBuffer_sp)
            return false;

        uint64_t buffer_data = smallBuffer_sp->GetValueAsUnsigned(invalidRepr);
        if (invalidRepr == buffer_data)
            return false;
        
        uint8_t bytes[] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00};
        uint64_t mask = 0x00000000000000FF;
        uint8_t shift = 0;
        uint8_t pos = 0;
        for(; pos < 7; pos++, mask<<=8, shift+=8)
        {
            auto val = (uint8_t)((mask & buffer_data) >> shift);
            if (0xFF == val)
                break;
            bytes[pos] = val;
        }
        
        DataExtractor data(bytes, 7, process_sp->GetByteOrder(), process_sp->GetAddressByteSize());
        
        StringPrinter::ReadBufferAndDumpToStreamOptions options;
        options.SetData(data)
        .SetEscapeNonPrintables(true)
        .SetPrefixToken(0)
        .SetQuote('"')
        .SetStream(&stream)
        .SetSourceSize(pos)
        .SetLanguage(lldb::eLanguageTypeSwift);
        
        return StringPrinter::ReadBufferAndDumpToStream<StringPrinter::StringElementType::UTF8>(options);
    }
    return false;
}

bool
lldb_private::formatters::swift::UnicodeScalar_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g_value("_value");
    ValueObjectSP value_sp(valobj.GetChildMemberWithName(g_value,true));
    if (!value_sp)
        return false;
    return Char32SummaryProvider(*value_sp.get(), stream, options);
}

bool
lldb_private::formatters::swift::StringCore_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    return StringCore_SummaryProvider(valobj, stream, options, StringPrinter::ReadStringAndDumpToStreamOptions());
}

bool
lldb_private::formatters::swift::StringCore_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& summary_options, StringPrinter::ReadStringAndDumpToStreamOptions read_options)
{
    static ConstString g_some("some");
    static ConstString g__baseAddress("_baseAddress");
    static ConstString g__countAndFlags("_countAndFlags");
    static ConstString g_value("_value");
    static ConstString g__rawValue("_rawValue");

    ProcessSP process_sp(valobj.GetProcessSP());
    if (!process_sp)
        return false;
    ValueObjectSP baseAddress_sp(valobj.GetChildAtNamePath({ g__baseAddress, g_some, g__rawValue }));
    ValueObjectSP _countAndFlags_sp(valobj.GetChildAtNamePath({ g__countAndFlags, g_value }));
    
    if (!_countAndFlags_sp)
        return false;
    
    lldb::addr_t baseAddress = baseAddress_sp ? baseAddress_sp->GetValueAsUnsigned(LLDB_INVALID_ADDRESS) : 0;
    InferiorSizedWord _countAndFlags = InferiorSizedWord(_countAndFlags_sp->GetValueAsUnsigned(0),*process_sp.get());
    
    if (baseAddress == LLDB_INVALID_ADDRESS)
        return false;
    
    bool hasCocoaBuffer = (_countAndFlags << 1).IsNegative();
    
    if (baseAddress == 0)
    {
        if (hasCocoaBuffer)
        {
            static ConstString g__owner("_owner");
            static ConstString g_Some("some");
            static ConstString g_instance_type("instance_type");
            
            ValueObjectSP dyn_inst_type0(valobj.GetChildAtNamePath({g__owner,g_Some,g_instance_type}));
            if (!dyn_inst_type0)
                return false;
            lldb::addr_t dyn_inst_type0_ptr = dyn_inst_type0->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
            if (dyn_inst_type0_ptr == 0 || dyn_inst_type0_ptr == LLDB_INVALID_ADDRESS)
                return false;
            
            InferiorSizedWord dataAddress_isw = InferiorSizedWord(dyn_inst_type0_ptr, *process_sp.get());
            
            DataExtractor id_ptr = dataAddress_isw.GetAsData(process_sp->GetByteOrder());
            CompilerType id_type = process_sp->GetTarget().GetScratchClangASTContext()->GetBasicType(lldb::eBasicTypeObjCID);
            
            if (!id_type)
                return false;
            
            ValueObjectSP nsstringhere_sp = ValueObject::CreateValueObjectFromData("nsstringhere", id_ptr, valobj.GetExecutionContextRef(), id_type);
            if (nsstringhere_sp)
                return NSStringSummaryProvider(*nsstringhere_sp.get(), stream, summary_options);
            return false;
        }
        else
        {
            stream.Printf("\"\"");
            return true;
        }
    }
    
    const InferiorSizedWord _countMask = InferiorSizedWord::GetMaximum(*process_sp.get()) >> 2;
    
    uint64_t count = (_countAndFlags & _countMask).GetValue();
    
    bool isASCII = ((_countAndFlags >> (_countMask.GetBitSize() - 1)).SignExtend() << 8).IsZero();
    
    if (count == 0)
    {
        stream.Printf("\"\"");
        return true;
    }
    
    read_options.SetLocation(baseAddress);
    read_options.SetProcessSP(process_sp);
    read_options.SetStream(&stream);
    read_options.SetSourceSize(count);
    read_options.SetNeedsZeroTermination(false);
    read_options.SetIgnoreMaxLength(summary_options.GetCapping() == lldb::eTypeSummaryUncapped);
    read_options.SetBinaryZeroIsTerminator(false);
    read_options.SetLanguage(lldb::eLanguageTypeSwift);
    
    if (isASCII)
        return StringPrinter::ReadStringAndDumpToStream<StringPrinter::StringElementType::UTF8>(read_options);
    else
        return StringPrinter::ReadStringAndDumpToStream<StringPrinter::StringElementType::UTF16>(read_options);
}

bool
lldb_private::formatters::swift::String_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    return String_SummaryProvider(valobj, stream, options, StringPrinter::ReadStringAndDumpToStreamOptions());
}

bool
lldb_private::formatters::swift::String_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& summary_options, StringPrinter::ReadStringAndDumpToStreamOptions read_options)
{
    static ConstString g_core("_core");
    ValueObjectSP core_sp = valobj.GetChildMemberWithName(g_core, true);
    if (core_sp)
        return StringCore_SummaryProvider(*core_sp, stream, summary_options, read_options);
    return false;
}

bool
lldb_private::formatters::swift::StaticString_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    return StaticString_SummaryProvider(valobj, stream, options, StringPrinter::ReadStringAndDumpToStreamOptions());
}

bool
lldb_private::formatters::swift::StaticString_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& summary_options, StringPrinter::ReadStringAndDumpToStreamOptions read_options)
{
    static ConstString g__startPtrOrData("_startPtrOrData");
    static ConstString g__byteSize("_utf8CodeUnitCount");
    static ConstString g__flags("_flags");

    ValueObjectSP flags_sp(valobj.GetChildMemberWithName(g__flags, true));
    if (!flags_sp)
        return false;
    
    ProcessSP process_sp(valobj.GetProcessSP());
    if (!process_sp)
        return false;
    
    // 0 == pointer representation
    InferiorSizedWord flags(flags_sp->GetValueAsUnsigned(0), *process_sp);
    if (0 != (flags & 0x1).GetValue())
        return false;
    
    ValueObjectSP startptr_sp(valobj.GetChildMemberWithName(g__startPtrOrData, true));
    ValueObjectSP bytesize_sp(valobj.GetChildMemberWithName(g__byteSize, true));
    if (!startptr_sp || !bytesize_sp)
        return false;
    
    lldb::addr_t start_ptr = startptr_sp->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
    uint64_t size = bytesize_sp->GetValueAsUnsigned(0);
    
    if (start_ptr == LLDB_INVALID_ADDRESS || start_ptr == 0)
        return false;
    
    if (size == 0)
    {
        stream.Printf("\"\"");
        return true;
    }
    
    read_options.SetProcessSP(process_sp);
    read_options.SetLocation(start_ptr);
    read_options.SetSourceSize(size);
    read_options.SetBinaryZeroIsTerminator(false);
    read_options.SetNeedsZeroTermination(false);
    read_options.SetStream(&stream);
    read_options.SetIgnoreMaxLength(summary_options.GetCapping() == lldb::eTypeSummaryUncapped);
    read_options.SetLanguage(lldb::eLanguageTypeSwift);
    
    return StringPrinter::ReadStringAndDumpToStream<StringPrinter::StringElementType::UTF8>(read_options);
}

bool
lldb_private::formatters::swift::NSContiguousString_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g_StringCoreType("_TtVs11_StringCore");
    lldb::addr_t core_location = valobj.GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
    if (core_location == LLDB_INVALID_ADDRESS)
        return false;
    ProcessSP process_sp(valobj.GetProcessSP());
    if (!process_sp)
        return false;
    size_t ptr_size = process_sp->GetAddressByteSize();
    core_location += 2*ptr_size;

    Error error;
    
    InferiorSizedWord isw_1(process_sp->ReadPointerFromMemory(core_location, error),*process_sp);
    InferiorSizedWord isw_2(process_sp->ReadPointerFromMemory(core_location+ptr_size, error),*process_sp);
    InferiorSizedWord isw_3(process_sp->ReadPointerFromMemory(core_location+ptr_size+ptr_size, error),*process_sp);
    
    DataBufferSP buffer_sp(new DataBufferHeap(3*ptr_size, 0));
    uint8_t* buffer = buffer_sp->GetBytes();
    
    buffer = isw_1.CopyToBuffer(buffer);
    buffer = isw_2.CopyToBuffer(buffer);
    buffer = isw_3.CopyToBuffer(buffer);

    DataExtractor data(buffer_sp, process_sp->GetByteOrder(), ptr_size);

    SwiftASTContext* lldb_swift_ast = process_sp->GetTarget().GetScratchSwiftASTContext(error);
    if (!lldb_swift_ast)
        return false;
    CompilerType string_core_type = lldb_swift_ast->GetTypeFromMangledTypename(g_StringCoreType.GetCString(), error);
    if (string_core_type.IsValid() == false)
        return false;
    
    ValueObjectSP string_core_sp = ValueObject::CreateValueObjectFromData("stringcore", data, valobj.GetExecutionContextRef(), string_core_type);
    if (string_core_sp)
        return StringCore_SummaryProvider(*string_core_sp, stream, options);
    return false;
}

bool
lldb_private::formatters::swift::Bool_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g_value("_value");
    ValueObjectSP value_child(valobj.GetChildMemberWithName(g_value, true));
    if (!value_child)
        return false;
    auto value = value_child->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
    switch (value)
    {
        case 0:
            stream.Printf("false");
            return true;
        case 1:
            stream.Printf("true");
            return true;
        case LLDB_INVALID_ADDRESS:
            return false;
        default:
            stream.Printf("<invalid> (0x%" PRIx8 ")",(uint8_t)value);
            return true;
    }
}

bool
lldb_private::formatters::swift::DarwinBoolean_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g__value("_value");
    ValueObjectSP value_child(valobj.GetChildMemberWithName(g__value, true));
    if (!value_child)
        return false;
    auto value = value_child->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
    switch (value)
    {
        case 0:
            stream.Printf("false");
            return true;
        default:
            stream.Printf("true");
            return true;
    }
}

bool
lldb_private::formatters::swift::Range_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g_startIndex("startIndex");
    static ConstString g_endIndex("endIndex");
    
    ValueObjectSP startIndex_sp(valobj.GetChildMemberWithName(g_startIndex, true));
    ValueObjectSP endIndex_sp(valobj.GetChildMemberWithName(g_endIndex, true));
    
    if (!startIndex_sp || !endIndex_sp)
        return false;
    
    startIndex_sp = startIndex_sp->GetQualifiedRepresentationIfAvailable(lldb::eDynamicDontRunTarget, true);
    endIndex_sp = endIndex_sp->GetQualifiedRepresentationIfAvailable(lldb::eDynamicDontRunTarget, true);
    
    auto start_summary = startIndex_sp->GetValueAsCString();
    auto end_summary = endIndex_sp->GetValueAsCString();
    
    // the Range should not have a summary unless both start and end indices have one - or it will look awkward
    if (!start_summary || !start_summary[0] || !end_summary || !end_summary[0])
        return false;
    
    stream.Printf("%s..<%s",start_summary, end_summary);
    
    return true;
}

bool
lldb_private::formatters::swift::StridedRangeGenerator_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g__bounds("_bounds");
    static ConstString g__stride("_stride");
    
    ValueObjectSP bounds_sp(valobj.GetChildMemberWithName(g__bounds,true));
    ValueObjectSP stride_sp(valobj.GetChildMemberWithName(g__stride,true));
    
    if (!bounds_sp || !stride_sp)
        return false;
    
    auto bounds_summary = bounds_sp->GetSummaryAsCString();
    auto stride_summary = stride_sp->GetValueAsCString();
    
    if (!bounds_summary || !bounds_summary[0] || !stride_summary || !stride_summary[0])
        return false;
    
    stream.Printf("(%s).by(%s)", bounds_summary, stride_summary);
    
    return true;
}

bool
lldb_private::formatters::swift::BuiltinObjC_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    stream.Printf("0x%" PRIx64 " ",valobj.GetValueAsUnsigned(0));
    stream.Printf("%s",valobj.GetObjectDescription());
    return true;
}

namespace lldb_private {
    namespace formatters {
        namespace swift {
            class EnumSyntheticFrontEnd : public SyntheticChildrenFrontEnd
            {
            public:
                EnumSyntheticFrontEnd (lldb::ValueObjectSP valobj_sp);
                
                virtual size_t
                CalculateNumChildren ();
                
                virtual lldb::ValueObjectSP
                GetChildAtIndex (size_t idx);
                
                virtual bool
                Update();
                
                virtual bool
                MightHaveChildren ();
                
                virtual size_t
                GetIndexOfChildWithName (const ConstString &name);
                
                virtual
                ~EnumSyntheticFrontEnd () = default;
            private:
                
                ExecutionContextRef m_exe_ctx_ref;
                ConstString m_element_name;
                size_t m_child_index;
            };
        }
    }
}

lldb_private::formatters::swift::EnumSyntheticFrontEnd::EnumSyntheticFrontEnd (lldb::ValueObjectSP valobj_sp) :
SyntheticChildrenFrontEnd(*valobj_sp.get()),
m_exe_ctx_ref(),
m_element_name(nullptr),
m_child_index(UINT32_MAX)
{
    if (valobj_sp)
        Update();
}

size_t
lldb_private::formatters::swift::EnumSyntheticFrontEnd::CalculateNumChildren ()
{
    return m_child_index != UINT32_MAX ? 1 : 0;
}

lldb::ValueObjectSP
lldb_private::formatters::swift::EnumSyntheticFrontEnd::GetChildAtIndex (size_t idx)
{
    if (idx)
        return ValueObjectSP();
    if (m_child_index == UINT32_MAX)
        return ValueObjectSP();
    return m_backend.GetChildAtIndex(m_child_index, true);
}

bool
lldb_private::formatters::swift::EnumSyntheticFrontEnd::Update()
{
    m_element_name.Clear();
    m_child_index = UINT32_MAX;
    m_exe_ctx_ref = m_backend.GetExecutionContextRef();
    m_element_name.SetCString(m_backend.GetValueAsCString());
    m_child_index = m_backend.GetIndexOfChildWithName(m_element_name);
    return false;
}

bool
lldb_private::formatters::swift::EnumSyntheticFrontEnd::MightHaveChildren ()
{
    return m_child_index != UINT32_MAX;
}

size_t
lldb_private::formatters::swift::EnumSyntheticFrontEnd::GetIndexOfChildWithName (const ConstString &name)
{
    if (name == m_element_name)
        return 0;
    return UINT32_MAX;
}

SyntheticChildrenFrontEnd*
lldb_private::formatters::swift::EnumSyntheticFrontEndCreator (CXXSyntheticChildren*, lldb::ValueObjectSP valobj_sp)
{
    if (!valobj_sp)
        return NULL;
    return (new EnumSyntheticFrontEnd(valobj_sp));
}

bool
lldb_private::formatters::swift::Metadata_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    ProcessSP process_sp(valobj.GetProcessSP());
    if (!process_sp)
        return false;
    SwiftLanguageRuntime *runtime(process_sp->GetSwiftLanguageRuntime());
    if (!runtime)
        return false;
    SwiftLanguageRuntime::MetadataSP metadata_sp(runtime->GetMetadataForLocation(valobj.GetValueAsUnsigned(0)));
    if (!metadata_sp)
        return false;
    if (SwiftLanguageRuntime::NominalTypeMetadata *nominal_type_metadata = llvm::dyn_cast<SwiftLanguageRuntime::NominalTypeMetadata>(metadata_sp.get()))
    {
        stream.Printf("typename=%s",nominal_type_metadata->GetMangledName().c_str());
        Mangled mangled( ConstString(nominal_type_metadata->GetMangledName().c_str()) );
        ConstString demangled = mangled.GetDemangledName(lldb::eLanguageTypeSwift);
        if (demangled)
            stream.Printf(" --> %s", demangled.GetCString());
        return true;
    }
    if (SwiftLanguageRuntime::TupleMetadata *tuple_metadata = llvm::dyn_cast<SwiftLanguageRuntime::TupleMetadata>(metadata_sp.get()))
    {
        stream.Printf("tuple with %zu items",tuple_metadata->GetNumElements());
        return true;
    }
    // what about other types? not sure
    return false;
}

bool
lldb_private::formatters::swift::ObjC_Selector_SummaryProvider (ValueObject& valobj, Stream& stream, const TypeSummaryOptions& options)
{
    static ConstString g_ptr("ptr");
    static ConstString g__rawValue("_rawValue");
    
    ValueObjectSP ptr_sp(valobj.GetChildAtNamePath({ g_ptr, g__rawValue }));
    if (!ptr_sp)
        return false;
    
    auto ptr_value = ptr_sp->GetValueAsUnsigned(LLDB_INVALID_ADDRESS);
    
    if (0 == ptr_value || LLDB_INVALID_ADDRESS == ptr_value)
        return false;
    
    StringPrinter::ReadStringAndDumpToStreamOptions read_options;
    read_options.SetLocation(ptr_value)
    .SetProcessSP(valobj.GetProcessSP())
    .SetStream(&stream)
    .SetQuote('"')
    .SetNeedsZeroTermination(true)
    .SetLanguage(lldb::eLanguageTypeSwift);

    return StringPrinter::ReadStringAndDumpToStream<StringPrinter::StringElementType::ASCII>(read_options);
}
