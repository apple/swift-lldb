//===-- SwiftASTContext.cpp -------------------------------------*- C++ -*-===//
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

#include "lldb/Symbol/SwiftASTContext.h"

// C++ Includes
#include <mutex> // std::once
#include <queue>
#include <set>

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Target/TargetOptions.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclObjC.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DebuggerClient.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/Mangle.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/SearchPathOptions.h"
#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "swift/ASTSectionImporter/ASTSectionImporter.h"
#include "swift/Basic/Demangle.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangImporterOptions.h"
#include "swift/Driver/Util.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/SIL/SILModule.h"

#include "swift/../../lib/IRGen/GenEnum.h"
#include "swift/../../lib/IRGen/FixedTypeInfo.h"
#include "swift/../../lib/IRGen/GenHeap.h"
#include "swift/../../lib/IRGen/IRGenModule.h"
#include "swift/../../lib/IRGen/TypeInfo.h"
#include "swift/../../lib/IRGen/Linking.h"

#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Strings.h"

#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/Error.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/Section.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/ThreadSafeDenseMap.h"
#include "Plugins/ExpressionParser/Swift/SwiftUserExpression.h"
#include "lldb/Host/FileSpec.h"
#include "lldb/Host/Host.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/StringConvert.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/Platform.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/SwiftLanguageRuntime.h"
#include "lldb/Target/Target.h"
#include "lldb/Utility/CleanUp.h"
#include "lldb/Utility/LLDBAssert.h"

#include "Plugins/SymbolFile/DWARF/DWARFASTParserSwift.h"

#ifdef LLDB_CONFIGURATION_DEBUG
#define VALID_OR_RETURN(value) do { lldbassert (!HasFatalErrors()); if (HasFatalErrors()) { return (value); } } while (0)
#define VALID_OR_RETURN_VOID() do { lldbassert (!HasFatalErrors()); if (HasFatalErrors()) { return; } } while (0)
#else
#define VALID_OR_RETURN(value) do { if (HasFatalErrors()) { return (value); } } while (0)
#define VALID_OR_RETURN_VOID() do { if (HasFatalErrors()) { return; } } while (0)
#endif

using namespace lldb;
using namespace lldb_private;

typedef lldb_private::ThreadSafeDenseMap<swift::ASTContext *, SwiftASTContext *> ThreadSafeSwiftASTMap;

// FIXME: We should add prototypes for all the VisitNode* static functions so we can move
// code around without breaking the compile...
static swift::TypeBase *FixCallingConv (swift::Decl *in_decl, swift::TypeBase *in_type);

static ThreadSafeSwiftASTMap &
GetASTMap()
{
    // The global destructor list will tear down all of the modules when the LLDB
    // shared library is being unloaded and this needs to live beyond all of those
    // and not be destructed before they have all gone away. So we will leak this
    // list intentionally so we can avoid global destructor problems.
    static ThreadSafeSwiftASTMap *g_map_ptr = NULL;
    static std::once_flag g_once_flag;
    std::call_once(g_once_flag, [](){
        g_map_ptr = new ThreadSafeSwiftASTMap(); // NOTE: Intentional leak
    });
    return *g_map_ptr;
}

static inline swift::Type
GetSwiftType (void* opaque_ptr)
{
    return swift::Type((swift::TypeBase *)opaque_ptr);
}

static inline swift::CanType
GetCanonicalSwiftType (void* opaque_ptr)
{
    return swift::Type(((swift::TypeBase *)opaque_ptr)->getDesugaredType())->getCanonicalType();
}

static inline swift::Type
GetSwiftType (CompilerType type)
{
    return swift::Type((swift::TypeBase *)type.GetOpaqueQualType());
}

static inline swift::CanType
GetCanonicalSwiftType (CompilerType type)
{
    return swift::Type(((swift::TypeBase *)type.GetOpaqueQualType())->getDesugaredType())->getCanonicalType();
}

enum class MemberType : uint32_t {
    Invalid,
    BaseClass,
    Field
};

static const char *
MemberTypeToCString (MemberType member_type)
{
    switch (member_type)
    {
        case MemberType::Invalid:       return "invalid";
        case MemberType::BaseClass:     return "base class";
        case MemberType::Field:         return "field";
    }
    return "???";
}

struct MemberInfo {
    CompilerType clang_type;
    lldb_private::ConstString name;
    uint64_t byte_size;
    uint32_t byte_offset;
    MemberType member_type;
    bool is_fragile;
    
    MemberInfo(MemberType member_type) :
    clang_type(),
    name(),
    byte_size(0),
    byte_offset(0),
    member_type(member_type),
    is_fragile(false)
    {
    }
    
    void
    Dump (uint32_t idx)
    {
        printf("[%i] %12s +%u (%s) %s <%" PRIu64 "> %s\n",
               idx,
               MemberTypeToCString(member_type),
               byte_offset,
               clang_type.GetTypeName().AsCString("<no type name>"),
               name.AsCString("<NULL>"),
               byte_size,
               is_fragile ? "[fragile]" : "");
    }
};

struct CachedMemberInfo {
    std::vector<MemberInfo> member_infos;
};

struct EnumElementInfo {
    CompilerType clang_type;
    lldb_private::ConstString name;
    uint64_t byte_size;
    uint32_t value;         // The value for this enumeration element
    uint32_t extra_value;   // If not UINT32_MAX, then this value is an extra value
    // that appears at offset 0 to tell one or more empty
    // enums apart. This value will only be filled in if there
    // are one ore more enum elements that have a non-zero byte size
    
    EnumElementInfo() :
    clang_type(),
    name(),
    byte_size(0),
    extra_value(UINT32_MAX)
    {
    }
    
    void
    Dump (Stream &strm) const
    {
        strm.Printf("<%2" PRIu64 "> %4u", byte_size, value);
        if (extra_value != UINT32_MAX)
            strm.Printf("%4u: ", extra_value);
        else
            strm.Printf("    : ");
        strm.Printf("case %s",
                    name.GetCString());
        if (clang_type)
            strm.Printf("%s", clang_type.GetTypeName().AsCString("<no type name>"));
        strm.EOL();
    }
};

class SwiftEnumDescriptor;

typedef std::shared_ptr<CachedMemberInfo> CachedMemberInfoSP;
typedef std::shared_ptr<SwiftEnumDescriptor> SwiftEnumDescriptorSP;
typedef llvm::DenseMap<lldb::opaque_compiler_type_t, CachedMemberInfoSP> MemberInfoCache;
typedef llvm::DenseMap<lldb::opaque_compiler_type_t, SwiftEnumDescriptorSP> EnumInfoCache;
typedef std::shared_ptr<MemberInfoCache> MemberInfoCacheSP;
typedef std::shared_ptr<EnumInfoCache> EnumInfoCacheSP;
typedef llvm::DenseMap<const swift::ASTContext *, MemberInfoCacheSP> ASTMemberInfoCacheMap;
typedef llvm::DenseMap<const swift::ASTContext *, EnumInfoCacheSP> ASTEnumInfoCacheMap;

static MemberInfoCache *
GetMemberInfoCache (const swift::ASTContext *a)
{
    static ASTMemberInfoCacheMap g_cache;
    static Mutex g_mutex;
    Mutex::Locker locker(g_mutex);
    ASTMemberInfoCacheMap::iterator pos = g_cache.find(a);
    if (pos == g_cache.end())
    {
        g_cache.insert(std::make_pair(a, std::shared_ptr<MemberInfoCache>(new MemberInfoCache())));
        return g_cache.find(a)->second.get();
    }
    return pos->second.get();
}

static EnumInfoCache *
GetEnumInfoCache (const swift::ASTContext *a)
{
    static ASTEnumInfoCacheMap g_cache;
    static Mutex g_mutex;
    Mutex::Locker locker(g_mutex);
    ASTEnumInfoCacheMap::iterator pos = g_cache.find(a);
    if (pos == g_cache.end())
    {
        g_cache.insert(std::make_pair(a, std::shared_ptr<EnumInfoCache>(new EnumInfoCache())));
        return g_cache.find(a)->second.get();
    }
    return pos->second.get();
}

CachedMemberInfo *
SwiftASTContext::GetCachedMemberInfo (void* type)
{
    if (type)
    {
        //printf("CompilerType::GetCachedMemberInfo () for %s...", GetTypeName().c_str());
        bool is_class = false;
        bool is_protocol = false;
        MemberInfoCache *member_info_cache = GetMemberInfoCache (GetASTContext());
        MemberInfoCache::const_iterator pos = member_info_cache->find(type);
        if (pos != member_info_cache->end())
        {
            //printf("cached: %p\n", pos->second.get());
            return pos->second.get();
        }
        
        CachedMemberInfoSP member_infos_sp(new CachedMemberInfo());
        //printf("creating in %p\n", member_infos_sp.get());
        
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        std::vector<const swift::irgen::TypeInfo *> field_type_infos;
        swift::irgen::LayoutStrategy layout_strategy = swift::irgen::LayoutStrategy::Optimal;
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Error:
            case swift::TypeKind::BuiltinInteger:
            case swift::TypeKind::BuiltinFloat:
            case swift::TypeKind::BuiltinRawPointer:
            case swift::TypeKind::BuiltinBridgeObject:
            case swift::TypeKind::BuiltinNativeObject:
            case swift::TypeKind::BuiltinUnsafeValueBuffer:
            case swift::TypeKind::BuiltinUnknownObject:
            case swift::TypeKind::BuiltinVector:
            case swift::TypeKind::NameAlias:
            case swift::TypeKind::Paren:
            case swift::TypeKind::UnownedStorage:
            case swift::TypeKind::WeakStorage:
            case swift::TypeKind::UnmanagedStorage:
            case swift::TypeKind::GenericTypeParam:
            case swift::TypeKind::AssociatedType:
            case swift::TypeKind::DependentMember:
            case swift::TypeKind::Optional:
            case swift::TypeKind::ImplicitlyUnwrappedOptional:
            case swift::TypeKind::Metatype:
            case swift::TypeKind::Module:
            case swift::TypeKind::Substituted:
            case swift::TypeKind::Function:
            case swift::TypeKind::GenericFunction:
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::ArraySlice:
            case swift::TypeKind::LValue:
            case swift::TypeKind::UnboundGeneric:
            case swift::TypeKind::Enum:
            case swift::TypeKind::BoundGenericEnum:
            case swift::TypeKind::ExistentialMetatype:
            case swift::TypeKind::DynamicSelf:
            case swift::TypeKind::SILBox:
            case swift::TypeKind::SILFunction:
            case swift::TypeKind::SILBlockStorage:
            case swift::TypeKind::InOut:
            case swift::TypeKind::Unresolved:
                assert(false && "Caller must only call this function with valid type_kind");
                break;
                
            case swift::TypeKind::Tuple:
            {
                layout_strategy = swift::irgen::LayoutStrategy::Universal;
                
                swift::TupleType *tuple_type = swift_can_type->getAs<swift::TupleType>();
                for (auto tuple_field : tuple_type->getElements())
                {
                    MemberInfo member_info(MemberType::Field);
                    member_info.clang_type = CompilerType(GetASTContext(), tuple_field.getType().getPointer());
                    member_info.byte_size = member_info.clang_type.GetByteSize(nullptr);
                    
                    const char *tuple_name = tuple_field.getName().get();
                    if (tuple_name)
                    {
                        member_info.name.SetCString(tuple_name);
                    }
                    else
                    {
                        StreamString tuple_name_strm;
                        tuple_name_strm.Printf("%u", (uint32_t)member_infos_sp->member_infos.size());
                        member_info.name.SetCString(tuple_name_strm.GetString().c_str());
                    }
                    
                    field_type_infos.push_back(GetSwiftTypeInfo(member_info.clang_type.GetOpaqueQualType()));
                    assert(field_type_infos.back() != nullptr);
                    member_infos_sp->member_infos.push_back(member_info);
                }
            }
                break;
                
            case swift::TypeKind::Protocol:
            case swift::TypeKind::ProtocolComposition:
            {
                ProtocolInfo protocol_info;
                if (!GetProtocolTypeInfo(CompilerType(GetASTContext(), GetSwiftType(type)),
                                         protocol_info))
                    break;
                is_protocol = true;
                uint32_t num_children = protocol_info.m_num_storage_words;
                if (protocol_info.IsOneWordStorage())
                    protocol_info.m_num_protocols = 0;
                
                for (uint32_t idx = 0;
                     idx < num_children;
                     idx++)
                {
                    MemberInfo member_info(MemberType::Field);
                    member_info.clang_type = CompilerType(GetASTContext(),GetASTContext()->TheRawPointerType.getPointer());
                    member_info.byte_size = member_info.clang_type.GetByteSize(nullptr);
                    member_info.byte_offset = idx * member_info.byte_size;
                    member_info.is_fragile = false;
                    StreamString child_name_stream;
                    if (protocol_info.IsOneWordStorage())
                        child_name_stream.Printf("instance_type");
                    else
                    {
                        if (idx < protocol_info.m_num_payload_words)
                            child_name_stream.Printf("payload_data_%u",idx);
                        else
                        {
                            int l_idx = idx - protocol_info.m_num_payload_words;
                            if (l_idx == 0)
                                child_name_stream.Printf("instance_type");
                            else
                                child_name_stream.Printf("protocol_witness_%u",l_idx-1);
                        }
                    }
                    member_info.name = ConstString(child_name_stream.GetData());
                    member_infos_sp->member_infos.push_back(member_info);
                }
            }
                break;
                
            case swift::TypeKind::Struct:
            case swift::TypeKind::Class:
            {
                swift::ClassDecl *class_decl = swift_can_type->getClassOrBoundGenericClass();
                swift::NominalType *nominal_type = swift_can_type->getAs<swift::NominalType>();
                if (nominal_type)
                {
                    swift::NominalTypeDecl *nominal_decl = nominal_type->getDecl();
                    if (nominal_decl)
                    {
                        if (class_decl)
                        {
                            is_class = true;
                            swift::Type superclass_type (class_decl->getSuperclass());
                            if (superclass_type)
                            {
                                MemberInfo member_info(MemberType::BaseClass);
                                member_info.clang_type = CompilerType(GetASTContext(), superclass_type.getPointer());
                                member_info.byte_size = member_info.clang_type.GetByteSize(nullptr);
                                member_info.is_fragile = false;
                                member_info.name.SetCString(member_info.clang_type.GetTypeName().AsCString("<no type name>"));
                                field_type_infos.push_back(GetSwiftTypeInfo(member_info.clang_type.GetOpaqueQualType()));
                                assert(field_type_infos.back() != nullptr);
                                member_infos_sp->member_infos.push_back(member_info);
                            }
                        }
                        
                        for (auto decl : nominal_decl->getMembers())
                        {
                            if (decl->getKind() == swift::DeclKind::Var)
                            {
                                swift::VarDecl *var_decl = llvm::cast<swift::VarDecl>(decl);
                                if (var_decl->hasStorage() && !var_decl->isStatic())
                                {
                                    MemberInfo member_info(MemberType::Field);
                                    member_info.clang_type = CompilerType(GetASTContext(), var_decl->getType().getPointer());
                                    member_info.byte_size = member_info.clang_type.GetByteSize(nullptr);
                                    member_info.is_fragile = is_class; // Class fields are all fragile...
                                    const char *child_name_cstr = var_decl->getName().get();
                                    if (child_name_cstr)
                                        member_info.name.SetCString(child_name_cstr);
                                    field_type_infos.push_back(GetSwiftTypeInfo(member_info.clang_type.GetOpaqueQualType()));
                                    assert(field_type_infos.back() != nullptr);
                                    member_infos_sp->member_infos.push_back(member_info);
                                    
                                }
                            }
                        }
                    }
                }
            }
                break;
                
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::BoundGenericClass:
            {
                swift::ClassDecl *class_decl = swift_can_type->getClassOrBoundGenericClass();
                swift::BoundGenericType *t = swift_can_type->getAs<swift::BoundGenericType>();
                if (t)
                {
                    swift::NominalTypeDecl *t_decl = t->getDecl();
                    if (t_decl)
                    {
                        if (class_decl)
                        {
                            is_class = true;
                            swift::LazyResolver * const lazy_resolver = nullptr;
                            swift::Type superclass_type (t->getSuperclass(lazy_resolver));
                            if (superclass_type)
                            {
                                MemberInfo member_info(MemberType::BaseClass);
                                member_info.clang_type = CompilerType(GetASTContext(), superclass_type.getPointer());
                                member_info.byte_size = member_info.clang_type.GetByteSize(nullptr);
                                // showing somemodule.sometype<A> is confusing to the user because it will show the *unboud* archetype name
                                // even though the type is actually properly bound (or it should!) and since one cannot overload a class
                                // on the number of generic arguments, somemodule.sometype is just as unique
                                member_info.name.SetCString(member_info.clang_type.GetUnboundType().GetTypeName().AsCString("<no type name>"));
                                field_type_infos.push_back(GetSwiftTypeInfo(member_info.clang_type.GetOpaqueQualType()));
                                assert(field_type_infos.back() != nullptr);
                                member_infos_sp->member_infos.push_back(member_info);
                            }
                        }
                        
                        for (auto decl : t_decl->getMembers())
                        {
                            // Find ivars that aren't properties
                            if (decl->getKind() == swift::DeclKind::Var)
                            {
                                swift::VarDecl *var_decl = llvm::cast<swift::VarDecl>(decl);
                                if (var_decl->hasStorage() && !var_decl->isStatic())
                                {
                                    MemberInfo member_info(MemberType::Field);
                                    swift::Type member_type = swift_can_type->getTypeOfMember(t_decl->getModuleContext(),
                                                                                              var_decl,
                                                                                              nullptr);
                                    member_info.clang_type = CompilerType(GetASTContext(), member_type.getPointer());
                                    member_info.byte_size = member_info.clang_type.GetByteSize(nullptr);
                                    member_info.is_fragile = is_class; // Class fields are all fragile...
                                    const char *child_name_cstr = var_decl->getName().get();
                                    if (child_name_cstr)
                                        member_info.name.SetCString(child_name_cstr);
                                    field_type_infos.push_back(GetSwiftTypeInfo(member_info.clang_type.GetOpaqueQualType()));
                                    assert(field_type_infos.back() != nullptr);
                                    member_infos_sp->member_infos.push_back(member_info);
                                }
                            }
                        }
                    }
                }
            }
                break;
                
            case swift::TypeKind::Dictionary:
            {
                swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
                if (t)
                    return GetCachedMemberInfo(t->getSinglyDesugaredType());
            }
                break;
                
            case swift::TypeKind::TypeVariable:
            case swift::TypeKind::Archetype:
                break;
        }
        if (!member_infos_sp->member_infos.empty())
        {
            if (is_class)
            {
                // If we have a class, then all offsets are fragile so we don't need to do layout
                // since we will need to lookup the ivar offset symbol, or munge the runtime data
                // to find the offsets.
            }
            else if (!is_protocol)
            {
                // Only do struct layout if we don't have a union since the only thing we need
                // layout for currently is for the byte offset and the byte offset of everything
                // in a union is zero.
                // As for protocols, their fields are artificially generated from what a protocol_container
                // contains in the Swift runtime itself, and it's just pointers, so no need to get fancy
                swift::irgen::StructLayout layout(GetIRGenModule(),
                                                  swift_can_type,
                                                  swift::irgen::LayoutKind::NonHeapObject,
                                                  layout_strategy,
                                                  field_type_infos);
                
                const size_t num_elements = layout.getElements().size();
                assert(num_elements == member_infos_sp->member_infos.size());
                for (int ii=0; ii<num_elements; ++ii)
                {
                    auto element = layout.getElements()[ii];
                    // check or crash
                    if (element.getKind() == swift::irgen::ElementLayout::Kind::Fixed)
                        member_infos_sp->member_infos[ii].byte_offset = element.getByteOffset().getValue();
                    else
                        member_infos_sp->member_infos[ii].byte_offset = 0; // TODO: dynamic layout
                    //member_infos_sp->member_infos[ii].Dump(ii);
                }
            }
            member_info_cache->insert(std::make_pair(type, member_infos_sp));
            return member_infos_sp.get();
        }
    }
    return nullptr;
}

class SwiftEnumDescriptor
{
public:
    enum class Kind {
        Empty, // no cases in this enum
        CStyle, // no cases have payloads
        AllPayload, // all cases have payloads
        Mixed // some cases have payloads
    };
    
    struct ElementInfo
    {
        lldb_private::ConstString name;
        CompilerType payload_type;
        bool has_payload : 1;
        bool is_indirect : 1;
    };
    
    Kind
    GetKind () const
    {
        return m_kind;
    }
    
    ConstString
    GetTypeName ()
    {
        return m_type_name;
    }
    
    virtual ElementInfo*
    GetElementFromData(const lldb_private::DataExtractor &data) = 0;
    
    virtual size_t
    GetNumElements ()
    {
        return GetNumElementsWithPayload () + GetNumCStyleElements ();
    }
    
    virtual size_t
    GetNumElementsWithPayload () = 0;
    
    virtual size_t
    GetNumCStyleElements () = 0;
    
    virtual ElementInfo*
    GetElementWithPayloadAtIndex(size_t idx) = 0;
    
    virtual ElementInfo*
    GetElementWithNoPayloadAtIndex(size_t idx) = 0;
    
    virtual
    ~SwiftEnumDescriptor() = default;
    
    static SwiftEnumDescriptor*
    CreateDescriptor (swift::ASTContext *ast,
                      swift::CanType swift_can_type,
                      swift::EnumDecl *enum_decl);
    
protected:
    SwiftEnumDescriptor(swift::ASTContext *ast,
                        swift::CanType swift_can_type,
                        swift::EnumDecl *enum_decl,
                        SwiftEnumDescriptor::Kind k) :
    m_kind(k),
    m_type_name()
    {
        if (swift_can_type.getPointer())
        {
            if (auto nominal = swift_can_type->getAnyNominal())
            {
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

class SwiftEmptyEnumDescriptor : public SwiftEnumDescriptor
{
public:
    SwiftEmptyEnumDescriptor(swift::ASTContext *ast,
                             swift::CanType swift_can_type,
                             swift::EnumDecl *enum_decl) :
    SwiftEnumDescriptor(ast,
                        swift_can_type,
                        enum_decl,
                        SwiftEnumDescriptor::Kind::Empty)
    {
    }
    
    virtual ElementInfo*
    GetElementFromData(const lldb_private::DataExtractor &data)
    {
        return nullptr;
    }
    
    virtual size_t
    GetNumElementsWithPayload ()
    {
        return 0;
    }
    
    virtual size_t
    GetNumCStyleElements ()
    {
        return 0;
    }
    
    virtual ElementInfo*
    GetElementWithPayloadAtIndex(size_t idx)
    {
        return nullptr;
    }
    
    virtual ElementInfo*
    GetElementWithNoPayloadAtIndex(size_t idx)
    {
        return nullptr;
    }
    
    static bool classof(const SwiftEnumDescriptor *S) {
        return S->GetKind() == SwiftEnumDescriptor::Kind::Empty;
    }
    
    virtual
    ~SwiftEmptyEnumDescriptor() = default;
};

namespace std
{
    template<> struct less<swift::ClusteredBitVector>
    {
        bool operator() (const swift::ClusteredBitVector& lhs, const swift::ClusteredBitVector& rhs) const
        {
            int iL = lhs.size()-1;
            int iR = rhs.size()-1;
            for(; iL >= 0 && iR >= 0; --iL, --iR)
            {
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

static std::string
Dump (const swift::ClusteredBitVector& bit_vector)
{
    std::string buffer;
    llvm::raw_string_ostream ostream(buffer);
    for (size_t i = 0;
         i < bit_vector.size();
         i++)
    {
        if (bit_vector[i])
            ostream << '1';
        else
            ostream << '0';
        if ( (i % 4) == 3 )
            ostream << ' ';
    }
    ostream.flush();
    return buffer;
}

class SwiftCStyleEnumDescriptor : public SwiftEnumDescriptor
{
public:
    SwiftCStyleEnumDescriptor(swift::ASTContext *ast,
                              swift::CanType swift_can_type,
                              swift::EnumDecl *enum_decl) :
    SwiftEnumDescriptor(ast,
                        swift_can_type,
                        enum_decl,
                        SwiftEnumDescriptor::Kind::CStyle),
    m_nopayload_elems_bitmask(),
    m_elements(),
    m_element_indexes()
    {
        Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TYPES));
        
        if (log)
            log->Printf("doing C-style enum layout for %s", GetTypeName().AsCString());
        
        SwiftASTContext *swift_ast_ctx = SwiftASTContext::GetSwiftASTContext(ast);
        swift::irgen::IRGenModule & irgen_module = swift_ast_ctx->GetIRGenModule();
        const swift::irgen::EnumImplStrategy& enum_impl_strategy = swift::irgen::getEnumImplStrategy(irgen_module, swift_can_type);
        llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element> elements_with_no_payload = enum_impl_strategy.getElementsWithNoPayload();
        const bool has_payload = false;
        const bool is_indirect = false;
        uint64_t case_counter = 0;
        m_nopayload_elems_bitmask = enum_impl_strategy.getBitMaskForNoPayloadElements();
        
        if (log)
            log->Printf("m_nopayload_elems_bitmask = %s", Dump(m_nopayload_elems_bitmask).c_str());
        
        
        for (auto enum_case : elements_with_no_payload)
        {
            ConstString case_name(enum_case.decl->getName().str().data());
            swift::ClusteredBitVector case_value = enum_impl_strategy.getBitPatternForNoPayloadElement(enum_case.decl);
            
            if (log)
                log->Printf("case_name = %s, unmasked value = %s", case_name.AsCString(), Dump(case_value).c_str());
            
            case_value &= m_nopayload_elems_bitmask;
            
            if (log)
                log->Printf("case_name = %s, masked value = %s", case_name.AsCString(), Dump(case_value).c_str());
            
            std::unique_ptr<ElementInfo> elem_info(new ElementInfo{case_name,
                CompilerType(),
                has_payload,
                is_indirect});
            m_element_indexes.emplace(case_counter,elem_info.get());
            case_counter++;
            m_elements.emplace(case_value,std::move(elem_info));
        }
    }
    
    virtual ElementInfo*
    GetElementFromData(const lldb_private::DataExtractor &data)
    {
        Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TYPES));
        
        if (log)
            log->Printf("C-style enum - inspecting data to find enum case for type %s", GetTypeName().AsCString());
        
        swift::ClusteredBitVector current_payload;
        lldb::offset_t offset = 0;
        for (size_t idx = 0;
             idx < data.GetByteSize();
             idx++)
        {
            uint64_t byte = data.GetU8(&offset);
            current_payload.add(8, byte);
        }
        
        if (log)
        {
            log->Printf("m_nopayload_elems_bitmask        = %s", Dump(m_nopayload_elems_bitmask).c_str());
            log->Printf("current_payload                  = %s", Dump(current_payload).c_str());
        }
        
        if (current_payload.size() != m_nopayload_elems_bitmask.size())
        {
            if (log)
                log->Printf("sizes don't match; getting out with an error");
            return nullptr;
        }
        
        current_payload &= m_nopayload_elems_bitmask;
        
        if (log)
            log->Printf("masked current_payload           = %s", Dump(current_payload).c_str());
        
        auto iter = m_elements.find(current_payload), end = m_elements.end();
        if (iter == end)
        {
            if (log)
                log->Printf("bitmask search failed");
            return nullptr;
        }
        if (log)
            log->Printf("bitmask search success - found case %s", iter->second.get()->name.AsCString());
        return iter->second.get();
    }
    
    virtual size_t
    GetNumElementsWithPayload ()
    {
        return 0;
    }
    
    virtual size_t
    GetNumCStyleElements ()
    {
        return m_elements.size();
    }
    
    virtual ElementInfo*
    GetElementWithPayloadAtIndex(size_t idx)
    {
        return nullptr;
    }
    
    virtual ElementInfo*
    GetElementWithNoPayloadAtIndex(size_t idx)
    {
        if (idx >= m_element_indexes.size())
            return nullptr;
        return m_element_indexes[idx];
    }
    
    static bool classof(const SwiftEnumDescriptor *S) {
        return S->GetKind() == SwiftEnumDescriptor::Kind::CStyle;
    }
    
    virtual
    ~SwiftCStyleEnumDescriptor() = default;
private:
    swift::ClusteredBitVector m_nopayload_elems_bitmask;
    std::map<swift::ClusteredBitVector,std::unique_ptr<ElementInfo>> m_elements;
    std::map<uint64_t,ElementInfo*> m_element_indexes;
};

static CompilerType
GetFunctionArgumentTuple (const CompilerType& compiler_type)
{
    if (compiler_type.IsValid() &&
        llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (compiler_type.GetOpaqueQualType()));
        swift::AnyFunctionType *func = llvm::dyn_cast_or_null<swift::AnyFunctionType>(swift_can_type.getPointer());
        if (func)
        {
            swift::TypeBase *input = func->getInput().getPointer();
            if (!input)
                return CompilerType();
            // see comment in swift::AnyFunctionType for rationale here:
            // a function can take either a tuple or a parentype, but if a parentype
            // (i.e. (Foo)), then it will be reduced down to just Foo, so if the input is
            // not a tuple, that must mean there is only 1 input
            swift::TupleType *tuple = llvm::dyn_cast_or_null<swift::TupleType>(input);
            if (tuple)
                return CompilerType(compiler_type.GetTypeSystem(), tuple);
            else
                return CompilerType(compiler_type.GetTypeSystem(), input);
        }
    }
    return CompilerType();
}

class SwiftAllPayloadEnumDescriptor : public SwiftEnumDescriptor
{
public:
    SwiftAllPayloadEnumDescriptor(swift::ASTContext *ast,
                                  swift::CanType swift_can_type,
                                  swift::EnumDecl *enum_decl) :
    SwiftEnumDescriptor(ast,
                        swift_can_type,
                        enum_decl,
                        SwiftEnumDescriptor::Kind::AllPayload),
    m_tag_bits(),
    m_elements()
    {
        Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TYPES));
        
        if (log)
            log->Printf("doing ADT-style enum layout for %s", GetTypeName().AsCString());
        
        SwiftASTContext *swift_ast_ctx = SwiftASTContext::GetSwiftASTContext(ast);
        swift::irgen::IRGenModule & irgen_module = swift_ast_ctx->GetIRGenModule();
        const swift::irgen::EnumImplStrategy& enum_impl_strategy = swift::irgen::getEnumImplStrategy(irgen_module, swift_can_type);
        llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element> elements_with_payload = enum_impl_strategy.getElementsWithPayload();
        m_tag_bits = enum_impl_strategy.getTagBitsForPayloads();
        
        if (log)
            log->Printf("tag_bits = %s", Dump(m_tag_bits).c_str());
        
        auto module_ctx = enum_decl->getModuleContext();
        const bool has_payload = true;
        for (auto enum_case : elements_with_payload)
        {
            ConstString case_name(enum_case.decl->getName().str().data());
            
            swift::EnumElementDecl *case_decl = enum_case.decl;
            assert(case_decl);
            CompilerType case_type(ast,swift_can_type->getTypeOfMember(module_ctx, case_decl, nullptr).getPointer());
            case_type = GetFunctionArgumentTuple(case_type.GetFunctionReturnType());
            
            const bool is_indirect = case_decl->isIndirect();
            
            if (log)
                log->Printf("case_name = %s, type = %s, is_indirect = %s",
                            case_name.AsCString(),
                            case_type.GetTypeName().AsCString(),
                            is_indirect ? "yes" : "no");
            
            std::unique_ptr<ElementInfo> elem_info(new ElementInfo{case_name,
                case_type,
                has_payload,
                is_indirect});
            m_elements.push_back(std::move(elem_info));
        }
    }
    
    virtual ElementInfo*
    GetElementFromData(const lldb_private::DataExtractor &data)
    {
        Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TYPES));
        
        if (log)
            log->Printf("ADT-style enum - inspecting data to find enum case for type %s", GetTypeName().AsCString());
        
        if (m_elements.size() == 0) // no elements, just fail
        {
            if (log)
                log->Printf("enum with no cases. getting out");
            return nullptr;
        }
        if (m_elements.size() == 1) // one element, so it's gotta be it
        {
            if (log)
                log->Printf("enum with one case. getting out easy with %s", m_elements.front().get()->name.AsCString());
            
            return m_elements.front().get();
        }
        
        swift::ClusteredBitVector current_payload;
        lldb::offset_t offset = 0;
        for (size_t idx = 0;
             idx < data.GetByteSize();
             idx++)
        {
            uint64_t byte = data.GetU8(&offset);
            current_payload.add(8, byte);
        }
        if (log)
        {
            log->Printf("tag_bits        = %s", Dump(m_tag_bits).c_str());
            log->Printf("current_payload = %s", Dump(current_payload).c_str());
        }
        
        if (current_payload.size() != m_tag_bits.size())
        {
            if (log)
                log->Printf("sizes don't match; getting out with an error");
            return nullptr;
        }
        
        size_t discriminator = 0;
        size_t power_of_2 = 1;
        auto enumerator = m_tag_bits.enumerateSetBits();
        for(llvm::Optional<size_t> next = enumerator.findNext();
            next.hasValue();
            next = enumerator.findNext())
        {
            discriminator = discriminator + (current_payload[next.getValue()] ? power_of_2 : 0);
            power_of_2 <<= 1;
        }
        
        if (discriminator >= m_elements.size()) // discriminator too large, get out
        {
            if (log)
                log->Printf("discriminator value of %" PRIu64 " too large, getting out", (uint64_t)discriminator);
            return nullptr;
        }
        else
        {
            auto ptr = m_elements[discriminator].get();
            if (log)
            {
                if (!ptr)
                    log->Printf("discriminator value of %" PRIu64 " acceptable, but null case matched - that's bad", (uint64_t)discriminator);
                else
                    log->Printf("discriminator value of %" PRIu64 " acceptable, case %s matched", (uint64_t)discriminator, ptr->name.AsCString());
            }
            return ptr;
        }
    }
    
    virtual size_t
    GetNumElementsWithPayload ()
    {
        return m_elements.size();
    }
    
    virtual size_t
    GetNumCStyleElements ()
    {
        return 0;
    }
    
    virtual ElementInfo*
    GetElementWithPayloadAtIndex(size_t idx)
    {
        if (idx >= m_elements.size())
            return nullptr;
        return m_elements[idx].get();
    }
    
    virtual ElementInfo*
    GetElementWithNoPayloadAtIndex(size_t idx)
    {
        return nullptr;
    }
    
    static bool classof(const SwiftEnumDescriptor *S) {
        return S->GetKind() == SwiftEnumDescriptor::Kind::AllPayload;
    }
    
    virtual
    ~SwiftAllPayloadEnumDescriptor() = default;
private:
    swift::ClusteredBitVector m_tag_bits;
    std::vector<std::unique_ptr<ElementInfo>> m_elements;
};

class SwiftMixedEnumDescriptor : public SwiftEnumDescriptor
{
public:
    SwiftMixedEnumDescriptor(swift::ASTContext *ast,
                             swift::CanType swift_can_type,
                             swift::EnumDecl *enum_decl) :
    SwiftEnumDescriptor(ast,
                        swift_can_type,
                        enum_decl,
                        SwiftEnumDescriptor::Kind::Mixed),
    m_non_payload_cases(ast,swift_can_type,enum_decl),
    m_payload_cases(ast,swift_can_type,enum_decl)
    {
    }
    
    virtual ElementInfo*
    GetElementFromData(const lldb_private::DataExtractor &data)
    {
        ElementInfo* elem_info = m_non_payload_cases.GetElementFromData(data);
        return elem_info ? elem_info : m_payload_cases.GetElementFromData(data);
    }
    
    static bool classof(const SwiftEnumDescriptor *S) {
        return S->GetKind() == SwiftEnumDescriptor::Kind::Mixed;
    }
    
    virtual size_t
    GetNumElementsWithPayload ()
    {
        return m_payload_cases.GetNumElementsWithPayload();
    }
    
    virtual size_t
    GetNumCStyleElements ()
    {
        return m_non_payload_cases.GetNumCStyleElements();
    }
    
    virtual ElementInfo*
    GetElementWithPayloadAtIndex(size_t idx)
    {
        return m_payload_cases.GetElementWithPayloadAtIndex(idx);
    }
    
    virtual ElementInfo*
    GetElementWithNoPayloadAtIndex(size_t idx)
    {
        return m_non_payload_cases.GetElementWithNoPayloadAtIndex(idx);
    }
    
    virtual
    ~SwiftMixedEnumDescriptor() = default;
private:
    SwiftCStyleEnumDescriptor m_non_payload_cases;
    SwiftAllPayloadEnumDescriptor m_payload_cases;
};

SwiftEnumDescriptor*
SwiftEnumDescriptor::CreateDescriptor (swift::ASTContext *ast,
                                       swift::CanType swift_can_type,
                                       swift::EnumDecl *enum_decl)
{
    assert(ast);
    assert(enum_decl);
    assert(swift_can_type.getPointer());
    SwiftASTContext *swift_ast_ctx = SwiftASTContext::GetSwiftASTContext(ast);
    assert(swift_ast_ctx);
    swift::irgen::IRGenModule & irgen_module = swift_ast_ctx->GetIRGenModule();
    const swift::irgen::EnumImplStrategy& enum_impl_strategy = swift::irgen::getEnumImplStrategy(irgen_module, swift_can_type);
    llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element> elements_with_payload = enum_impl_strategy.getElementsWithPayload();
    llvm::ArrayRef<swift::irgen::EnumImplStrategy::Element> elements_with_no_payload = enum_impl_strategy.getElementsWithNoPayload();
    if (elements_with_no_payload.size() == 0)
    {
        // nothing with no payload.. empty or all payloads?
        if (elements_with_payload.size() == 0)
            return new SwiftEmptyEnumDescriptor(ast,swift_can_type,enum_decl);
        else
            return new SwiftAllPayloadEnumDescriptor(ast,swift_can_type,enum_decl);
    }
    else
    {
        // something with no payload.. mixed or C-style?
        if (elements_with_payload.size() == 0)
            return new SwiftCStyleEnumDescriptor(ast,swift_can_type,enum_decl);
        else
            return new SwiftMixedEnumDescriptor(ast,swift_can_type,enum_decl);
    }
}

static SwiftEnumDescriptor*
GetEnumInfoFromEnumDecl (swift::ASTContext *ast,
                         swift::CanType swift_can_type,
                         swift::EnumDecl *enum_decl)
{
    return SwiftEnumDescriptor::CreateDescriptor(ast, swift_can_type, enum_decl);
}

SwiftEnumDescriptor*
SwiftASTContext::GetCachedEnumInfo (void* type)
{
    if (type)
    {
        EnumInfoCache *enum_info_cache = GetEnumInfoCache (GetASTContext());
        EnumInfoCache::const_iterator pos = enum_info_cache->find(type);
        if (pos != enum_info_cache->end())
            return pos->second.get();

        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        if (!SwiftASTContext::IsFullyRealized(CompilerType(GetASTContext(), swift_can_type)))
            return nullptr;
        
        SwiftEnumDescriptorSP enum_info_sp;
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        if (type_kind == swift::TypeKind::Enum)
        {
            swift::EnumType *enum_type = swift_can_type->getAs<swift::EnumType>();
            enum_info_sp.reset(GetEnumInfoFromEnumDecl(GetASTContext(),
                                                       swift_can_type,
                                                       enum_type->getDecl()));
        }
        else if (type_kind == swift::TypeKind::BoundGenericEnum)
        {
            swift::BoundGenericEnumType *bound_enum_type = swift_can_type->getAs<swift::BoundGenericEnumType>();
            if (bound_enum_type)
                enum_info_sp.reset(GetEnumInfoFromEnumDecl(GetASTContext(),
                                                           swift_can_type,
                                                           bound_enum_type->getDecl()));
        }
        
        if (enum_info_sp.get())
            enum_info_cache->insert(std::make_pair(type,enum_info_sp));
        return enum_info_sp.get();
    }
    return nullptr;
}

namespace
{
    static inline bool SwiftASTContextSupportsLanguage (lldb::LanguageType language)
    {
        return language == eLanguageTypeSwift;
    }

    static bool
    IsDeviceSupport(const char *path)
    {
        // The old-style check, which we preserve for safety.
        if (path && strstr(path, "iOS DeviceSupport"))
            return true;

        // The new-style check, which should cover more devices.
        if (path)
            if (const char *Developer_Xcode = strstr(path, "Developer"))
                if (const char *DeviceSupport = strstr(Developer_Xcode, "DeviceSupport"))
                    if (strstr(DeviceSupport, "Symbols"))
                        return true;

        return false;
    }
}

SwiftASTContext::SwiftASTContext (const char *triple, Target *target) :
    TypeSystem (TypeSystem::eKindSwift),
    m_source_manager_ap(),
    m_diagnostic_engine_ap(),
    m_ast_context_ap(),
    m_ir_gen_module_ap(),
    m_compiler_invocation_ap (new swift::CompilerInvocation()),
    m_dwarf_ast_parser_ap (),
    m_scratch_module(NULL),
    m_sil_module_ap(),
    m_serialized_module_loader(NULL),
    m_clang_importer(NULL),
    m_swift_module_cache(),
    m_mangled_name_to_type_map(),
    m_type_to_mangled_name_map(),
    m_pointer_byte_size(0),
    m_pointer_bit_align(0),
    m_void_function_type (),
    m_target_wp (),
    m_process (NULL),
    m_platform_sdk_path (),
    m_resource_dir (),
    m_ast_file_data_map (),
    m_initialized_language_options (false),
    m_initialized_search_path_options (false),
    m_initialized_clang_importer_options (false),
    m_reported_fatal_error (false),
    m_fatal_errors(),
    m_negative_type_cache(),
    m_extra_type_info_cache(),
    m_swift_type_map()
{
    // Set the module-cache path if it has been specified:
    if (target)
    {
        FileSpec &module_cache = target->GetModuleCachePath();
        if (module_cache && module_cache.Exists())
        {

            std::string module_cache_path = module_cache.GetPath();
            llvm::StringRef module_cache_ref(module_cache_path);
            m_compiler_invocation_ap->setClangModuleCachePath (module_cache_ref);
        }
        m_target_wp = target->shared_from_this();
    }

    if (triple)
        SetTriple(triple);
    swift::IRGenOptions &ir_gen_opts = m_compiler_invocation_ap->getIRGenOptions();
    ir_gen_opts.OutputKind = swift::IRGenOutputKind::Module;
    ir_gen_opts.UseJIT = true;
}

SwiftASTContext::SwiftASTContext (const SwiftASTContext &rhs) :
    TypeSystem (rhs.getKind()),
    m_source_manager_ap(),
    m_diagnostic_engine_ap(),
    m_ast_context_ap(),
    m_ir_gen_module_ap(),
    m_compiler_invocation_ap (new swift::CompilerInvocation()),
    m_dwarf_ast_parser_ap (),
    m_scratch_module(NULL),
    m_sil_module_ap(),
    m_serialized_module_loader(NULL),
    m_clang_importer(NULL),
    m_swift_module_cache(),
    m_mangled_name_to_type_map(),
    m_type_to_mangled_name_map(),
    m_pointer_byte_size(0),
    m_pointer_bit_align(0),
    m_void_function_type (),
    m_target_wp (),
    m_process (NULL),
    m_platform_sdk_path (),
    m_resource_dir (),
    m_ast_file_data_map (),
    m_initialized_language_options (false),
    m_initialized_search_path_options (false),
    m_initialized_clang_importer_options (false),
    m_reported_fatal_error (false),
    m_fatal_errors(),
    m_negative_type_cache(),
    m_extra_type_info_cache(),
    m_swift_type_map()
{
    if (rhs.m_compiler_invocation_ap)
    {
        std::string rhs_triple = rhs.GetTriple();
        if (!rhs_triple.empty())
        {
            SetTriple(rhs_triple.c_str());
        }
        llvm::StringRef module_cache_path = rhs.m_compiler_invocation_ap->getClangModuleCachePath();
        if (!module_cache_path.empty())
            m_compiler_invocation_ap->setClangModuleCachePath(module_cache_path);
    }

    swift::IRGenOptions &ir_gen_opts = m_compiler_invocation_ap->getIRGenOptions();
    ir_gen_opts.OutputKind = swift::IRGenOutputKind::Module;
    ir_gen_opts.UseJIT = true;
    
    TargetSP target_sp = rhs.m_target_wp.lock();
    if (target_sp)
        m_target_wp = target_sp;
    
    m_platform_sdk_path = rhs.m_platform_sdk_path;
    m_resource_dir = rhs.m_resource_dir;
    
    swift::ASTContext *lhs_ast = GetASTContext();
    swift::ASTContext *rhs_ast = const_cast<SwiftASTContext &>(rhs).GetASTContext();

    if (lhs_ast && rhs_ast)
    {
        lhs_ast->SearchPathOpts = rhs_ast->SearchPathOpts;
    }
    GetClangImporter();
}

SwiftASTContext::~SwiftASTContext ()
{
    if (m_ast_context_ap.get())
    {
        GetASTMap().Erase(m_ast_context_ap.get());
    }
}

ConstString
SwiftASTContext::GetPluginNameStatic()
{
    return ConstString("swift");
}

ConstString
SwiftASTContext::GetPluginName()
{
    return ClangASTContext::GetPluginNameStatic();
}

uint32_t
SwiftASTContext::GetPluginVersion()
{
    return 1;
}

lldb::TypeSystemSP
SwiftASTContext::CreateInstance (lldb::LanguageType language, Module *module, Target *target, const char *extra_options)
{
    if (SwiftASTContextSupportsLanguage(language))
    {
        ArchSpec arch;
        if (module)
        {
            arch = module->GetArchitecture();

            ObjectFile * objfile = module->GetObjectFile();
            ArchSpec object_arch;

            if (!objfile || !objfile->GetArchitecture(object_arch))
                return TypeSystemSP();
            
            lldb::CompUnitSP main_compile_unit_sp = module->GetCompileUnitAtIndex(0);
            
            Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

            if (main_compile_unit_sp && !main_compile_unit_sp->Exists())
            {
                if (log)
                {
                    StreamString ss;
                    module->GetDescription(&ss);
                    
                    log->Printf ("Corresponding source not found for %s, loading module %s is unlikely to succeed", main_compile_unit_sp->GetCString(), ss.GetData());
                }
            }
                
            std::shared_ptr<SwiftASTContext> swift_ast_sp(new SwiftASTContext());

            swift_ast_sp->GetLanguageOptions().DebuggerSupport = true;
            swift_ast_sp->GetLanguageOptions().EnableAccessControl = false;

            if (!arch.IsValid())
                return TypeSystemSP();

            llvm::Triple triple = arch.GetTriple();

            if (triple.getOS() == llvm::Triple::UnknownOS)
            {
                // cl_kernels are the only binaries that don't have an LC_MIN_VERSION_xxx load command.
                // This avoids a swift assertion.

#if defined(__APPLE__)
                switch (triple.getArch())
                {
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
                // Not an elegant hack on OS X, not an elegant hack elsewheere.
                // But we shouldn't be claiming things are Mac binaries when they are not.
                triple.setOS(HostInfo::GetArchitecture().GetTriple().getOS());
#endif
            }

            swift_ast_sp->SetTriple(triple.getTriple().c_str(), module);

            bool set_triple = false;

            SymbolVendor *sym_vendor = module->GetSymbolVendor();

            std::string resource_dir;
            std::string target_triple;

            if (sym_vendor)
            {
                // Use the new loadFromSerializedAST if possible:

                DataBufferSP ast_file_data_sp = sym_vendor->GetASTData(eLanguageTypeSwift);

                bool got_serialized_options = false;
                if (ast_file_data_sp)
                {
                    if (log)
                        log->Printf ("Found AST file data for library: %s.", module->GetSpecificationDescription().c_str());
                    llvm::StringRef section_data_ref((const char *)ast_file_data_sp->GetBytes(), ast_file_data_sp->GetByteSize());

                    swift::serialization::Status result = swift_ast_sp->GetCompilerInvocation().loadFromSerializedAST(section_data_ref);

                    switch (result)
                    {
                        case swift::serialization::Status::Valid:
                            got_serialized_options = true;
                            break;

                        case swift::serialization::Status::FormatTooOld:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file format is too old to be used by the version of the swift compiler in LLDB");
                            return swift_ast_sp;

                        case swift::serialization::Status::FormatTooNew:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file format is too new to be used by this version of the swift compiler in LLDB");
                            return swift_ast_sp;

                        case swift::serialization::Status::MissingDependency:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file depends on another module that can't be loaded");
                            return swift_ast_sp;

                        case swift::serialization::Status::MissingShadowedModule:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file is an overlay for a clang module, which can't be found");
                            return swift_ast_sp;

                        case swift::serialization::Status::FailedToLoadBridgingHeader:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file depends on a bridging header that can't be loaded");
                            return swift_ast_sp;

                        case swift::serialization::Status::Malformed:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file is malformed");
                            return swift_ast_sp;

                        case swift::serialization::Status::MalformedDocumentation:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module documentation file is malformed in some way");
                            return swift_ast_sp;

                        case swift::serialization::Status::NameMismatch:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file's name does not match the module it is being loaded into");
                            return swift_ast_sp;

                        case swift::serialization::Status::TargetIncompatible:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file was built for a different target platform");
                            return swift_ast_sp;

                        case swift::serialization::Status::TargetTooNew:
                            swift_ast_sp->m_fatal_errors.SetErrorString("the swift module file was built for a target newer than the current target");
                            return swift_ast_sp;
                    }
                }

                // TODO: make sure we only get options for swift files, we really
                // should be passing down a language enumeration into sym_vendor->GetCompileOption()
                // so we don't get compiler options for a C/C++ file...

                if (got_serialized_options)
                {
                    // Some of the bits in the compiler options we keep separately, so we need to populate them from the
                    // serialized options:
                    llvm::StringRef serialized_triple = swift_ast_sp->GetCompilerInvocation().getTargetTriple();
                    if (serialized_triple.empty())
                    {
                        if (log)
                            log->Printf ("\tSerialized triple for %s was empty.", module->GetSpecificationDescription().c_str());
                    }
                    else
                    {
                        if (log)
                            log->Printf ("\tFound serialized triple for %s: %s.", module->GetSpecificationDescription().c_str(),
                                         serialized_triple.data());
                        swift_ast_sp->SetTriple(serialized_triple.data(), module);
                        set_triple = true;
                    }

                    llvm::StringRef serialized_sdk_path = swift_ast_sp->GetCompilerInvocation().getSDKPath();
                    if (serialized_sdk_path.empty())
                    {
                        if (log)
                            log->Printf ("\tNo serialized SDK path.");
                    }
                    else
                    {
                        if (log)
                            log->Printf ("\tGot serialized SDK path %s.", serialized_sdk_path.data());
                        FileSpec sdk_spec (serialized_sdk_path.data(), false);
                        if (sdk_spec.Exists())
                        {
                            swift_ast_sp->SetPlatformSDKPath(serialized_sdk_path.data());
                        }
                    }

                }

                if (!got_serialized_options || !swift_ast_sp->GetPlatformSDKPath())
                {
                    std::string platform_sdk_path;
                    if (sym_vendor->GetCompileOption("-sdk", platform_sdk_path))
                    {
                        FileSpec sdk_spec (platform_sdk_path.c_str(), false);
                        if (sdk_spec.Exists())
                        {
                            swift_ast_sp->SetPlatformSDKPath(platform_sdk_path.c_str());
                        }

                        if (sym_vendor->GetCompileOption("-target", target_triple))
                        {
                            llvm::StringRef parsed_triple(target_triple);

                            swift_ast_sp->SetTriple (target_triple.c_str(), module);
                            set_triple = true;
                        }
                    }
                }

                if (sym_vendor->GetCompileOption("-resource-dir", resource_dir))
                {
                    swift_ast_sp->SetResourceDir(resource_dir.c_str());
                }

                if (!got_serialized_options)
                {

                    std::vector<std::string> framework_search_paths;

                    if (sym_vendor->GetCompileOptions("-F", framework_search_paths))
                    {
                        for (std::string &search_path : framework_search_paths)
                        {
                            swift_ast_sp->AddFrameworkSearchPath(search_path.c_str());
                        }
                    }

                    std::vector<std::string> include_paths;

                    if (sym_vendor->GetCompileOptions("-I", include_paths))
                    {
                        for (std::string &search_path : include_paths)
                        {
                            const FileSpec path_spec(search_path.c_str(), false);

                            if (path_spec.Exists())
                            {
                                static const ConstString s_hmap_extension("hmap");

                                if (path_spec.GetFileType() == FileSpec::FileType::eFileTypeDirectory)
                                {
                                    swift_ast_sp->AddModuleSearchPath(search_path.c_str());
                                }
                                else if (path_spec.GetFileType() == FileSpec::FileType::eFileTypeRegular &&
                                         path_spec.GetFileNameExtension() == s_hmap_extension)
                                {
                                    std::string argument("-I");
                                    argument.append(search_path);
                                    swift_ast_sp->AddClangArgument(argument.c_str());
                                }
                            }
                        }
                    }

                    std::vector<std::string> cc_options;

                    if (sym_vendor->GetCompileOptions("-Xcc", cc_options))
                    {
                        for (int i = 0; i < cc_options.size(); ++i)
                        {
                            if (!cc_options[i].compare("-iquote") &&
                                i + 1 < cc_options.size())
                            {
                                swift_ast_sp->AddClangArgumentPair("-iquote", cc_options[i+1].c_str());
                            }
                        }
                    }
                }

                FileSpecList loaded_modules;

                sym_vendor->GetLoadedModules(lldb::eLanguageTypeSwift, loaded_modules);

                for (size_t mi = 0, me = loaded_modules.GetSize();
                     mi != me;
                     ++mi)
                {
                    const FileSpec &loaded_module = loaded_modules.GetFileSpecAtIndex(mi);
                    
                    if (loaded_module.Exists())
                        swift_ast_sp->AddModuleSearchPath(loaded_module.GetDirectory().GetCString());
                }
            }
            
            if (!set_triple)
            {
                llvm::Triple llvm_triple (swift_ast_sp->GetTriple());
                
                // LLVM wants this to be set to iOS or MacOSX; if we're working on
                // a bare-boards type image, change the triple for llvm's benefit.
                if (llvm_triple.getVendor() == llvm::Triple::Apple && llvm_triple.getOS() == llvm::Triple::UnknownOS)
                {
                    if (llvm_triple.getArch() == llvm::Triple::arm ||
                        llvm_triple.getArch() == llvm::Triple::thumb)
                    {
                        llvm_triple.setOS(llvm::Triple::IOS);
                    }
                    else
                    {
                        llvm_triple.setOS(llvm::Triple::MacOSX);
                    }
                    swift_ast_sp->SetTriple (llvm_triple.str().c_str(), module);
                }
            }
            
            if (!swift_ast_sp->GetClangImporter())
            {
                if (log)
                {
                    log->Printf ("((Module*)%p) [%s]->GetSwiftASTContext() returning NULL - couldn't create a ClangImporter", module, module->GetFileSpec().GetFilename().AsCString("<anonymous>"));
                }
                
                return TypeSystemSP();
            }
            
            std::vector<std::string> module_names;
            swift_ast_sp->RegisterSectionModules(*module, module_names);
            swift_ast_sp->ValidateSectionModules(*module, module_names);
            
            if (log)
            {
                log->Printf ("((Module*)%p) [%s]->GetSwiftASTContext() = %p", module, module->GetFileSpec().GetFilename().AsCString("<anonymous>"), swift_ast_sp.get());
                swift_ast_sp->DumpConfiguration(log);
            }
            return swift_ast_sp;
        }
        else if (target)
        {
            arch = target->GetArchitecture();

            // Make an AST but don't set the triple yet. We need to try and detect
            // if we have a iOS simulator...
            std::shared_ptr<SwiftASTContextForExpressions> swift_ast_sp(new SwiftASTContextForExpressions(*target));

            if (!arch.IsValid())
                return TypeSystemSP();

            bool handled_sdk_path = false;
            bool handled_resource_dir = false;
            const size_t num_images = target->GetImages().GetSize();
            // Set the SDK path and resource dir prior to doing search paths. Otherwise
            // when we create search path options we put in the wrong SDK path.

            FileSpec &target_sdk_spec = target->GetSDKPath();
            if (target_sdk_spec && target_sdk_spec.Exists())
            {
                std::string platform_sdk_path(target_sdk_spec.GetPath());
                swift_ast_sp->SetPlatformSDKPath(std::move(platform_sdk_path));
                handled_sdk_path = true;
            }

            Error module_error;
            for (size_t mi = 0; mi != num_images; ++mi)
            {
                ModuleSP module_sp = target->GetImages().GetModuleAtIndex(mi);

                SwiftASTContext *module_swift_ast = llvm::dyn_cast_or_null<SwiftASTContext>(module_sp->GetTypeSystemForLanguage(lldb::eLanguageTypeSwift));

                if (!module_swift_ast || !module_swift_ast->GetClangImporter())
                {
                    // Make sure we warn about this module load failure, the one that comes from loading types
                    // often gets swallowed up and not seen, this is the only reliable point where we can show this.
                    // But only do it once per UUID so we don't overwhelm the user with warnings...
                    std::unordered_set<std::string> m_swift_warnings_issued;

                    UUID module_uuid(module_sp->GetUUID());
                    std::pair<std::unordered_set<std::string>::iterator, bool> result(m_swift_warnings_issued.insert(module_uuid.GetAsString()));
                    if (result.second)
                    {
                        StreamString ss;
                        module_sp->GetDescription(&ss, eDescriptionLevelBrief);
                        target->GetDebugger().GetErrorFile()->Printf("warning: Swift error in module %s"/*": \n    %s\n"*/
                                                                     "Debug info from this module will be unavailable in the debugger.\n\n",
                                                                     ss.GetData());
                    }
                }

                if (!module_swift_ast)
                    continue;
                
                assert (module_swift_ast->GetClangImporter()); // if this isn't here, then the AST should not have returned as non-NULL

                if (!handled_sdk_path)
                {
                    const char *platform_sdk_path = module_swift_ast->GetPlatformSDKPath ();

                    if (platform_sdk_path)
                    {
                        handled_sdk_path = true;
                        swift_ast_sp->SetPlatformSDKPath (platform_sdk_path);
                    }
                }

                if (!handled_resource_dir)
                {
                    const char *resource_dir = module_swift_ast->GetResourceDir ();
                    if (resource_dir)
                    {
                        handled_resource_dir = true;
                        swift_ast_sp->SetResourceDir (resource_dir);
                    }
                }

                if (handled_sdk_path && handled_resource_dir)
                    break;
            }

            // First, prime the compiler with the options from the main executable:
            bool read_options_from_ast = false;
            ModuleSP exe_module_sp (target->GetExecutableModule());
            if (exe_module_sp)
            {
                SymbolVendor *sym_vendor = exe_module_sp->GetSymbolVendor();
                if (sym_vendor)
                {
                    DataBufferSP ast_data_sp = sym_vendor->GetASTData(eLanguageTypeSwift);
                    if (ast_data_sp)
                    {
                        llvm::StringRef section_data_ref((const char *) ast_data_sp->GetBytes(), ast_data_sp->GetByteSize());
                        swift::serialization::Status result = swift_ast_sp->GetCompilerInvocation().loadFromSerializedAST(section_data_ref);
                        if (result == swift::serialization::Status::Valid)
                        {
                            read_options_from_ast = true;
                        }
                        else
                        {
                            Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
                            if (log)
                                log->Printf("Attempt to load compiler options from Serialized AST failed: %d.", result);
                        }
                    }
                }
            }

            // Now if the user fully specified the triple, let that override the one we got from executable's options:

            if (target->GetArchitecture().IsFullySpecifiedTriple())
            {
                swift_ast_sp->SetTriple(target->GetArchitecture().GetTriple().str().c_str());
            }
            else
            {
                // Always run using the Host OS triple...
                bool set_triple = false;
                PlatformSP platform_sp (target->GetPlatform());
                uint32_t major, minor, update;
                if (platform_sp && platform_sp->GetOSVersion(major, minor, update, target->GetProcessSP().get()))
                {
                    StreamString full_triple_name;
                    full_triple_name.GetString() = std::move(target->GetArchitecture().GetTriple().str());
                    if (major != UINT32_MAX)
                    {
                        full_triple_name.Printf("%u", major);
                        if (minor != UINT32_MAX)
                        {
                            full_triple_name.Printf(".%u", minor);
                            if (update != UINT32_MAX)
                                full_triple_name.Printf(".%u", update);
                        }
                    }
                    swift_ast_sp->SetTriple(full_triple_name.GetString().c_str());
                    set_triple = true;
                }

                if (!set_triple)
                {
                    ModuleSP exe_module_sp (target->GetExecutableModule());
                    if (exe_module_sp)
                    {
                        Error exe_error;
                        SwiftASTContext *exe_swift_ctx = llvm::dyn_cast_or_null<SwiftASTContext>(exe_module_sp->GetTypeSystemForLanguage(lldb::eLanguageTypeSwift));
                        if (exe_swift_ctx)
                        {
                            swift_ast_sp->SetTriple(exe_swift_ctx->GetLanguageOptions().Target.str().c_str());
                        }
                    }
                }
            }

            const bool use_all_compiler_flags = !read_options_from_ast || target->GetUseAllCompilerFlags();

            std::function<void (ModuleSP &&)> process_one_module = [target, &swift_ast_sp, use_all_compiler_flags](ModuleSP &&module_sp)
            {
                const FileSpec &module_file = module_sp->GetFileSpec();

                std::string module_path = module_file.GetPath();

                // Add the containing framework to the framework search path.  Don't do that if this is the
                // executable module, since it might be buried in some framework that we don't care about.
                if (use_all_compiler_flags && target->GetExecutableModulePointer() != module_sp.get())
                {
                    size_t framework_offset = module_path.rfind(".framework/");

                    if (framework_offset != std::string::npos)
                    {
                        while (framework_offset && (module_path[framework_offset] != '/'))
                            framework_offset--;

                        if (module_path[framework_offset] == '/')
                        {
                            // framework_offset now points to the '/';

                            std::string parent_path = module_path.substr(0, framework_offset);

                            if (strncmp(parent_path.c_str(), "/System/Library", strlen("/System/Library")) &&
                                !IsDeviceSupport(parent_path.c_str()))
                            {
                                swift_ast_sp->AddFrameworkSearchPath(parent_path.c_str());
                            }
                        }
                    }
                }

                SymbolVendor *sym_vendor = module_sp->GetSymbolVendor();

                if (sym_vendor)
                {
                    std::vector <std::string> module_names;

                    SymbolFile *sym_file = sym_vendor->GetSymbolFile();
                    if (sym_file)
                    {
                        Error sym_file_error;
                        SwiftASTContext *ast_context = llvm::dyn_cast_or_null<SwiftASTContext>(sym_file->GetTypeSystemForLanguage(lldb::eLanguageTypeSwift));
                        if (ast_context)
                        {
                            if (use_all_compiler_flags || target->GetExecutableModulePointer() == module_sp.get())
                            {
                                for (size_t msi = 0, mse = ast_context->GetNumModuleSearchPaths();
                                     msi < mse;
                                     ++msi)
                                {
                                    const char *search_path = ast_context->GetModuleSearchPathAtIndex(msi);
                                    swift_ast_sp->AddModuleSearchPath(search_path);
                                }

                                for (size_t fsi = 0, fse = ast_context->GetNumFrameworkSearchPaths();
                                     fsi < fse;
                                     ++fsi)
                                {
                                    const char *search_path = ast_context->GetFrameworkSearchPathAtIndex(fsi);
                                    swift_ast_sp->AddFrameworkSearchPath(search_path);
                                }

                                for (size_t osi = 0, ose = ast_context->GetNumClangArguments();
                                     osi < ose;
                                     ++osi)
                                {
                                    const char *clang_argument = ast_context->GetClangArgumentAtIndex(osi);
                                    swift_ast_sp->AddClangArgument(clang_argument, true);
                                }
                            }

                            swift_ast_sp->RegisterSectionModules(*module_sp, module_names);
                        }
                    }
                }
            };

            for (size_t mi = 0; mi != num_images; ++mi)
            {
                process_one_module (target->GetImages().GetModuleAtIndex(mi));
            }

            FileSpecList &framework_search_paths = target->GetSwiftFrameworkSearchPaths();
            FileSpecList &module_search_paths = target->GetSwiftModuleSearchPaths();

            for (size_t fi = 0, fe = framework_search_paths.GetSize(); fi != fe; ++fi)
            {
                swift_ast_sp->AddFrameworkSearchPath(framework_search_paths.GetFileSpecAtIndex(fi).GetPath().c_str());
            }

            for (size_t mi = 0, me = framework_search_paths.GetSize(); mi != me; ++mi)
            {
                swift_ast_sp->AddFrameworkSearchPath(module_search_paths.GetFileSpecAtIndex(mi).GetPath().c_str());
            }
            
            // Now fold any extra options we were passed.  This has to be done BEFORE the
            // ClangImporter is made by calling GetClangImporter or these options will be
            // ignored.
            
            if (extra_options)
            {
                swift::CompilerInvocation &compiler_invocation = swift_ast_sp->GetCompilerInvocation();
                Args extra_args (extra_options);
                llvm::ArrayRef<const char *> extra_args_ref (extra_args.GetArgumentVector(), extra_args.GetArgumentCount());
                compiler_invocation.parseArgs(extra_args_ref, swift_ast_sp->GetDiagnosticEngine());
            }
            
            Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
            
            // this needs to happen once all the import paths are set, or otherwise no modules will be found
            if (!swift_ast_sp->GetClangImporter())
            {
                if (log)
                {
                    log->Printf ("((Target*)%p)->GetSwiftASTContext() returning NULL - couldn't create a ClangImporter", target);
                }
                
                return TypeSystemSP();
            }
            
            if (log)
            {
                log->Printf ("((Target*)%p)->GetSwiftASTContext() = %p", target, swift_ast_sp.get());
                swift_ast_sp->DumpConfiguration(log);
            }
            
            if (swift_ast_sp->HasFatalErrors())
            {
                swift_ast_sp->m_error.SetErrorStringWithFormat("Error creating target Swift AST context: %s", swift_ast_sp->GetFatalErrors().AsCString());
            }
            else
            {
                return swift_ast_sp;
            }
        }
    }
    return lldb::TypeSystemSP();
}

void
SwiftASTContext::EnumerateSupportedLanguages(std::set<lldb::LanguageType> &languages_for_types,
                                             std::set<lldb::LanguageType> &languages_for_expressions)
{
    static std::vector<lldb::LanguageType> s_supported_languages_for_types({
        lldb::eLanguageTypeSwift});
    
    static std::vector<lldb::LanguageType> s_supported_languages_for_expressions({
        lldb::eLanguageTypeSwift});
    
    languages_for_types.insert(s_supported_languages_for_types.begin(), s_supported_languages_for_types.end());
    languages_for_expressions.insert(s_supported_languages_for_expressions.begin(), s_supported_languages_for_expressions.end());
}

void
SwiftASTContext::Initialize()
{
    PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                   "swift AST context plug-in",
                                   CreateInstance,
                                   EnumerateSupportedLanguages);
}

void
SwiftASTContext::Terminate()
{
    PluginManager::UnregisterPlugin (CreateInstance);
}

bool
SwiftASTContext::SupportsLanguage (lldb::LanguageType language)
{
    return SwiftASTContextSupportsLanguage(language);
}

Error
SwiftASTContext::IsCompatible ()
{
    return GetFatalErrors();
}

Error
SwiftASTContext::GetFatalErrors ()
{
    Error error;
    if (HasFatalErrors())
    {
        error = m_fatal_errors;
        if (error.Success())
            error.SetErrorString("unknown fatal error in swift AST context");
    }
    return error;
}



swift::IRGenOptions &
SwiftASTContext::GetIRGenOptions ()
{
    return m_compiler_invocation_ap->getIRGenOptions();
}


std::string
SwiftASTContext::GetTriple () const
{
    return m_compiler_invocation_ap->getTargetTriple();
}

// Conditions a triple string to be safe for use with Swift.
// Right now this just strips the Haswell marker off the CPU name.
// TODO make Swift more robust
static std::string
GetSwiftFriendlyTriple (const std::string &triple)
{
    static std::string s_x86_64h("x86_64h");
    static std::string::size_type s_x86_64h_size = s_x86_64h.size();
    
    if (0 == triple.compare(0, s_x86_64h_size, s_x86_64h))
    {
        std::string fixed_triple("x86_64");
        fixed_triple.append(triple.substr(s_x86_64h_size, triple.size() - s_x86_64h_size));
        return fixed_triple;
    }
    return triple;
}

bool
SwiftASTContext::SetTriple (const char *triple_cstr, Module *module)
{
    if (triple_cstr && triple_cstr[0])
    {
        Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));

        // We can change our triple up until we create the swift::irgen::IRGenModule
        if (m_ir_gen_module_ap.get() == NULL)
        {
            std::string raw_triple(triple_cstr);
            std::string triple = GetSwiftFriendlyTriple(raw_triple);
            
            llvm::Triple llvm_triple (triple);
            const unsigned unspecified = 0;
            // If the OS version is unspecified, do fancy things
            if (llvm_triple.getOSMajorVersion() == unspecified)
            {
                // If a triple is "<arch>-apple-darwin" change it to be "<arch>-apple-macosx" otherwise
                // the major and minor OS version we append below would be wrong
                if (llvm_triple.getVendor() == llvm::Triple::VendorType::Apple &&
                    llvm_triple.getOS() == llvm::Triple::OSType::Darwin)
                {
                    llvm_triple.setOS(llvm::Triple::OSType::MacOSX);
                    triple = llvm_triple.str();
                }

                // Append the min OS to the triple if we have a target
                ModuleSP module_sp;
                if (module == NULL)
                {
                    TargetSP target_sp (m_target_wp.lock());
                    if (target_sp)
                    {
                        module_sp = target_sp->GetExecutableModule();
                        if (module_sp)
                            module = module_sp.get();
                    }
                }

                if (module)
                {
                    ObjectFile *objfile = module->GetObjectFile();
                    uint32_t versions[3];
                    if (objfile)
                    {
                        uint32_t num_versions = objfile->GetMinimumOSVersion(versions, 3);
                        StreamString strm;
                        if (num_versions)
                        {
                            for (uint32_t v = 0; v < 3; ++v)
                            {
                                if (v < num_versions)
                                {
                                    if (versions[v] == UINT32_MAX)
                                        versions[v] = 0;
                                }
                                else
                                    versions[v] = 0;
                            }
                            strm.Printf("%s%u.%u.%u", llvm_triple.getOSName().str().c_str(), versions[0], versions[1], versions[2]);
                            llvm_triple.setOSName(strm.GetString());
                            triple = llvm_triple.str();
                        }
                    }
                }
            }
            if (log)
                log->Printf ("%p: SwiftASTContext::SetTriple('%s') setting to '%s'%s", this, triple_cstr, triple.c_str(), m_target_wp.lock() ? " (target)" : "");
            m_compiler_invocation_ap->setTargetTriple(triple);
            return true;
        }
        else
        {
            if (log)
                log->Printf ("%p: SwiftASTContext::SetTriple('%s') ignoring triple since the IRGenModule has already been created", this, triple_cstr);
        }
    }
    return false;
}

static std::string
GetXcodeContentsPath ()
{
    const char substr[] = ".app/Contents/";

    // First, try based on the current shlib's location
    
    {
        FileSpec fspec;

        if (HostInfo::GetLLDBPath (ePathTypeLLDBShlibDir, fspec))
        {
            std::string path_to_shlib = fspec.GetPath();
            size_t pos = path_to_shlib.rfind(substr);
            if (pos != std::string::npos)
            {
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
        lldb_private::Error error = Host::RunShellCommand (command,   // shell command to run
                                                           NULL,      // current working directory
                                                           &status,   // Put the exit status of the process in here
                                                           &signo,    // Put the signal that caused the process to exit in here
                                                           &output,   // Get the output from the command and place it in this string
                                                           3);        // Timeout in seconds to wait for shell program to finish
        if (status == 0 && !output.empty())
        {
            size_t first_non_newline = output.find_last_not_of("\r\n");
            if (first_non_newline != std::string::npos)
            {
                output.erase(first_non_newline+1);
            }
            
            size_t pos = output.rfind(substr);
            if (pos != std::string::npos)
            {
                output.erase(pos + strlen(substr));
                return output;
            }
        }
    }
    
    return std::string();
}

static std::string
GetCurrentToolchainPath ()
{
    const char substr[] = ".xctoolchain/";
    
    {
        FileSpec fspec;
        
        if (HostInfo::GetLLDBPath (ePathTypeLLDBShlibDir, fspec))
        {
            std::string path_to_shlib = fspec.GetPath();
            size_t pos = path_to_shlib.rfind(substr);
            if (pos != std::string::npos)
            {
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
    "macosx",
    "iphonesimulator",
    "iphoneos",
    "appletvsimulator",
    "appletvos",
    "watchsimulator",
    "watchos",
};
    
struct SDKEnumeratorInfo {
    FileSpec    found_path;
    SDKType     sdk_type;
    uint32_t    least_major;
    uint32_t    least_minor;
};
    
static bool
SDKSupportsSwift (const FileSpec &sdk_path, SDKType desired_type)
{    
    ConstString last_path_component = sdk_path.GetLastPathComponent();
    
    if (last_path_component)
    {
        const llvm::StringRef sdk_name_raw = last_path_component.GetStringRef();
        std::string sdk_name_lower = sdk_name_raw.lower();
        const llvm::StringRef sdk_name(sdk_name_lower);
        
        llvm::StringRef version_part;
        
        SDKType sdk_type = SDKType::unknown;
        
        if (desired_type == SDKType::unknown)
        {
            for (int i = (int)SDKType::MacOSX; i < (int)SDKType::numSDKTypes; ++i)
            {
                if (sdk_name.startswith(sdk_strings[i]))
                {
                    version_part = sdk_name.drop_front(strlen(sdk_strings[i]));
                    sdk_type = (SDKType)i;
                    break;
                }
            }
            
            if (sdk_type == SDKType::unknown)
                return false;
        }
        else
        {
            if (sdk_name.startswith(sdk_strings[(int)desired_type]))
            {
                version_part = sdk_name.drop_front(strlen(sdk_strings[(int)desired_type]));
                sdk_type = desired_type;
            }
            else
            {
                return false;
            }
        }
        
        const size_t major_dot_offset = version_part.find('.');
        if (major_dot_offset == llvm::StringRef::npos)
            return false;
        
        const llvm::StringRef major_version = version_part.slice(0, major_dot_offset);
        const llvm::StringRef minor_part = version_part.drop_front(major_dot_offset + 1);
        
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
        
        switch (sdk_type)
        {
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
    
FileSpec::EnumerateDirectoryResult DirectoryEnumerator(void *baton,
                                                       FileSpec::FileType file_type,
                                                       const FileSpec &spec)
{
    SDKEnumeratorInfo *enumerator_info = static_cast<SDKEnumeratorInfo*>(baton);
    
    if (SDKSupportsSwift(spec, enumerator_info->sdk_type))
    {
        enumerator_info->found_path = spec;
        return FileSpec::EnumerateDirectoryResult::eEnumerateDirectoryResultNext;
    }
    
    return FileSpec::EnumerateDirectoryResult::eEnumerateDirectoryResultNext;
};
    
static ConstString
EnumerateSDKsForVersion (FileSpec sdks_spec, SDKType sdk_type, uint32_t least_major, uint32_t least_minor)
{
    if (!sdks_spec.IsDirectory())
        return ConstString();
    
    const bool find_directories = true;
    const bool find_files = false;
    const bool find_other = true; // include symlinks
    
    SDKEnumeratorInfo enumerator_info;
    
    enumerator_info.sdk_type = sdk_type;
    enumerator_info.least_major = least_major;
    enumerator_info.least_minor = least_minor;
    
    FileSpec::EnumerateDirectory(sdks_spec.GetPath().c_str(),
                                 find_directories,
                                 find_files,
                                 find_other,
                                 DirectoryEnumerator,
                                 &enumerator_info);
    
    if (enumerator_info.found_path.IsDirectory())
        return ConstString(enumerator_info.found_path.GetPath());
    else
        return ConstString();
}

static ConstString
GetSDKDirectory (SDKType sdk_type, uint32_t least_major, uint32_t least_minor)
{
    if (sdk_type != SDKType::MacOSX)
    {
        // Look inside Xcode for the required installed iOS SDK version
        
        std::string sdks_path = GetXcodeContentsPath();
        sdks_path.append("Developer/Platforms");
        
        if (sdk_type == SDKType::iPhoneSimulator)
        {
            sdks_path.append("/iPhoneSimulator.platform/");
        }
        else if (sdk_type == SDKType::AppleTVSimulator)
        {
            sdks_path.append("/AppleTVSimulator.platform/");
        }
        else if (sdk_type == SDKType::AppleTVOS)
        {
            sdks_path.append("/AppleTVOS.platform/");
        }
        else if (sdk_type == SDKType::WatchSimulator)
        {
            sdks_path.append("/WatchSimulator.platform/");
        }
        else if (sdk_type == SDKType::watchOS)
        {
            // For now, we need to be prepared to handle either capitalization of this path.
            
            std::string WatchOS_candidate_path = sdks_path + "/WatchOS.platform/";
            if (FileSpec(WatchOS_candidate_path.c_str(), false).IsDirectory()) {
                sdks_path = WatchOS_candidate_path;
            } else {
                std::string watchOS_candidate_path = sdks_path + "/watchOS.platform/";
                if (FileSpec(watchOS_candidate_path.c_str(), false).IsDirectory()) {
                    sdks_path = watchOS_candidate_path;
                } else {
                    return ConstString();
                }
            }
        }
        else
        {
            sdks_path.append("/iPhoneOS.platform/");
        }
        
        sdks_path.append("Developer/SDKs/");
        
        FileSpec sdks_spec(sdks_path.c_str(), false);
        
        return EnumerateSDKsForVersion(sdks_spec, sdk_type, least_major, least_major);
    }
    
    // The SDK type is Mac OS X
    
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t update = 0;
    
    if (!HostInfo::GetOSVersion(major, minor, update))
        return ConstString();
    
    // if there are minimum requirements that exceed the current OS, apply those
    
    if (least_major > major)
    {
        major = least_major;
        minor = least_minor;
    }
    else if (least_major == major)
    {
        if (least_minor > minor)
            minor = least_minor;
    }
    
    typedef std::map<uint64_t, ConstString> SDKDirectoryCache;
    static Mutex g_sdk_cache_mutex;
    static SDKDirectoryCache g_sdk_cache;
    Mutex::Locker locker(g_sdk_cache_mutex);
    const uint64_t major_minor = (uint64_t)major << 32 | (uint64_t)minor;
    SDKDirectoryCache::iterator pos = g_sdk_cache.find(major_minor);
    if (pos != g_sdk_cache.end())
        return pos->second;
    
    FileSpec fspec;
    std::string xcode_contents_path;

    if (xcode_contents_path.empty())
        xcode_contents_path = GetXcodeContentsPath();
    
    if (!xcode_contents_path.empty())
    {
        StreamString sdk_path;
        sdk_path.Printf("%sDeveloper/Platforms/MacOSX.platform/Developer/SDKs/MacOSX%u.%u.sdk", xcode_contents_path.c_str(), major, minor);
        fspec.SetFile(sdk_path.GetString().c_str(), false);
        if (fspec.Exists())
        {
            ConstString path(sdk_path.GetString().c_str());
            // Cache results
            g_sdk_cache[major_minor] = path;
            return path;
        }
        else if ((least_major != major) || (least_minor != minor))
        {
            // Try the required SDK
            sdk_path.Clear();
            sdk_path.Printf("%sDeveloper/Platforms/MacOSX.platform/Developer/SDKs/MacOSX%u.%u.sdk", xcode_contents_path.c_str(), least_major, least_minor);
            fspec.SetFile(sdk_path.GetString().c_str(), false);
            if (fspec.Exists())
            {
                ConstString path(sdk_path.GetString().c_str());
                // Cache results
                g_sdk_cache[major_minor] = path;
                return path;
            }
            else
            {
                // Okay, we're going to do an exhaustive search for *any* SDK that has an adequate version.
                
                std::string sdks_path = GetXcodeContentsPath();
                sdks_path.append("Developer/Platforms/MacOSX.platform/Developer/SDKs");

                FileSpec sdks_spec(sdks_path.c_str(), false);
                
                ConstString sdk_path = EnumerateSDKsForVersion(sdks_spec, sdk_type, least_major, least_major);

                if (sdk_path)
                {
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

static ConstString
GetResourceDir ()
{
    static ConstString g_cached_resource_dir;
    static std::once_flag g_once_flag;
    std::call_once(g_once_flag, [](){
        // First, check if there's something in our bundle
        
        {
            FileSpec swift_dir_spec;
            
            if (HostInfo::GetLLDBPath (ePathTypeSwiftDir, swift_dir_spec))
            {
                // We can't just check for the Swift directory, because that always exists.  We have to look for "clang" inside that.
                
                FileSpec swift_clang_dir_spec = swift_dir_spec;
                swift_clang_dir_spec.AppendPathComponent("clang");
                
                if (swift_clang_dir_spec.IsDirectory())
                {
                    g_cached_resource_dir = ConstString(swift_dir_spec.GetPath());
                    return;
                }
            }
        }
        
        // Nothing in our bundle.  Are we in a toolchain that has its own Swift compiler resource dir?
        
        {
            std::string xcode_toolchain_path = GetCurrentToolchainPath();
            
            if (!xcode_toolchain_path.empty())
            {
                xcode_toolchain_path.append("usr/lib/swift");
                
                if (FileSpec(xcode_toolchain_path, false).IsDirectory())
                {
                    g_cached_resource_dir = ConstString(xcode_toolchain_path);
                    return;
                }
            }
        }
        
        // We're not in a toolchain that has one.  Use the Xcode default toolchain.
        
        {
            std::string xcode_contents_path = GetXcodeContentsPath();
            
            if (!xcode_contents_path.empty())
            {
                xcode_contents_path.append("Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift");

                if (FileSpec(xcode_contents_path, false).IsDirectory())
                {
                    g_cached_resource_dir = ConstString(xcode_contents_path);
                    return;
                }
            }
        }
    });
    
    return g_cached_resource_dir;
}

} // anonymous namespace

swift::CompilerInvocation &
SwiftASTContext::GetCompilerInvocation ()
{
    return *m_compiler_invocation_ap;
}

swift::SourceManager &
SwiftASTContext::GetSourceManager ()
{
    if (m_source_manager_ap.get() == NULL)
        m_source_manager_ap.reset (new swift::SourceManager());
    return *m_source_manager_ap;
}

swift::LangOptions &
SwiftASTContext::GetLanguageOptions ()
{
    return GetCompilerInvocation().getLangOptions();
}

swift::DiagnosticEngine &
SwiftASTContext::GetDiagnosticEngine ()
{
    if (m_diagnostic_engine_ap.get() == NULL)
        m_diagnostic_engine_ap.reset (new swift::DiagnosticEngine(GetSourceManager()));
    return *m_diagnostic_engine_ap;
}

// This code comes from CompilerInvocation.cpp (setRuntimeResourcePath)

static void ConfigureResourceDirs(swift::CompilerInvocation &invocation,
                                  FileSpec resource_dir,
                                  llvm::Triple triple)
{
    // Make sure the triple is right:
    invocation.setTargetTriple(triple.str());
    invocation.setRuntimeResourcePath (resource_dir.GetPath().c_str());
}

swift::SILOptions &
SwiftASTContext::GetSILOptions ()
{
    return GetCompilerInvocation ().getSILOptions();
}

bool
SwiftASTContext::TargetHasNoSDK ()
{
  llvm::Triple triple(GetTriple ());

  switch (triple.getOS()) {
  case llvm::Triple::OSType::MacOSX:
  case llvm::Triple::OSType::Darwin:
  case llvm::Triple::OSType::IOS:
    return false;
  default:
    return true;
  }
}

swift::ClangImporterOptions &
SwiftASTContext::GetClangImporterOptions ()
{
    swift::ClangImporterOptions &clang_importer_options = GetCompilerInvocation ().getClangImporterOptions();
    if (!m_initialized_clang_importer_options)
    {
        m_initialized_clang_importer_options = true;
        FileSpec clang_dir_spec;
        if (HostInfo::GetLLDBPath (ePathTypeClangDir, clang_dir_spec))
            clang_importer_options.OverrideResourceDir = std::move(clang_dir_spec.GetPath());
    }
    return clang_importer_options;
}

swift::SearchPathOptions &
SwiftASTContext::GetSearchPathOptions ()
{
    swift::SearchPathOptions &search_path_opts = GetCompilerInvocation ().getSearchPathOptions();

    if (!m_initialized_search_path_options)
    {
        m_initialized_search_path_options = true;
        
        bool set_sdk = false;
        bool set_resource_dir = false;
        
        if (!m_platform_sdk_path.empty())
        {
            FileSpec platform_sdk(m_platform_sdk_path.c_str(), false);
            
            if (platform_sdk.Exists() && SDKSupportsSwift(platform_sdk, SDKType::unknown))
            {
                search_path_opts.SDKPath = m_platform_sdk_path.c_str();
                set_sdk = true;
            }
        }
        
        llvm::Triple triple(GetTriple ());
        
        if (!m_resource_dir.empty())
        {
            FileSpec resource_dir(m_resource_dir.c_str(), false);
            
            if (resource_dir.Exists())
            {
                ConfigureResourceDirs(GetCompilerInvocation(), resource_dir, triple);
                set_resource_dir = true;
            }
        }
            
        if (!set_sdk)
        {
            if (triple.getOS() == llvm::Triple::OSType::MacOSX ||
                triple.getOS() == llvm::Triple::OSType::Darwin)
            {
                search_path_opts.SDKPath = GetSDKDirectory(SDKType::MacOSX, 10, 10).AsCString(""); // we need the 10.10 SDK
            }
            else if (triple.getOS() == llvm::Triple::OSType::IOS)
            {
                if (triple.getArchName().startswith("arm"))
                {
                    search_path_opts.SDKPath = GetSDKDirectory(SDKType::iPhoneOS, 8, 0).AsCString("");
                }
                else
                {
                    search_path_opts.SDKPath = GetSDKDirectory(SDKType::iPhoneSimulator, 8, 0).AsCString("");
                }
            }
            // explicitly leave the SDKPath blank on other platforms
        }
        
        if (!set_resource_dir)
        {
            FileSpec resource_dir(::GetResourceDir().AsCString(""), false);
            if (resource_dir.Exists())
                ConfigureResourceDirs(GetCompilerInvocation(), resource_dir, triple);
        }
    }
    
    return search_path_opts;
}

namespace lldb_private
{
    
    class ANSIColorStringStream : public llvm::raw_string_ostream
    {
    public:
        ANSIColorStringStream (bool colorize) :
            llvm::raw_string_ostream(m_buffer),
            m_colorize(colorize)
        {
        }
        /// Changes the foreground color of text that will be output from this point
        /// forward.
        /// @param Color ANSI color to use, the special SAVEDCOLOR can be used to
        /// change only the bold attribute, and keep colors untouched
        /// @param Bold bold/brighter text, default false
        /// @param BG if true change the background, default: change foreground
        /// @returns itself so it can be used within << invocations
        virtual raw_ostream &changeColor(enum Colors colors,
                                         bool bold = false,
                                         bool bg = false)
        {
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
        virtual raw_ostream &resetColor()
        {
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
        virtual raw_ostream &reverseColor()
        {
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

    class StoringDiagnosticConsumer : public swift::DiagnosticConsumer
    {
    public:
        StoringDiagnosticConsumer (SwiftASTContext &ast_context) :
            m_ast_context(ast_context),
            m_diagnostics (),
            m_num_errors (0),
            m_colorize (false)
        {
            m_ast_context.GetDiagnosticEngine().resetHadAnyError();
            m_ast_context.GetDiagnosticEngine().addConsumer(*this);
        }
        
        ~StoringDiagnosticConsumer ()
        {
            m_ast_context.GetDiagnosticEngine().takeConsumers();
        }
        
        virtual void handleDiagnostic (swift::SourceManager &source_mgr,
                                       swift::SourceLoc source_loc,
                                       swift::DiagnosticKind kind,
                                       llvm::StringRef text,
                                       const swift::DiagnosticInfo &info)
        {
            const char *bufferName = "<anonymous>";
            unsigned bufferID = 0;
            std::pair<unsigned, unsigned> line_col = { 0, 0 };

            if (source_loc.isValid())
            {
                bufferID = source_mgr.findBufferContainingLoc(source_loc);
                bufferName = source_mgr.getBufferIdentifierForLoc(source_loc);
                line_col = source_mgr.getLineAndColumn(source_loc);
            }

            if (line_col.first != 0)
            {
                ANSIColorStringStream os(m_colorize);

                // Determine what kind of diagnostic we're emitting.
                llvm::SourceMgr::DiagKind source_mgr_kind;
                switch (kind) {
                    default:
                    case swift::DiagnosticKind::Error:
                        source_mgr_kind = llvm::SourceMgr::DK_Error;
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

                auto message = source_mgr.GetMessage(source_loc, source_mgr_kind, text, ranges, fix_its);
                source_mgr.getLLVMSourceMgr().PrintMessage(os, message);

                // Use the llvm::raw_string_ostream::str() accessor as it will flush
                // the stream into our "message" and return us a reference to "message"
                std::string &message_ref = os.str();

                if (message_ref.empty())
                    m_diagnostics.push_back({ text.str(), kind, bufferName, bufferID, line_col.first, line_col.second });
                else
                    m_diagnostics.push_back({ message_ref, kind, bufferName, bufferID, line_col.first, line_col.second  });
            }
            else
            {
                m_diagnostics.push_back({ text.str(), kind, bufferName, bufferID, line_col.first, line_col.second  });
            }
            
            if (kind == swift::DiagnosticKind::Error)
                m_num_errors++;
        }
        
        void Clear ()
        {
            m_ast_context.GetDiagnosticEngine().resetHadAnyError();
            m_diagnostics.clear();
            m_num_errors = 0;
        }
        
        unsigned NumErrors ()
        {
            if (m_num_errors)
                return m_num_errors;
            else if (m_ast_context.GetASTContext()->hadError())
                return 1;
            else
                return 0;
        }

        void
        PrintDiagnostics (Stream &stream,
                          uint32_t bufferID = UINT32_MAX,
                          uint32_t first_line = 0,
                          uint32_t last_line = UINT32_MAX,
                          uint32_t line_offset = 0)
        {
            for (const Diagnostic &diagnostic : m_diagnostics)
            {
                // We often make expressions and wrap them in some code.
                // When we see errors we want the line numbers to be correct so
                // we correct them below. LLVM stores in SourceLoc objects as character
                // offsets so there is no way to get LLVM to move its error line numbers
                // around by adjusting the source location, we must do it manually. We
                // also want to use the same error formatting as llvm and clang, so we
                // must muck with the string.
                if (first_line > 0 &&
                    bufferID != UINT32_MAX &&
                    diagnostic.bufferID == bufferID &&
                    diagnostic.bufferName != NULL)
                {
                    // Make sure the error line is in range
                    if (diagnostic.line >= first_line && diagnostic.line <= last_line)
                    {
                        // Need to remap the error/warning to a different line
                        StreamString match;
                        match.Printf ("%s:%u:", diagnostic.bufferName, diagnostic.line);
                        const size_t match_len = match.GetString().size();
                        size_t match_pos = diagnostic.description.find(match.GetString());
                        if (match_pos != std::string::npos)
                        {
                            // We have some <file>:<line>:" instances that need to be updated
                            StreamString fixed_description;
                            size_t start_pos = 0;
                            do
                            {
                                if (match_pos > start_pos)
                                    fixed_description.GetString().append(diagnostic.description, start_pos, match_pos);
                                fixed_description.Printf("%s:%u:",
                                                         diagnostic.bufferName,
                                                         diagnostic.line - first_line + line_offset + 1);
                                start_pos = match_pos + match_len;
                                match_pos = diagnostic.description.find(match.GetString(), start_pos);
                            } while (match_pos != std::string::npos);
                            
                            // Append any last remainging text
                            if (start_pos < diagnostic.description.size())
                                fixed_description.GetString().append(diagnostic.description, start_pos, diagnostic.description.size() - start_pos);
                            
                            stream.Write(fixed_description.GetString().c_str(), fixed_description.GetString().size());

                            continue;
                        }
                    }
                }
                stream.Write(diagnostic.description.c_str(), diagnostic.description.size());
            }
        }
        
        bool
        GetColorize () const
        {
            return m_colorize;
        }
        
        bool
        SetColorize (bool b)
        {
            const bool old = m_colorize;
            m_colorize = b;
            return old;
        }

    private:
        struct Diagnostic {
            std::string description;
            swift::DiagnosticKind kind;
            const char *bufferName;
            unsigned bufferID;
            uint32_t line;
            uint32_t column;
        };
        typedef std::vector<Diagnostic> DiagnosticBuffer;
        
        SwiftASTContext    &m_ast_context;
        DiagnosticBuffer    m_diagnostics;
        unsigned            m_num_errors = 0;
        bool                m_colorize;
    };
    
}

swift::ASTContext *
SwiftASTContext::GetASTContext ()
{
    if (m_ast_context_ap.get() == NULL)
    {
        m_ast_context_ap.reset (new swift::ASTContext(GetLanguageOptions(), GetSearchPathOptions(), GetSourceManager(), GetDiagnosticEngine()));
        m_diagnostic_consumer_ap.reset(new StoringDiagnosticConsumer(*this));

        if (getenv("LLDB_SWIFT_DUMP_DIAGS"))
        {
            // NOTE: leaking a swift::PrintingDiagnosticConsumer() here, but this only
            // gets enabled when the above environment variable is set.
            GetDiagnosticEngine().addConsumer(*new swift::PrintingDiagnosticConsumer());

        }
        // Install the serialized module loader
        
        std::unique_ptr<swift::ModuleLoader> serialized_module_loader_ap(swift::SerializedModuleLoader::create(*m_ast_context_ap));

        if (serialized_module_loader_ap)
        {
            m_serialized_module_loader = (swift::SerializedModuleLoader*)serialized_module_loader_ap.get();
            m_ast_context_ap->addModuleLoader(std::move(serialized_module_loader_ap));
        }
        
        GetASTMap().Insert(m_ast_context_ap.get(), this);
        
        // store common useful manglings for quick lookup - this also ensures that types that didn't come out of the visitor
        // (e.g. fallback ObjCPointers) still exist in our tables for later mangled name retrieval (the expression parser needs to do this)
        CacheDemangledType(ConstString("_TtBO").GetCString(), m_ast_context_ap->TheUnknownObjectType.getPointer());
        CacheDemangledType(ConstString("_TtBp").GetCString(), m_ast_context_ap->TheRawPointerType.getPointer());
        CacheDemangledType(ConstString("_TtBb").GetCString(), m_ast_context_ap->TheBridgeObjectType.getPointer());
        CacheDemangledType(ConstString("_TtBo").GetCString(), m_ast_context_ap->TheNativeObjectType.getPointer());
        CacheDemangledType(ConstString("_TtT_").GetCString(), m_ast_context_ap->TheEmptyTupleType.getPointer());
    }
    
    VALID_OR_RETURN(nullptr);

    return m_ast_context_ap.get();
}

swift::SerializedModuleLoader *
SwiftASTContext::GetSerializeModuleLoader ()
{
    GetASTContext();
    return m_serialized_module_loader;
}

swift::ClangImporter *
SwiftASTContext::GetClangImporter ()
{
    if (m_clang_importer == NULL)
    {
        swift::ASTContext *ast_ctx = GetASTContext();
        
        // Install the Clang module loader
        TargetSP target_sp (m_target_wp.lock());
        if (true /*target_sp*/ )
        {
            // PlatformSP platform_sp = target_sp->GetPlatform();
            if (true /*platform_sp*/ )
            {
                if (!ast_ctx->SearchPathOpts.SDKPath.empty() || TargetHasNoSDK())
                {
                    swift::ClangImporterOptions &clang_importer_options = GetClangImporterOptions();
                    if (!clang_importer_options.OverrideResourceDir.empty())
                    {
                        std::unique_ptr<swift::ModuleLoader> clang_importer_ap(swift::ClangImporter::create (*m_ast_context_ap,
                                                                                                             clang_importer_options));
                        
                        if (clang_importer_ap)
                        {
                            const bool isClang = true;
                            m_clang_importer = (swift::ClangImporter*)clang_importer_ap.get();
                            m_ast_context_ap->addModuleLoader(std::move(clang_importer_ap), isClang);
                        }
                    }
                }
            }
        }
    }
    return m_clang_importer;
}

bool
SwiftASTContext::AddModuleSearchPath (const char *path)
{
    if (path && path[0])
    {
        swift::ASTContext *ast = GetASTContext();
        std::string path_str (path);
        bool add_search_path = true;
        for (auto path : ast->SearchPathOpts.ImportSearchPaths)
        {
            if (path == path_str)
            {
                add_search_path = false;
                break;
            }
        }

        if (add_search_path)
        {
            ast->SearchPathOpts.ImportSearchPaths.push_back(path);
            return true;
        }
    }
    return false;
}

bool
SwiftASTContext::AddFrameworkSearchPath (const char *path)
{
    if (path && path[0])
    {
        swift::ASTContext *ast = GetASTContext();
        std::string path_str (path);
        bool add_search_path = true;
        for (std::string swift_path : ast->SearchPathOpts.FrameworkSearchPaths)
        {
            if (swift_path == path_str)
            {
                add_search_path = false;
                break;
            }
        }
        
        if (add_search_path)
        {
            ast->SearchPathOpts.FrameworkSearchPaths.push_back(path);
            return true;
        }
    }
    return false;
}

bool
SwiftASTContext::AddClangArgument (const char *clang_arg, bool force)
{
    if (clang_arg && clang_arg[0])
    {
        swift::ClangImporterOptions &importer_options = GetClangImporterOptions();
        
        bool add_hmap = true;
        
        if (!force)
        {
            for (std::string &arg : importer_options.ExtraArgs)
            {
                if (!arg.compare(clang_arg))
                {
                    add_hmap = false;
                    break;
                }
            }
        }
        
        if (add_hmap)
        {
            importer_options.ExtraArgs.push_back(clang_arg);
            return true;
        }
    }
    return false;
}

bool
SwiftASTContext::AddClangArgumentPair (const char *clang_arg_1, const char *clang_arg_2)
{
    if (clang_arg_1 && clang_arg_2 && clang_arg_1[0] && clang_arg_2[0])
    {
        swift::ClangImporterOptions &importer_options = GetClangImporterOptions();
        
        bool add_hmap = true;
        
        for (ssize_t ai = 0, ae = importer_options.ExtraArgs.size() - 1; // -1 because we look at the next one too
             ai < ae;
             ++ai) {
            if (!importer_options.ExtraArgs[ai].compare(clang_arg_1) &&
                !importer_options.ExtraArgs[ai+1].compare(clang_arg_2)) {
                add_hmap = false;
                break;
            }
        }
        
        if (add_hmap)
        {
            importer_options.ExtraArgs.push_back(clang_arg_1);
            importer_options.ExtraArgs.push_back(clang_arg_2);
            return true;
        }
    }
    return false;
}

size_t
SwiftASTContext::GetNumModuleSearchPaths () const
{
    if (m_ast_context_ap.get())
        return m_ast_context_ap->SearchPathOpts.ImportSearchPaths.size();
    return 0;
}

const char *
SwiftASTContext::GetModuleSearchPathAtIndex (size_t idx) const
{
    if (m_ast_context_ap.get())
    {
        if (idx < m_ast_context_ap->SearchPathOpts.ImportSearchPaths.size())
            return m_ast_context_ap->SearchPathOpts.ImportSearchPaths[idx].c_str();
    }
    return NULL;
}

size_t
SwiftASTContext::GetNumFrameworkSearchPaths () const
{
    if (m_ast_context_ap.get())
        return m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths.size();
    return 0;
}

const char *
SwiftASTContext::GetFrameworkSearchPathAtIndex (size_t idx) const
{
    if (m_ast_context_ap.get())
    {
        if (idx < m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths.size())
            return m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths[idx].c_str();
    }
    return NULL;
}

size_t
SwiftASTContext::GetNumClangArguments ()
{
    swift::ClangImporterOptions &importer_options = GetClangImporterOptions();
    
    return importer_options.ExtraArgs.size();
}

const char *
SwiftASTContext::GetClangArgumentAtIndex (size_t idx)
{
    swift::ClangImporterOptions &importer_options = GetClangImporterOptions();

    if (idx < importer_options.ExtraArgs.size())
        return importer_options.ExtraArgs[idx].c_str();
    
    return NULL;
}


swift::ModuleDecl *
SwiftASTContext::GetCachedModule (const ConstString &module_name)
{
    VALID_OR_RETURN(nullptr);
    
    SwiftModuleMap::const_iterator iter = m_swift_module_cache.find(module_name.GetCString());

    if (iter != m_swift_module_cache.end())
        return iter->second;
    return NULL;
}

swift::ModuleDecl *
SwiftASTContext::CreateModule (const ConstString &module_basename, Error &error)
{
    VALID_OR_RETURN(nullptr);

    if (module_basename)
    {
        swift::ModuleDecl *module = GetCachedModule (module_basename);
        if (module)
        {
            error.SetErrorStringWithFormat("module already exists for '%s'", module_basename.GetCString());
            return NULL;
        }

        swift::ASTContext *ast = GetASTContext();
        if (ast)
        {
            swift::Identifier module_id (ast->getIdentifier(module_basename.GetCString()));
            module = swift::Module::create(module_id, *ast);
            if (module)
            {
                m_swift_module_cache[module_basename.GetCString()] = module;
                return module;
            }
            else
            {
                error.SetErrorStringWithFormat("invalid swift AST (NULL)");
            }
        }
        else
        {
            error.SetErrorStringWithFormat("invalid swift AST (NULL)");
        }
    }
    else
    {
        error.SetErrorStringWithFormat("invalid module name (empty)");
    }
    return NULL;
}

void
SwiftASTContext::CacheModule (swift::ModuleDecl* module)
{
    VALID_OR_RETURN_VOID();

    if (!module)
        return;
    auto ID = module->getName().get();
    if (nullptr == ID || 0 == ID[0])
        return;
    if (m_swift_module_cache.find(ID) != m_swift_module_cache.end())
        return;
    m_swift_module_cache.insert({ID,module});
}

swift::ModuleDecl *
SwiftASTContext::GetModule (const ConstString &module_basename, Error &error)
{
    VALID_OR_RETURN(nullptr);

    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
    if (log)
        log->Printf ("((SwiftASTContext*)%p)->GetModule('%s')", this, module_basename.AsCString("<no name>"));
    
    if (module_basename)
    {
        swift::ModuleDecl *module = GetCachedModule (module_basename);
        if (module)
            return module;
        if (swift::ASTContext *ast = GetASTContext())
        {
            typedef std::pair<swift::Identifier, swift::SourceLoc> ModuleNameSpec;
            llvm::StringRef module_basename_sref (module_basename.GetCString());
            ModuleNameSpec name_pair(ast->getIdentifier(module_basename_sref), swift::SourceLoc());
            
            if (HasFatalErrors())
            {
                error.SetErrorStringWithFormat("failed to get module '%s' from AST context:\nAST context is in a fatal error state", module_basename.GetCString());
                printf("error in SwiftASTContext::GetModule(%s): AST context is in a fatal error stat",
                       module_basename.GetCString());
                return nullptr;
            }
            
            ClearDiagnostics();
            
            module = ast->getModuleByName(module_basename_sref);
            
            if (HasErrors())
            {
                StreamString ss;
                PrintDiagnostics(ss);
                error.SetErrorStringWithFormat("failed to get module '%s' from AST context:\n%s", module_basename.GetCString(), ss.GetData());
#ifdef LLDB_CONFIGURATION_DEBUG
                printf("error in SwiftASTContext::GetModule(%s): '%s'",
                       module_basename.GetCString(), ss.GetData());
#endif
                if (log)
                    log->Printf ("((SwiftASTContext*)%p)->GetModule('%s') -- error: %s", this, module_basename.GetCString(), ss.GetData());
            }
            else if (module)
            {
                if (log)
                    log->Printf ("((SwiftASTContext*)%p)->GetModule('%s') -- found %s", this, module_basename.GetCString(), module->getName().str().str().c_str());
                
                m_swift_module_cache[module_basename.GetCString()] = module;
                return module;
            }
            else
            {
                if (log)
                    log->Printf ("((SwiftASTContext*)%p)->GetModule('%s') -- failed with no error", this, module_basename.GetCString());
                
                error.SetErrorStringWithFormat("failed to get module '%s' from AST context", module_basename.GetCString());
            }
        }
        else
        {
            if (log)
                log->Printf ("((SwiftASTContext*)%p)->GetModule('%s') -- invalid ASTContext", this, module_basename.GetCString());
            
            error.SetErrorString("invalid swift::ASTContext");
        }
    }
    else
    {
        if (log)
            log->Printf ("((SwiftASTContext*)%p)->GetModule('%s') -- empty module name", this, module_basename.GetCString());
        
        error.SetErrorString("invalid module name (empty)");
    }
    return NULL;
}

swift::ModuleDecl *
SwiftASTContext::GetModule (const FileSpec &module_spec, Error &error)
{
    VALID_OR_RETURN(nullptr);

    ConstString module_basename(module_spec.GetFileNameStrippingExtension());
    
    Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
    if (log)
        log->Printf ("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s')", this, module_spec.GetPath().c_str());
    
    if (module_basename)
    {
        SwiftModuleMap::const_iterator iter = m_swift_module_cache.find(module_basename.GetCString());
        
        if (iter != m_swift_module_cache.end())
            return iter->second;

        if (module_spec.Exists())
        {
            swift::ASTContext *ast = GetASTContext();
            if (!GetClangImporter())
            {
                if (log)
                    log->Printf ("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- no ClangImporter so giving up", this, module_spec.GetPath().c_str());
                error.SetErrorStringWithFormat("couldn't get a ClangImporter");
                return nullptr;
            }
            
            std::string module_directory (module_spec.GetDirectory().GetCString());
            bool add_search_path = true;
            for (auto path : ast->SearchPathOpts.ImportSearchPaths)
            {
                if (path == module_directory)
                {
                    add_search_path = false;
                    break;
                }
            }
            // Add the search path if needed so we can find the module by basename
            if (add_search_path)
                ast->SearchPathOpts.ImportSearchPaths.push_back(std::move(module_directory));

            typedef std::pair<swift::Identifier, swift::SourceLoc> ModuleNameSpec;
            llvm::StringRef module_basename_sref (module_basename.GetCString());
            ModuleNameSpec name_pair(ast->getIdentifier(module_basename_sref), swift::SourceLoc());
            swift::ModuleDecl *module = ast->getModule(llvm::ArrayRef<ModuleNameSpec>(name_pair));
            if (module)
            {
                if (log)
                    log->Printf ("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- found %s", this, module_spec.GetPath().c_str(), module->getName().str().str().c_str());
                
                m_swift_module_cache[module_basename.GetCString()] = module;
                return module;
            }
            else
            {
                if (log)
                    log->Printf ("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- couldn't get from AST context", this, module_spec.GetPath().c_str());
                
                error.SetErrorStringWithFormat("failed to get module '%s' from AST context", module_basename.GetCString());
            }
        }
        else
        {
            if (log)
                log->Printf ("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- doesn't exist", this, module_spec.GetPath().c_str());
            
            error.SetErrorStringWithFormat("module '%s' doesn't exist", module_spec.GetPath().c_str());
        }
    }
    else
    {
        if (log)
            log->Printf ("((SwiftASTContext*)%p)->GetModule((FileSpec)'%s') -- no basename", this, module_spec.GetPath().c_str());
        
        error.SetErrorStringWithFormat("no module basename in '%s'", module_spec.GetPath().c_str());
    }
    return NULL;
}

swift::ModuleDecl *
SwiftASTContext::FindAndLoadModule (const ConstString &module_basename, Process &process, Error &error)
{
    VALID_OR_RETURN(nullptr);

    swift::ModuleDecl *swift_module = GetModule (module_basename, error);
    if (!swift_module)
        return nullptr;
    LoadModule (swift_module, process, error);
    return swift_module;
}

swift::ModuleDecl *
SwiftASTContext::FindAndLoadModule (const FileSpec &module_spec, Process &process, Error &error)
{
    VALID_OR_RETURN(nullptr);

    swift::ModuleDecl *swift_module = GetModule (module_spec, error);
    if (!swift_module)
        return nullptr;
    LoadModule (swift_module, process, error);
    return swift_module;
}

bool
SwiftASTContext::LoadOneImage (Process &process, FileSpec &link_lib_spec, Error &error)
{
    VALID_OR_RETURN(false);

    error.Clear();

    PlatformSP platform_sp = process.GetTarget().GetPlatform();
    if (platform_sp)
        return platform_sp->LoadImage(&process, FileSpec(), link_lib_spec, error) != LLDB_INVALID_IMAGE_TOKEN;
    else
        return false;
}

static void
GetLibrarySearchPaths (std::vector<std::string> &paths, const swift::SearchPathOptions &search_path_opts)
{
    paths.clear();
    paths.resize(search_path_opts.LibrarySearchPaths.size() + 1);
    std::copy(search_path_opts.LibrarySearchPaths.begin(), search_path_opts.LibrarySearchPaths.end(), paths.begin());
    paths.push_back(search_path_opts.RuntimeLibraryPath);
}

void
SwiftASTContext::LoadModule (swift::ModuleDecl *swift_module, Process &process, Error &error)
{
    VALID_OR_RETURN_VOID();

    Error current_error;
    auto addLinkLibrary = [&](swift::LinkLibrary link_lib)
    {
        Error load_image_error;
        StreamString all_dlopen_errors;
        const char *library_name = link_lib.getName().data();
        
        if (library_name == NULL || library_name[0] == '\0')
        {
            error.SetErrorString("Empty library name passed to addLinkLibrary");
            return;
        }

        SwiftLanguageRuntime *runtime = process.GetSwiftLanguageRuntime();

        if (runtime && runtime->IsInLibraryNegativeCache(library_name))
            return;        

        swift::LibraryKind library_kind = link_lib.getKind();

        Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_TYPES|LIBLLDB_LOG_TEMPORARY));
        if (log)
            log->Printf ("\nLoading link library \"%s\" of kind: %d.", library_name, library_kind);

        switch (library_kind)
        {
            case swift::LibraryKind::Framework:
            {
                
                // First make sure the library isn't already loaded, since this is a framework, we make sure the file name
                // and the framework name are the same, and that we are contained in FileName.framework with no other
                // intervening frameworks.  We can get more restrictive if this gives false positives.
                
                ConstString library_cstr(library_name);

                std::string framework_name(library_name);
                framework_name.append(".framework");

                // Lookup the module by file basename and make sure that basename has
                // "<basename>.framework" in the path.

                ModuleSpec module_spec;
                module_spec.GetFileSpec().GetFilename() = library_cstr;
                lldb_private::ModuleList matching_module_list;
                bool module_already_loaded = false;
                if (process.GetTarget().GetImages().FindModules(module_spec, matching_module_list))
                {
                    matching_module_list.ForEach ([&module_already_loaded, &module_spec, &framework_name](const ModuleSP &module_sp) -> bool {
                        module_already_loaded = module_spec.GetFileSpec().GetPath().find(framework_name) != std::string::npos;
                        return module_already_loaded == false; // Keep iterating if we didn't find the right module
                    });
                }
                // If we already have this library loaded, don't try and load it again.
                if (module_already_loaded)
                {
                    // Then Framework is already loaded, so we don't need to try to load it again.
                    if (log)
                        log->Printf("Skipping load of %s as it is already loaded.", framework_name.c_str());
                    return;
                }

                for (auto module : process.GetTarget().GetImages().Modules())
                {
                    FileSpec module_file = module->GetFileSpec();
                    if (module_file.GetFilename() == library_cstr)
                    {
                        std::string module_path = module_file.GetPath();
                    
                        size_t framework_offset = module_path.rfind(framework_name);
                
                        if (framework_offset != std::string::npos)
                        {
                            // Then Framework is already loaded, so we don't need to try to load it again.
                            if (log)
                                log->Printf("Skipping load of %s as it is already loaded.", framework_name.c_str());
                            return;
                        }
                    }
                }
                
                std::string framework_path("@rpath/");
                framework_path.append(library_name);
                framework_path.append(".framework/");
                framework_path.append(library_name);
                FileSpec framework_spec(framework_path.c_str(), false);
                
                if (LoadOneImage(process, framework_spec, load_image_error))
                {
                    if (log)
                        log->Printf ("Found framework at: %s.", framework_path.c_str());

                    return;
                }
                else
                    all_dlopen_errors.Printf("Looking for \"%s\", error: %s\n", framework_path.c_str(),
                                             load_image_error.AsCString());
                
                
                // And then in the various framework search paths.
                std::unordered_set<std::string> seen_paths;
                for (const std::string &framework_search_dir :
                         swift_module->getASTContext().SearchPathOpts.FrameworkSearchPaths)
                {
                    // The framework search dir as it comes from the AST context often has duplicate entries, don't
                    // try to load along the same path twice.
               
                    std::pair<std::unordered_set<std::string>::iterator, bool> insert_result = seen_paths.insert(framework_search_dir);
                    if (!insert_result.second)
                        continue;
               
                    framework_path = framework_search_dir;
                    framework_path.append("/");
                    framework_path.append(library_name);
                    framework_path.append(".framework/");
                    framework_path.append(library_name);
                    framework_spec.SetFile(framework_path.c_str(), false);
                    
                    if (LoadOneImage(process, framework_spec, load_image_error))
                    {
                        if (log)
                            log->Printf ("Found framework at: %s.", framework_path.c_str());

                        return;
                    }
                    else
                        all_dlopen_errors.Printf("Looking for \"%s\"\n,    error: %s\n", framework_path.c_str(),
                                                 load_image_error.AsCString());
                }
                
                // Maybe we were told to add a link library that exists in the system.  I tried just specifying
                // Foo.framework/Foo and letting the System search figure that out, but if DYLD_FRAMEWORK_FALLBACK_PATH
                // is see (e.g. in Xcode's test scheme) then these aren't found.  So for now I dial them in explicitly:
                
                std::string system_path("/System/Library/Frameworks/");
                system_path.append (library_name);
                system_path.append(".framework/");
                system_path.append(library_name);
                framework_spec.SetFile(system_path.c_str(), true);
                if (LoadOneImage(process, framework_spec, load_image_error))
                    return;
                else
                    all_dlopen_errors.Printf("Looking for \"%s\"\n,    error: %s\n", framework_path.c_str(), load_image_error.AsCString());
            }
            break;
            case swift::LibraryKind::Library:
            {
                std::vector<std::string> search_paths;
                
                GetLibrarySearchPaths(search_paths, swift_module->getASTContext().SearchPathOpts);
                
                if (LoadLibraryUsingPaths (process,
                                           library_name,
                                           search_paths,
                                           true,
                                           all_dlopen_errors))
                    return;
            }
            break;
        }

        // If we get here, we aren't going to find this image, so add it to a negative cache:
        if (runtime)
            runtime->AddToLibraryNegativeCache(library_name);

        current_error.SetErrorStringWithFormat ("Failed to load linked library %s of module %s - errors:\n%s\n",
                                        library_name, swift_module->getName().str().str().c_str(), all_dlopen_errors.GetData());
    };
    
    swift_module->forAllVisibleModules({},
                                       true, // includePrivateTopLevel
                                       [&](swift::Module::ImportedModule import) {
                                           import.second->collectLinkLibraries(addLinkLibrary);
                                       });
    error = current_error;
}

bool
SwiftASTContext::LoadLibraryUsingPaths (Process &process,
                                        llvm::StringRef library_name,
                                        std::vector<std::string> &search_paths,
                                        bool check_rpath,
                                        StreamString &all_dlopen_errors)
{
    VALID_OR_RETURN(false);

    Log *log(GetLogIfAnyCategoriesSet(LIBLLDB_LOG_TYPES|LIBLLDB_LOG_TEMPORARY));
    
    PlatformSP platform_sp(process.GetTarget().GetPlatform());
    
    std::string library_fullname;
    
    if (platform_sp)
    {
        library_fullname = platform_sp->GetFullNameForDylib(ConstString(library_name)).AsCString();
    }
    else // This is the old way, and we shouldn't use it except on Mac OS
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

    if (process.GetTarget().GetImages().FindModules(module_spec, matching_module_list) > 0)
    {
        if (log)
            log->Printf("Skipping module %s as it is already loaded.", library_fullname.c_str());
        return true;
    }

    FileSpec library_spec;
    std::string library_path;
    std::unordered_set<std::string> seen_paths;
    Error load_image_error;

    for (const std::string &library_search_dir :
           search_paths)
    {
        // The library search dir as it comes from the AST context often has duplicate entries, don't
        // try to load along the same path twice.
   
        std::pair<std::unordered_set<std::string>::iterator, bool> insert_result = seen_paths.insert(library_search_dir);
        if (!insert_result.second)
            continue;

        library_path = library_search_dir;
        library_path.append("/");
        library_path.append(library_fullname);
        library_spec.SetFile(library_path.c_str(), false);
        if (LoadOneImage(process, library_spec, load_image_error))
        {
            if (log)
                log->Printf ("Found library at: %s.", library_path.c_str());
            return true;
        }
        else
          all_dlopen_errors.Printf("Looking for \"%s\"\n,    error: %s\n", library_path.c_str(),
                                   load_image_error.AsCString());
    }

    if (check_rpath)
    {
        // Let our RPATH help us out when finding the right library
        library_path = "@rpath/";
        library_path += library_fullname;

        FileSpec link_lib_spec(library_path.c_str(), false);

        if (LoadOneImage(process, link_lib_spec, load_image_error))
        {
            if (log)
                log->Printf ("Found library at: %s.", library_path.c_str());
            return true;
        }
        else
            all_dlopen_errors.Printf("Looking for \"%s\", error: %s\n", library_path.c_str(), load_image_error.AsCString());
    }
    return false;
}

void
SwiftASTContext::LoadExtraDylibs (Process &process, Error &error)
{
    VALID_OR_RETURN_VOID();

    error.Clear();
    swift::IRGenOptions &irgen_options = GetIRGenOptions();
    for (const swift::LinkLibrary &link_lib : irgen_options.LinkLibraries)
    {
        // We don't have to do frameworks here, they actually record their link libraries properly.
        if (link_lib.getKind() == swift::LibraryKind::Library)
        {
             const char *library_name = link_lib.getName().data();
             StreamString errors;
            
             std::vector<std::string> search_paths;
            
             GetLibrarySearchPaths(search_paths, m_compiler_invocation_ap->getSearchPathOptions());
            
             bool success = LoadLibraryUsingPaths (process,
                                                   library_name,
                                                   search_paths,
                                                   false,
                                                   errors);
             if (!success)
             {
                 error.SetErrorString(errors.GetData());
             }
        }
    }
}

bool
SwiftASTContext::RegisterSectionModules (Module &module, std::vector<std::string> &module_names)
{
    VALID_OR_RETURN(false);

    swift::SerializedModuleLoader *sml = GetSerializeModuleLoader();
    if (sml)
    {
        SectionList *section_list = module.GetSectionList();
        if (section_list)
        {
            SectionSP section_sp (section_list->FindSectionByType (eSectionTypeSwiftModules, true));
            if (section_sp)
            {
                DataExtractor section_data;
                
                if (section_sp->GetSectionData(section_data))
                {
                    llvm::StringRef section_data_ref((const char *)section_data.GetDataStart(), section_data.GetByteSize());
                    llvm::SmallVector<std::string, 4> llvm_modules;
                    if (swift::parseASTSection(sml, section_data_ref, llvm_modules))
                    {
                        for (auto module_name : llvm_modules)
                            module_names.push_back(module_name);
                        return true;
                    }
                }
            }
            else
            {
                if (m_ast_file_data_map.find(&module) != m_ast_file_data_map.end())
                    return true;
                
                SymbolVendor *sym_vendor = module.GetSymbolVendor();
                if (sym_vendor)
                {
                    DataBufferSP ast_file_data_sp = sym_vendor->GetASTData (eLanguageTypeSwift);
                    if (ast_file_data_sp)
                    {
                        m_ast_file_data_map[&module] = ast_file_data_sp;
                        llvm::StringRef section_data_ref((const char *)ast_file_data_sp->GetBytes(), ast_file_data_sp->GetByteSize());
                        llvm::SmallVector<std::string, 4> llvm_modules;
                        if (swift::parseASTSection(sml, section_data_ref, llvm_modules))
                        {
                            for (auto module_name : llvm_modules)
                                module_names.push_back(module_name);
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

void
SwiftASTContext::ValidateSectionModules (Module &module,
                                         const std::vector<std::string> &module_names)
{
    VALID_OR_RETURN_VOID();

    Error error;
    
    for (const std::string &module_name : module_names)
        if (!GetModule (ConstString(module_name.c_str()), error))
            module.ReportWarning ("unable to load swift module '%s' (%s)", module_name.c_str(), error.AsCString());
}


swift::Identifier
SwiftASTContext::GetIdentifier (const char *name)
{
    VALID_OR_RETURN(swift::Identifier());

    return GetASTContext()->getIdentifier(llvm::StringRef(name));
}

swift::Identifier
SwiftASTContext::GetIdentifier (const llvm::StringRef &name)
{
    VALID_OR_RETURN(swift::Identifier());

    return GetASTContext()->getIdentifier(name);
}

class DeclsLookupSource {
public:
    typedef llvm::SmallVectorImpl<swift::ValueDecl *> ValueDecls;

private:
    class VisibleDeclsConsumer : public swift::VisibleDeclConsumer {
    private:
        std::vector<swift::ValueDecl *> m_decls;
    public:
        virtual void foundDecl(swift::ValueDecl *VD, swift::DeclVisibilityKind Reason)
        {
            m_decls.push_back(VD);
        }
        virtual ~VisibleDeclsConsumer() = default;
        explicit operator bool()
        {
            return m_decls.size() > 0;
        }
        
        decltype(m_decls)::const_iterator begin ()
        {
            return m_decls.begin();
        }
        
        decltype(m_decls)::const_iterator end ()
        {
            return m_decls.end();
        }
    };

    bool
    lookupQualified (swift::ModuleDecl* entry,
                     swift::Identifier name,
                     unsigned options,
                     swift::LazyResolver *typeResolver,
                     ValueDecls& decls)
    {
        if (!entry)
            return false;
        
        Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES | LIBLLDB_LOG_VERBOSE));
        
        if (log)
            log->Printf("entering DeclsLookupSource::lookupQualified(%s,%s)", entry->getName().get(), name.get());
        
        size_t decls_size = decls.size();
        
        entry->lookupQualified(swift::ModuleType::get(entry), name, options, typeResolver, decls);
        
        if (decls.size() > decls_size)
        {
            if (log)
                log->Printf("DeclsLookupSource::lookupQualified(%s,%s) success", entry->getName().get(), name.get());
            return true;
        }
        
        if (log)
            log->Printf("DeclsLookupSource::lookupQualified(%s,%s) failure", entry->getName().get(), name.get());
        
        return false;
    }
    
    bool
    lookupValue (swift::ModuleDecl* entry,
                 swift::Identifier name,
                 swift::Module::AccessPathTy accessPath,
                 swift::NLKind lookupKind,
                 ValueDecls &decls)
    {
        if (!entry)
            return false;
        
        Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES | LIBLLDB_LOG_VERBOSE));
        
        if (log)
            log->Printf("entering DeclsLookupSource::lookupValue(%s,%s)", entry->getName().get(), name.get());
        
        size_t decls_size = decls.size();
        
        entry->lookupValue(accessPath, name, lookupKind, decls);
        
        if (decls.size() > decls_size)
        {
            if (log)
                log->Printf("DeclsLookupSource::lookupValue(%s,%s) success", entry->getName().get(), name.get());
            return true;
        }
        
        if (log)
            log->Printf("DeclsLookupSource::lookupValue(%s,%s) failure", entry->getName().get(), name.get());
        
        return false;
    }
    
public:
    enum class Type {
        SwiftModule,
        Crawler,
        Decl,
        Extension,
        Invalid
    };
    
    typedef llvm::Optional<std::string> PrivateDeclIdentifier;
    
    static DeclsLookupSource
    GetDeclsLookupSource (SwiftASTContext& ast,
                          ConstString module_name,
                          bool allow_crawler = true)
    {
        assert(module_name.GetCString());
        static ConstString g_ObjectiveCModule(swift::MANGLING_MODULE_OBJC);
        static ConstString g_BuiltinModule("Builtin");
        static ConstString g_CModule(swift::MANGLING_MODULE_C);
        if (allow_crawler)
        {
            if (module_name == g_ObjectiveCModule || module_name == g_CModule)
                return DeclsLookupSource(ast.GetASTContext(), module_name);
        }
        Error error;
        swift::ModuleDecl * module = module_name == g_BuiltinModule ? ast.GetASTContext()->TheBuiltinModule : ast.GetModule(module_name, error);
        if (module == nullptr)
            return DeclsLookupSource();
        return DeclsLookupSource(module);
    }
    
    static DeclsLookupSource
    GetDeclsLookupSource (swift::NominalTypeDecl *decl)
    {
        assert(decl);
        return DeclsLookupSource(decl);
    }
    
    static DeclsLookupSource
    GetDeclsLookupSource (DeclsLookupSource source, swift::NominalTypeDecl *decl)
    {
        assert(source._type == Type::SwiftModule);
        assert(source._module);
        assert(decl);
        return DeclsLookupSource(source._module, decl);
    }

    void lookupQualified(swift::Identifier name,
                               unsigned options,
                               swift::LazyResolver *typeResolver,
                               ValueDecls &result)
    {
        if (_type == Type::Crawler)
        {
            SwiftASTContext *ast_ctx = SwiftASTContext::GetSwiftASTContext(_crawler._ast);
            if (ast_ctx)
            {
                VisibleDeclsConsumer consumer;
                swift::ClangImporter *swift_clang_importer = ast_ctx->GetClangImporter();
                if (!swift_clang_importer)
                    return;
                swift_clang_importer->lookupValue(name, consumer);
                if (consumer)
                {
                    auto iter = consumer.begin(), end = consumer.end();
                    while (iter != end)
                    {
                        result.push_back(*iter);
                        iter++;
                    }
                    return;
                }
                else
                {
                    const bool allow_crawler = false;
                    if (_crawler._module)
                        GetDeclsLookupSource(*ast_ctx, ConstString(_crawler._module), allow_crawler).lookupQualified(name, options, typeResolver, result);
                }
            }
        }
        else if (_type == Type::SwiftModule)
            lookupQualified(_module, name, options, typeResolver, result);
        return;
    }
    
    void lookupValue(swift::Module::AccessPathTy path,
                           swift::Identifier name,
                           swift::NLKind kind,
                           ValueDecls &result)
    {
        if (_type == Type::Crawler)
        {
            SwiftASTContext *ast_ctx = SwiftASTContext::GetSwiftASTContext(_crawler._ast);
            if (ast_ctx)
            {
                VisibleDeclsConsumer consumer;
                swift::ClangImporter *swift_clang_importer = ast_ctx->GetClangImporter();
                if (!swift_clang_importer)
                    return;
                swift_clang_importer->lookupValue(name, consumer);
                if (consumer)
                {
                    auto iter = consumer.begin(), end = consumer.end();
                    while (iter != end)
                    {
                        result.push_back(*iter);
                        iter++;
                    }
                    return;
                }
                else
                {
                    const bool allow_crawler = false;
                    if (_crawler._module)
                        GetDeclsLookupSource(*ast_ctx, ConstString(_crawler._module), allow_crawler).lookupValue(path, name, kind, result);
                }
            }
        }
        else if (_type == Type::SwiftModule)
            _module->lookupValue(path, name, kind, result);
        return;
    }
    
    void lookupMember (swift::DeclName id,
                             swift::Identifier priv_decl_id,
                             ValueDecls &result)
    {
        if (_type == Type::Decl)
            return lookupMember(_decl, id, priv_decl_id, result);
        if (_type == Type::SwiftModule)
            return lookupMember(_module, id, priv_decl_id, result);
        if (_type == Type::Extension)
            return lookupMember(_extension._decl, id, priv_decl_id, result);
        return;
    }
    
    void
    lookupMember (swift::DeclContext *decl_ctx,
                  swift::DeclName id,
                  swift::Identifier priv_decl_id,
                  ValueDecls &result)
    {
        if (_type == Type::Decl)
            _decl->getModuleContext()->lookupMember(result, decl_ctx, id, priv_decl_id);
        else if (_type == Type::SwiftModule)
            _module->lookupMember(result, decl_ctx, id, priv_decl_id);
        else if (_type == Type::Extension)
            _extension._module->lookupMember(result, decl_ctx, id, priv_decl_id);
        return;
    }
    
    swift::TypeDecl *
    lookupLocalType (llvm::StringRef key)
    {
        switch (_type)
        {
            case Type::SwiftModule:
                return _module->lookupLocalType(key);
            case Type::Decl:
                return _decl->getModuleContext()->lookupLocalType(key);
            case Type::Extension:
                return _extension._module->lookupLocalType(key);
            case Type::Invalid:
            case Type::Crawler:
                return nullptr;
        }
    }
    
    ConstString
    GetName () const
    {
        switch(_type)
        {
            case Type::Invalid:
                return ConstString("Invalid");
            case Type::Crawler:
                return ConstString("Crawler");
            case Type::SwiftModule:
                return ConstString(_module->getName().get());
            case Type::Decl:
                return ConstString(_decl->getName().get());
            case Type::Extension:
            {
                StreamString stream;
                stream.Printf("ext %s in %s",
                              _extension._decl->getName().get(),
                              _module->getName().get());
                return ConstString(stream.GetString().c_str());
            }
        }
    }

    DeclsLookupSource (const DeclsLookupSource &rhs) :
    _type(rhs._type)
    {
        switch (_type)
        {
            case Type::Invalid:
                break;
            case Type::Crawler:
                _crawler._ast = rhs._crawler._ast;
                _crawler._module = rhs._crawler._module;
                break;
            case Type::SwiftModule:
                _module = rhs._module;
                break;
            case Type::Decl:
                _decl = rhs._decl;
            case Type::Extension:
                _extension._decl = rhs._extension._decl;
                _extension._module = rhs._extension._module;
                break;
        }
    }
    
    DeclsLookupSource&
    operator = (const DeclsLookupSource& rhs)
    {
        if (this != &rhs)
        {
            _type = rhs._type;
            switch (_type)
            {
                case Type::Invalid:
                    break;
                case Type::Crawler:
                    _crawler._ast = rhs._crawler._ast;
                    _crawler._module = rhs._crawler._module;
                    break;
                case Type::SwiftModule:
                    _module = rhs._module;
                    break;
                case Type::Decl:
                    _decl = rhs._decl;
                case Type::Extension:
                    _extension._decl = rhs._extension._decl;
                    _extension._module = rhs._extension._module;
                    break;
            }
        }
        return *this;
    }
    
    void
    Clear ()
    {
        // no need to explicitly clean either pointer
        _type = Type::Invalid;
    }
    
    DeclsLookupSource () :
    _type(Type::Invalid),
    _module(nullptr)
    {}
   
    operator bool ()
    {
        switch (_type)
        {
            case Type::Invalid:
                return false;
            case Type::Crawler:
                return _crawler._ast != nullptr;
            case Type::SwiftModule:
                return _module != nullptr;
            case Type::Decl:
                return _decl != nullptr;
            case Type::Extension:
                return (_extension._decl != nullptr) && (_extension._module != nullptr);
        }
    }
    
    bool
    IsExtension ()
    {
        return (this->operator bool()) && (_type == Type::Extension);
    }
    
    swift::Type
    GetQualifiedArchetype (size_t index,
                           swift::ASTContext* ast)
    {
        if (this->operator bool() && ast)
        {
            switch (_type)
            {
                case Type::Extension:
                {
                    swift::TypeBase *type_ptr = _extension._decl->getType().getPointer();
                    if (swift::MetatypeType *metatype_ptr = type_ptr->getAs<swift::MetatypeType>())
                        type_ptr = metatype_ptr->getInstanceType().getPointer();
                    CompilerType clang_type(ast, type_ptr);
                    lldb::TemplateArgumentKind arg_kind;
                    CompilerType archetype(clang_type.GetTemplateArgument(index, arg_kind));
                    if (llvm::dyn_cast_or_null<SwiftASTContext>(archetype.GetTypeSystem()))
                        return GetSwiftType(archetype);
                }
                    break;
                default:
                    break;
            }
        }
        return swift::Type();
    }
    
private:
    Type _type;
    
    union {
        swift::ModuleDecl *_module;
        struct {
            swift::ASTContext* _ast;
            const char* _module;
        } _crawler;
        swift::NominalTypeDecl *_decl;
        struct {
            swift::ModuleDecl *_module; // extension in this module
            swift::NominalTypeDecl *_decl; // for this type
        } _extension;
    };
    
    DeclsLookupSource(swift::ModuleDecl* _m)
    {
        lldbassert(_m && "DeclsLookupSource needs a valid ModuleDec");
        if (_m)
        {
            _module = _m;
            _type = Type::SwiftModule;
        }
        else
            _type = Type::Invalid;
    }
    
    DeclsLookupSource(swift::ASTContext* _a,
                      ConstString _m)
    {
        // it is fine for the ASTContext to be null, so don't actually even lldbassert there
        if (_a)
        {
            _crawler._ast = _a;
            _crawler._module = _m.GetCString();
            _type = Type::Crawler;
        }
        else
            _type = Type::Invalid;
    }
    
    DeclsLookupSource (swift::NominalTypeDecl * _d)
    {
        lldbassert(_d && "DeclsLookupSource needs a valid NominalTypeDecl");
        if (_d)
        {
            _decl = _d;
            _type = Type::Decl;
        }
        else
            _type = Type::Invalid;
    }
    
    DeclsLookupSource (swift::ModuleDecl *_m,
                       swift::NominalTypeDecl * _d)
    {
        lldbassert(_m && "DeclsLookupSource needs a valid ModuleDec");
        lldbassert(_d && "DeclsLookupSource needs a valid NominalTypeDecl");
        if (_m && _d)
        {
            _extension._decl = _d;
            _extension._module = _m;
            _type = Type::Extension;
        }
        else
            _type = Type::Invalid;
    }
};

struct VisitNodeResult {
    DeclsLookupSource _module;
    std::vector<swift::Decl *> _decls;
    std::vector<swift::Type> _types;
    swift::TupleTypeElt _tuple_type_element;
    Error _error;
    VisitNodeResult () :
        _module(),
        _decls(),
        _types(),
        _tuple_type_element(),
        _error()
    {
    }
    
    bool
    HasSingleType ()
    {
        return _types.size() == 1 && _types.front();
    }

    bool
    HasSingleDecl ()
    {
        return _decls.size() == 1 && _decls.front();
    }
    
    swift::Type
    GetFirstType ()
    {
        // Must ensure there is a type prior to calling this
        return _types.front();
    }

    swift::Decl*
    GetFirstDecl ()
    {
        // Must ensure there is a decl prior to calling this
        return _decls.front();
    }
    
    void
    Clear()
    {
        _module.Clear();
        _decls.clear();
        _types.clear();
        _tuple_type_element = swift::TupleTypeElt();
        _error.Clear();
    }
    
    bool
    HasAnyDecls ()
    {
        return !_decls.empty();
    }
    
    bool
    HasAnyTypes ()
    {
        return !_types.empty();
    }
};

static swift::Identifier
GetIdentifier (SwiftASTContext *ast,
               const DeclsLookupSource::PrivateDeclIdentifier& priv_decl_id)
{
    do {
        if (!ast)
            break;
        if (!priv_decl_id.hasValue())
            break;
        return ast->GetIdentifier(priv_decl_id.getValue().c_str());
    } while (false);
    return swift::Identifier();
}

static bool
FindFirstNamedDeclWithKind (SwiftASTContext *ast,
                            const llvm::StringRef &name,
                            swift::DeclKind decl_kind,
                            VisitNodeResult &result,
                            DeclsLookupSource::PrivateDeclIdentifier priv_decl_id = DeclsLookupSource::PrivateDeclIdentifier())
                            
{
    if (!result._decls.empty())
    {
        swift::Decl *parent_decl = result._decls.back();
        if (parent_decl)
        {
            auto nominal_decl = llvm::dyn_cast<swift::NominalTypeDecl>(parent_decl);

            if (nominal_decl)
            {
                bool check_type_aliases = false;

                DeclsLookupSource lookup(DeclsLookupSource::GetDeclsLookupSource(nominal_decl));
                llvm::SmallVector<swift::ValueDecl *, 4> decls;
                lookup.lookupMember(ast->GetIdentifier(name),
                                    GetIdentifier(ast,priv_decl_id),
                                    decls);

                for (auto decl : decls)
                {
                    const swift::DeclKind curr_decl_kind = decl->getKind();

                    if (curr_decl_kind == decl_kind)
                    {
                        result._decls.back() = decl;
                        swift::Type decl_type;
                        if (decl->hasType())
                        {
                            decl_type = decl->getType();
                            swift::MetatypeType *meta_type = decl_type->getAs<swift::MetatypeType>();
                            if (meta_type)
                                decl_type = meta_type->getInstanceType();
                        }
                        if (result._types.empty())
                            result._types.push_back(decl_type);
                        else
                            result._types.back() = decl_type;
                        return true;
                    } else if (curr_decl_kind == swift::DeclKind::TypeAlias)
                        check_type_aliases = true;
                }
                
                if (check_type_aliases)
                {
                    for (auto decl : decls)
                    {
                        const swift::DeclKind curr_decl_kind = decl->getKind();
                        
                        if (curr_decl_kind == swift::DeclKind::TypeAlias)
                        {
                            result._decls.back() = decl;
                            swift::Type decl_type;
                            if (decl->hasType())
                            {
                                decl_type = decl->getType();
                                swift::MetatypeType *meta_type = decl_type->getAs<swift::MetatypeType>();
                                if (meta_type)
                                    decl_type = meta_type->getInstanceType();
                            }
                            if (result._types.empty())
                                result._types.push_back(decl_type);
                            else
                                result._types.back() = decl_type;
                            return true;
                        }
                    }
                }
            }
        }
    }
    else if (result._module)
    {
        swift::Module::AccessPathTy access_path;
        swift::Identifier name_ident(ast->GetIdentifier(name));
        llvm::SmallVector<swift::ValueDecl*, 4> decls;
        if (priv_decl_id)
            result._module.lookupMember(name_ident, ast->GetIdentifier(priv_decl_id.getValue().c_str()), decls);
        else
            result._module.lookupQualified(name_ident, 0,  NULL, decls);
        if (!decls.empty())
        {
            bool check_type_aliases = false;
            // Look for an exact match first
            for (auto decl : decls)
            {
                const swift::DeclKind curr_decl_kind = decl->getKind();
                if (curr_decl_kind == decl_kind)
                {
                    result._decls.assign(1, decl);
                    if (decl->hasType())
                    {
                        result._types.assign(1, decl->getType());
                        swift::MetatypeType *meta_type = result._types.back()->getAs<swift::MetatypeType>();
                        if (meta_type)
                            result._types.back() = meta_type->getInstanceType();
                    }
                    else
                    {
                        result._types.assign(1, swift::Type());
                    }
                    return true;
                } else if (curr_decl_kind == swift::DeclKind::TypeAlias)
                    check_type_aliases = true;
            }
            // If we didn't find any exact matches, accept any type aliases
            if (check_type_aliases)
            {
                for (auto decl : decls)
                {
                    if (decl->getKind() == swift::DeclKind::TypeAlias)
                    {
                        result._decls.assign(1, decl);
                        if (decl->hasType())
                        {
                            result._types.assign(1, decl->getType());
                            swift::MetatypeType *meta_type = result._types.back()->getAs<swift::MetatypeType>();
                            if (meta_type)
                                result._types.back() = meta_type->getInstanceType();
                        }
                        else
                        {
                            result._types.assign(1, swift::Type());
                        }
                        return true;
                    }
                }
            }
        }
    }
    result.Clear();
    result._error.SetErrorToGenericError();
    return false;
}

static size_t
FindNamedDecls (SwiftASTContext *ast,
                const llvm::StringRef &name,
                VisitNodeResult &result,
                DeclsLookupSource::PrivateDeclIdentifier priv_decl_id = DeclsLookupSource::PrivateDeclIdentifier())
{
    if (!result._decls.empty())
    {
        swift::Decl *parent_decl = result._decls.back();
        result._decls.clear();
        result._types.clear();
        if (parent_decl)
        {
            auto nominal_decl = llvm::dyn_cast<swift::NominalTypeDecl>(parent_decl);
            
            if (nominal_decl)
            {
                DeclsLookupSource lookup(DeclsLookupSource::GetDeclsLookupSource(nominal_decl));
                llvm::SmallVector<swift::ValueDecl *, 4> decls;
                lookup.lookupMember(ast->GetIdentifier(name),
                                    GetIdentifier(ast,priv_decl_id),
                                    decls);
                if (decls.empty())
                {
                    result._error.SetErrorStringWithFormat("no decl named '%s' found in '%s' (DeclKind=%u)",
                                                           name.str().c_str(),
                                                           nominal_decl->getName().get(),
                                                           (uint32_t)nominal_decl->getKind());
                }
                else
                {
                    for (swift::ValueDecl *decl : decls)
                    {
                        if (decl->hasType())
                        {
                            result._decls.push_back(decl);
                            swift::Type decl_type;
                            if (decl->hasType())
                            {
                                decl_type = decl->getType();
                                swift::MetatypeType *meta_type = decl_type->getAs<swift::MetatypeType>();
                                if (meta_type)
                                    decl_type = meta_type->getInstanceType();
                            }
                            result._types.push_back(decl_type);
                        }
                    }
                    return result._types.size();
                }
            }
            else
            {
                result._error.SetErrorStringWithFormat("decl is not a nominal_decl (DeclKind=%u), lookup for '%s' failed",
                                                       (uint32_t)parent_decl->getKind(),
                                                       name.str().c_str());
            }
        }
    }
    else  if (result._module)
    {
        swift::Module::AccessPathTy access_path;
        llvm::SmallVector<swift::ValueDecl*, 4> decls;
        if (priv_decl_id)
            result._module.lookupMember(ast->GetIdentifier(name),
                                        ast->GetIdentifier(priv_decl_id.getValue().c_str()),
                                        decls);
        else
            result._module.lookupValue(access_path, ast->GetIdentifier(name), swift::NLKind::QualifiedLookup, decls);
        if (decls.empty())
        {
            result._error.SetErrorStringWithFormat("no decl named '%s' found in module '%s'",
                                                   name.str().c_str(),
                                                   result._module.GetName().GetCString());
        }
        else
        {
            for (auto decl : decls)
            {
                if (decl->hasType())
                {
                    result._decls.push_back(decl);
                    if (decl->hasType())
                    {
                        result._types.push_back(decl->getType());
                        swift::MetatypeType *meta_type = result._types.back()->getAs<swift::MetatypeType>();
                        if (meta_type)
                            result._types.back() = meta_type->getInstanceType();
                    }
                    else
                    {
                        result._types.push_back(swift::Type());
                    }
                }
            }
            return result._types.size();
        }
    }
    result.Clear();
    result._error.SetErrorToGenericError();
    return false;
}

//static size_t
//FindDeclsWithKind (SwiftASTContext *ast,
//                   swift::DeclKind decl_kind,
//                   VisitNodeResult &result)
//{
//    if (!result._decls.empty())
//    {
//        swift::Decl *parent_decl = result._decls.back();
//        if (parent_decl)
//        {
//            auto nominal_decl = llvm::dyn_cast<swift::NominalTypeDecl>(parent_decl);
//            
//            if (nominal_decl)
//            {
//                for (auto decl : nominal_decl->lookupDirect(ast->GetIdentifier(name)))
//                {
//                    if (decl->getKind() == decl_kind)
//                    {
//                        result._decls.back() = decl;
//                        swift::Type decl_type;
//                        if (decl->hasType())
//                        {
//                            decl_type = decl->getType();
//                            swift::MetatypeType *meta_type = decl_type->getAs<swift::MetatypeType>();
//                            if (meta_type)
//                                decl_type = meta_type->getInstanceType();
//                        }
//                        if (result._types.empty())
//                            result._types.push_back(decl_type);
//                        else
//                            result._types.back() = decl_type;
//                        return true;
//                    }
//                }
//            }
//        }
//    }
//    else if (result._module)
//    {
//        swift::Module::AccessPathTy access_path;
//        llvm::SmallVector<swift::ValueDecl*, 4> decls;
//        swift::Identifier name_ident(ast->GetIdentifier(name));
//        swift::Type module_type(swift::ModuleType::get(result._module));
//        result._module->lookupQualified(module_type, name_ident, 0, NULL, decls);
//        if (!decls.empty())
//        {
//            for (auto decl : decls)
//            {
//                if (decl->getKind() == decl_kind)
//                {
//                    result._decls.assign(1, decl);
//                    if (decl->hasType())
//                    {
//                        result._types.assign(1, decl->getType());
//                        swift::MetatypeType *meta_type = result._types.back()->getAs<swift::MetatypeType>();
//                        if (meta_type)
//                            result._types.back() = meta_type->getInstanceType();
//                    }
//                    else
//                    {
//                        result._types.assign(1, swift::Type());
//                    }
//                    return true;
//                }
//            }
//        }
//    }
//    result.Clear();
//    result._error.SetErrorToGenericError();
//    return false;
//}

static const char *
SwiftDemangleNodeKindToCString(const swift::Demangle::Node::Kind node_kind)
{
#define NODE(e) case swift::Demangle::Node::Kind::e: return #e;

    switch (node_kind)
    {
#include "swift/Basic/DemangleNodes.def"
    }
    return "swift::Demangle::Node::Kind::???";
#undef NODE
}

static
swift::DeclKind
GetKindAsDeclKind (swift::Demangle::Node::Kind node_kind)
{
    switch (node_kind)
    {
        case swift::Demangle::Node::Kind::TypeAlias:
            return swift::DeclKind::TypeAlias;
        case swift::Demangle::Node::Kind::Structure:
            return swift::DeclKind::Struct;
        case swift::Demangle::Node::Kind::Class:
            return swift::DeclKind::Class;
        case swift::Demangle::Node::Kind::Allocator:
            return swift::DeclKind::Constructor;
        case swift::Demangle::Node::Kind::Function:
            return swift::DeclKind::Func;
        case swift::Demangle::Node::Kind::Enum:
            return swift::DeclKind::Enum;
        case swift::Demangle::Node::Kind::Protocol:
            return swift::DeclKind::Protocol;
        default:
            printf ("Missing alias for %s.\n", SwiftDemangleNodeKindToCString(node_kind));
            assert (0);
    }
}

// This should be called with a function type & its associated Decl.  If the type is not a function type,
// then we just return the original type, but we don't check that the Decl is the associated one.
// It is assumed you will get that right in calling this.
// Returns a version of the input type with the ExtInfo AbstractCC set correctly.
// This allows CompilerType::IsSwiftMethod to work properly off the swift Type.
// FIXME: we don't currently distinguish between Method & Witness.  These types don't actually get used
// to make Calling Convention choices - originally we were leaving them all at Normal...  But if we ever
// need to set it for that purpose we will have to fix that here.
static swift::TypeBase *
FixCallingConv (swift::Decl *in_decl, swift::TypeBase *in_type)
{
    if (!in_decl)
        return in_type;

    swift::AnyFunctionType *func_type = llvm::dyn_cast<swift::AnyFunctionType>(in_type);
    if (func_type)
    {
        swift::DeclContext *decl_context = in_decl->getDeclContext();
        if (decl_context && decl_context->isTypeContext())
        {
            // Add the ExtInfo:
            swift::AnyFunctionType::ExtInfo new_info(func_type->getExtInfo().withSILRepresentation(swift::SILFunctionTypeRepresentation::Method));
            return func_type->withExtInfo(new_info);
        }
    }
    return in_type;
}

static void
VisitNode (SwiftASTContext *ast,
           std::vector<swift::Demangle::NodePointer> &nodes,
           VisitNodeResult &result,
           const VisitNodeResult &generic_context, // set by GenericType case
           Log *log);


static void
VisitNodeAddressor (SwiftASTContext *ast,
                   std::vector<swift::Demangle::NodePointer> &nodes,
                   swift::Demangle::NodePointer& cur_node,
                   VisitNodeResult &result,
                   const VisitNodeResult &generic_context, // set by GenericType case
                   Log *log)
{
    // Addressors are apparently SIL-level functions of the form () -> RawPointer and they bear no connection to their original variable at the interface level
    swift::CanFunctionType swift_can_func_type = swift::CanFunctionType::get(ast->GetASTContext()->TheEmptyTupleType, ast->GetASTContext()->TheRawPointerType);
    result._types.push_back(swift_can_func_type.getPointer());
}

static void
VisitNodeGenerics (SwiftASTContext *ast,
                   std::vector<swift::Demangle::NodePointer> &nodes,
                   swift::Demangle::NodePointer& cur_node,
                   VisitNodeResult &result,
                   const VisitNodeResult &generic_context, // set by GenericType case
                   Log *log)
{
    llvm::SmallVector<swift::Type, 4> nested_types;
    VisitNodeResult associated_type_result;
    VisitNodeResult archetype_ref_result;
    VisitNodeResult archetype_type_result;
    for (swift::Demangle::Node::iterator pos = cur_node->begin(), end = cur_node->end(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::ArchetypeRef:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, archetype_ref_result, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::Archetype:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, archetype_type_result, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::AssociatedType:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, associated_type_result, generic_context, log);
                if (associated_type_result.HasSingleType())
                    nested_types.push_back(associated_type_result.GetFirstType());
                break;
            default:
                result._error.SetErrorStringWithFormat("%s encountered in generics children", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }
    
    if (archetype_ref_result.HasAnyDecls() || archetype_ref_result.HasAnyTypes())
        result = archetype_ref_result;
    else
        result = archetype_type_result;
}

static void
VisitNodeArchetype(SwiftASTContext *ast,
                   std::vector<swift::Demangle::NodePointer> &nodes,
                   swift::Demangle::NodePointer& cur_node,
                   VisitNodeResult &result,
                   const VisitNodeResult &generic_context, // set by GenericType case
                   Log *log)
{
    const llvm::StringRef& archetype_name(cur_node->getText());
    VisitNodeResult protocol_list;
    for (swift::Demangle::Node::iterator pos = cur_node->begin(), end = cur_node->end(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::ProtocolList:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, protocol_list, generic_context, log);
                break;
            default:
                result._error.SetErrorStringWithFormat("%s encountered in generics children", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }
    
    llvm::SmallVector<swift::Type, 1> conforms_to;
    if (protocol_list.HasSingleType())
        conforms_to.push_back(protocol_list.GetFirstType());
    
    if (auto ast_context = ast->GetASTContext())
    {
        result._types.push_back(swift::ArchetypeType::getNew(*ast_context,
                                                             nullptr,
                                                             (swift::AssociatedTypeDecl *)nullptr,
                                                             ast->GetASTContext()->getIdentifier(archetype_name),
                                                             conforms_to, swift::Type()));
    }
    else
    {
        result._error.SetErrorString("invalid ASTContext");
    }
}


static void
VisitNodeArchetypeRef(SwiftASTContext *ast,
                      std::vector<swift::Demangle::NodePointer> &nodes,
                      swift::Demangle::NodePointer& cur_node,
                      VisitNodeResult &result,
                      const VisitNodeResult &generic_context, // set by GenericType case
                      Log *log)
{
    const llvm::StringRef& archetype_name(cur_node->getText());
    swift::Type result_type;
    for (const swift::Type &archetype : generic_context._types)
    {
        const swift::ArchetypeType *cast_archetype = llvm::dyn_cast<swift::ArchetypeType>(archetype.getPointer());
        
        if (cast_archetype && !cast_archetype->getName().str().compare(archetype_name))
        {
            result_type = archetype;
            break;
        }
    }
    
    if (result_type)
        result._types.push_back(result_type);
    else
    {
        if (auto ast_context = ast->GetASTContext())
        {
            result._types.push_back(swift::ArchetypeType::getNew(*ast_context,
                                                                 nullptr,
                                                                 (swift::AssociatedTypeDecl *)nullptr,
                                                                 ast->GetASTContext()->getIdentifier(archetype_name),
                                                                 llvm::ArrayRef<swift::Type>(), swift::Type()));
        }
        else
        {
            result._error.SetErrorString("invalid ASTContext");
        }
    }
}

static void
VisitNodeAssociatedTypeRef (SwiftASTContext *ast,
                            std::vector<swift::Demangle::NodePointer> &nodes,
                            swift::Demangle::NodePointer& cur_node,
                            VisitNodeResult &result,
                            const VisitNodeResult &generic_context, // set by GenericType case
                            Log *log)
{
    swift::Demangle::NodePointer root = cur_node->getChild(0);
    swift::Demangle::NodePointer ident = cur_node->getChild(1);
    if (!root || !ident)
        return;
    nodes.push_back(root);
    VisitNodeResult type_result;
    VisitNode (ast, nodes, type_result, generic_context, log);
    if (type_result._types.size() == 1)
    {
        swift::TypeBase* type = type_result._types[0].getPointer();
        if (type)
        {
            swift::ArchetypeType* archetype = type->getAs<swift::ArchetypeType>();
            if (archetype)
            {
                swift::Identifier identifier = ast->GetASTContext()->getIdentifier(ident->getText());
                if (archetype->hasNestedType(identifier))
                {
                    swift::Type nested = archetype->getNestedTypeValue(identifier);
                    if (nested)
                    {
                        result._types.push_back(nested);
                        result._module = type_result._module;
                        return;
                    }
                }
            }
        }
    }
    result._types.clear();
    result._error.SetErrorStringWithFormat("unable to find associated type %s in context", ident->getText().c_str());
}

static void
VisitNodeBoundGeneric (SwiftASTContext *ast,
                       std::vector<swift::Demangle::NodePointer> &nodes,
                       swift::Demangle::NodePointer& cur_node,
                       VisitNodeResult &result,
                       const VisitNodeResult &generic_context, // set by GenericType case
                       Log *log)
{
    if (cur_node->begin() != cur_node->end())
    {
        VisitNodeResult generic_type_result;
        VisitNodeResult template_types_result;
        
        swift::Demangle::Node::iterator end = cur_node->end();
        for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
        {
            const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
            switch (child_node_kind)
            {
                case swift::Demangle::Node::Kind::Type:
                case swift::Demangle::Node::Kind::Metatype:
                    nodes.push_back(*pos);
                    VisitNode (ast, nodes, generic_type_result, generic_context, log);
                    break;
                case swift::Demangle::Node::Kind::TypeList:
                    nodes.push_back(*pos);
                    VisitNode (ast, nodes, template_types_result, generic_context, log);
                    break;
                default:
                    if (log)
                        log->Printf ("error: unsupported child node: %s", SwiftDemangleNodeKindToCString(child_node_kind));
                    break;
            }
        }

        if (generic_type_result._types.size() == 1 && !template_types_result._types.empty())
        {
            swift::NominalTypeDecl *nominal_type_decl = llvm::dyn_cast<swift::NominalTypeDecl>(generic_type_result._decls.front());
            swift::DeclContext * parent_decl = nominal_type_decl->getParent();
            swift::Type parent_type;
            if (parent_decl->isTypeContext())
                parent_type = parent_decl->getDeclaredTypeOfContext();
            result._types.push_back(swift::Type(swift::BoundGenericType::get(nominal_type_decl,
                                                                             parent_type,
                                                                             template_types_result._types)));

        }
    }
}

static void
VisitNodeBuiltinTypeName (SwiftASTContext *ast,
                          std::vector<swift::Demangle::NodePointer> &nodes,
                          swift::Demangle::NodePointer& cur_node,
                          VisitNodeResult &result,
                          const VisitNodeResult &generic_context, // set by GenericType case
                          Log *log)
{
    std::string builtin_name = cur_node->getText();
    
    llvm::StringRef builtin_name_ref(builtin_name);
    
    if (builtin_name_ref.startswith("Builtin."))
    {
        llvm::StringRef stripped_name_ref = builtin_name_ref.drop_front(strlen("Builtin."));
        llvm::SmallVector<swift::ValueDecl *, 1> builtin_decls;
    
        result._module = DeclsLookupSource::GetDeclsLookupSource(*ast, ConstString("Builtin"));

        if (!FindNamedDecls(ast, stripped_name_ref, result))
        {
            result.Clear();
            result._error.SetErrorStringWithFormat("Couldn't find %s in the builtin module", builtin_name.c_str());
        }
    }
    else
    {
        result._error.SetErrorStringWithFormat("BuiltinTypeName %s doesn't start with Builtin.", builtin_name.c_str());
    }
}

static void
VisitNodeConstructor (SwiftASTContext *ast,
                      std::vector<swift::Demangle::NodePointer> &nodes,
                      swift::Demangle::NodePointer& cur_node,
                      VisitNodeResult &result,
                      const VisitNodeResult &generic_context, // set by GenericType case
                      Log *log)
{
    VisitNodeResult kind_type_result;
    VisitNodeResult type_result;
    
    swift::Demangle::Node::iterator end = cur_node->end();
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::Enum:
            case swift::Demangle::Node::Kind::Class:
            case swift::Demangle::Node::Kind::Structure:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, kind_type_result, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::Type:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, type_result, generic_context, log);
                break;
            default:
                if (log)
                    log->Printf ("error: unsupported child node: %s", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }
    
    if (kind_type_result.HasSingleType() && type_result.HasSingleType())
    {
        bool found = false;
        const size_t n = FindNamedDecls(ast, llvm::StringRef("init"), kind_type_result);
        if (n == 1)
        {
            found = true;
            kind_type_result._types[0] = FixCallingConv(kind_type_result._decls[0], kind_type_result._types[0].getPointer());
            result = kind_type_result;
        }
        else if (n > 0)
        {
            const size_t num_kind_type_results = kind_type_result._types.size();
            for (size_t i=0; i<num_kind_type_results && !found; ++i)
            {
                auto &identifier_type = kind_type_result._types[i];
                if (identifier_type && identifier_type->getKind() == type_result._types.front()->getKind())
                {
                    // These are the same kind of type, we need to disambiguate them
                    switch (identifier_type->getKind())
                    {
                    default:
                        break;
                    case swift::TypeKind::Function:
                        {
                            const swift::AnyFunctionType* identifier_func = identifier_type->getAs<swift::AnyFunctionType>();
                            const swift::AnyFunctionType* type_func = type_result._types.front()->getAs<swift::AnyFunctionType>();
                            if (swift::CanType(identifier_func->getResult()->getDesugaredType()->getCanonicalType()) == swift::CanType(type_func->getResult()->getDesugaredType()->getCanonicalType()) &&
                                swift::CanType(identifier_func->getInput()->getDesugaredType()->getCanonicalType()) == swift::CanType(type_func->getInput()->getDesugaredType()->getCanonicalType()))
                            {
                                result._module = kind_type_result._module;
                                result._decls.push_back(kind_type_result._decls[i]);
                                result._types.push_back(FixCallingConv(kind_type_result._decls[i], kind_type_result._types[i].getPointer()));
                                found = true;
                            }
                        }
                        break;
                    }
                }
            }
        }
        // Didn't find a match, just return the raw function type
        if (!found)
            result = type_result;

    }
}

static void
VisitNodeDestructor (SwiftASTContext *ast,
                     std::vector<swift::Demangle::NodePointer> &nodes,
                     swift::Demangle::NodePointer& cur_node,
                     VisitNodeResult &result,
                     const VisitNodeResult &generic_context, // set by GenericType case
                     Log *log)
{
    VisitNodeResult kind_type_result;
    
    swift::Demangle::Node::iterator end = cur_node->end();
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::Enum:
            case swift::Demangle::Node::Kind::Class:
            case swift::Demangle::Node::Kind::Structure:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, kind_type_result, generic_context, log);
                break;
            default:
                if (log)
                    log->Printf ("error: unsupported child node: %s", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }
    
    if (kind_type_result.HasSingleType())
    {
        bool found = false;
        const size_t n = FindNamedDecls(ast, llvm::StringRef("deinit"), kind_type_result);
        if (n == 1)
        {
            found = true;
            kind_type_result._types[0] = FixCallingConv(kind_type_result._decls[0],
                                                        kind_type_result._types[0].getPointer());
            result = kind_type_result;
        }
        else if (n > 0)
        {
            // I can't think of a reason why we would get more than one decl called deinit here, but
            // just in case, if it is a function type, we should remember it.
            const size_t num_kind_type_results = kind_type_result._types.size();
            for (size_t i=0; i<num_kind_type_results && !found; ++i)
            {
                auto &identifier_type = kind_type_result._types[i];
                if (identifier_type)
                {
                    switch (identifier_type->getKind())
                    {
                    default:
                        break;
                    case swift::TypeKind::Function:
                        {
                            result._module = kind_type_result._module;
                            result._decls.push_back(kind_type_result._decls[i]);
                            result._types.push_back(FixCallingConv(kind_type_result._decls[i],
                                                                   kind_type_result._types[i].getPointer()));
                            found = true;
                        }
                        break;
                    }
                }
            }
        }
    }
}

static void
VisitNodeDeclContext (SwiftASTContext *ast,
                      std::vector<swift::Demangle::NodePointer> &nodes,
                      swift::Demangle::NodePointer& cur_node,
                      VisitNodeResult &result,
                      const VisitNodeResult &generic_context, // set by GenericType case
                      Log *log)
{
    switch (cur_node->getNumChildren())
    {
        default:
            result._error.SetErrorStringWithFormat("DeclContext had %llu children, giving up", (unsigned long long)cur_node->getNumChildren());
            break;
        case 0:
            result._error.SetErrorString("empty DeclContext unusable");
            break;
        case 1:
            // nominal type
            nodes.push_back(cur_node->getFirstChild());
            VisitNode (ast, nodes, result, generic_context, log);
            break;
        case 2:
            // function type: decl-ctx + type
            // FIXME: we should just be able to demangle the DeclCtx and resolve the function
            // this is fragile and will easily break
            swift::Demangle::NodePointer path = cur_node->getFirstChild();
            nodes.push_back(path);
            VisitNodeResult found_decls;
            VisitNode (ast, nodes, found_decls, generic_context, log);
            swift::Demangle::NodePointer generics = cur_node->getChild(1);
            if (generics->getChild(0) == nullptr)
                break;
            generics = generics->getFirstChild();
            if (generics->getKind() != swift::Demangle::Node::Kind::GenericType)
                break;
            if (generics->getChild(0) == nullptr)
                break;
            generics = generics->getFirstChild();
//                        if (generics->getKind() != swift::Demangle::Node::Kind::ArchetypeList)
//                            break;
            swift::AbstractFunctionDecl *func_decl = nullptr;
            for (swift::Decl* decl : found_decls._decls)
            {
                func_decl = llvm::dyn_cast<swift::AbstractFunctionDecl>(decl);
                if (!func_decl)
                    continue;
                swift::GenericParamList *gen_params = func_decl->getGenericParams();
                if (!gen_params)
                    continue;
            }
            if (func_decl)
            {
                result._module = found_decls._module;
                result._decls.push_back(func_decl);
                result._types.push_back(func_decl->getType().getPointer());
            }
            else
                result._error.SetErrorString("could not find a matching function for the DeclContext");
            break;
    }
}

static void
VisitNodeExplicitClosure (SwiftASTContext *ast,
                          std::vector<swift::Demangle::NodePointer> &nodes,
                          swift::Demangle::NodePointer& cur_node,
                          VisitNodeResult &result,
                          const VisitNodeResult &generic_context, // set by GenericType case
                          Log *log)
{
    // FIXME: closures are mangled as hanging off a function, but they really aren't
    // so we cannot really do a lot about them, other than make a function type
    // for whatever their advertised type is, and cross fingers
    VisitNodeResult function_result;
    uint64_t index = UINT64_MAX;
    VisitNodeResult closure_type_result;
    VisitNodeResult module_result;
    swift::Demangle::Node::iterator end = cur_node->end();
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            default:
                result._error.SetErrorStringWithFormat("%s encountered in ExplicitClosure children", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
            case swift::Demangle::Node::Kind::Module:
                nodes.push_back((*pos));
                VisitNode (ast, nodes, module_result, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::Function:
                nodes.push_back((*pos));
                VisitNode (ast, nodes, function_result, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::Number:
                index = (*pos)->getIndex();
                break;
            case swift::Demangle::Node::Kind::Type:
                nodes.push_back((*pos));
                VisitNode (ast, nodes, closure_type_result, generic_context, log);
                break;
        }
    }
    if (closure_type_result.HasSingleType())
        result._types.push_back(closure_type_result._types.front());
    else
        result._error.SetErrorString("multiple potential candidates to be this closure's type");
    // FIXME: closures are not lookupable by compiler team's decision
    // ("You cannot perform lookup into local contexts." x3)
    // so we at least store the module the closure came from to enable
    // further local types lookups
    if (module_result._module)
        result._module = module_result._module;
}

static void
VisitNodeExtension (SwiftASTContext *ast,
                    std::vector<swift::Demangle::NodePointer> &nodes,
                    swift::Demangle::NodePointer& cur_node,
                    VisitNodeResult &result,
                    const VisitNodeResult &generic_context, // set by GenericType case
                    Log *log)
{
    VisitNodeResult module_result;
    VisitNodeResult type_result;
    Error error;
    swift::Demangle::Node::iterator end = cur_node->end();
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            default:
                result._error.SetErrorStringWithFormat("%s encountered in extension children", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
    
            case swift::Demangle::Node::Kind::Module:
                nodes.push_back((*pos));
                VisitNode(ast, nodes, module_result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Class:
            case swift::Demangle::Node::Kind::Enum:
            case swift::Demangle::Node::Kind::Structure:
                nodes.push_back((*pos));
                VisitNode(ast, nodes, type_result, generic_context, log);
                break;
        }
    }
    
    if (module_result._module)
    {
        if (type_result._decls.size() == 1)
        {
            swift::Decl *decl = type_result._decls[0];
            swift::NominalTypeDecl *nominal_decl = llvm::dyn_cast_or_null<swift::NominalTypeDecl>(decl);
            if (nominal_decl)
            {
                result._module = DeclsLookupSource::GetDeclsLookupSource(module_result._module, nominal_decl);
            }
            else
                result._error.SetErrorString("unable to find nominal type for extension");
        }
        else
            result._error.SetErrorString("unable to find unique type for extension");
    }
    else
        result._error.SetErrorString("unable to find module name for extension");
}

static bool
AreBothFunctionTypes (swift::TypeKind a,
                      swift::TypeKind b)
{
    bool is_first = false, is_second = false;
    if (a >= swift::TypeKind::First_AnyFunctionType &&
        a <= swift::TypeKind::Last_AnyFunctionType)
        is_first = true;
    if (b >= swift::TypeKind::First_AnyFunctionType &&
        b <= swift::TypeKind::Last_AnyFunctionType)
        is_second = true;
    return (is_first && is_second);
}

static bool
CompareFunctionTypes (const swift::AnyFunctionType *f,
                      const swift::AnyFunctionType *g,
                      bool *input_matches = nullptr,
                      bool *output_matches = nullptr)
{
    bool in_matches = false, out_matches = false;
    if (nullptr == f)
        return (nullptr == g);
    if (nullptr == g)
        return false;
    
    auto f_input = f->getInput().getCanonicalTypeOrNull();
    auto g_input = g->getInput().getCanonicalTypeOrNull();
    
    auto f_output = f->getResult().getCanonicalTypeOrNull();
    auto g_output = g->getResult().getCanonicalTypeOrNull();
    
    if (f_input == g_input)
    {
        in_matches = true;
        if (f_output == g_output)
            out_matches = true;
    }
    
    if (input_matches)
        *input_matches = in_matches;
    if (output_matches)
        *output_matches = out_matches;
    
    return (in_matches && out_matches);
}

                      

// VisitNodeFunction gets used for Function, Variable and Allocator:
static void
VisitNodeFunction (SwiftASTContext *ast,
                   std::vector<swift::Demangle::NodePointer> &nodes,
                   swift::Demangle::NodePointer& cur_node,
                   VisitNodeResult &result,
                   const VisitNodeResult &generic_context, // set by GenericType case
                   Log *log)
{
    VisitNodeResult identifier_result;
    VisitNodeResult type_result;
    VisitNodeResult decl_scope_result;
    swift::Demangle::Node::iterator end = cur_node->end();
    bool found_univocous = false;
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
    {
        if (found_univocous)
            break;
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            default:
                result._error.SetErrorStringWithFormat("%s encountered in function children", SwiftDemangleNodeKindToCString(child_node_kind));
                break;

                // TODO: any other possible containers?
            case swift::Demangle::Node::Kind::Class:
            case swift::Demangle::Node::Kind::Enum:
            case swift::Demangle::Node::Kind::Module:
            case swift::Demangle::Node::Kind::Structure:
                nodes.push_back((*pos));
                VisitNode (ast, nodes, decl_scope_result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Identifier:
            case swift::Demangle::Node::Kind::InfixOperator:
            case swift::Demangle::Node::Kind::PrefixOperator:
            case swift::Demangle::Node::Kind::PostfixOperator:
                FindNamedDecls(ast, (*pos)->getText(), decl_scope_result);
                if (decl_scope_result._decls.size() == 0)
                {
                    result._error.SetErrorStringWithFormat("demangled identifier %s could not be found by name lookup",(*pos)->getText().c_str());
                    break;
                }
                std::copy(decl_scope_result._decls.begin(),
                          decl_scope_result._decls.end(),
                          back_inserter(identifier_result._decls));
                std::copy(decl_scope_result._types.begin(),
                          decl_scope_result._types.end(),
                          back_inserter(identifier_result._types));
                identifier_result._module = decl_scope_result._module;
                if (decl_scope_result._decls.size() == 1)
                    found_univocous = true;
                break;
            
            case swift::Demangle::Node::Kind::Type:
                nodes.push_back((*pos));
                VisitNode (ast, nodes, type_result, generic_context, log);
                break;
        }
    }
    
//                    if (node_kind == swift::Demangle::Node::Kind::Allocator)
//                    {
//                        // For allocators we don't have an identifier for the name, we will
//                        // need to extract it from the class or struct in "identifier_result"
//                        //Find
//                        if (identifier_result.HasSingleType())
//                        {
//                            // This contains the class or struct
//                            llvm::StringRef init_name("init");
//                        
//                            if (FindFirstNamedDeclWithKind(ast, init_name, swift::DeclKind::Constructor, identifier_result))
//                            {
//                            }
//                        }
//                    }
    
    if (identifier_result._types.size() == 1)
    {
        result._module = identifier_result._module;
        result._decls.push_back(identifier_result._decls[0]);
        result._types.push_back(FixCallingConv(identifier_result._decls[0], identifier_result._types[0].getPointer()));
    }
    else if (type_result.HasSingleType())
    {
        const size_t num_identifier_results = identifier_result._types.size();
        bool found = false;
        for (size_t i=0; i<num_identifier_results && !found; ++i)
        {
            auto &identifier_type = identifier_result._types[i];
            if (!identifier_type)
                continue;
            if (AreBothFunctionTypes(identifier_type->getKind(), type_result._types.front()->getKind()))
            {
                const swift::AnyFunctionType* identifier_func = identifier_type->getAs<swift::AnyFunctionType>();
                const swift::AnyFunctionType* type_func = type_result._types.front()->getAs<swift::AnyFunctionType>();
                if (CompareFunctionTypes(identifier_func, type_func))
                {
                    result._module = identifier_result._module;
                    result._decls.push_back(identifier_result._decls[i]);
                    result._types.push_back(FixCallingConv(identifier_result._decls[i], identifier_result._types[i].getPointer()));
                    found = true;
                }
            }
        }
        // Didn't find a match, just return the raw function type
        if (!found)
            result = type_result;
    }
}

static void
VisitNodeFunctionType (SwiftASTContext *ast,
                       std::vector<swift::Demangle::NodePointer> &nodes,
                       swift::Demangle::NodePointer& cur_node,
                       VisitNodeResult &result,
                       const VisitNodeResult &generic_context, // set by GenericType case
                       Log *log)
{
    VisitNodeResult arg_type_result;
    VisitNodeResult return_type_result;
    swift::Demangle::Node::iterator end = cur_node->end();
    bool is_in_class = false;
    bool throws = false;
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::Class:
                is_in_class = true;
                // Fall through.
            case swift::Demangle::Node::Kind::Structure:
                {
                    VisitNodeResult class_type_result;
                    nodes.push_back(*pos);
                    VisitNode (ast, nodes, class_type_result, generic_context, log);
                }
                break;
            case swift::Demangle::Node::Kind::ArgumentTuple:
            case swift::Demangle::Node::Kind::Metatype:
                {
                    nodes.push_back(*pos);
                    VisitNode (ast, nodes, arg_type_result, generic_context, log);
                }
                break;
            case swift::Demangle::Node::Kind::ThrowsAnnotation:
                throws = true;
                break;
            case swift::Demangle::Node::Kind::ReturnType:
                {
                    nodes.push_back(*pos);
                    VisitNode (ast, nodes, return_type_result, generic_context, log);
                }
                break;
            default:
                if (log)
                    log->Printf ("error: unsupported child node under a swift::Demangle::Node::Kind::FunctionType: %s",
                                 SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }
    CompilerType arg_clang_type;
    CompilerType return_clang_type;
    
    switch (arg_type_result._types.size())
    {
        case 0:
            arg_clang_type = ast->CreateTupleType(std::vector<CompilerType>());
            break;
        case 1:
            arg_clang_type = CompilerType(ast->GetASTContext(), arg_type_result._types.front().getPointer());
            break;
        default:
            result._error.SetErrorString("too many argument types for a function type");
            break;
    }
    
    switch (return_type_result._types.size())
    {
        case 0:
            return_clang_type = ast->CreateTupleType(std::vector<CompilerType>());
            break;
        case 1:
            return_clang_type = CompilerType(ast->GetASTContext(), return_type_result._types.front().getPointer());
            break;
        default:
            result._error.SetErrorString("too many return types for a function type");
            break;
    }
    
    if (arg_clang_type && return_clang_type)
    {
        if (log)
        {
            log->Printf ("arg_type: %s", arg_clang_type.GetTypeName().GetCString());
            log->Printf ("return_type: %s", return_clang_type.GetTypeName().GetCString());
        }
        result._types.push_back(GetSwiftType(ast->CreateFunctionType(arg_clang_type, return_clang_type, throws)));
    }
}

static void
VisitNodeGenericType (SwiftASTContext *ast,
                      std::vector<swift::Demangle::NodePointer> &nodes,
                      swift::Demangle::NodePointer& cur_node,
                      VisitNodeResult &result,
                      const VisitNodeResult &generic_context, // set by GenericType case
                      Log *log)
{
    VisitNodeResult new_generic_context;
    std::copy(generic_context._types.begin(), generic_context._types.end(), back_inserter(new_generic_context._types));
    for (swift::Demangle::Node::iterator pos = cur_node->begin(), end = cur_node->end(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::Generics:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, new_generic_context, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::Type:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, result, new_generic_context, log);
                break;
            default:
                result._error.SetErrorStringWithFormat("%s encountered in generic type children", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }
}

static void
VisitNodeSetterGetter (SwiftASTContext *ast,
                       std::vector<swift::Demangle::NodePointer> &nodes,
                       swift::Demangle::NodePointer& cur_node,
                       VisitNodeResult &result,
                       const VisitNodeResult &generic_context, // set by GenericType case
                       Log *log)
{
    VisitNodeResult decl_ctx_result;
    std::string identifier;
    VisitNodeResult type_result;
    swift::Demangle::Node::Kind node_kind = cur_node->getKind();
    
    for (swift::Demangle::Node::iterator pos = cur_node->begin(), end = cur_node->end(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::Class:
            case swift::Demangle::Node::Kind::Module:
            case swift::Demangle::Node::Kind::Structure:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, decl_ctx_result, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::Identifier:
                identifier.assign((*pos)->getText());
                break;
            case swift::Demangle::Node::Kind::Type:
                nodes.push_back(*pos);
                VisitNode (ast, nodes, type_result, generic_context, log);
                break;
            default:
                result._error.SetErrorStringWithFormat("%s encountered in generic type children", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }
    
    if (identifier == "subscript")
    {
        // Subscript setters and getters are named with the reserved word "subscript".
        // Since there can be many subscripts for the same nominal type, we need to
        // find the one matching the specified type.

        FindNamedDecls (ast, identifier, decl_ctx_result);
        size_t num_decls = decl_ctx_result._decls.size();
        
        if (num_decls == 0)
        {
            result._error.SetErrorStringWithFormat("Could not find a subscript decl");
            return;
        }
        
        swift::SubscriptDecl *subscript_decl;
        const swift::AnyFunctionType* type_func = type_result._types.front()->getAs<swift::AnyFunctionType>();
        
        swift::CanType type_result_type(type_func->getResult()->getDesugaredType()->getCanonicalType());
        swift::CanType type_input_type(type_func->getInput()->getDesugaredType()->getCanonicalType());
        
        
        swift::FuncDecl *identifier_func = nullptr;
        
        for (size_t i = 0; i < num_decls; i++)
        {
            subscript_decl = llvm::dyn_cast_or_null<swift::SubscriptDecl>(decl_ctx_result._decls[i]);
            if (subscript_decl)
            {
                switch (node_kind)
                {
                case swift::Demangle::Node::Kind::Getter:
                    identifier_func = subscript_decl->getGetter();
                    break;
                case swift::Demangle::Node::Kind::Setter:
                    identifier_func = subscript_decl->getGetter();
                    break;
                case swift::Demangle::Node::Kind::DidSet:
                    identifier_func = subscript_decl->getDidSetFunc();
                    break;
                case swift::Demangle::Node::Kind::WillSet:
                    identifier_func = subscript_decl->getWillSetFunc();
                    break;
                default:
                    identifier_func = nullptr;
                    break;
                }
                
                if (identifier_func && identifier_func->getType())
                {
                    const swift::AnyFunctionType *identifier_func_type = identifier_func->getType()->getAs<swift::AnyFunctionType>();
                    if (identifier_func_type)
                    {
                        // Swift function types are formally functions that take the class and return the method,
                        // we have to strip off the first level of function call to compare against the type
                        // from the demangled name.
                        const swift::AnyFunctionType *identifier_uncurried_result = identifier_func_type->getResult()->getAs<swift::AnyFunctionType>();
                        if (identifier_uncurried_result)
                        {
                            swift::CanType identifier_result_type(identifier_uncurried_result->getResult()->getDesugaredType()->getCanonicalType());
                            swift::CanType identifier_input_type(identifier_uncurried_result->getInput()->getDesugaredType()->getCanonicalType());
                            if (identifier_result_type == type_result_type &&
                                identifier_input_type == type_input_type)
                            {
                                break;
                            }
                        }
                    }
                }
            }
            identifier_func = nullptr;
        }
        
        if (identifier_func)
        {
            result._decls.push_back(identifier_func);
            result._types.push_back(FixCallingConv(identifier_func, identifier_func->getType().getPointer()));
        }
        else
        {
            result._error.SetErrorStringWithFormat("could not find a matching subscript signature");
        }
    }
    else
    {
        // Otherwise this is a getter/setter/etc for an variable.  Currently you can't write a getter/setter that
        // takes a different type from the type of the variable.  So there is only one possible function.
        swift::AbstractStorageDecl *var_decl = nullptr;
    
        FindFirstNamedDeclWithKind(ast, identifier, swift::DeclKind::Var, decl_ctx_result);
        
        if (decl_ctx_result._decls.size() == 1)
        {
            var_decl = llvm::dyn_cast_or_null<swift::VarDecl>(decl_ctx_result._decls[0]);
        }
        else if (decl_ctx_result._decls.size() > 0)
        {
            // TODO: can we use the type to pick the right one? can we really have multiple variables with the same name?
            result._error.SetErrorStringWithFormat("multiple variables with the same name %s",identifier.c_str());
            return;
        }
        else
        {
            result._error.SetErrorStringWithFormat("no variables with the name %s",identifier.c_str());
            return;
        }
        
        if (var_decl)
        {
            swift::FuncDecl *decl = nullptr;
            
            if (node_kind == swift::Demangle::Node::Kind::DidSet && var_decl->getDidSetFunc())
            {
                decl = var_decl->getDidSetFunc();
            }
            else if (node_kind == swift::Demangle::Node::Kind::Getter && var_decl->getGetter())
            {
                decl = var_decl->getGetter();
            }
            else if (node_kind == swift::Demangle::Node::Kind::Setter && var_decl->getSetter())
            {
                decl = var_decl->getSetter();
            }
            else if (node_kind == swift::Demangle::Node::Kind::WillSet && var_decl->getWillSetFunc())
            {
                decl = var_decl->getWillSetFunc();
            }
            
            if (decl)
            {
                result._decls.push_back(decl);
                result._types.push_back(FixCallingConv(decl, decl->getType().getPointer()));
            }
            else
            {
                result._error.SetErrorStringWithFormat("could not retrieve %s for variable %s",
                                                       SwiftDemangleNodeKindToCString(node_kind),
                                                       identifier.c_str());
                return;
            }
        }
        else
        {
            result._error.SetErrorStringWithFormat("no decl object for %s",identifier.c_str());
            return;
        }
    }
}

static void
VisitNodeIdentifier (SwiftASTContext *ast,
                     std::vector<swift::Demangle::NodePointer> &nodes,
                     swift::Demangle::NodePointer& cur_node,
                     VisitNodeResult &result,
                     const VisitNodeResult &generic_context, // set by GenericType case
                     Log *log)
{
    swift::Demangle::NodePointer parent_node = nodes[nodes.size() - 2];
    swift::DeclKind decl_kind = GetKindAsDeclKind (parent_node->getKind());
    
    if (!FindFirstNamedDeclWithKind (ast, cur_node->getText(), decl_kind, result))
    {
        if (log)
            log->Printf("error: swift::Demangle::Node::Kind::Identifier '%s'", cur_node->getText().c_str());
        if (result._error.AsCString() == NULL)
            result._error.SetErrorStringWithFormat("unable to find Node::Kind::Identifier '%s'", cur_node->getText().c_str());
    }
}

static void
VisitNodeLocalDeclName (SwiftASTContext *ast,
                        std::vector<swift::Demangle::NodePointer> &nodes,
                        swift::Demangle::NodePointer& cur_node,
                        VisitNodeResult &result,
                        const VisitNodeResult &generic_context, // set by GenericType case
                        Log *log)
{
    swift::Demangle::NodePointer parent_node = nodes[nodes.size() - 2];
    std::string remangledNode = swift::Demangle::mangleNode(parent_node);
    swift::TypeDecl *decl = result._module.lookupLocalType(remangledNode);
    if (!decl)
        result._error.SetErrorStringWithFormat("unable to lookup local type %s", remangledNode.c_str());
    else
    {
        // if this were to come from a closure, there may be no decl - just a module
        if (!result._decls.empty())
            result._decls.pop_back();
        if (!result._types.empty())
            result._types.pop_back();
        
        result._decls.push_back(decl);
        auto type = decl->getType();
        if (swift::MetatypeType *metatype = llvm::dyn_cast_or_null<swift::MetatypeType>(type.getPointer()))
            type = metatype->getInstanceType();
        result._types.push_back(type);
    }
}

static void
VisitNodePrivateDeclName (SwiftASTContext *ast,
                          std::vector<swift::Demangle::NodePointer> &nodes,
                          swift::Demangle::NodePointer& cur_node,
                          VisitNodeResult &result,
                          const VisitNodeResult &generic_context, // set by GenericType case
                          Log *log)
{
    swift::Demangle::NodePointer parent_node = nodes[nodes.size() - 2];
    swift::DeclKind decl_kind = GetKindAsDeclKind (parent_node->getKind());
    
    if (cur_node->getNumChildren() != 2)
    {
        if (log)
            log->Printf("error: swift::Demangle::Node::Kind::PrivateDeclName expected to have two children");
        if (result._error.AsCString() == NULL)
            result._error.SetErrorStringWithFormat("unable to retrieve content for Node::Kind::PrivateDeclName");
        return;
    }
    
    swift::Demangle::NodePointer priv_decl_id_node (cur_node->getChild(0));
    swift::Demangle::NodePointer id_node (cur_node->getChild(1));
    
    if (!priv_decl_id_node->hasText() || !id_node->hasText())
    {
        if (log)
            log->Printf("error: swift::Demangle::Node::Kind::PrivateDeclName expected to have two children with text content");
        if (result._error.AsCString() == NULL)
            result._error.SetErrorStringWithFormat("unable to retrieve content for Node::Kind::PrivateDeclName");
        return;
    }
    
    if (!FindFirstNamedDeclWithKind (ast, id_node->getText(), decl_kind, result, priv_decl_id_node->getText()))
    {
        if (log)
            log->Printf("error: swift::Demangle::Node::Kind::PrivateDeclName '%s' in '%s'", id_node->getText().c_str(), priv_decl_id_node->getText().c_str());
        if (result._error.AsCString() == NULL)
            result._error.SetErrorStringWithFormat("unable to find Node::Kind::PrivateDeclName '%s' in '%s'", id_node->getText().c_str(), priv_decl_id_node->getText().c_str());
    }
}

static void
VisitNodeInOut (SwiftASTContext *ast,
                std::vector<swift::Demangle::NodePointer> &nodes,
                swift::Demangle::NodePointer& cur_node,
                VisitNodeResult &result,
                const VisitNodeResult &generic_context, // set by GenericType case
                Log *log)
{
    nodes.push_back(cur_node->getFirstChild());
    VisitNodeResult type_result;
    VisitNode (ast, nodes, type_result, generic_context, log);
    if (type_result._types.size() == 1 && type_result._types[0])
    {
        result._types.push_back(swift::Type(swift::LValueType::get(type_result._types[0])));
    }
    else
    {
        result._error.SetErrorString("couldn't resolve referent type");
    }
}

static void
VisitNodeMetatype (SwiftASTContext *ast,
                   std::vector<swift::Demangle::NodePointer> &nodes,
                   swift::Demangle::NodePointer& cur_node,
                   VisitNodeResult &result,
                   const VisitNodeResult &generic_context, // set by GenericType case
                   Log *log)
{
    auto iter = cur_node->begin();
    auto end = cur_node->end();
    
    llvm::Optional<swift::MetatypeRepresentation> metatype_repr;
    VisitNodeResult type_result;
    
    for (; iter != end; ++iter)
    {
        switch ((*iter)->getKind())
        {
            case swift::Demangle::Node::Kind::Type:
                nodes.push_back(*iter);
                VisitNode(ast, nodes, type_result, generic_context, log);
                break;
            case swift::Demangle::Node::Kind::MetatypeRepresentation:
                if ( (*iter)->getText() == "@thick" )
                    metatype_repr = swift::MetatypeRepresentation::Thick;
                else if ( (*iter)->getText() == "@thin" )
                    metatype_repr = swift::MetatypeRepresentation::Thin;
                else if ( (*iter)->getText() == "@objc" )
                    metatype_repr = swift::MetatypeRepresentation::ObjC;
                else
                    ; // leave it alone if we don't understand the representation
                break;
            default:
                break;
        }
        
    }

    if (type_result.HasSingleType())
    {
        result._types.push_back(swift::MetatypeType::get(type_result._types[0], metatype_repr));
    }
    else
    {
        result._error.SetErrorStringWithFormat("instance type for metatype cannot be uniquely resolved");
        return;
    }
    
}

static void
VisitNodeModule (SwiftASTContext *ast,
                 std::vector<swift::Demangle::NodePointer> &nodes,
                 swift::Demangle::NodePointer& cur_node,
                 VisitNodeResult &result,
                 const VisitNodeResult &generic_context, // set by GenericType case
                 Log *log)
{
    Error error;
    const char *module_name = cur_node->getText().c_str();
    if (!module_name || *module_name == '\0')
    {
        result._error.SetErrorStringWithFormat("error: empty module name.");
        return;
    }
    
    result._module = DeclsLookupSource::GetDeclsLookupSource(*ast, ConstString(module_name));
    if (!result._module)
    {
        if (log)
            log->Printf("error: unable to load module '%s'", module_name);
        result._error.SetErrorStringWithFormat("unable to load module '%s' (%s)", module_name, error.AsCString());
    }
}

static void
VisitNodeNonVariadicTuple (SwiftASTContext *ast,
                          std::vector<swift::Demangle::NodePointer> &nodes,
                          swift::Demangle::NodePointer& cur_node,
                          VisitNodeResult &result,
                          const VisitNodeResult &generic_context, // set by GenericType case
                          Log *log)
{
    if (cur_node->begin() == cur_node->end())
    {
        // No children of this tuple, make an empty tuple

        if (auto ast_context = ast->GetASTContext())
        {
            result._types.push_back(swift::TupleType::getEmpty(*ast_context));
        }
        else
        {
            result._error.SetErrorString("invalid ASTContext");
        }
    }
    else
    {
        std::vector<swift::TupleTypeElt> tuple_fields;
        swift::Demangle::Node::iterator end = cur_node->end();
        for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
        {
            nodes.push_back(*pos);
            VisitNodeResult tuple_element_result;
            VisitNode (ast, nodes, tuple_element_result, generic_context, log);
            if (tuple_element_result._error.Success() && tuple_element_result._tuple_type_element.getType())
            {
                tuple_fields.push_back(tuple_element_result._tuple_type_element);
            }
            else
            {
                result._error = tuple_element_result._error;
            }
        }
        if (result._error.Success())
        {
            if (auto ast_context = ast->GetASTContext())
            {
                result._types.push_back(swift::TupleType::get(tuple_fields, *ast_context));
            }
            else
            {
                result._error.SetErrorString("invalid ASTContext");
            }
        }
    }
}

static void
VisitNodeProtocolList (SwiftASTContext *ast,
                       std::vector<swift::Demangle::NodePointer> &nodes,
                       swift::Demangle::NodePointer& cur_node,
                       VisitNodeResult &result,
                       const VisitNodeResult &generic_context, // set by GenericType case
                       Log *log)
{
    if (cur_node->begin() != cur_node->end())
    {
        VisitNodeResult protocol_types_result;
        nodes.push_back(cur_node->getFirstChild());
        VisitNode (ast, nodes, protocol_types_result, generic_context, log);
        if (protocol_types_result._error.Success() /* cannot check for empty type list as protocol<> is allowed */)
        {
            if (auto ast_context = ast->GetASTContext())
            {
                result._types.push_back(swift::ProtocolCompositionType::get(*ast_context,protocol_types_result._types));
            }
            else
            {
                result._error.SetErrorString("invalid ASTContext");
            }
        }
    }
}

static void
VisitNodeQualifiedArchetype (SwiftASTContext *ast,
                             std::vector<swift::Demangle::NodePointer> &nodes,
                             swift::Demangle::NodePointer& cur_node,
                             VisitNodeResult &result,
                             const VisitNodeResult &generic_context, // set by GenericType case
                             Log *log)
{
    if (cur_node->begin() != cur_node->end())
    {
        swift::Demangle::Node::iterator end = cur_node->end();
        VisitNodeResult type_result;
        uint64_t index = LLDB_INVALID_ADDRESS;
        for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
        {
            switch (pos->get()->getKind())
            {
                case swift::Demangle::Node::Kind::Number:
                    index = pos->get()->getIndex();
                    break;
                case swift::Demangle::Node::Kind::DeclContext:
                    nodes.push_back(*pos);
                    VisitNode (ast, nodes, type_result, generic_context, log);
                    break;
                default:
                    break;
            }
        }
        if (index != LLDB_INVALID_ADDRESS)
        {
            if (type_result._types.size() == 1 && type_result._decls.size() == 1)
            {
                // given a method defined as func ... (args) -> (ret) {...}
                // the Swift type system represents it as
                // (SomeTypeMoniker) -> (args) -> (ret), where SomeTypeMoniker is an appropriately crafted
                // reference to the type that contains the method (e.g. for a struct, an @inout StructType)
                // For a qualified archetype of a method, we do not care about the first-level function, but about
                // the returned function, which is the thing whose archetypes we truly care to extract
                // TODO: this might be a generally useful operation, but it requires a Decl as well as a type
                // to be reliably performed, and as such we cannot just put it in CompilerType as of now
                // (consider, func foo (@inout StructType) -> (Int) -> () vs struct StructType {func foo(Int) -> ()} to see why)
                swift::TypeBase *type_ptr = type_result._types[0].getPointer();
                swift::Decl* decl_ptr = type_result._decls[0];
                // if this is a function...
                if (type_ptr && type_ptr->is<swift::AnyFunctionType>())
                {
                    // if this is defined in a type...
                    if (decl_ptr->getDeclContext()->isTypeContext())
                    {
                        // if I can get the function type from it
                        if (auto func_type = llvm::dyn_cast_or_null<swift::AnyFunctionType>(type_ptr))
                        {
                            // and it has a return type which is itself a function
                            auto return_func_type = llvm::dyn_cast_or_null<swift::AnyFunctionType>(func_type->getResult().getPointer());
                            if (return_func_type)
                                type_ptr = return_func_type; // then use IT as our source of archetypes
                        }
                    }
                }
                CompilerType clang_type(ast->GetASTContext(), type_ptr);
                lldb::TemplateArgumentKind kind;
                swift::TypeBase *arg_type = GetSwiftType(clang_type.GetTemplateArgument(index, kind)).getPointer();
                if (kind == lldb::eTemplateArgumentKindType)
                    result._types.push_back(swift::Type(arg_type));
            }
            else if (type_result._module.IsExtension())
            {
                result._types.push_back(type_result._module.GetQualifiedArchetype(index, ast->GetASTContext()));
            }
        }
    }
}

static void
VisitNodeSelfTypeRef (SwiftASTContext *ast,
                      std::vector<swift::Demangle::NodePointer> &nodes,
                      swift::Demangle::NodePointer& cur_node,
                      VisitNodeResult &result,
                      const VisitNodeResult &generic_context, // set by GenericType case
                      Log *log)
{
    nodes.push_back(cur_node->getFirstChild());
    VisitNodeResult type_result;
    VisitNode (ast, nodes, type_result, generic_context, log);
    if (type_result.HasSingleType())
    {
        swift::Type supposed_protocol_type(type_result.GetFirstType());
        swift::ProtocolType *protocol_type = supposed_protocol_type->getAs<swift::ProtocolType>();
        swift::ProtocolDecl *protocol_decl = protocol_type ? protocol_type->getDecl() : nullptr;
        if (protocol_decl)
        {
            swift::ArchetypeType::AssocTypeOrProtocolType assoc_protocol_type(protocol_decl);
            if (auto ast_context = ast->GetASTContext())
            {
                swift::CanTypeWrapper<swift::ArchetypeType> self_type = swift::ArchetypeType::getNew(*ast_context,
                                                                                                     nullptr,
                                                                                                     assoc_protocol_type,
                                                                                                     ast->GetIdentifier("Self"),
                                                                                                     {supposed_protocol_type},
                                                                                                     swift::Type(),
                                                                                                     false);
                if (self_type.getPointer())
                    result._types.push_back(swift::Type(self_type));
                else
                    result._error.SetErrorString("referent type cannot be made into an archetype");
            }
            else
            {
                result._error.SetErrorString("invalid ASTContext");
            }
        }
        else
        {
            result._error.SetErrorString("referent type does not resolve to a protocol");
        }
    }
    else
    {
        result._error.SetErrorString("couldn't resolve referent type");
    }
}

static void
VisitNodeTupleElement (SwiftASTContext *ast,
                          std::vector<swift::Demangle::NodePointer> &nodes,
                          swift::Demangle::NodePointer& cur_node,
                          VisitNodeResult &result,
                          const VisitNodeResult &generic_context, // set by GenericType case
                          Log *log)
{
    const char *tuple_name = NULL;
    VisitNodeResult tuple_type_result;
    swift::Demangle::Node::iterator end = cur_node->end();
    for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
    {
        const swift::Demangle::Node::Kind child_node_kind = (*pos)->getKind();
        switch (child_node_kind)
        {
            case swift::Demangle::Node::Kind::TupleElementName:
                tuple_name = (*pos)->getText().c_str();
                break;
            case swift::Demangle::Node::Kind::Type:
                nodes.push_back((*pos)->getFirstChild());
                VisitNode (ast, nodes, tuple_type_result, generic_context, log);
                break;
            default:
                if (log)
                    log->Printf ("error: unsupported child node under a swift::Demangle::Node::Kind::TupleElement: %s", SwiftDemangleNodeKindToCString(child_node_kind));
                break;
        }
    }

    if (tuple_type_result._error.Success() && tuple_type_result._types.size() == 1)
    {
        if (tuple_name)
            result._tuple_type_element = swift::TupleTypeElt(tuple_type_result._types.front().getPointer(), ast->GetIdentifier(tuple_name));
        else
            result._tuple_type_element = swift::TupleTypeElt(tuple_type_result._types.front().getPointer());
    }
}

static void
VisitNodeTypeList (SwiftASTContext *ast,
                   std::vector<swift::Demangle::NodePointer> &nodes,
                   swift::Demangle::NodePointer& cur_node,
                   VisitNodeResult &result,
                   const VisitNodeResult &generic_context, // set by GenericType case
                   Log *log)
{
    if (cur_node->begin() != cur_node->end())
    {
        swift::Demangle::Node::iterator end = cur_node->end();
        for (swift::Demangle::Node::iterator pos = cur_node->begin(); pos != end; ++pos)
        {
            nodes.push_back(*pos);
            VisitNodeResult type_result;
            VisitNode (ast, nodes, type_result, generic_context, log);
            if (type_result._error.Success() && type_result._types.size() == 1)
            {
                if (type_result._decls.empty())
                    result._decls.push_back(NULL);
                else
                    result._decls.push_back(type_result._decls.front());
                result._types.push_back(type_result._types.front());
            }
            else
            {
                result._error = type_result._error;
            }
        }
    }
}

static void
VisitNodeUnowned (SwiftASTContext *ast,
                  std::vector<swift::Demangle::NodePointer> &nodes,
                  swift::Demangle::NodePointer& cur_node,
                  VisitNodeResult &result,
                  const VisitNodeResult &generic_context, // set by GenericType case
                  Log *log)
{
    nodes.push_back(cur_node->getFirstChild());
    VisitNodeResult type_result;
    VisitNode (ast, nodes, type_result, generic_context, log);
    if (type_result._types.size() == 1 && type_result._types[0])
    {
        if (auto ast_context = ast->GetASTContext())
        {
            result._types.push_back(swift::Type(swift::UnownedStorageType::get(type_result._types[0],
                                                                               *ast_context)));
        }
        else
        {
            result._error.SetErrorString("invalid ASTContext");
        }
    }
    else
    {
        result._error.SetErrorString("couldn't resolve referent type");
    }
}

static void
VisitNodeWeak (SwiftASTContext *ast,
               std::vector<swift::Demangle::NodePointer> &nodes,
               swift::Demangle::NodePointer& cur_node,
               VisitNodeResult &result,
               const VisitNodeResult &generic_context, // set by GenericType case
               Log *log)
{
    nodes.push_back(cur_node->getFirstChild());
    VisitNodeResult type_result;
    VisitNode (ast, nodes, type_result, generic_context, log);
    if (type_result._types.size() == 1 && type_result._types[0])
    {
        if (auto ast_context = ast->GetASTContext())
        {
            result._types.push_back(swift::Type(swift::WeakStorageType::get(type_result._types[0],
                                                                            *ast_context)));
        }
        else
        {
            result._error.SetErrorString("invalid ASTContext");
        }
    }
    else
    {
        result._error.SetErrorString("couldn't resolve referent type");
    }
}

static void
VisitFirstChildNode (SwiftASTContext *ast,
                     std::vector<swift::Demangle::NodePointer> &nodes,
                     swift::Demangle::NodePointer& cur_node,
                     VisitNodeResult &result,
                     const VisitNodeResult &generic_context, // set by GenericType case
                     Log *log)
{
    if (cur_node->begin() != cur_node->end())
    {
        nodes.push_back(cur_node->getFirstChild());
        VisitNode (ast, nodes, result, generic_context, log);
    }
}

static void
VisitAllChildNodes (SwiftASTContext *ast,
                          std::vector<swift::Demangle::NodePointer> &nodes,
                          swift::Demangle::NodePointer& cur_node,
                          VisitNodeResult &result,
                          const VisitNodeResult &generic_context, // set by GenericType case
                          Log *log)
{
    swift::Demangle::Node::iterator child_end = cur_node->end();
    for (swift::Demangle::Node::iterator child_pos = cur_node->begin(); child_pos != child_end; ++child_pos)
    {
        nodes.push_back(*child_pos);
        VisitNode (ast, nodes, result, generic_context, log);
    }
}

static void
VisitNode (SwiftASTContext *ast,
           std::vector<swift::Demangle::NodePointer> &nodes,
           VisitNodeResult &result,
           const VisitNodeResult &generic_context, // set by GenericType case
           Log *log)
{
    if (nodes.empty())
        result._error.SetErrorString("no node");
    else if (nodes.back() == nullptr)
        result._error.SetErrorString("last node is NULL");
    else
        result._error.Clear();

    if (result._error.Success())
    {
        swift::Demangle::NodePointer node = nodes.back();
        const swift::Demangle::Node::Kind node_kind = node->getKind();
        if (log)
        {
            if (!node->hasText() || node->getText().empty())
                log->Printf("%p: %*s%s", node.get(), (int)nodes.size()*2, "", SwiftDemangleNodeKindToCString(node_kind));
            else
                log->Printf("%p: %*s%s (\"%s\")", node.get(), (int)nodes.size()*2, "", SwiftDemangleNodeKindToCString(node_kind), node->getText().c_str());
        }

        switch (node_kind)
        {
            case swift::Demangle::Node::Kind::OwningAddressor:
            case swift::Demangle::Node::Kind::OwningMutableAddressor:
            case swift::Demangle::Node::Kind::UnsafeAddressor:
            case swift::Demangle::Node::Kind::UnsafeMutableAddressor:
                VisitNodeAddressor (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Generics:
                VisitNodeGenerics (ast, nodes, node, result, generic_context, log);
                break;

            case swift::Demangle::Node::Kind::ArchetypeRef:
                VisitNodeArchetypeRef (ast, nodes, node, result, generic_context, log);
                break;

            case swift::Demangle::Node::Kind::Archetype:
                VisitNodeArchetype (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::ArgumentTuple:
                VisitFirstChildNode (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::AssociatedTypeRef:
                VisitNodeAssociatedTypeRef (ast, nodes, node, result, generic_context, log);
                break;
            
            case swift::Demangle::Node::Kind::BoundGenericClass:
            case swift::Demangle::Node::Kind::BoundGenericStructure:
            case swift::Demangle::Node::Kind::BoundGenericEnum:
                VisitNodeBoundGeneric (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::BuiltinTypeName:
                VisitNodeBuiltinTypeName (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Structure:
            case swift::Demangle::Node::Kind::Class:
            case swift::Demangle::Node::Kind::Enum:
            case swift::Demangle::Node::Kind::Global:
            case swift::Demangle::Node::Kind::Static:
            case swift::Demangle::Node::Kind::TypeAlias:
            case swift::Demangle::Node::Kind::Type:
            case swift::Demangle::Node::Kind::TypeMangling:
            case swift::Demangle::Node::Kind::ReturnType:
            case swift::Demangle::Node::Kind::Protocol:
                VisitAllChildNodes (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Constructor:
                VisitNodeConstructor (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Destructor:
                VisitNodeDestructor (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::DeclContext:
                VisitNodeDeclContext (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::ErrorType:
                result._error.SetErrorString("error type encountered while demangling name");
                break;
                
            case swift::Demangle::Node::Kind::Extension:
                VisitNodeExtension(ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::ExplicitClosure:
                VisitNodeExplicitClosure (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Function:
            case swift::Demangle::Node::Kind::Allocator:
            case swift::Demangle::Node::Kind::Variable:    // Out of order on purpose
                VisitNodeFunction (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::FunctionType:
            case swift::Demangle::Node::Kind::UncurriedFunctionType:      // Out of order on purpose.
                VisitNodeFunctionType (ast, nodes, node, result, generic_context, log);
                break;
            
            case swift::Demangle::Node::Kind::GenericType:
                VisitNodeGenericType (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::DidSet:
            case swift::Demangle::Node::Kind::Getter:
            case swift::Demangle::Node::Kind::Setter:
            case swift::Demangle::Node::Kind::WillSet: // out of order on purpose
                VisitNodeSetterGetter (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::LocalDeclName:
                VisitNodeLocalDeclName(ast, nodes, node, result, generic_context, log);
                break;

            case swift::Demangle::Node::Kind::Identifier:
                VisitNodeIdentifier (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::InOut:
                VisitNodeInOut (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Metatype:
                VisitNodeMetatype (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Module:
                VisitNodeModule (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::NonVariadicTuple:
                VisitNodeNonVariadicTuple (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::PrivateDeclName:
                VisitNodePrivateDeclName(ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::ProtocolList:
                VisitNodeProtocolList (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::QualifiedArchetype:
                VisitNodeQualifiedArchetype (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::SelfTypeRef:
                VisitNodeSelfTypeRef (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::TupleElement:
                VisitNodeTupleElement (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::TypeList:
                VisitNodeTypeList (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Unowned:
                VisitNodeUnowned (ast, nodes, node, result, generic_context, log);
                break;
                
            case swift::Demangle::Node::Kind::Weak:
                VisitNodeWeak (ast, nodes, node, result, generic_context, log);
                break;
            default:
                if (log)
                    log->Printf ("error: node not supported -- %s = '%s'",
                                 SwiftDemangleNodeKindToCString(node_kind),
                                 node->hasText() ? node->getText().c_str() : "<un-named node>");
                break;
        }
    }
    nodes.pop_back();
}

ConstString
SwiftASTContext::GetMangledTypeName (swift::TypeBase * type_base)
{
    VALID_OR_RETURN(ConstString());

    auto iter = m_type_to_mangled_name_map.find(type_base),
    end = m_type_to_mangled_name_map.end();
    if (iter != end)
        return ConstString(iter->second);
    
    swift::Type swift_type(type_base);
    
    bool has_archetypes = false;
    
    swift_type.visit([&has_archetypes](swift::Type part_type) -> void {
        if (part_type->getKind() == swift::TypeKind::Archetype)
        {
            has_archetypes = true;
        }
    });
    
    if (!has_archetypes)
    {
        std::string s;
        llvm::raw_string_ostream ss(s);
        swift::Mangle::Mangler mangler(ss, true);
        mangler.mangleTypeForDebugger(swift_type, nullptr);
        ss.flush();
        
        if (!s.empty())
        {
            ConstString mangled_cs(s.c_str());
            CacheDemangledType(mangled_cs.AsCString(), type_base);
            return mangled_cs;
        }
    }
    
    return ConstString();
}

void
SwiftASTContext::CacheDemangledType (const char* name, swift::TypeBase* found_type)
{
    VALID_OR_RETURN_VOID();
    
    m_type_to_mangled_name_map.insert(std::make_pair(found_type, name));
    m_mangled_name_to_type_map.insert(std::make_pair(name, found_type));
}

void
SwiftASTContext::CacheDemangledTypeFailure (const char* name)
{
    VALID_OR_RETURN_VOID();

    m_negative_type_cache.Insert(name);
}

CompilerType
SwiftASTContext::GetTypeFromMangledTypename (const char *mangled_typename, Error &error)
{
    VALID_OR_RETURN(CompilerType());

    if (mangled_typename && mangled_typename[0] == '_' && mangled_typename[1] == 'T')
    {
        Log *log(GetLogIfAllCategoriesSet(LIBLLDB_LOG_TYPES));
        if (log)
            log->Printf ("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s')", this, mangled_typename);
        
        // if we were to crash doing this, remember what type caused it
        Host::SetCrashDescriptionWithFormat ("error finding type for %s", mangled_typename);
        // Make a scoped cleanup object that will clear the crash description string
        // on exit of this function.
        lldb_utility::CleanUp <const char *> crash_description_cleanup(NULL, Host::SetCrashDescription);
        swift::ASTContext * ast_ctx = GetASTContext();
        ConstString mangled_name (mangled_typename);
        swift::TypeBase *found_type = m_mangled_name_to_type_map.lookup (mangled_name.GetCString());
        if (found_type)
        {
            if (log)
                log->Printf ("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') -- found in the positive cache", this, mangled_typename);
            return CompilerType (ast_ctx, found_type);
        }
        
        if (m_negative_type_cache.Lookup(mangled_name.GetCString()))
        {
            if (log)
                log->Printf ("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') -- found in the negative cache", this, mangled_typename);
            return CompilerType();
        }
        
        if (log)
            log->Printf ("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') -- not cached, searching", this, mangled_typename);
        
        std::vector<swift::Demangle::NodePointer> nodes;
        nodes.push_back(swift::Demangle::demangleSymbolAsNode(mangled_typename, mangled_name.GetLength()));
        VisitNodeResult empty_generic_context;
        VisitNodeResult result;
       
        VisitNode (this, nodes, result, empty_generic_context, log);
        error = result._error;
        if (error.Success() && result._types.size() == 1)
        {
            swift::TypeBase *found_type = result._types.front().getPointer();
            CacheDemangledType(mangled_name.GetCString(), found_type);
            CompilerType result_type(ast_ctx, found_type);
            if (log)
                log->Printf ("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') -- found %s", this, mangled_typename, result_type.GetTypeName().GetCString());
            return result_type;
        }
        else
        {
            if (log)
                log->Printf ("((SwiftASTContext*)%p)->GetTypeFromMangledTypename('%s') -- error: %s", this, mangled_typename, result._error.AsCString());
            
            error.SetErrorStringWithFormat("type for typename '%s' was not found",mangled_typename);
            CacheDemangledTypeFailure(mangled_name.GetCString());
            return CompilerType();
        }
    }
    error.SetErrorStringWithFormat("typename '%s' is not a valid Swift mangled typename, it should begin with _T",mangled_typename);
    return CompilerType();
}


CompilerType
SwiftASTContext::GetVoidFunctionType()
{
    VALID_OR_RETURN(CompilerType());

    if (!m_void_function_type)
    {
        swift::ASTContext *ast = GetASTContext();
        swift::Type empty_tuple_type(swift::TupleType::getEmpty(*ast));
        m_void_function_type = CompilerType(ast, swift::FunctionType::get(empty_tuple_type, empty_tuple_type));
    }
    return m_void_function_type;
}

CompilerType
SwiftASTContext::FindQualifiedType (const char *qualified_name)
{
    VALID_OR_RETURN(CompilerType());

    if (qualified_name && qualified_name[0])
    {
        const char *dot_pos = strchr(qualified_name, '.');
        if (dot_pos)
        {
            ConstString module_name(qualified_name, dot_pos - qualified_name);
            swift::ModuleDecl *swift_module = GetCachedModule (module_name);
            if (swift_module)
            {
                swift::Module::AccessPathTy access_path;
                llvm::SmallVector<swift::ValueDecl*, 4> decls;
                const char *module_type_name = dot_pos + 1;
                swift_module->lookupValue(access_path, GetIdentifier(module_type_name), swift::NLKind::UnqualifiedLookup, decls);
                if (!decls.empty())
                {
                    for (auto decl : decls)
                    {
                        switch (decl->getKind())
                        {
                            case swift::DeclKind::Import:
                            case swift::DeclKind::Extension:
                            case swift::DeclKind::PatternBinding:
                            case swift::DeclKind::TopLevelCode:
                            case swift::DeclKind::InfixOperator:
                            case swift::DeclKind::PrefixOperator:
                            case swift::DeclKind::PostfixOperator:
                            case swift::DeclKind::GenericTypeParam:
                            case swift::DeclKind::AssociatedType:
                            case swift::DeclKind::EnumElement:
                            case swift::DeclKind::EnumCase:
                            case swift::DeclKind::IfConfig:
                            case swift::DeclKind::Param:
                            case swift::DeclKind::Module:
                                break;
                                
                            case swift::DeclKind::TypeAlias:
                            case swift::DeclKind::Enum:
                            case swift::DeclKind::Struct:
                            case swift::DeclKind::Class:
                                if (decl->hasType())
                                {
                                    swift::Type swift_type = decl->getType();
                                    swift::MetatypeType *meta_type = swift_type->getAs<swift::MetatypeType>();
                                    swift::ASTContext *ast = GetASTContext ();
                                    if (meta_type)
                                        return CompilerType (ast, meta_type->getInstanceType().getPointer());
                                    else
                                        return CompilerType (ast, swift_type.getPointer());
                                }
                                break;
                            case swift::DeclKind::Protocol:
                            case swift::DeclKind::Var:
                            case swift::DeclKind::Func:
                            case swift::DeclKind::Subscript:
                            case swift::DeclKind::Constructor:
                            case swift::DeclKind::Destructor:
                                break;
                                
                        }
                    }
                }
            }
        }
    }
    return CompilerType();
}

// FIXME: other guys try to do similar stuff - can we grand-unify all of them?
static CompilerType
ValueDeclToType (swift::ValueDecl *decl,
                 swift::ASTContext *ast,
                 bool metatype_is_instance = true)
{
    if (decl)
    {
        switch (decl->getKind())
        {
            case swift::DeclKind::TypeAlias:
            case swift::DeclKind::Enum:
            case swift::DeclKind::Struct:
            case swift::DeclKind::Class:
            case swift::DeclKind::Protocol:
            case swift::DeclKind::Func:
                if (decl->hasType())
                {
                    swift::Type swift_type = decl->getType();
                    swift::MetatypeType *meta_type = swift_type->getAs<swift::MetatypeType>();
                    if (meta_type && metatype_is_instance)
                        return CompilerType (ast, meta_type->getInstanceType().getPointer());
                    else
                        return CompilerType (ast, swift_type.getPointer());
                }
                break;
                
            default:
                break;
        }
    }
    return CompilerType();
}

static SwiftASTContext::TypeOrDecl
DeclToTypeOrDecl (swift::ASTContext* ast,
                  swift::ValueDecl* decl)
{
    if (decl)
    {
        switch (decl->getKind())
        {
            case swift::DeclKind::Import:
            case swift::DeclKind::Extension:
            case swift::DeclKind::PatternBinding:
            case swift::DeclKind::TopLevelCode:
            case swift::DeclKind::InfixOperator:
            case swift::DeclKind::PrefixOperator:
            case swift::DeclKind::PostfixOperator:
            case swift::DeclKind::GenericTypeParam:
            case swift::DeclKind::AssociatedType:
            case swift::DeclKind::EnumElement:
            case swift::DeclKind::EnumCase:
            case swift::DeclKind::IfConfig:
            case swift::DeclKind::Param:
            case swift::DeclKind::Module:
                break;
                
            case swift::DeclKind::TypeAlias:
            case swift::DeclKind::Enum:
            case swift::DeclKind::Struct:
            case swift::DeclKind::Class:
            case swift::DeclKind::Protocol:
                if (decl->hasType())
                {
                    swift::Type swift_type = decl->getType();
                    swift::MetatypeType *meta_type = swift_type->getAs<swift::MetatypeType>();
                    if (meta_type)
                        return CompilerType (ast, meta_type->getInstanceType().getPointer());
                    else
                        return CompilerType (ast, swift_type.getPointer());
                }
                break;
                
            case swift::DeclKind::Func:
            case swift::DeclKind::Var:
                return decl;
                break;
                
            case swift::DeclKind::Subscript:
            case swift::DeclKind::Constructor:
            case swift::DeclKind::Destructor:
                break;
        }
    }
    return CompilerType();
}

size_t
SwiftASTContext::FindContainedType (llvm::StringRef name,
                                    CompilerType container_type,
                                    std::set<CompilerType> &results,
                                    bool append)
{
    VALID_OR_RETURN(0);

    if (!append)
        results.clear();
    
    size_t size_before = results.size();
    
    TypesOrDecls types_or_decl_results;
    FindContainedTypeOrDecl(name, container_type, types_or_decl_results);
    
    for (const auto& result : types_or_decl_results)
    {
        CompilerType type = result.Apply<CompilerType> ([] (CompilerType type) -> CompilerType {
                                              return type;
                                          },
                                          [this] (swift::ValueDecl* decl) -> CompilerType {
                                              return ValueDeclToType(decl, GetASTContext());
                                          });
        results.emplace(type);
    }

    return results.size()-size_before;
}

size_t
SwiftASTContext::FindContainedTypeOrDecl (llvm::StringRef name,
                                          TypeOrDecl container_type_or_decl,
                                          TypesOrDecls &results,
                                          bool append)
{
    VALID_OR_RETURN(0);
    
    if (!append)
        results.clear();
    size_t size_before = results.size();

    CompilerType container_type = container_type_or_decl.Apply<CompilerType> ([] (CompilerType type) -> CompilerType {
        return type;
    },
                                                    [this] (swift::ValueDecl* decl) -> CompilerType {
                                                        return ValueDeclToType(decl, GetASTContext());
                                                    });
    
    if (false == name.empty() &&
        llvm::dyn_cast_or_null<SwiftASTContext>(container_type.GetTypeSystem()))
    {
        swift::Type swift_type(GetSwiftType(container_type));
        if (!swift_type)
            return 0;
        swift::CanType swift_can_type(swift_type->getCanonicalType());
        if (!swift_can_type)
            return 0;
        swift::NominalType *nominal_type = swift_can_type->getAs<swift::NominalType>();
        if (!nominal_type)
            return 0;
        swift::NominalTypeDecl *nominal_decl = nominal_type->getDecl();
        if (!nominal_decl)
            return 0;
        llvm::ArrayRef<swift::ValueDecl *> decls = nominal_type->getDecl()->lookupDirect(swift::DeclName(m_ast_context_ap->getIdentifier(name)));
        for (auto decl : decls)
            results.emplace(DeclToTypeOrDecl(GetASTContext(), decl));
    }
    return results.size()-size_before;
}

CompilerType
SwiftASTContext::FindType (const char *name, swift::ModuleDecl *swift_module)
{
    VALID_OR_RETURN(CompilerType());

    std::set<CompilerType> search_results;

    FindTypes (name, swift_module, search_results, false);

    if (search_results.empty())
        return CompilerType();
    else
        return *search_results.begin();
}

llvm::Optional<SwiftASTContext::TypeOrDecl>
SwiftASTContext::FindTypeOrDecl (const char *name,
                                 swift::ModuleDecl *swift_module)
{
    VALID_OR_RETURN(llvm::Optional<SwiftASTContext::TypeOrDecl>());
    
    TypesOrDecls search_results;
    
    FindTypesOrDecls (name, swift_module, search_results, false);
    
    if (search_results.empty())
        return llvm::Optional<SwiftASTContext::TypeOrDecl>();
    else
        return *search_results.begin();
}

size_t
SwiftASTContext::FindTypes (const char *name,
                            swift::ModuleDecl *swift_module,
                            std::set<CompilerType> &results,
                            bool append)
{
    VALID_OR_RETURN(0);

    if (!append)
        results.clear();
    
    size_t before = results.size();

    TypesOrDecls types_or_decls_results;
    FindTypesOrDecls(name, swift_module, types_or_decls_results);
    
    for (const auto& result : types_or_decls_results)
    {
        CompilerType type = result.Apply<CompilerType> ([] (CompilerType type) -> CompilerType {
                                             return type;
                                         },
                                         [this] (swift::ValueDecl* decl) -> CompilerType {
                                             if (decl->hasType())
                                             {
                                                 swift::Type swift_type = decl->getType();
                                                 swift::MetatypeType *meta_type = swift_type->getAs<swift::MetatypeType>();
                                                 swift::ASTContext *ast = GetASTContext ();
                                                 if (meta_type)
                                                     return CompilerType (ast, meta_type->getInstanceType().getPointer());
                                                 else
                                                     return CompilerType (ast, swift_type.getPointer());
                                             }
                                             return CompilerType();
                                         });
        results.emplace(type);
    }
    
    return results.size() - before;
}

size_t
SwiftASTContext::FindTypesOrDecls (const char *name,
                                   swift::ModuleDecl *swift_module,
                                   TypesOrDecls &results,
                                   bool append)
{
    VALID_OR_RETURN(0);
    
    if (!append)
        results.clear();
    
    size_t before = results.size();
    
    if (name && name[0] && swift_module)
    {
        swift::Module::AccessPathTy access_path;
        llvm::SmallVector<swift::ValueDecl*, 4> decls;
        if (strchr(name, '.'))
            swift_module->lookupValue(access_path, GetIdentifier(name), swift::NLKind::QualifiedLookup, decls);
        else
            swift_module->lookupValue(access_path, GetIdentifier(name), swift::NLKind::UnqualifiedLookup, decls);
        if (!decls.empty())
        {
            for (auto decl : decls)
                results.emplace(DeclToTypeOrDecl(GetASTContext(), decl));
        }
    }
    
    return results.size() - before;
}

size_t
SwiftASTContext::FindType (const char *name,
                           std::set<CompilerType> &results,
                           bool append)
{
    VALID_OR_RETURN(0);

    if (!append)
        results.clear();
    auto iter = m_swift_module_cache.begin(),
    end = m_swift_module_cache.end();
    
    size_t count = 0;
    
    std::function<void(swift::ModuleDecl*)> lookup_func = [this, name, &results, &count] (swift::ModuleDecl *module) -> void {
        CompilerType candidate(this->FindType(name, module));
        if (candidate)
        {
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

CompilerType
SwiftASTContext::FindFirstType (const char *name, const ConstString &module_name)
{
    VALID_OR_RETURN(CompilerType());

    if (name && name[0])
    {
        if (module_name)
        {
            return FindType (name, GetCachedModule (module_name));
        }
        else
        {
            std::set<CompilerType> types;
            FindType(name, types);
            if (!types.empty())
                return *types.begin();
        }
    }
    return CompilerType();
}

CompilerType
SwiftASTContext::ImportType (CompilerType &type, Error &error)
{
    VALID_OR_RETURN(CompilerType());

    if (m_ast_context_ap.get() == NULL)
        return CompilerType();

    SwiftASTContext *swift_ast_ctx = llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem());
    
    if (swift_ast_ctx == nullptr)
    {
        error.SetErrorString("Can't import clang type into a Swift ASTContext.");
        return CompilerType();
    }
    else if (swift_ast_ctx == this)
    {
        // This is the same AST context, so the type is already imported...
        return type;
    }
    
    // For now we're going to do this all using mangled names.  If we find that is too slow, we can use the
    // TypeBase * in the CompilerType to match this to the version of the type we got from the mangled name
    // in the original swift::ASTContext.
    
    ConstString mangled_name(type.GetMangledTypeName());
    if (mangled_name)
    {
        swift::TypeBase *our_type_base = m_mangled_name_to_type_map.lookup(mangled_name.GetCString());
        if (our_type_base)
            return CompilerType (m_ast_context_ap.get(), our_type_base);
        else
        {
            Error error;
            
            CompilerType our_type(GetTypeFromMangledTypename(mangled_name.GetCString(), error));
            if (error.Success())
                return our_type;
        }
    }
    return CompilerType();
}

swift::IRGenDebugInfoKind
SwiftASTContext::GetGenerateDebugInfo ()
{
    return GetIRGenOptions().DebugInfoKind;
}

swift::PrintOptions
SwiftASTContext::GetUserVisibleTypePrintingOptions (bool print_help_if_available)
{
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
    print_options.PrintRegularClangComments =
    print_help_if_available;
    return print_options;
}

void
SwiftASTContext::SetGenerateDebugInfo (swift::IRGenDebugInfoKind b)
{
    GetIRGenOptions().DebugInfoKind = b;
}

llvm::TargetOptions *
SwiftASTContext::getTargetOptions()
{
    if (m_target_options_ap.get() == NULL)
    {
        m_target_options_ap.reset (new llvm::TargetOptions());
    }
    return m_target_options_ap.get();
}

swift::ModuleDecl *
SwiftASTContext::GetScratchModule ()
{
    VALID_OR_RETURN(nullptr);

    if (m_scratch_module == nullptr)
        m_scratch_module = swift::Module::create(GetASTContext()->getIdentifier("__lldb_scratch_module"), *GetASTContext());
    return m_scratch_module;
}

swift::SILModule *
SwiftASTContext::GetSILModule ()
{
    VALID_OR_RETURN(nullptr);

    if (m_sil_module_ap.get() == NULL)
        m_sil_module_ap = swift::SILModule::createEmptyModule(GetScratchModule(),
                                                              GetSILOptions());
    return m_sil_module_ap.get();
}

swift::irgen::IRGenModuleDispatcher &
SwiftASTContext::GetIRGenModuleDispatcher ()
{
    if (m_ir_gen_module_dispatcher_ap.get() == nullptr)
    {
        m_ir_gen_module_dispatcher_ap.reset(new swift::irgen::IRGenModuleDispatcher());
    }
    
    return *m_ir_gen_module_dispatcher_ap.get();
}

swift::irgen::IRGenModule &
SwiftASTContext::GetIRGenModule ()
{
    VALID_OR_RETURN(*m_ir_gen_module_ap);
    
    if (m_ir_gen_module_ap.get() == NULL)
    {
        // Make sure we have a good ClangImporter.
        GetClangImporter();
        
        swift::IRGenOptions &ir_gen_opts = GetIRGenOptions();

        std::string error_str;
        std::string triple = GetTriple ();
        const llvm::Target *llvm_target = llvm::TargetRegistry::lookupTarget(triple, error_str);
            
        llvm::CodeGenOpt::Level optimization_level = llvm::CodeGenOpt::Level::None;
        
        // Create a target machine.
        llvm::TargetMachine *target_machine = llvm_target->createTargetMachine (triple,
                                                                                "generic", // cpu
                                                                                "",            // features
                                                                                *getTargetOptions(),
                                                                                llvm::Reloc::Default,
                                                                                llvm::CodeModel::Default,
                                                                                optimization_level);
        if (target_machine)
        {
            // Set the module's string representation.
            const llvm::DataLayout data_layout = target_machine->createDataLayout();

            llvm::Triple llvm_triple(triple);
            
            m_ir_gen_module_ap.reset (new swift::irgen::IRGenModule(GetIRGenModuleDispatcher(),
                                                                    nullptr,
                                                                    *GetASTContext(),
                                                                    llvm::getGlobalContext(),
                                                                    ir_gen_opts,
                                                                    ir_gen_opts.ModuleName,
                                                                    data_layout,
                                                                    llvm_triple,
                                                                    target_machine,
                                                                    GetSILModule (),
                                                                    ir_gen_opts.getSingleOutputFilename()));
            llvm::Module *llvm_module = m_ir_gen_module_ap->getModule();
            llvm_module->setDataLayout(data_layout.getStringRepresentation());
            llvm_module->setTargetTriple(triple);

        }
    }
    return *m_ir_gen_module_ap;
}

CompilerType
SwiftASTContext::CreateTupleType (const std::vector<CompilerType>& elements)
{
    VALID_OR_RETURN(CompilerType());
    
    Error error;
    if (elements.size() == 0)
        return GetTypeFromMangledTypename("_TtT_", error);
    else
    {
        std::vector<swift::TupleTypeElt> tuple_elems;
        for (const CompilerType& type : elements)
        {
            if (auto swift_type = GetSwiftType(type))
                tuple_elems.push_back(swift::TupleTypeElt(swift_type));
            else
                return CompilerType();
        }
        llvm::ArrayRef<swift::TupleTypeElt> fields(tuple_elems);
        return CompilerType(GetASTContext(),swift::TupleType::get(fields, *GetASTContext()).getPointer());
    }
}

CompilerType
SwiftASTContext::CreateTupleType (const std::vector<TupleElement>& elements)
{
    VALID_OR_RETURN(CompilerType());

    Error error;
    if (elements.size() == 0)
        return GetTypeFromMangledTypename("_TtT_", error);
    else
    {
        std::vector<swift::TupleTypeElt> tuple_elems;
        for (const TupleElement& element : elements)
        {
            if (auto swift_type = GetSwiftType(element.element_type))
            {
                if (element.element_name.IsEmpty())
                    tuple_elems.push_back(swift::TupleTypeElt(swift_type));
                else
                    tuple_elems.push_back(swift::TupleTypeElt(swift_type,m_ast_context_ap->getIdentifier(element.element_name.GetCString())));
            }
            else
                return CompilerType();
        }
        llvm::ArrayRef<swift::TupleTypeElt> fields(tuple_elems);
        return CompilerType(GetASTContext(),swift::TupleType::get(fields, *GetASTContext()).getPointer());
    }
}

CompilerType
SwiftASTContext::CreateFunctionType (CompilerType arg_type, CompilerType ret_type, bool throws)
{
    VALID_OR_RETURN(CompilerType());

    if (!llvm::dyn_cast_or_null<SwiftASTContext>(arg_type.GetTypeSystem()) ||
        !llvm::dyn_cast_or_null<SwiftASTContext>(ret_type.GetTypeSystem()))
        return CompilerType();
    swift::FunctionType::ExtInfo ext_info;
    if (throws)
        ext_info = ext_info.withThrows();
    return CompilerType(GetASTContext(),
                        swift::FunctionType::get(
                                                 GetSwiftType(arg_type),
                                                 GetSwiftType(ret_type),
                                                 ext_info));
}


CompilerType
SwiftASTContext::GetErrorProtocolType ()
{
    VALID_OR_RETURN(CompilerType());

    swift::ASTContext *swift_ctx = GetASTContext();
    if (swift_ctx)
    {
        // Getting the error type requires the Stdlib module be loaded, but doesn't cause it to be loaded.
        // Do that here:
        swift_ctx->getStdlibModule(true);
        swift::NominalTypeDecl *error_type_decl = GetASTContext()->getExceptionTypeDecl();
        if (error_type_decl)
        {
            auto error_type = error_type_decl->getType().getPointer();
            if (swift::MetatypeType *error_metatype = error_type->getAs<swift::MetatypeType>())
                error_type = error_metatype->getInstanceType().getPointer();
            return CompilerType(GetASTContext(), error_type);
        }
    }
    return CompilerType();
}

CompilerType
SwiftASTContext::GetNSErrorType (Error& error)
{
    VALID_OR_RETURN(CompilerType());

    return GetTypeFromMangledTypename("_TtC10Foundation7NSError", error);
}

CompilerType
SwiftASTContext::CreateProtocolCompositionType (const std::vector<CompilerType>& protocols)
{
    VALID_OR_RETURN(CompilerType());

    std::vector<swift::Type> protocol_types;
    for (const auto& protocol: protocols)
    {
        if (auto swift_type = GetSwiftType(protocol))
            protocol_types.push_back(swift::Type(swift_type));
        else
            return CompilerType();
    }
    return CompilerType(GetASTContext(),
                        swift::ProtocolCompositionType::get(*GetASTContext(),
                                                            llvm::ArrayRef<swift::Type>(protocol_types)).getPointer());
}

CompilerType
SwiftASTContext::CreateMetatypeType (CompilerType instance_type)
{
    VALID_OR_RETURN(CompilerType());

    if (llvm::dyn_cast_or_null<SwiftASTContext>(instance_type.GetTypeSystem()))
        return CompilerType(GetASTContext(),
                            swift::MetatypeType::get(GetSwiftType(instance_type), *GetASTContext()));
    return CompilerType();
}

CompilerType
SwiftASTContext::BindGenericType (CompilerType type,
                                  std::vector<CompilerType> generic_args,
                                  bool rebind_if_necessary)
{
    if (type.IsValid() && llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::UnboundGeneric:
            {
                swift::NominalTypeDecl *nominal_type_decl = swift_can_type->getAs<swift::UnboundGenericType>()->getDecl();
                swift::DeclContext * parent_decl = nominal_type_decl->getParent();
                swift::Type parent_type;
                if (parent_decl->isTypeContext())
                    parent_type = parent_decl->getDeclaredTypeOfContext();
                std::vector<swift::Type> generic_args_type;
                for (CompilerType generic_arg : generic_args)
                {
                    if (!llvm::dyn_cast_or_null<SwiftASTContext>(generic_arg.GetTypeSystem()))
                        return CompilerType();
                    generic_args_type.push_back(GetSwiftType(generic_arg));
                }
                return CompilerType(GetASTContext(),swift::BoundGenericType::get(nominal_type_decl,
                                                                                 parent_type,
                                                                                 generic_args_type));
            }
                break;
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericEnum:
            case swift::TypeKind::BoundGenericStruct:
                if (rebind_if_necessary)
                    return BindGenericType(type.GetUnboundType(),generic_args, false);
            default:
                break;
        }
    }
    return CompilerType();
}

SwiftASTContext *
SwiftASTContext::GetSwiftASTContext (swift::ASTContext *ast)
{
    SwiftASTContext *swift_ast = GetASTMap().Lookup(ast);
    return swift_ast;
}

uint32_t
SwiftASTContext::GetPointerByteSize()
{
    VALID_OR_RETURN(0);

    if (m_pointer_byte_size == 0)
    {
        swift::ASTContext *ast = GetASTContext();
        m_pointer_byte_size = CompilerType(ast, ast->TheRawPointerType.getPointer()).GetByteSize(nullptr);
    }
    return m_pointer_byte_size;
}

uint32_t
SwiftASTContext::GetPointerBitAlignment()
{
    VALID_OR_RETURN(0);

    if (m_pointer_bit_align == 0)
    {
        swift::ASTContext *ast = GetASTContext();
        m_pointer_bit_align = CompilerType(ast, ast->TheRawPointerType.getPointer()).GetAlignedBitSize();
    }
    return m_pointer_bit_align;
}

bool
SwiftASTContext::HasErrors()
{
    if (m_diagnostic_consumer_ap.get())
        return (static_cast<StoringDiagnosticConsumer*>(m_diagnostic_consumer_ap.get())->NumErrors() != 0);
    else
        return false;
}

bool
SwiftASTContext::HasFatalErrors(swift::ASTContext *ast_context)
{
    return (ast_context && ast_context->Diags.hasFatalErrorOccurred());
}

void
SwiftASTContext::ClearDiagnostics()
{
    assert (!HasFatalErrors() && "Never clear a fatal diagnostic!");
    if (m_diagnostic_consumer_ap.get())
        static_cast<StoringDiagnosticConsumer*>(m_diagnostic_consumer_ap.get())->Clear();
}

bool
SwiftASTContext::SetColorizeDiagnostics(bool b)
{
    if (m_diagnostic_consumer_ap.get())
        return static_cast<StoringDiagnosticConsumer*>(m_diagnostic_consumer_ap.get())->SetColorize(b);
    return false;
}

void
SwiftASTContext::PrintDiagnostics(Stream &stream,
                                  uint32_t bufferID,
                                  uint32_t first_line,
                                  uint32_t last_line,
                                  uint32_t line_offset)
{
    // If this is a fatal error, copy the error into the AST Context's fatal error field,
    // and then put it to the stream, otherwise just dump the diagnostics to the stream.

    if (m_ast_context_ap->Diags.hasFatalErrorOccurred() && !m_reported_fatal_error)
    {
        StreamString strm;

        if (m_diagnostic_consumer_ap.get())
            static_cast<StoringDiagnosticConsumer*>(m_diagnostic_consumer_ap.get())->PrintDiagnostics(strm,
                                                                                                      bufferID,
                                                                                                      first_line,
                                                                                                      last_line,
                                                                                                      line_offset);
        if (strm.GetSize() != 0)
            m_fatal_errors.SetErrorString(strm.GetData());
        else
            m_fatal_errors.SetErrorString("Unknown fatal error occurred.");
            
        m_reported_fatal_error = true;
        stream.PutCString(strm.GetData());
    }
    else
    {
        if (m_diagnostic_consumer_ap.get())
            static_cast<StoringDiagnosticConsumer*>(m_diagnostic_consumer_ap.get())->PrintDiagnostics(stream,
                                                                                                      bufferID,
                                                                                                      first_line,
                                                                                                      last_line,
                                                                                                      line_offset);
    
    }
}

void
SwiftASTContext::ModulesDidLoad (ModuleList &module_list)
{
    ClearModuleDependentCaches();
}

void
SwiftASTContext::ClearModuleDependentCaches ()
{
    m_negative_type_cache.Clear();
    m_extra_type_info_cache.Clear();
}

void
SwiftASTContext::DumpConfiguration(Log *log)
{
    if (!log)
        return;
    
    log->Printf("(SwiftASTContext*)%p:", this);
    
    if (!m_ast_context_ap)
        log->Printf("  (no AST context)");

    log->Printf("  Architecture                 : %s", m_ast_context_ap->LangOpts.Target.getTriple().c_str());
    
    log->Printf("  SDK path                     : %s", m_ast_context_ap->SearchPathOpts.SDKPath.c_str());
    log->Printf("  Runtime resource path        : %s", m_ast_context_ap->SearchPathOpts.RuntimeResourcePath.c_str());
    log->Printf("  Runtime library path         : %s", m_ast_context_ap->SearchPathOpts.RuntimeLibraryPath.c_str());
    log->Printf("  Runtime library import path  : %s", m_ast_context_ap->SearchPathOpts.RuntimeLibraryImportPath.c_str());
    
    log->Printf("  Framework search paths       : (%llu items)", (unsigned long long)m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths.size());
    for (std::string &framework_search_path : m_ast_context_ap->SearchPathOpts.FrameworkSearchPaths)
    {
        log->Printf("    %s", framework_search_path.c_str());
    }
    
    log->Printf("  Import search paths          : (%llu items)", (unsigned long long)m_ast_context_ap->SearchPathOpts.ImportSearchPaths.size());
    for (std::string &import_search_path : m_ast_context_ap->SearchPathOpts.ImportSearchPaths)
    {
        log->Printf("    %s", import_search_path.c_str());
    }
    
    swift::ClangImporterOptions &clang_importer_options = GetClangImporterOptions();
    
    log->Printf("  Extra clang arguments        : (%llu items)", (unsigned long long)clang_importer_options.ExtraArgs.size());
    for (std::string &extra_arg : clang_importer_options.ExtraArgs)
    {
        log->Printf("    %s", extra_arg.c_str());
    }

}

bool
SwiftASTContext::HasTarget() const
{
    lldb::TargetWP empty_wp;
    
    // If either call to "std::weak_ptr::owner_before(...) value returns true, this
    // indicates that m_section_wp once contained (possibly still does) a reference
    // to a valid shared pointer. This helps us know if we had a valid reference to
    // a target which is now invalid because the target was deleted.
    return empty_wp.owner_before(m_target_wp) || m_target_wp.owner_before(empty_wp);
}


bool
SwiftASTContext::CheckProcessChanged ()
{
    if (HasTarget())
    {
        TargetSP target_sp (m_target_wp.lock());
        if (target_sp)
        {
            Process *process = target_sp->GetProcessSP().get();
            if (m_process == NULL)
            {
                if (process)
                    m_process = process;
            }
            else
            {
                if (m_process != process)
                    return true;
            }
        }
    }
    return false;
}

void
SwiftASTContext::AddDebuggerClient (swift::DebuggerClient *debugger_client)
{
    m_debugger_clients.push_back(std::unique_ptr<swift::DebuggerClient>(debugger_client));
}

SwiftASTContext::ExtraTypeInformation::ExtraTypeInformation ()
: m_is_trivial_option_set(false),
m_is_zero_size(false)
{
}

SwiftASTContext::ExtraTypeInformation::ExtraTypeInformation (swift::CanType swift_can_type)
: m_is_trivial_option_set(false),
m_is_zero_size(false)
{
    static ConstString g_rawValue("rawValue");
    
    swift::ASTContext &ast_ctx = swift_can_type->getASTContext();
    SwiftASTContext *swift_ast = SwiftASTContext::GetSwiftASTContext(&ast_ctx);
    if (swift_ast)
    {
        swift::ProtocolDecl *option_set = ast_ctx.getProtocol(swift::KnownProtocolKind::OptionSetType);
        if (option_set)
        {
            if (auto nominal_decl = swift_can_type.getNominalOrBoundGenericNominal())
            {
                for (swift::ProtocolDecl *protocol_decl : nominal_decl->getAllProtocols())
                {
                    if (protocol_decl == option_set)
                    {
                        for (swift::VarDecl *stored_property : nominal_decl->getStoredProperties())
                        {
                            swift::Identifier name = stored_property->getName();
                            if (name.str() == g_rawValue.GetStringRef())
                            {
                                m_is_trivial_option_set = true;
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
    
    if (auto metatype_type = llvm::dyn_cast_or_null<swift::MetatypeType>(swift_can_type.getPointer()))
    {
        if (metatype_type &&
            (
             (false == metatype_type->hasRepresentation()) ||
             (swift::MetatypeRepresentation::Thin == metatype_type->getRepresentation())
             )
            )
            m_is_zero_size = true;
    }
    else if (auto enum_decl = swift_can_type->getEnumOrBoundGenericEnum())
    {
        size_t num_nopayload=0, num_payload=0;
        for (auto the_case : enum_decl->getAllElements())
        {
            if (the_case->hasArgumentType())
            {
                num_payload = 1;
                break;
            }
            else
            {
                if (++num_nopayload > 1)
                    break;
            }
        }
        if (num_nopayload == 1 && num_payload == 0)
            m_is_zero_size = true;
    }
    else if (auto struct_decl = swift_can_type->getStructOrBoundGenericStruct())
    {
        bool has_storage = false;
        auto members = struct_decl->getMembers();
        for (const auto& member : members)
        {
            if (swift::VarDecl *var_decl = llvm::dyn_cast_or_null<swift::VarDecl>(member))
            {
                if (!var_decl->isStatic() && var_decl->hasStorage())
                {
                    has_storage = true;
                    break;
                }
            }
        }
        m_is_zero_size = !has_storage;
    }
    else if (auto tuple_type = llvm::dyn_cast_or_null<swift::TupleType>(swift_can_type.getPointer()))
    {
        m_is_zero_size = (tuple_type->getNumElements() == 0);
    }
}

SwiftASTContext::ExtraTypeInformation
SwiftASTContext::GetExtraTypeInformation (void* type)
{
    if (!type)
        return ExtraTypeInformation();
    
    swift::CanType swift_can_type;
    void *swift_can_type_ptr = nullptr;
    if (auto swift_type = GetSwiftType(type))
    {
        swift_can_type = swift_type->getCanonicalType();
        swift_can_type_ptr = swift_can_type.getPointer();
    }
    if (!swift_can_type_ptr)
        return ExtraTypeInformation();
    
    ExtraTypeInformation eti;
    if (!m_extra_type_info_cache.Lookup(swift_can_type_ptr, eti))
    {
        ExtraTypeInformation extra_info(swift_can_type);
        m_extra_type_info_cache.Insert(swift_can_type_ptr, extra_info);
        return extra_info;
    }
    else
    {
        return eti;
    }
}

bool
SwiftASTContext::DeclContextIsStructUnionOrClass (void *opaque_decl_ctx)
{
    return false;
}

ConstString
SwiftASTContext::DeclContextGetName (void *opaque_decl_ctx)
{
    return ConstString();
}

bool
SwiftASTContext::DeclContextIsClassMethod (void *opaque_decl_ctx,
                                           lldb::LanguageType *language_ptr,
                                           bool *is_instance_method_ptr,
                                           ConstString *language_object_name_ptr)
{
    return false;
}

    ///////////
////////////////////
    ///////////

bool
SwiftASTContext::IsArrayType (void* type,
                              CompilerType *element_type_ptr,
                              uint64_t *size,
                              bool *is_incomplete)
{
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::BoundGenericStruct:
        {
            swift::BoundGenericStructType* struct_type = swift_can_type->getAs<swift::BoundGenericStructType>();
            if (struct_type)
            {
                swift::StructDecl* struct_decl = struct_type->getDecl();
                if (struct_decl)
                {
                    if (struct_decl->getName().get() && strcmp(struct_decl->getName().get(),"Array") != 0)
                        break;
                    if (!struct_decl->getModuleContext() || !struct_decl->getModuleContext()->isStdlibModule())
                        break;
                    const llvm::ArrayRef<swift::Type> &args = struct_type->getGenericArgs();
                    if (args.size() != 1)
                        break;
                    if (is_incomplete)
                        *is_incomplete = true;
                    if (size)
                        *size = 0;
                    if (element_type_ptr)
                        *element_type_ptr = CompilerType(GetASTContext(), args[0].getPointer());
                    return true;
                }
            }
        }
            break;
        default:
            break;
    }
    
    return false;
}

bool
SwiftASTContext::IsAggregateType (void * type)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::ExistentialMetatype:
            case swift::TypeKind::Metatype:
                return false;
            case swift::TypeKind::Dictionary:
                return true;
            case swift::TypeKind::UnmanagedStorage:
            case swift::TypeKind::UnownedStorage:
            case swift::TypeKind::WeakStorage:
                return IsAggregateType(swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer());
            case swift::TypeKind::Optional:
            case swift::TypeKind::ImplicitlyUnwrappedOptional:
            case swift::TypeKind::GenericTypeParam:
            case swift::TypeKind::AssociatedType:
            case swift::TypeKind::DependentMember:
            case swift::TypeKind::NameAlias:
                break;
            case swift::TypeKind::Paren:
                return IsAggregateType(llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer());
            case swift::TypeKind::Error:
            case swift::TypeKind::BuiltinInteger:
            case swift::TypeKind::BuiltinFloat:
            case swift::TypeKind::BuiltinRawPointer:
            case swift::TypeKind::BuiltinNativeObject:
            case swift::TypeKind::BuiltinUnsafeValueBuffer:
            case swift::TypeKind::BuiltinUnknownObject:
            case swift::TypeKind::BuiltinBridgeObject:
            case swift::TypeKind::Protocol:
            case swift::TypeKind::Module:
            case swift::TypeKind::Archetype:
            case swift::TypeKind::Substituted:
            case swift::TypeKind::Function:
            case swift::TypeKind::GenericFunction:
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::ProtocolComposition:
                break;
            case swift::TypeKind::LValue:
                break;
            case swift::TypeKind::UnboundGeneric:
            case swift::TypeKind::TypeVariable:
                return false;
            case swift::TypeKind::Tuple:
            case swift::TypeKind::ArraySlice:
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericEnum:
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::BuiltinVector:
            case swift::TypeKind::Class:
            case swift::TypeKind::Struct:
            case swift::TypeKind::Enum:
                return true;
                
            case swift::TypeKind::DynamicSelf:
            case swift::TypeKind::SILBox:
            case swift::TypeKind::SILFunction:
            case swift::TypeKind::SILBlockStorage:
            case swift::TypeKind::InOut:
            case swift::TypeKind::Unresolved:
                return false;
        }
    }

    return false;
}

bool
SwiftASTContext::IsVectorType (void* type,
                               CompilerType *element_type,
                               uint64_t *size)
{
    return false;
}

bool
SwiftASTContext::IsRuntimeGeneratedType (void* type)
{
    return false;
}

bool
SwiftASTContext::IsCharType (void* type)
{
    return false;
}


bool
SwiftASTContext::IsCompleteType (void* type)
{
    return true;
}

bool
SwiftASTContext::IsConst(void* type)
{
    return false;
}

bool
SwiftASTContext::IsCStringType (void* type, uint32_t &length)
{
    return false;
}

bool
SwiftASTContext::IsFunctionType (void* type, bool *is_variadic_ptr)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Function:
            case swift::TypeKind::PolymorphicFunction:
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
uint32_t
SwiftASTContext::IsHomogeneousAggregate (void* type, CompilerType* base_type_ptr)
{
    return 0;
}

size_t
SwiftASTContext::GetNumberOfFunctionArguments (void* type)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        swift::AnyFunctionType *func = llvm::dyn_cast_or_null<swift::AnyFunctionType>(swift_can_type.getPointer());
        if (func)
        {
            swift::TypeBase *input = func->getInput().getPointer();
            if (!input)
                return 0;
            // see comment in swift::AnyFunctionType for rationale here:
            // a function can take either a tuple or a parentype, but if a parentype
            // (i.e. (Foo)), then it will be reduced down to just Foo, so if the input is
            // not a tuple, that must mean there is only 1 input
            swift::TupleType *tuple = llvm::dyn_cast_or_null<swift::TupleType>(input);
            if (tuple)
                return tuple->getNumElements();
            else
                return 1;
        }
    }
    return 0;
}

CompilerType
SwiftASTContext::GetFunctionArgumentAtIndex (void* type, const size_t index)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        swift::AnyFunctionType *func = llvm::dyn_cast_or_null<swift::AnyFunctionType>(swift_can_type.getPointer());
        if (func)
        {
            swift::TypeBase *input = func->getInput().getPointer();
            if (!input)
                return CompilerType();
            // see comment in swift::AnyFunctionType for rationale here:
            // a function can take either a tuple or a parentype, but if a parentype
            // (i.e. (Foo)), then it will be reduced down to just Foo, so if the input is
            // not a tuple, that must mean there is only 1 input
            swift::TupleType *tuple = llvm::dyn_cast_or_null<swift::TupleType>(input);
            if (tuple)
            {
                if (index < tuple->getNumElements())
                    return CompilerType(GetASTContext(),tuple->getElementType(index).getPointer());
            }
            else
                return CompilerType(GetASTContext(),input);
        }
    }
    return CompilerType();
}

bool
SwiftASTContext::IsFunctionPointerType (void* type)
{
    return IsFunctionType(type, nullptr); // FIXME: think about this
}

bool
SwiftASTContext::IsIntegerType (void* type, bool &is_signed)
{
    return (GetTypeInfo(type, nullptr) & eTypeIsInteger);
}

bool
SwiftASTContext::IsPointerType (void* type, CompilerType *pointee_type)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Error:
            case swift::TypeKind::BuiltinInteger:
            case swift::TypeKind::BuiltinFloat:
                return false;
            case swift::TypeKind::BuiltinRawPointer:
            case swift::TypeKind::BuiltinNativeObject:
            case swift::TypeKind::BuiltinUnsafeValueBuffer:
            case swift::TypeKind::BuiltinUnknownObject:
            case swift::TypeKind::BuiltinBridgeObject:
                return true;
            case swift::TypeKind::BuiltinVector:
            case swift::TypeKind::NameAlias:
                return false;
            case swift::TypeKind::Paren:
                return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).IsPointerType(pointee_type);
            case swift::TypeKind::Tuple:
                return false;
            case swift::TypeKind::UnmanagedStorage:
            case swift::TypeKind::UnownedStorage:
            case swift::TypeKind::WeakStorage:
                return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).IsPointerType(pointee_type);
            case swift::TypeKind::Optional:
            case swift::TypeKind::ImplicitlyUnwrappedOptional:
            case swift::TypeKind::GenericTypeParam:
            case swift::TypeKind::AssociatedType:
            case swift::TypeKind::DependentMember:
            case swift::TypeKind::Enum:
            case swift::TypeKind::Struct:
            case swift::TypeKind::Dictionary:
                return false;
            case swift::TypeKind::Class:
            case swift::TypeKind::BoundGenericClass:
                return false; // Do we return true for classes since instances are usually pointers???
            case swift::TypeKind::Protocol:
                return false;
                
            case swift::TypeKind::ExistentialMetatype:
            case swift::TypeKind::Metatype:
                return false;
                
            case swift::TypeKind::Module:
            case swift::TypeKind::Archetype:
            case swift::TypeKind::Substituted:
            case swift::TypeKind::Function:
            case swift::TypeKind::GenericFunction:
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::ArraySlice:
            case swift::TypeKind::ProtocolComposition:
            case swift::TypeKind::DynamicSelf:
            case swift::TypeKind::SILBox:
            case swift::TypeKind::SILFunction:
            case swift::TypeKind::SILBlockStorage:
            case swift::TypeKind::InOut:
                break;
            case swift::TypeKind::LValue:
            case swift::TypeKind::UnboundGeneric:
            case swift::TypeKind::BoundGenericEnum:
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::TypeVariable:
            case swift::TypeKind::Unresolved:
                return false;
        }
    }
    if (pointee_type)
        pointee_type->Clear();
    return false;
}


bool
SwiftASTContext::IsPointerOrReferenceType (void* type, CompilerType *pointee_type)
{
    return IsPointerType (type, pointee_type) || IsReferenceType(type, pointee_type, nullptr);
}

bool
SwiftASTContext::ShouldTreatScalarValueAsAddress (lldb::opaque_compiler_type_t type)
{
    return Flags(GetTypeInfo(type, nullptr)).AnySet(eTypeInstanceIsPointer | eTypeIsReference);
}

bool
SwiftASTContext::IsReferenceType (void* type, CompilerType *pointee_type, bool* is_rvalue)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
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

bool
SwiftASTContext::IsInoutType (const CompilerType& compiler_type, CompilerType *original_type)
{
    bool return_value = false;
    
    if (compiler_type.IsValid())
    {
        if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
        {
            swift::CanType swift_can_type (GetCanonicalSwiftType (compiler_type));
            const swift::TypeKind type_kind = swift_can_type->getKind();
            switch (type_kind)
            {
                case swift::TypeKind::LValue:
                {
                    swift::LValueType *lvalue = swift_can_type->getAs<swift::LValueType>();
                    if (lvalue)
                    {
                        if (original_type)
                            *original_type = CompilerType(ast, lvalue->getObjectType().getPointer());
                        return_value = true;
                    }
                }
                    break;
            }
        }
    }
    
    return return_value;
}

bool
SwiftASTContext::IsFloatingPointType (void* type, uint32_t &count, bool &is_complex)
{
    if (type)
    {
        if (GetTypeInfo(type, nullptr) & eTypeIsFloat)
        {
            count = 1;
            is_complex = false;
            return true;
        }
    }
    count = 0;
    is_complex = false;
    return false;
}


bool
SwiftASTContext::IsDefined(void* type)
{
    if (!type)
        return false;
    
    return true;
}

bool
SwiftASTContext::IsPolymorphicClass (void* type)
{
    return false;
}

bool
SwiftASTContext::IsPossibleDynamicType (void* type,
                                        CompilerType *dynamic_pointee_type,
                                        bool check_cplusplus,
                                        bool check_objc,
                                        bool check_swift)
{
    if (type && check_swift)
    {
        // FIXME: use the dynamic_pointee_type
        Flags type_flags(GetTypeInfo(type, nullptr));
        
        if (type_flags.AnySet(eTypeIsArchetype | eTypeIsClass | eTypeIsProtocol))
            return true;
        
        if (type_flags.AnySet(eTypeIsStructUnion | eTypeIsEnumeration | eTypeIsTuple))
        {
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


bool
SwiftASTContext::IsScalarType (void* type)
{
    if (!type)
        return false;
    
    return (GetTypeInfo (type, nullptr) & eTypeIsScalar) != 0;
}

bool
SwiftASTContext::IsTypedefType (void* type)
{
    if (!type)
        return false;
    return GetSwiftType (type)->getKind() == swift::TypeKind::NameAlias;
}

bool
SwiftASTContext::IsVoidType (void* type)
{
    if (!type)
        return false;
    return type == GetASTContext()->TheEmptyTupleType.getPointer();
}

bool
SwiftASTContext::IsArchetypeType (const CompilerType& compiler_type)
{
    if (!compiler_type.IsValid())
        return false;

    if (llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (compiler_type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        return (type_kind == swift::TypeKind::Archetype);
    }
    return false;
}

bool
SwiftASTContext::IsPossibleZeroSizeType (const CompilerType& compiler_type)
{
    if (!compiler_type.IsValid())
        return false;
    
    if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
        return ast->GetExtraTypeInformation(GetCanonicalSwiftType(compiler_type).getPointer()).m_is_zero_size;
    return false;
}

bool
SwiftASTContext::IsErrorType (const CompilerType& compiler_type)
{
    if (compiler_type.IsValid() &&
        llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
    {
        ProtocolInfo protocol_info;
        
        if (GetProtocolTypeInfo(compiler_type,
                                protocol_info))
            return protocol_info.m_is_errortype;
        return false;
    }
    return false;
}

CompilerType
SwiftASTContext::GetReferentType (const CompilerType& compiler_type)
{
    if (compiler_type.IsValid() &&
        llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (compiler_type));
        swift::ReferenceStorageType *ref_type = llvm::dyn_cast_or_null<swift::ReferenceStorageType>(swift_can_type.getPointer());
        if (ref_type)
        {
            swift::TypeBase *referent_type = ref_type->getReferentType().getPointer();
            if (referent_type)
            {
                swift::CanType referent_can_type(referent_type->getDesugaredType()->getCanonicalType());
                return CompilerType(GetASTContext(), referent_can_type.getPointer());
            }
        }
        else
            return compiler_type;
    }
    
    return CompilerType();
}


bool
SwiftASTContext::IsTrivialOptionSetType (const CompilerType& compiler_type)
{
    if (compiler_type.IsValid() &&
        llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
        return GetExtraTypeInformation(compiler_type.GetOpaqueQualType()).m_is_trivial_option_set;
    return false;
}

bool
SwiftASTContext::IsFullyRealized (const CompilerType& compiler_type)
{
    if (!compiler_type.IsValid())
        return false;
    
    if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(compiler_type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (compiler_type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Archetype:
            case swift::TypeKind::UnboundGeneric:
                return false;
            case swift::TypeKind::Paren: {
                swift::ParenType *paren_type = llvm::dyn_cast<swift::ParenType>(swift_can_type.getPointer());
                if (paren_type)
                {
                    CompilerType nested_type(ast->GetASTContext(),paren_type->getUnderlyingType().getPointer());
                    return IsFullyRealized(nested_type);
                }
                return true;
            }
                break;
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::BoundGenericEnum: {
                for (size_t idx = 0;
                     idx < compiler_type.GetNumTemplateArguments();
                     idx++)
                {
                    lldb::TemplateArgumentKind kind;
                    CompilerType argtype = compiler_type.GetTemplateArgument(idx, kind);
                    if (!IsFullyRealized(argtype))
                        return false;
                }
            }
                return true;
            case swift::TypeKind::Tuple:
                for (uint32_t idx = 0;
                     idx < compiler_type.GetNumFields();
                     idx++)
                {
                    std::string name;
                    CompilerType field = compiler_type.GetFieldAtIndex(idx, name, nullptr, nullptr, nullptr);
                    if (IsFullyRealized(field) == false)
                        return false;
                }
                return true;
            default:
                return true;
        }
    }

    return false;
}

bool
SwiftASTContext::GetProtocolTypeInfo (const CompilerType& type,
                                      ProtocolInfo& protocol_info)
{
    if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Protocol:
            {
                swift::ProtocolType *t = swift_can_type->getAs<swift::ProtocolType>();
                protocol_info.m_is_class_only = t->getDecl()->requiresClass();
                protocol_info.m_num_protocols = 1;
                protocol_info.m_num_payload_words = 3;
                protocol_info.m_is_objc = t->getDecl()->isObjC();
                protocol_info.m_is_anyobject = (t->getDecl() == ast->GetASTContext()->getProtocol(swift::KnownProtocolKind::AnyObject));
                protocol_info.m_is_errortype = (t->getDecl() == ast->GetASTContext()->getExceptionTypeDecl());
                protocol_info.m_num_payload_words = (protocol_info.m_is_errortype ? 0 : 3);
                if (protocol_info.IsOneWordStorage()) // @objc protocols only wrap an ISA/metadata pointer
                    protocol_info.m_num_storage_words = 1;
                else
                    protocol_info.m_num_storage_words = (protocol_info.m_is_class_only ? 0 : 1) + protocol_info.m_num_protocols + 3;
                return true;
            }
            case swift::TypeKind::ProtocolComposition:
            {
                swift::ProtocolCompositionType *t = swift_can_type->getAs<swift::ProtocolCompositionType>();
                protocol_info.m_is_class_only = t->requiresClass();
                protocol_info.m_num_protocols = t->getProtocols().size();
                protocol_info.m_num_payload_words = 3;
                protocol_info.m_is_objc = false;
                protocol_info.m_is_errortype = false;
                protocol_info.m_is_anyobject = false;
                protocol_info.m_num_storage_words = (protocol_info.m_is_class_only ? 0 : 1) + protocol_info.m_num_protocols + 3;
                return true;
            }
            default:
                return false;
        }
    }
    
    return false;
}

SwiftASTContext::TypeAllocationStrategy
SwiftASTContext::GetAllocationStrategy (const CompilerType& type)
{
    if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem()))
    {
        const swift::irgen::TypeInfo *type_info = ast->GetSwiftTypeInfo(type.GetOpaqueQualType());
        if (!type_info)
            return TypeAllocationStrategy::eUnknown;
        switch (type_info->getFixedPacking(ast->GetIRGenModule()))
        {
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

bool
SwiftASTContext::IsBeingDefined (void* type)
{
    return false;
}

bool
SwiftASTContext::IsObjCObjectPointerType (const CompilerType& type, CompilerType *class_type_ptr)
{
    if (!type)
        return false;
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
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

bool
SwiftASTContext::GetCompleteType (void* type)
{
    return true;
}

ConstString
SwiftASTContext::GetTypeName (void* type)
{
    std::string type_name;
    if (type)
    {
        swift::Type swift_type(GetSwiftType (type));
        
        swift::Type normalized_type =
        swift_type.transform([] (swift::Type type) -> swift::Type {
            if (swift::SyntaxSugarType *syntax_sugar_type = llvm::dyn_cast<swift::SyntaxSugarType>(type.getPointer())) {
                return syntax_sugar_type->getSinglyDesugaredType();
            }
            if (swift::DictionaryType *dictionary_type = llvm::dyn_cast<swift::DictionaryType>(type.getPointer())) {
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

static swift::Type
TransformUntilStable (swift::Type type,
                      std::function<swift::Type(swift::Type)> f)
{
    unsigned max_iter = 100;
    void* ptr = nullptr;
    do {
        ptr = type.getPointer();
        type = type.transform(f);
    } while ((--max_iter > 0) && (ptr != type.getPointer()));
    return type;
}

ConstString
SwiftASTContext::GetDisplayTypeName (void * type)
{
    std::string type_name(GetTypeName(type).AsCString(""));

    if (type)
    {
        swift::Type swift_type(GetSwiftType (type));
        swift::Type normalized_type = TransformUntilStable(swift_type,
                                                           [] (swift::Type type) -> swift::Type {
                                                               return type.getPointer() ? type->getWithoutParens() : type;
                                                           });
        
        swift::PrintOptions print_options;
        print_options.FullyQualifiedTypes = false;
        print_options.SynthesizeSugarOnTypes = true;
        print_options.FullyQualifiedTypesIfAmbiguous = true;
        type_name = normalized_type.getString(print_options);
    }
    
    return ConstString(type_name);
}

ConstString
SwiftASTContext::GetTypeSymbolName (void * type)
{
    swift::Type swift_type(GetSwiftType (type));
    return GetTypeName(swift_type->getWithoutParens().getPointer());
}

ConstString
SwiftASTContext::GetMangledTypeName (void * type)
{
    return GetMangledTypeName(GetSwiftType(type).getPointer());
}

uint32_t
SwiftASTContext::GetTypeInfo (void* type, CompilerType *pointee_or_element_clang_type)
{
    if (!type)
        return 0;
    
    if (pointee_or_element_clang_type)
        pointee_or_element_clang_type->Clear();
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    const swift::TypeKind type_kind = swift_can_type->getKind();
    uint32_t swift_flags = eTypeIsSwift;
    switch (type_kind)
    {
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
        case swift::TypeKind::Error:
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::Module:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::TypeVariable:
            break;
        case swift::TypeKind::UnboundGeneric:
            swift_flags |= eTypeIsGeneric;
            break;
            
        case swift::TypeKind::GenericFunction:
            swift_flags |= eTypeIsGeneric;
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::Function:             swift_flags |= eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeInstanceIsPointer; break;
        case swift::TypeKind::BuiltinInteger:       swift_flags |= eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeIsInteger; break;
        case swift::TypeKind::BuiltinFloat:         swift_flags |= eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeIsFloat; break;
        case swift::TypeKind::BuiltinRawPointer:    swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer | eTypeIsScalar | eTypeHasValue; break;
        case swift::TypeKind::BuiltinNativeObject:  swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer | eTypeIsScalar | eTypeHasValue; break;
        case swift::TypeKind::BuiltinUnknownObject: swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer | eTypeIsScalar | eTypeHasValue | eTypeIsObjC; break;
        case swift::TypeKind::BuiltinBridgeObject:  swift_flags |= eTypeIsBuiltIn | eTypeHasChildren | eTypeIsPointer | eTypeIsScalar | eTypeHasValue | eTypeIsObjC; break;
        case swift::TypeKind::BuiltinUnsafeValueBuffer: swift_flags |= eTypeIsBuiltIn | eTypeIsPointer | eTypeIsScalar | eTypeHasValue; break;
        case swift::TypeKind::BuiltinVector:
            // TODO: OR in eTypeIsFloat or eTypeIsInteger as needed
            return eTypeIsBuiltIn | eTypeHasChildren | eTypeIsVector; break;
        case swift::TypeKind::NameAlias:            swift_flags |= eTypeIsTypedef; break;
        case swift::TypeKind::Paren:
            swift_flags |= CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetTypeInfo(pointee_or_element_clang_type); break;
            
        case swift::TypeKind::Tuple:                swift_flags |= eTypeHasChildren | eTypeIsTuple; break;
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            swift_flags |= CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetTypeInfo(pointee_or_element_clang_type); break;
        case swift::TypeKind::Optional:             swift_flags |= eTypeHasChildren;  break; // TODO: verify if this is a pointer?
        case swift::TypeKind::ImplicitlyUnwrappedOptional:    swift_flags |= eTypeHasChildren;  break; // TODO: as above
        case swift::TypeKind::BoundGenericEnum:
            swift_flags |= eTypeIsGeneric | eTypeIsBound;
        case swift::TypeKind::Enum:
        {
            SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
            if (cached_enum_info)
            {
                if (cached_enum_info->GetNumElementsWithPayload() == 0)
                    swift_flags |= eTypeHasValue | eTypeIsEnumeration;
                else
                    swift_flags |= eTypeHasValue | eTypeIsEnumeration | eTypeHasChildren;
            }
            else
                swift_flags |= eTypeIsEnumeration;
        }
            break;
            
        case swift::TypeKind::Dictionary:
        case swift::TypeKind::BoundGenericStruct:
            swift_flags |= eTypeIsGeneric | eTypeIsBound;
        case swift::TypeKind::Struct:
            swift_flags |= eTypeHasChildren | eTypeIsStructUnion; break;

        case swift::TypeKind::BoundGenericClass:
            swift_flags |= eTypeIsGeneric | eTypeIsBound;
        case swift::TypeKind::Class:
            swift_flags |= eTypeHasChildren | eTypeIsClass | eTypeHasValue | eTypeInstanceIsPointer; break;
            
        case swift::TypeKind::Protocol:
        case swift::TypeKind::ProtocolComposition:
            swift_flags |=  eTypeHasChildren | eTypeIsStructUnion | eTypeIsProtocol; break;
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
            swift_flags |= eTypeIsMetatype | eTypeHasValue; break;
            
        case swift::TypeKind::Archetype:            swift_flags |= eTypeHasValue | eTypeIsScalar | eTypeIsPointer | eTypeIsArchetype; break;
        case swift::TypeKind::ArraySlice:
            // TODO: extract element type
            //                if (pointee_or_element_clang_type)
            //                    pointee_or_element_clang_type->SetClangType(m_ast, llvm::cast<clang::ArrayType>(qual_type.getTypePtr())->getElementType());
            swift_flags |= eTypeHasChildren | eTypeIsArray; break;
            
        case swift::TypeKind::LValue:
            if (pointee_or_element_clang_type)
                *pointee_or_element_clang_type = GetNonReferenceType(type);
            swift_flags |= eTypeHasChildren | eTypeIsReference | eTypeHasValue; break;
        case swift::TypeKind::DynamicSelf:
        case swift::TypeKind::SILBox:
        case swift::TypeKind::SILFunction:
        case swift::TypeKind::SILBlockStorage:
        case swift::TypeKind::InOut:
        case swift::TypeKind::Unresolved:
            break;
            
    }
    return swift_flags;
}



lldb::LanguageType
SwiftASTContext::GetMinimumLanguage (void* type)
{
    if (!type)
        return lldb::eLanguageTypeC;
    
    return lldb::eLanguageTypeSwift;
}

lldb::TypeClass
SwiftASTContext::GetTypeClass (void* type)
{
    if (!type)
        return lldb::eTypeClassInvalid;
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::Error:                    return lldb::eTypeClassOther;
        case swift::TypeKind::BuiltinInteger:           return lldb::eTypeClassBuiltin;
        case swift::TypeKind::BuiltinFloat:             return lldb::eTypeClassBuiltin;
        case swift::TypeKind::BuiltinRawPointer:        return lldb::eTypeClassBuiltin;
        case swift::TypeKind::BuiltinNativeObject:      return lldb::eTypeClassBuiltin;
        case swift::TypeKind::BuiltinUnsafeValueBuffer: return lldb::eTypeClassBuiltin;
        case swift::TypeKind::BuiltinUnknownObject:     return lldb::eTypeClassBuiltin;
        case swift::TypeKind::BuiltinBridgeObject:      return lldb::eTypeClassBuiltin;
        case swift::TypeKind::BuiltinVector:            return lldb::eTypeClassVector;
        case swift::TypeKind::NameAlias:                return lldb::eTypeClassTypedef;
        case swift::TypeKind::Paren:                    return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetTypeClass();
        case swift::TypeKind::Tuple:                    return lldb::eTypeClassArray;
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), llvm::cast<swift::ReferenceStorageType>(swift_can_type.getPointer())->getReferentType().getPointer()).GetTypeClass();
        case swift::TypeKind::Optional:                 return lldb::eTypeClassEnumeration;
        case swift::TypeKind::ImplicitlyUnwrappedOptional:        return lldb::eTypeClassEnumeration;
        case swift::TypeKind::GenericTypeParam:         return lldb::eTypeClassOther;
        case swift::TypeKind::AssociatedType:           return lldb::eTypeClassOther;
        case swift::TypeKind::DependentMember:          return lldb::eTypeClassOther;
        case swift::TypeKind::Enum:                     return lldb::eTypeClassUnion;
        case swift::TypeKind::Struct:                   return lldb::eTypeClassStruct;
        case swift::TypeKind::Class:                    return lldb::eTypeClassClass;
        case swift::TypeKind::Protocol:                 return lldb::eTypeClassOther;
        case swift::TypeKind::Metatype:                 return lldb::eTypeClassOther;
        case swift::TypeKind::Module:                   return lldb::eTypeClassOther;
        case swift::TypeKind::Archetype:                return lldb::eTypeClassOther;
        case swift::TypeKind::Substituted:              return lldb::eTypeClassOther;
        case swift::TypeKind::Function:                 return lldb::eTypeClassFunction;
        case swift::TypeKind::GenericFunction:          return lldb::eTypeClassFunction;
        case swift::TypeKind::PolymorphicFunction:      return lldb::eTypeClassFunction;
        case swift::TypeKind::ArraySlice:               return lldb::eTypeClassArray;
        case swift::TypeKind::ProtocolComposition:      return lldb::eTypeClassOther;
        case swift::TypeKind::LValue:                   return lldb::eTypeClassReference;
        case swift::TypeKind::UnboundGeneric:           return lldb::eTypeClassOther;
        case swift::TypeKind::BoundGenericClass:        return lldb::eTypeClassClass;
        case swift::TypeKind::BoundGenericEnum:         return lldb::eTypeClassUnion;
        case swift::TypeKind::BoundGenericStruct:       return lldb::eTypeClassStruct;
        case swift::TypeKind::Dictionary:               return lldb::eTypeClassStruct;
        case swift::TypeKind::TypeVariable:             return lldb::eTypeClassOther;
        case swift::TypeKind::ExistentialMetatype:      return lldb::eTypeClassOther;
        case swift::TypeKind::DynamicSelf:              return lldb::eTypeClassOther;
        case swift::TypeKind::SILBox:                   return lldb::eTypeClassOther;
        case swift::TypeKind::SILFunction:              return lldb::eTypeClassFunction;
        case swift::TypeKind::SILBlockStorage:          return lldb::eTypeClassOther;
        case swift::TypeKind::InOut:                    return lldb::eTypeClassOther;
        case swift::TypeKind::Unresolved:               return lldb::eTypeClassOther;
    }
    
    return lldb::eTypeClassOther;
}

unsigned
SwiftASTContext::GetTypeQualifiers(void* type)
{
    return 0;
}

//----------------------------------------------------------------------
// Creating related types
//----------------------------------------------------------------------

CompilerType
SwiftASTContext::GetArrayElementType (void* type, uint64_t *stride)
{
    CompilerType element_type;
    if (type)
    {
        swift::CanType swift_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::ArraySlice:
            {
                swift::ArraySliceType *array_slice_type = llvm::dyn_cast<swift::ArraySliceType>(swift_type.getPointer());
                if (!array_slice_type)
                    break;
                swift::Type baseType(array_slice_type->getBaseType());
                element_type = CompilerType(GetASTContext(),baseType.getPointer());
            }
                break;
            case swift::TypeKind::BoundGenericStruct:
            {
                // there are a couple of structs that mean "Array" in Swift:
                // Array<T>
                // NativeArray<T>
                // Slice<T>
                // treat them as arrays for convenience sake
                swift::BoundGenericStructType *boundGenericStructType(swift_type->getAs<swift::BoundGenericStructType>());
                if (!boundGenericStructType)
                    break;
                
                auto args = boundGenericStructType->getGenericArgs();
                if (args.size() != 1)
                    break;
                
                swift::StructDecl *decl = boundGenericStructType->getDecl();
                
                if (!decl || !decl->getModuleContext() || !decl->getModuleContext()->isStdlibModule())
                    break;
                
                const char* declname = decl->getName().get();
                
                if (!declname)
                    break;
                
                if (0 == strcmp(declname, "NativeArray") ||
                    0 == strcmp(declname, "Array") ||
                    0 == strcmp(declname, "ArraySlice"))
                    element_type = CompilerType(GetASTContext(),args[0].getPointer());
            }
                break;
            default:
                break;
        }
    }
    return element_type;
}

CompilerType
SwiftASTContext::GetCanonicalType (void* type)
{
    if (type)
        return CompilerType (GetASTContext(), GetCanonicalSwiftType(type).getPointer());
    return CompilerType();
}

CompilerType
SwiftASTContext::GetInstanceType (void* type)
{
    if (!type)
        return CompilerType();

    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    switch (swift_can_type->getKind())
    {
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
        {
            swift::AnyMetatypeType *metatype_type = llvm::dyn_cast<swift::AnyMetatypeType>(swift_can_type.getPointer());
            if (metatype_type)
                return CompilerType(GetASTContext(),metatype_type->getInstanceType().getPointer());
            return CompilerType();
        }
        default:
            break;
    }
    
    return CompilerType(GetASTContext(), GetSwiftType(type));
}

CompilerType
SwiftASTContext::GetFullyUnqualifiedType (void* type)
{
    return CompilerType(GetASTContext(), GetSwiftType(type));
}


int
SwiftASTContext::GetFunctionArgumentCount (void* type)
{
    if (type)
    {
        const swift::AnyFunctionType* func = llvm::dyn_cast<swift::AnyFunctionType>(GetCanonicalSwiftType (type).getPointer());
        if (func)
        {
            swift::Type arg_type = func->getInput();
            if (arg_type)
            {
                swift::TupleType *tuple_type = llvm::dyn_cast<swift::TupleType>(arg_type.getPointer());
                if (tuple_type)
                {
                    return tuple_type->getNumElements();
                }
            }
        }
    }
    return -1;
}

CompilerType
SwiftASTContext::GetFunctionArgumentTypeAtIndex (void* type, size_t idx)
{
    if (type)
    {
        const swift::AnyFunctionType* func = llvm::dyn_cast<swift::AnyFunctionType>(GetCanonicalSwiftType (type).getPointer());
        if (func)
        {
            swift::Type arg_type = func->getInput();
            if (arg_type)
            {
                swift::TupleType *tuple_type = llvm::dyn_cast<swift::TupleType>(arg_type.getPointer());
                if (tuple_type)
                {
                    if (tuple_type->getNumElements())
                    {
                        return CompilerType(GetASTContext(), tuple_type->getElementType(idx).getPointer());
                    }
                }
                else
                {
                    return CompilerType(GetASTContext(), arg_type);
                }
            }
        }
    }
    return CompilerType();
}

CompilerType
SwiftASTContext::GetFunctionReturnType (void* type)
{
    if (type)
    {
        const swift::AnyFunctionType* func = llvm::dyn_cast<swift::AnyFunctionType>(GetCanonicalSwiftType (type).getPointer());
        if (func)
            return CompilerType(GetASTContext(), func->getResult().getPointer());
    }
    return CompilerType();
}

size_t
SwiftASTContext::GetNumMemberFunctions (void* type)
{
    size_t num_functions = 0;
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::BoundGenericEnum:
            {
                swift::BoundGenericType *bound_type = llvm::dyn_cast_or_null<swift::BoundGenericType>(swift_can_type.getPointer());
                if (bound_type)
                {
                    swift::NominalTypeDecl *nominal_decl = bound_type->getDecl();
                    if (nominal_decl)
                    {
                        auto iter = nominal_decl->getMembers().begin();
                        auto end = nominal_decl->getMembers().end();
                        for (;iter != end;iter++)
                        {
                            switch (iter->getKind())
                            {
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
            }
                break;
            case swift::TypeKind::Class:
            case swift::TypeKind::Struct:
            case swift::TypeKind::Enum:
            {
                swift::NominalType *nominal_type = llvm::dyn_cast_or_null<swift::NominalType>(swift_can_type.getPointer());
                if (nominal_type)
                {
                    swift::NominalTypeDecl *nominal_decl = nominal_type->getDecl();
                    if (nominal_decl)
                    {
                        auto iter = nominal_decl->getMembers().begin();
                        auto end = nominal_decl->getMembers().end();
                        for (;iter != end;iter++)
                        {
                            switch (iter->getKind())
                            {
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
            }
                break;
            default:
                break;
        }
    }
    return num_functions;
}

TypeMemberFunctionImpl
SwiftASTContext::GetMemberFunctionAtIndex (void* type, size_t idx)
{
    std::string name("");
    CompilerType result_type;
    MemberFunctionKind kind(MemberFunctionKind::eMemberFunctionKindUnknown);
    swift::AbstractFunctionDecl *the_decl_we_care_about = nullptr;
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::BoundGenericEnum:
            {
                swift::BoundGenericType *bound_type = llvm::dyn_cast_or_null<swift::BoundGenericType>(swift_can_type.getPointer());
                if (bound_type)
                {
                    swift::NominalTypeDecl *nominal_decl = bound_type->getDecl();
                    if (nominal_decl)
                    {
                        auto iter = nominal_decl->getMembers().begin();
                        auto end = nominal_decl->getMembers().end();
                        for (;iter != end;iter++)
                        {
                            auto decl_kind = iter->getKind();
                            switch (decl_kind)
                            {
                                case swift::DeclKind::Constructor:
                                case swift::DeclKind::Destructor:
                                case swift::DeclKind::Func:
                                {
                                    if (idx == 0)
                                    {
                                        swift::AbstractFunctionDecl *abstract_func_decl = llvm::dyn_cast_or_null<swift::AbstractFunctionDecl>(*iter);
                                        if (abstract_func_decl)
                                        {
                                            switch (decl_kind)
                                            {
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
                                                default:    // I know that this can only be one of three kinds since I am here..
                                                {
                                                    swift::FuncDecl *func_decl = llvm::dyn_cast<swift::FuncDecl>(*iter);
                                                    if (func_decl)
                                                    {
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
                                            result_type = CompilerType(GetASTContext(),abstract_func_decl->getType().getPointer());
                                        }
                                    }
                                    else
                                        --idx;
                                }
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                }
            }
                break;
            case swift::TypeKind::Class:
            case swift::TypeKind::Struct:
            case swift::TypeKind::Enum:
            {
                swift::NominalType *nominal_type = llvm::dyn_cast_or_null<swift::NominalType>(swift_can_type.getPointer());
                if (nominal_type)
                {
                    swift::NominalTypeDecl *nominal_decl = nominal_type->getDecl();
                    if (nominal_decl)
                    {
                        auto iter = nominal_decl->getMembers().begin();
                        auto end = nominal_decl->getMembers().end();
                        for (;iter != end;iter++)
                        {
                            auto decl_kind = iter->getKind();
                            switch (decl_kind)
                            {
                                case swift::DeclKind::Constructor:
                                case swift::DeclKind::Destructor:
                                case swift::DeclKind::Func:
                                {
                                    if (idx == 0)
                                    {
                                        swift::AbstractFunctionDecl *abstract_func_decl = llvm::dyn_cast_or_null<swift::AbstractFunctionDecl>(*iter);
                                        if (abstract_func_decl)
                                        {
                                            switch (decl_kind)
                                            {
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
                                                default:    // I know that this can only be one of three kinds since I am here..
                                                {
                                                    swift::FuncDecl *func_decl = llvm::dyn_cast<swift::FuncDecl>(*iter);
                                                    if (func_decl)
                                                    {
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
                                            result_type = CompilerType(GetASTContext(),abstract_func_decl->getType().getPointer());
                                        }
                                    }
                                    else
                                        --idx;
                                }
                                    break;
                                default:
                                    break;
                            }
                        }
                    }
                }
            }
                break;
            default:
                break;
        }
    }

    if (type && the_decl_we_care_about && (kind != eMemberFunctionKindUnknown))
        return TypeMemberFunctionImpl(result_type, CompilerDecl(this, the_decl_we_care_about), name, kind);

    return TypeMemberFunctionImpl();
}

CompilerType
SwiftASTContext::GetLValueReferenceType (void *type)
{
    if (type)
        return CompilerType(GetASTContext(), swift::LValueType::get(GetSwiftType(type)));
    return CompilerType();
}

CompilerType
SwiftASTContext::GetRValueReferenceType (void *type)
{
    return CompilerType();
}

CompilerType
SwiftASTContext::GetNonReferenceType (void* type)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::LValue:
            {
                swift::LValueType *lvalue = swift_can_type->getAs<swift::LValueType>();
                if (lvalue)
                    return CompilerType(GetASTContext(), lvalue->getObjectType().getPointer());
            }
                break;
            default:
                break;
        }
    }
    return CompilerType();
}

CompilerType
SwiftASTContext::GetPointeeType (void* type)
{
    return CompilerType();
}

CompilerType
SwiftASTContext::GetPointerType (void* type)
{
    if (type)
    {
        swift::Type swift_type (::GetSwiftType(type));
        const swift::TypeKind type_kind = swift_type->getKind();
        if (type_kind == swift::TypeKind::BuiltinRawPointer)
            return CompilerType(GetASTContext(), swift_type);
    }
    return CompilerType();
}

CompilerType
SwiftASTContext::GetTypedefedType (void* type)
{
    if (type)
    {
        swift::Type swift_type (::GetSwiftType(type));
        const swift::TypeKind type_kind = swift_type->getKind();
        if (type_kind == swift::TypeKind::NameAlias)
        {
            swift::NameAliasType *name_alias_type = llvm::dyn_cast_or_null<swift::NameAliasType>(swift_type.getPointer());
            if (name_alias_type)
            {
                return CompilerType (GetASTContext(), name_alias_type->getSinglyDesugaredType());
            }
        }
    }
    return CompilerType();
}

CompilerType
SwiftASTContext::GetUnboundType (void* type)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::BoundGenericEnum:
            {
                swift::BoundGenericType *bound_generic_type = swift_can_type->getAs<swift::BoundGenericType>();
                if (!bound_generic_type)
                    break;
                swift::NominalTypeDecl *nominal_type_decl = bound_generic_type->getDecl();
                if (!nominal_type_decl)
                    break;
                return CompilerType(GetASTContext(), nominal_type_decl->getDeclaredType());
            }
                break;
            default:
                break;
        }
    }
    
    return CompilerType(GetASTContext(), GetSwiftType(type));
}

//----------------------------------------------------------------------
// Create related types using the current type's AST
//----------------------------------------------------------------------

CompilerType
SwiftASTContext::GetBasicTypeFromAST (lldb::BasicType basic_type)
{
    return CompilerType();
}

CompilerType
SwiftASTContext::GetIntTypeFromBitSize (size_t bit_size, bool is_signed)
{
    return CompilerType();

}

CompilerType
SwiftASTContext::GetFloatTypeFromBitSize (size_t bit_size)
{
    return CompilerType();
}

//----------------------------------------------------------------------
// Exploring the type
//----------------------------------------------------------------------

const swift::irgen::TypeInfo *
SwiftASTContext::GetSwiftTypeInfo (void* type)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        switch (swift_can_type->getKind())
        {
            case swift::TypeKind::Metatype:
            {
                swift::MetatypeType* metatype_type = swift_can_type->getAs<swift::MetatypeType>();
                if (!metatype_type)
                    return nullptr;
                else
                    return GetSwiftTypeInfo(GetASTContext()->TheRawPointerType.getPointer());
            }
            case swift::TypeKind::ExistentialMetatype:
            {
                swift::ExistentialMetatypeType* metatype_type = swift_can_type->getAs<swift::ExistentialMetatypeType>();
                if (!metatype_type)
                    return nullptr;
                // existential metatypes can't be thin
                return GetSwiftTypeInfo(GetASTContext()->TheRawPointerType.getPointer());
            }
            case swift::TypeKind::Function:
                return GetSwiftTypeInfo(GetASTContext()->TheEmptyTupleType.getPointer());
            default:
                if (swift_can_type->isLegalSILType()) // avoid going the SIL route if it would crash us anyway
                    return &GetIRGenModule().getTypeInfo(swift::SILType::getPrimitiveObjectType(swift_can_type));
                else
                {
                    // if you encounter one of these, print out a message - this is temporary to help us figure out what bases we didn't cover well enough
                    // it should be removed at some point before GM though; and since we don't know what to do here,
                    printf("GetSwiftTypeInfo() on non-legal SIL type not special cased. Name: %s - Kind: %u\n",
                           GetTypeName(type).AsCString("<unknown>"),
                           swift_can_type->getKind());
                    // go for a pointer - it's probably a reasonable assumption in most cases
                    return GetSwiftTypeInfo(GetASTContext()->TheRawPointerType.getPointer());
                }
        }
    }
    return nullptr;
}

const swift::irgen::FixedTypeInfo *
SwiftASTContext::GetSwiftFixedTypeInfo(void* type)
{
    VALID_OR_RETURN(nullptr);
    
    const swift::irgen::TypeInfo *type_info = GetSwiftTypeInfo(type);
    if (type_info)
    {
        if (type_info->isFixedSize())
            return llvm::cast<const swift::irgen::FixedTypeInfo>(type_info);
    }
    return nullptr;
}

static swift::Type
StripRedundantParentheses (swift::Type t)
{
    switch (t->getKind())
    {
        case swift::TypeKind::Paren:
        {
            swift::ParenType *paren_type = (swift::ParenType*)t.getPointer();
            if (!paren_type)
                return swift::Type();
            return StripRedundantParentheses(swift::Type(paren_type->getUnderlyingType()));
        }
            break;
        default:
            return t;
    }
}

CompilerType
SwiftASTContext::StripRedundantParentheses (void* type)
{
    swift::Type swift_type(GetSwiftType(type));
    swift_type = swift_type.transform(::StripRedundantParentheses);
    return CompilerType(GetASTContext(),swift_type.getPointer());
}

uint64_t
SwiftASTContext::GetBitSize (void* type, ExecutionContextScope *exe_scope)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Archetype:
            case swift::TypeKind::LValue:
            case swift::TypeKind::UnboundGeneric:
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::GenericFunction:
            case swift::TypeKind::Function:
                return GetPointerByteSize()*8;
            default:
                break;
        }
        const swift::irgen::FixedTypeInfo *fixed_type_info = GetSwiftFixedTypeInfo(type);
        if (fixed_type_info)
            return fixed_type_info->getFixedSize().getValue() * 8;
    }
    return 0;
}

uint64_t
SwiftASTContext::GetByteStride (void* type)
{
    if (type)
    {
        const swift::irgen::FixedTypeInfo *fixed_type_info = GetSwiftFixedTypeInfo(type);
        if (fixed_type_info)
            return fixed_type_info->getFixedStride().getValue();
    }
    return 0;
}

size_t
SwiftASTContext::GetTypeBitAlign (void* type)
{
    if (type)
    {
        const swift::irgen::FixedTypeInfo *fixed_type_info = GetSwiftFixedTypeInfo(type);
        if (fixed_type_info)
            return fixed_type_info->getFixedAlignment().getValue();
    }
    return 0;
}


lldb::Encoding
SwiftASTContext::GetEncoding (void* type, uint64_t &count)
{
    if (!type)
        return lldb::eEncodingInvalid;
    
    count = 1;
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
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
        case swift::TypeKind::Class:  // Classes are pointers in swift...
        case swift::TypeKind::BoundGenericClass:
            return lldb::eEncodingUint;
            
        case swift::TypeKind::BuiltinVector:
        case swift::TypeKind::NameAlias:
            break;
        case swift::TypeKind::Paren:
            return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetEncoding(count);
        case swift::TypeKind::Tuple:
            break;
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetEncoding(count);
        case swift::TypeKind::Optional:
        case swift::TypeKind::ImplicitlyUnwrappedOptional:
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
            break;
            
        case swift::TypeKind::Enum:
        case swift::TypeKind::BoundGenericEnum:
            return lldb::eEncodingUint;
            
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
            return lldb::eEncodingUint;
            
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::GenericFunction:
        case swift::TypeKind::Function:
            return lldb::eEncodingUint;
            
        case swift::TypeKind::Struct:
        case swift::TypeKind::Dictionary:
        case swift::TypeKind::Protocol:
        case swift::TypeKind::Module:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::ArraySlice:
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
    }
    count = 0;
    return lldb::eEncodingInvalid;
}

lldb::Format
SwiftASTContext::GetFormat (void* type)
{
    if (!type)
        return lldb::eFormatDefault;
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
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
        case swift::TypeKind::NameAlias:
            break;
        case swift::TypeKind::Paren:
            return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetFormat();
        case swift::TypeKind::Tuple:
            break;
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetFormat();
        case swift::TypeKind::Optional:
        case swift::TypeKind::ImplicitlyUnwrappedOptional:
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
            break;
            
        case swift::TypeKind::Enum:
        case swift::TypeKind::BoundGenericEnum:
            return eFormatUnsigned;
            
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::GenericFunction:
        case swift::TypeKind::Function:
            return lldb::eFormatAddressInfo;
            
        case swift::TypeKind::Struct:
        case swift::TypeKind::Dictionary:
        case swift::TypeKind::Protocol:
        case swift::TypeKind::Metatype:
        case swift::TypeKind::Module:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::ArraySlice:
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
    }
    // We don't know hot to display this type...
    return lldb::eFormatBytes;
}

uint32_t
SwiftASTContext::GetNumChildren (void* type, bool omit_empty_base_classes)
{
    if (!type)
        return 0;
    
    uint32_t num_children = 0;

    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::Error:
        case swift::TypeKind::BuiltinInteger:
        case swift::TypeKind::BuiltinFloat:
        case swift::TypeKind::BuiltinRawPointer:
        case swift::TypeKind::BuiltinNativeObject:
        case swift::TypeKind::BuiltinUnknownObject:
        case swift::TypeKind::BuiltinUnsafeValueBuffer:
        case swift::TypeKind::BuiltinBridgeObject:
        case swift::TypeKind::BuiltinVector:
        case swift::TypeKind::NameAlias:
        case swift::TypeKind::Module:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::Function:
        case swift::TypeKind::GenericFunction:
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::DynamicSelf:
        case swift::TypeKind::SILBox:
        case swift::TypeKind::SILFunction:
        case swift::TypeKind::InOut:
            break;
        case swift::TypeKind::Paren:
            return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetNumChildren(omit_empty_base_classes);
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetNumChildren(omit_empty_base_classes);
        case swift::TypeKind::Optional:
        case swift::TypeKind::ImplicitlyUnwrappedOptional:
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
            break;
            
        case swift::TypeKind::Enum:
        case swift::TypeKind::BoundGenericEnum:
        {
            SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
            if (cached_enum_info)
                return cached_enum_info->GetNumElementsWithPayload();
        }
            break;
            
        case swift::TypeKind::ArraySlice:
        {
            swift::ArraySliceType *array_slice_type = llvm::dyn_cast<swift::ArraySliceType>(swift_can_type.getPointer());
            if (array_slice_type)
            {
                CompilerType desugared_type(GetASTContext(), array_slice_type->getDesugaredType());
                return desugared_type.GetNumChildren(omit_empty_base_classes);
            }
        }
            break;
            
        case swift::TypeKind::Dictionary:
        {
            swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
            if (t)
                return CompilerType(GetASTContext(),t->getSinglyDesugaredType()).GetNumChildren(omit_empty_base_classes);
            break;
        }
            
        case swift::TypeKind::Tuple:
        case swift::TypeKind::Struct:
        case swift::TypeKind::Class:
        case swift::TypeKind::BoundGenericClass:
        case swift::TypeKind::BoundGenericStruct:
        case swift::TypeKind::Protocol:
        case swift::TypeKind::ProtocolComposition:
        case swift::TypeKind::Archetype:
        {
            CachedMemberInfo *cached_member_info = GetCachedMemberInfo (type);
            if (cached_member_info)
                return cached_member_info->member_infos.size();
        }
            break;
            
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
            return 0;
            
        case swift::TypeKind::LValue:
        {
            swift::LValueType *lvalue_type = swift_can_type->getAs<swift::LValueType>();
            if (!lvalue_type)
                break;
            swift::TypeBase* deref_type = lvalue_type->getObjectType().getPointer();
            if (!deref_type)
                break;
            
            uint32_t num_pointee_children = CompilerType(GetASTContext(), deref_type).GetNumChildren (omit_empty_base_classes);
            // If this type points to a simple type (or to a class), then it has 1 child
            if (num_pointee_children == 0 || deref_type->getClassOrBoundGenericClass())
                num_children = 1;
            else
                num_children = num_pointee_children;
        }
            break;
            
        case swift::TypeKind::UnboundGeneric:
            break;
        case swift::TypeKind::TypeVariable:
            break;
            
        case swift::TypeKind::SILBlockStorage:
        case swift::TypeKind::Unresolved:
            break;
            
    }

    return num_children;
}

lldb::BasicType
SwiftASTContext::GetBasicTypeEnumeration (void* type)
{
    return eBasicTypeInvalid;
}


#pragma mark Aggregate Types

uint32_t
SwiftASTContext::GetNumDirectBaseClasses (void* opaque_type)
{
    if (!opaque_type)
        return 0;

    swift::CanType swift_can_type (GetCanonicalSwiftType (opaque_type));
    swift::ClassDecl *class_decl = swift_can_type->getClassOrBoundGenericClass();
    if (class_decl)
    {
        if (class_decl->hasSuperclass())
            return 1;
    }
    
    return 0;
}

uint32_t
SwiftASTContext::GetNumVirtualBaseClasses (void* opaque_type)
{
    return 0;
}

uint32_t
SwiftASTContext::GetNumFields (void* type)
{
    if (!type)
        return 0;
    
    uint32_t count = 0;

    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::Error:
        case swift::TypeKind::BuiltinInteger:
        case swift::TypeKind::BuiltinFloat:
        case swift::TypeKind::BuiltinRawPointer:
        case swift::TypeKind::BuiltinNativeObject:
        case swift::TypeKind::BuiltinUnknownObject:
        case swift::TypeKind::BuiltinUnsafeValueBuffer:
        case swift::TypeKind::BuiltinBridgeObject:
        case swift::TypeKind::BuiltinVector:
        case swift::TypeKind::NameAlias:
            break;
        case swift::TypeKind::Paren:
            return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetNumFields();
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetNumFields();
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
        case swift::TypeKind::Optional:
        case swift::TypeKind::ImplicitlyUnwrappedOptional:
            break;
            
        case swift::TypeKind::Enum:
        case swift::TypeKind::BoundGenericEnum:
        {
            SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
            if (cached_enum_info)
                return cached_enum_info->GetNumElementsWithPayload();
        }
            break;
            
        case swift::TypeKind::Dictionary:
        {
            swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
            if (t)
                return CompilerType(GetASTContext(),t->getSinglyDesugaredType()).GetNumFields();
            break;
        }
            
        case swift::TypeKind::Tuple:
        case swift::TypeKind::Struct:
        case swift::TypeKind::Class:
        case swift::TypeKind::Protocol:
        case swift::TypeKind::ProtocolComposition:
        case swift::TypeKind::BoundGenericClass:
        case swift::TypeKind::BoundGenericStruct:
        {
            CachedMemberInfo *cached_member_info = GetCachedMemberInfo (type);
            if (cached_member_info)
            {
                const size_t num_members = cached_member_info->member_infos.size();
                if (num_members > 0 && cached_member_info->member_infos.front().member_type == MemberType::BaseClass)
                    return num_members - 1;
                else
                    return num_members;
            }
        }
            break;
            
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
            return 0;
            
        case swift::TypeKind::Module:
        case swift::TypeKind::Archetype:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::Function:
        case swift::TypeKind::GenericFunction:
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::ArraySlice:
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
    }
    
    return count;
}

CompilerType
SwiftASTContext::GetDirectBaseClassAtIndex (void* opaque_type, size_t idx, uint32_t *bit_offset_ptr)
{
    if (opaque_type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (opaque_type));
        swift::ClassDecl *class_decl = swift_can_type->getClassOrBoundGenericClass();
        if (class_decl)
        {
            swift::Type base_class_type = class_decl->getSuperclass();
            if (base_class_type)
                return CompilerType(GetASTContext(), base_class_type.getPointer());
        }
    }
    return CompilerType();
}

CompilerType
SwiftASTContext::GetVirtualBaseClassAtIndex (void* opaque_type, size_t idx, uint32_t *bit_offset_ptr)
{
    return CompilerType();
}

CompilerType
SwiftASTContext::GetFieldAtIndex (void* type, size_t idx,
                                  std::string& name,
                                  uint64_t *bit_offset_ptr,
                                  uint32_t *bitfield_bit_size_ptr,
                                  bool *is_bitfield_ptr)
{
    if (!type)
        return CompilerType();
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::Error:
        case swift::TypeKind::BuiltinInteger:
        case swift::TypeKind::BuiltinFloat:
        case swift::TypeKind::BuiltinRawPointer:
        case swift::TypeKind::BuiltinNativeObject:
        case swift::TypeKind::BuiltinUnsafeValueBuffer:
        case swift::TypeKind::BuiltinUnknownObject:
        case swift::TypeKind::BuiltinBridgeObject:
        case swift::TypeKind::BuiltinVector:
        case swift::TypeKind::NameAlias:
            break;
        case swift::TypeKind::Paren:
            return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetFieldAtIndex(idx, name, bit_offset_ptr, bitfield_bit_size_ptr, is_bitfield_ptr);
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetFieldAtIndex (idx,
                                                                                                                                                        name,
                                                                                                                                                        bit_offset_ptr,
                                                                                                                                                        bitfield_bit_size_ptr,
                                                                                                                                                        is_bitfield_ptr);
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
        case swift::TypeKind::Optional:
        case swift::TypeKind::ImplicitlyUnwrappedOptional:
            break;
            
        case swift::TypeKind::Enum:
        case swift::TypeKind::BoundGenericEnum:
        {
            SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
            if (cached_enum_info && idx < cached_enum_info->GetNumElementsWithPayload())
            {
                const SwiftEnumDescriptor::ElementInfo *enum_element_info = cached_enum_info->GetElementWithPayloadAtIndex(idx);
                name.assign(enum_element_info->name.GetCString());
                if (bit_offset_ptr)
                    *bit_offset_ptr = 0;
                if (bitfield_bit_size_ptr)
                    *bitfield_bit_size_ptr = 0;
                if (is_bitfield_ptr)
                    *is_bitfield_ptr = false;
                return enum_element_info->payload_type;
            }
        }
            break;
            
        case swift::TypeKind::Dictionary:
        {
            swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
            if (t)
                return CompilerType(GetASTContext(),t->getSinglyDesugaredType()).GetFieldAtIndex(idx, name, bit_offset_ptr, bitfield_bit_size_ptr, is_bitfield_ptr);
            break;
        }
            
        case swift::TypeKind::Tuple:
        case swift::TypeKind::Struct:
        case swift::TypeKind::Class:
        case swift::TypeKind::Protocol:
        case swift::TypeKind::ProtocolComposition:
        case swift::TypeKind::BoundGenericClass:
        case swift::TypeKind::BoundGenericStruct:
        {
            CachedMemberInfo *cached_member_info = GetCachedMemberInfo (type);
            if (cached_member_info)
            {
                const size_t num_members = cached_member_info->member_infos.size();
                uint32_t actual_idx = idx;
                if (num_members > 0 && cached_member_info->member_infos.front().member_type == MemberType::BaseClass)
                    ++actual_idx; // Skip base class since we are looking for fields only
                if (actual_idx < num_members) {
                    if (cached_member_info->member_infos[actual_idx].name)
                        name = cached_member_info->member_infos[actual_idx].name.GetCString();
                    if (bit_offset_ptr)
                        *bit_offset_ptr = cached_member_info->member_infos[actual_idx].byte_offset * 8;
                    if (bitfield_bit_size_ptr)
                        *bitfield_bit_size_ptr = 0;
                    if (is_bitfield_ptr)
                        *is_bitfield_ptr = false;
                    return cached_member_info->member_infos[actual_idx].clang_type;
                }
            }
        }
            break;
            
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
            break;
            
        case swift::TypeKind::Module:
        case swift::TypeKind::Archetype:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::Function:
        case swift::TypeKind::GenericFunction:
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::ArraySlice:
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
    }

    return CompilerType();
}

// If a pointer to a pointee type (the clang_type arg) says that it has no
// children, then we either need to trust it, or override it and return a
// different result. For example, an "int *" has one child that is an integer,
// but a function pointer doesn't have any children. Likewise if a Record type
// claims it has no children, then there really is nothing to show.
uint32_t
SwiftASTContext::GetNumPointeeChildren (void* type)
{
    if (!type)
        return 0;
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::Error:                    return 0;
        case swift::TypeKind::BuiltinInteger:           return 1;
        case swift::TypeKind::BuiltinFloat:             return 1;
        case swift::TypeKind::BuiltinRawPointer:        return 1;
        case swift::TypeKind::BuiltinUnsafeValueBuffer: return 1;
        case swift::TypeKind::BuiltinNativeObject:      return 1;
        case swift::TypeKind::BuiltinUnknownObject:     return 1;
        case swift::TypeKind::BuiltinBridgeObject:      return 1;
        case swift::TypeKind::BuiltinVector:            return 0;
        case swift::TypeKind::NameAlias:                return 0;
            break;
        case swift::TypeKind::Paren:
            return GetNumPointeeChildren(llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer());
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return GetNumPointeeChildren(llvm::cast<swift::ReferenceStorageType>(swift_can_type.getPointer())->getReferentType().getPointer());
        case swift::TypeKind::Tuple:                    return 0;
        case swift::TypeKind::GenericTypeParam:         return 0;
        case swift::TypeKind::AssociatedType:           return 0;
        case swift::TypeKind::DependentMember:          return 0;
        case swift::TypeKind::Optional:                 return 0;
        case swift::TypeKind::ImplicitlyUnwrappedOptional:        return 0;
        case swift::TypeKind::Enum:                     return 0;
        case swift::TypeKind::Struct:                   return 0;
        case swift::TypeKind::Class:                    return 0;
        case swift::TypeKind::Protocol:                 return 0;
        case swift::TypeKind::Metatype:                 return 0;
        case swift::TypeKind::Dictionary:               return 0;
        case swift::TypeKind::Module:                   return 0;
        case swift::TypeKind::Archetype:                return 0;
        case swift::TypeKind::Substituted:              return 0;
        case swift::TypeKind::Function:                 return 0;
        case swift::TypeKind::GenericFunction:          return 0;
        case swift::TypeKind::PolymorphicFunction:      return 0;
        case swift::TypeKind::ArraySlice:               return 0;
        case swift::TypeKind::ProtocolComposition:      return 0;
        case swift::TypeKind::LValue:                   return 1;
        case swift::TypeKind::UnboundGeneric:           return 0;
        case swift::TypeKind::BoundGenericClass:        return 0;
        case swift::TypeKind::BoundGenericEnum:         return 0;
        case swift::TypeKind::BoundGenericStruct:       return 0;
        case swift::TypeKind::TypeVariable:             return 0;
        case swift::TypeKind::ExistentialMetatype:      return 0;
        case swift::TypeKind::DynamicSelf:              return 0;
        case swift::TypeKind::SILBox:                   return 0;
        case swift::TypeKind::SILFunction:              return 0;
        case swift::TypeKind::SILBlockStorage:          return 0;
        case swift::TypeKind::InOut:                    return 0;
        case swift::TypeKind::Unresolved:               return 0;
            
            break;
    }

    return 0;
}

static int64_t
GetInstanceVariableOffset_Symbol (ExecutionContext *exe_ctx,
                                  const CompilerType &type,
                                  const char *ivar_name,
                                  const CompilerType &ivar_type)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TYPES));

    ConstString class_name (type.GetTypeSymbolName());
    Target *target = exe_ctx->GetTargetPtr();
    
    if (log)
        log->Printf("[GetInstanceVariableOffset_Symbol] ivar_name = %s, type = %s class_name = %s", ivar_name, type.GetTypeName().AsCString(), class_name.AsCString());
    
    if (target && class_name && ivar_type.IsValid() && ivar_name)
    {
        swift::NominalTypeDecl *nominal_decl = GetSwiftType(type)->getNominalOrBoundGenericNominal();
        
        if (nominal_decl)
        {
            swift::ValueDecl *the_value_decl = nullptr;
            SwiftASTContext *swift_ast_ctx = llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem());

            auto decls = nominal_decl->lookupDirect(swift::DeclName(swift_ast_ctx->GetASTContext()->getIdentifier(ivar_name)));
            for (auto& decl : decls)
            {
                swift::VarDecl *var_decl = llvm::dyn_cast_or_null<swift::VarDecl>(decl);
                if (var_decl && var_decl->hasStorage())
                {
                    the_value_decl = var_decl;
                    break;
                }
            }
            
            if (the_value_decl)
            {
                std::string buffer;
                llvm::raw_string_ostream stream{buffer};
                swift::Mangle::Mangler mangler{stream};
                mangler.mangleFieldOffsetFull(the_value_decl, false);
                
                StreamString symbol_name;
                symbol_name.Printf("%s",stream.str().c_str());
                ConstString ivar_const_str (symbol_name.GetString().c_str());
                
                lldb::addr_t ivar_offset_ptr = target->FindLoadAddrForNameInSymbolsAndPersistentVariables(ivar_const_str, eSymbolTypeIVarOffset);
                
                if (log)
                    log->Printf("[GetInstanceVariableOffset_Symbol] symbol_name = %s ivar_offset_ptr = 0x%" PRIx64, ivar_const_str.AsCString(), ivar_offset_ptr);
                
                if (ivar_offset_ptr != LLDB_INVALID_ADDRESS)
                {
                    Error error;
                    return target->ReadUnsignedIntegerFromMemory (ivar_offset_ptr,
                                                                  false,    // prefer_file_cache
                                                                  type.GetPointerByteSize(),  // byte size of integer to read
                                                                  LLDB_INVALID_IVAR_OFFSET,
                                                                  error);
                }
            }
            else if (log)
                log->Printf("[GetInstanceVariableOffset_Symbol] no the_value_decl");
        }
        else if (log)
            log->Printf("[GetInstanceVariableOffset_Symbol] no nominal_decl");
    }
    return LLDB_INVALID_IVAR_OFFSET;
}

static int64_t
GetInstanceVariableOffset_Metadata (ExecutionContext *exe_ctx,
                                    const CompilerType &type,
                                    ConstString ivar_name,
                                    const CompilerType &ivar_type)
{
    Log *log(lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_TYPES));
    if (log)
        log->Printf("[GetInstanceVariableOffset_Metadata] ivar_name = %s, type = %s", ivar_name.AsCString(), type.GetTypeName().AsCString());

    Process *process = exe_ctx->GetProcessPtr();
    if (process)
    {
        SwiftLanguageRuntime *runtime = process->GetSwiftLanguageRuntime();
        if (runtime)
        {
            auto metadata_sp = runtime->GetMetadataForType(type);
            SwiftLanguageRuntime::FieldContainerTypeMetadata *fields_metadata =
            llvm::dyn_cast_or_null<SwiftLanguageRuntime::FieldContainerTypeMetadata>(metadata_sp.get());
            if (fields_metadata)
            {
                if(log)
                    log->Printf("[GetInstanceVariableOffset_Metadata] fields_metadata = %p - name = %s", fields_metadata, fields_metadata->GetMangledName().c_str());

                for (size_t idx = 0;
                     idx < fields_metadata->GetNumFields();
                     idx++)
                {
                    auto field = fields_metadata->GetFieldAtIndex(idx);
                    ConstString field_name = field.GetName();
                    if (log)
                        log->Printf("[GetInstanceVariableOffset_Metadata] field_name = %s - field_offset = 0x%" PRIx64, field_name.GetCString(), field.GetOffset());

                    if (field_name && field_name == ivar_name)
                        return field.GetOffset();
                }
            }
            else if (log)
                log->Printf("[GetInstanceVariableOffset_Metadata] no fields_metadata");
        }
        else if (log)
            log->Printf("[GetInstanceVariableOffset_Metadata] no runtime");
    }
    else if (log)
        log->Printf("[GetInstanceVariableOffset_Metadata] no process");
    return LLDB_INVALID_IVAR_OFFSET;
}

static int64_t
GetInstanceVariableOffset (ExecutionContext *exe_ctx,
                           const CompilerType &class_type,
                           const char *ivar_name,
                           const CompilerType &ivar_type)
{
    int64_t offset = LLDB_INVALID_IVAR_OFFSET;
    
    if (ivar_name  && ivar_name[0])
    {
        if (exe_ctx)
        {
            Target *target = exe_ctx->GetTargetPtr();
            if (target)
            {
                // given a type there are three cases:
                //   non generic type - field offset symbols are emitted
                //   generic type:
                //     iVar offsets depend on the type arguments - no field offsets emitted
                //     iVar offsets do not depend on the type arguments - field offsets emitted for the *unbound* type
                
                bool is_generic = (class_type.GetTypeInfo() & eTypeIsGeneric);
                
                bool try_symbol = false;
                bool try_metadata = true;
                
                if (!is_generic)
                    try_symbol = true;
                
                if (try_symbol)
                {
                    offset = GetInstanceVariableOffset_Symbol(exe_ctx, class_type, ivar_name, ivar_type);
                    if (offset != LLDB_INVALID_IVAR_OFFSET)
                        return offset;
                }
                
                if (try_metadata)
                {
                    offset = GetInstanceVariableOffset_Symbol(exe_ctx, class_type, ivar_name, ivar_type);
                    if (offset == LLDB_INVALID_IVAR_OFFSET)
                        offset = GetInstanceVariableOffset_Metadata(exe_ctx, class_type, ConstString(ivar_name), ivar_type);
                }
            }
        }
    }
    return offset;
}

CompilerType
SwiftASTContext::GetChildCompilerTypeAtIndex (void* type,
                                              ExecutionContext *exe_ctx,
                                              size_t idx,
                                              bool transparent_pointers,
                                              bool omit_empty_base_classes,
                                              bool ignore_array_bounds,
                                              std::string& child_name,
                                              uint32_t &child_byte_size,
                                              int32_t &child_byte_offset,
                                              uint32_t &child_bitfield_bit_size,
                                              uint32_t &child_bitfield_bit_offset,
                                              bool &child_is_base_class,
                                              bool &child_is_deref_of_parent,
                                              ValueObject *valobj,
                                              uint64_t &language_flags)
{
    if (!type)
        return CompilerType();
    
    language_flags = 0;
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::Error:
        case swift::TypeKind::BuiltinInteger:
        case swift::TypeKind::BuiltinFloat:
        case swift::TypeKind::BuiltinRawPointer:
        case swift::TypeKind::BuiltinNativeObject:
        case swift::TypeKind::BuiltinUnknownObject:
        case swift::TypeKind::BuiltinUnsafeValueBuffer:
        case swift::TypeKind::BuiltinBridgeObject:
        case swift::TypeKind::BuiltinVector:
        case swift::TypeKind::NameAlias:
            break;
        case swift::TypeKind::Paren:
            return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetChildCompilerTypeAtIndex (exe_ctx,
                                                                                                                                                                          idx,
                                                                                                                                                                          transparent_pointers,
                                                                                                                                                                          omit_empty_base_classes,
                                                                                                                                                                          ignore_array_bounds,
                                                                                                                                                                          child_name,
                                                                                                                                                                          child_byte_size,
                                                                                                                                                                          child_byte_offset,
                                                                                                                                                                          child_bitfield_bit_size,
                                                                                                                                                                          child_bitfield_bit_offset,
                                                                                                                                                                          child_is_base_class,
                                                                                                                                                                          child_is_deref_of_parent,
                                                                                                                                                                          valobj,
                                                                                                                                                                          language_flags);
            //            case swift::TypeKind::Tuple:
            //                {
            //                    swift::TupleType *tuple_type = swift_can_type->getAs<swift::TupleType>();
            //                    if (tuple_type && idx < tuple_type->getNumElements())
            //                    {
            //                        child_byte_offset = 0; // TODO: figure out how to get byte offset of tuple field...
            //                        child_bitfield_bit_size = 0;
            //                        child_bitfield_bit_offset = 0;
            //                        child_is_base_class = false;
            //                        child_is_deref_of_parent = false;
            //
            //                        uint32_t tuple_idx = 0;
            //                        for (auto tuple_field : tuple_type->getFields())
            //                        {
            //                            CompilerType tuple_field_type(m_swift_ast, tuple_field.getType().getPointer());
            //                            auto tuple_field_byte_size = tuple_field_type.GetByteSize();
            //
            //                            if (tuple_idx == idx)
            //                            {
            //                                child_byte_size = tuple_field_byte_size;
            //                                const char *tuple_name = tuple_field.getName().get();
            //                                if (tuple_name)
            //                                {
            //                                    child_name = tuple_name;
            //                                }
            //                                else
            //                                {
            //                                    StreamString tuple_name_strm;
            //                                    tuple_name_strm.Printf("%u", (uint32_t)idx);
            //                                    child_name = std::move(tuple_name_strm.GetString());
            //                                }
            //                                return tuple_field_type;
            //                            }
            //                            else
            //                            {
            //                                const uint64_t tuple_field_bit_size = tuple_field_byte_size * 8;
            //                                const uint64_t tuple_aligned_bit_size = tuple_field_type.GetAlignedBitSize();
            //                                child_byte_offset += llvm::RoundUpToAlignment(tuple_field_bit_size, tuple_aligned_bit_size) / 8;
            //                                ++tuple_idx;
            //                            }
            //                        }
            //                    }
            //                }
            //                break;
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetChildCompilerTypeAtIndex (exe_ctx,
                                                                                                                                                                    idx,
                                                                                                                                                                    transparent_pointers,
                                                                                                                                                                    omit_empty_base_classes,
                                                                                                                                                                    ignore_array_bounds,
                                                                                                                                                                    child_name,
                                                                                                                                                                    child_byte_size,
                                                                                                                                                                    child_byte_offset,
                                                                                                                                                                    child_bitfield_bit_size,
                                                                                                                                                                    child_bitfield_bit_offset,
                                                                                                                                                                    child_is_base_class,
                                                                                                                                                                    child_is_deref_of_parent,
                                                                                                                                                                    valobj,
                                                                                                                                                                    language_flags);
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
        case swift::TypeKind::Optional:
        case swift::TypeKind::ImplicitlyUnwrappedOptional:
            break;
            
        case swift::TypeKind::Enum:
        case swift::TypeKind::BoundGenericEnum:
        {
            SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
            if (cached_enum_info && idx < cached_enum_info->GetNumElementsWithPayload())
            {
                const SwiftEnumDescriptor::ElementInfo *element_info = cached_enum_info->GetElementWithPayloadAtIndex(idx);
                child_name.assign(element_info->name.GetCString());
                child_byte_size = element_info->payload_type.GetByteSize(exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
                child_byte_offset = 0;
                child_bitfield_bit_size = 0;
                child_bitfield_bit_offset = 0;
                child_is_base_class = false;
                child_is_deref_of_parent = false;
                if (element_info->is_indirect)
                {
                    language_flags |= LanguageFlags::eIsIndirectEnumCase;
                    return CompilerType(GetASTContext(), GetASTContext()->TheRawPointerType.getPointer());
                }
                else
                    return element_info->payload_type;
            }
        }
            break;
            
        case swift::TypeKind::Dictionary:
        {
            swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
            
            if (t)
                return CompilerType(GetASTContext(),t->getSinglyDesugaredType()).GetChildCompilerTypeAtIndex(exe_ctx,
                                                                                                             idx,
                                                                                                             transparent_pointers,
                                                                                                             omit_empty_base_classes,
                                                                                                             ignore_array_bounds,
                                                                                                             child_name,
                                                                                                             child_byte_size,
                                                                                                             child_byte_offset,
                                                                                                             child_bitfield_bit_size,
                                                                                                             child_bitfield_bit_offset,
                                                                                                             child_is_base_class,
                                                                                                             child_is_deref_of_parent,
                                                                                                             valobj,
                                                                                                             language_flags);
            break;
        }
            
        case swift::TypeKind::Tuple:
        case swift::TypeKind::Struct:
        case swift::TypeKind::Class:
        case swift::TypeKind::BoundGenericClass:
        case swift::TypeKind::BoundGenericStruct:
        case swift::TypeKind::Protocol:
        case swift::TypeKind::ProtocolComposition:
        {
            CachedMemberInfo *cached_member_info = GetCachedMemberInfo (type);
            if (cached_member_info)
            {
                const size_t num_members = cached_member_info->member_infos.size();
                if (idx < num_members) {
                    if (cached_member_info->member_infos[idx].name)
                        child_name = cached_member_info->member_infos[idx].name.GetCString();
                    else
                        child_name.clear();
                    child_byte_size = cached_member_info->member_infos[idx].byte_size;
                    // Check for fragile ivar offsets and look them up and cache them.
                    if (cached_member_info->member_infos[idx].is_fragile && cached_member_info->member_infos[idx].byte_offset == 0)
                    {
                        CompilerType compiler_type(GetASTContext(), GetSwiftType(type));
                        const int64_t fragile_ivar_offset = GetInstanceVariableOffset (exe_ctx, compiler_type, child_name.c_str(), cached_member_info->member_infos[idx].clang_type);
                        if (fragile_ivar_offset != LLDB_INVALID_IVAR_OFFSET)
                            cached_member_info->member_infos[idx].byte_offset = fragile_ivar_offset;
                    }
                    child_byte_offset = cached_member_info->member_infos[idx].byte_offset;
                    child_bitfield_bit_size = 0;
                    child_bitfield_bit_offset = 0;
                    if ((child_is_base_class = cached_member_info->member_infos[idx].member_type == MemberType::BaseClass))
                    {
                        language_flags |= LanguageFlags::eIgnoreInstancePointerness;
                    }
                    child_is_deref_of_parent = false;
                    return cached_member_info->member_infos[idx].clang_type;
                }
            }
        }
            break;
            
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
            break;
            
        case swift::TypeKind::Module:
        case swift::TypeKind::Archetype:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::Function:
        case swift::TypeKind::GenericFunction:
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::ArraySlice:
            break;
        case swift::TypeKind::LValue:
            if (idx < GetNumChildren(type, omit_empty_base_classes))
            {
                CompilerType pointee_clang_type (GetNonReferenceType(type));
                Flags pointee_clang_type_flags(pointee_clang_type.GetTypeInfo());
                const char *parent_name = valobj ? valobj->GetName().GetCString() : NULL;
                if (parent_name)
                {
                    child_name.assign(1, '&');
                    child_name += parent_name;
                }
                
                // We have a pointer to an simple type
                if (idx == 0)
                {
                    child_byte_size = pointee_clang_type.GetByteSize(exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
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
// If we have a clang type that describes "class C", and we wanted to looked
// "m_b" in it:
//
// With omit_empty_base_classes == false we would get an integer array back with:
// { 1,  1 }
// The first index 1 is the child index for "class A" within class C
// The second index 1 is the child index for "m_b" within class A
//
// With omit_empty_base_classes == true we would get an integer array back with:
// { 0,  1 }
// The first index 0 is the child index for "class A" within class C (since class B doesn't have any members it doesn't count)
// The second index 1 is the child index for "m_b" within class A

size_t
SwiftASTContext::GetIndexOfChildMemberWithName (void* type, const char *name,
                                                bool omit_empty_base_classes,
                                                std::vector<uint32_t>& child_indexes)
{
    if (type && name && name[0])
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Error:
            case swift::TypeKind::BuiltinInteger:
            case swift::TypeKind::BuiltinFloat:
            case swift::TypeKind::BuiltinRawPointer:
            case swift::TypeKind::BuiltinNativeObject:
            case swift::TypeKind::BuiltinUnknownObject:
            case swift::TypeKind::BuiltinUnsafeValueBuffer:
            case swift::TypeKind::BuiltinBridgeObject:
            case swift::TypeKind::BuiltinVector:
            case swift::TypeKind::NameAlias:
                break;
            case swift::TypeKind::Paren:
                return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetIndexOfChildMemberWithName (name,
                                                                                                                                                                            omit_empty_base_classes,
                                                                                                                                                                            child_indexes);
            case swift::TypeKind::UnmanagedStorage:
            case swift::TypeKind::UnownedStorage:
            case swift::TypeKind::WeakStorage:
                return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetIndexOfChildMemberWithName (name,
                                                                                                                                                                          omit_empty_base_classes,
                                                                                                                                                                          child_indexes);
            case swift::TypeKind::GenericTypeParam:
            case swift::TypeKind::AssociatedType:
            case swift::TypeKind::DependentMember:
            case swift::TypeKind::Optional:
            case swift::TypeKind::ImplicitlyUnwrappedOptional:
                break;
                
                
            case swift::TypeKind::Enum:
            case swift::TypeKind::BoundGenericEnum:
            {
                SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
                if (cached_enum_info)
                {
                    ConstString const_name(name);
                    const size_t num_sized_elements = cached_enum_info->GetNumElementsWithPayload();
                    for (size_t i=0; i<num_sized_elements; ++i)
                    {
                        if (cached_enum_info->GetElementWithPayloadAtIndex(i)->name == const_name)
                        {
                            child_indexes.push_back(i);
                            return child_indexes.size();
                        }
                    }
                }
            }
                break;
                
            case swift::TypeKind::Dictionary:
            {
                swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
                
                if (t)
                    return CompilerType(GetASTContext(),t->getSinglyDesugaredType()).GetIndexOfChildMemberWithName(name, omit_empty_base_classes, child_indexes);
                break;
            }
                
            case swift::TypeKind::Tuple:
            {
                // For tuples only always look for the member by number first as a tuple
                // element can be named, yet still be accessed by the number...
                swift::TupleType *tuple_type = swift_can_type->getAs<swift::TupleType>();
                if (tuple_type)
                {
                    uint32_t tuple_idx = StringConvert::ToUInt32(name, UINT32_MAX);
                    if (tuple_idx != UINT32_MAX)
                    {
                        if (tuple_idx < tuple_type->getNumElements())
                        {
                            child_indexes.push_back(tuple_idx);
                            return child_indexes.size();
                        }
                        else
                            return 0;
                    }
                }
            }
                // Fall through to class/union/struct case...
            case swift::TypeKind::Struct:
            case swift::TypeKind::Class:
            case swift::TypeKind::Protocol:
            case swift::TypeKind::ProtocolComposition:
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericStruct:
            {
                CachedMemberInfo *cached_member_info = GetCachedMemberInfo (type);
                if (cached_member_info)
                {
                    ConstString const_name(name);
                    const size_t num_members = cached_member_info->member_infos.size();
                    if (num_members > 0)
                    {
                        for (size_t i=0; i<num_members; ++i)
                        {
                            const MemberInfo &member_info = cached_member_info->member_infos[i];
                            if (member_info.name && member_info.member_type != MemberType::BaseClass)
                            {
                                if (const_name == member_info.name)
                                {
                                    child_indexes.push_back (i);
                                    return child_indexes.size();
                                }
                            }
                        }
                        // Check the base class if we have one...
                        if (cached_member_info->member_infos[0].member_type == MemberType::BaseClass)
                        {
                            // Push index zero for the base class
                            child_indexes.push_back (0);
                            
                            if (cached_member_info->member_infos[0].clang_type.GetIndexOfChildMemberWithName (name,
                                                                                                              omit_empty_base_classes,
                                                                                                              child_indexes))
                            {
                                // We did find an ivar in a superclass so just
                                // return the results!
                                return child_indexes.size();
                            }
                            // We didn't find an ivar matching "name" in our
                            // superclass, pop the superclass zero index that
                            // we pushed on above.
                            child_indexes.pop_back();
                        }
                    }
                }
            }
                break;
                
            case swift::TypeKind::ExistentialMetatype:
            case swift::TypeKind::Metatype:
                break;
                
            case swift::TypeKind::Module:
            case swift::TypeKind::Archetype:
            case swift::TypeKind::Substituted:
            case swift::TypeKind::Function:
            case swift::TypeKind::GenericFunction:
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::ArraySlice:
                break;
            case swift::TypeKind::LValue:
            {
                CompilerType pointee_clang_type (GetNonReferenceType(type));
                
                if (pointee_clang_type.IsAggregateType ())
                {
                    return pointee_clang_type.GetIndexOfChildMemberWithName (name,
                                                                             omit_empty_base_classes,
                                                                             child_indexes);
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
        }
    }
    return 0;
}

// Get the index of the child of "clang_type" whose name matches. This function
// doesn't descend into the children, but only looks one level deep and name
// matches can include base class names.

uint32_t
SwiftASTContext::GetIndexOfChildWithName (void* type, const char *name, bool omit_empty_base_classes)
{
    if (type && name && name[0])
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Error:
            case swift::TypeKind::BuiltinInteger:
            case swift::TypeKind::BuiltinFloat:
            case swift::TypeKind::BuiltinRawPointer:
            case swift::TypeKind::BuiltinNativeObject:
            case swift::TypeKind::BuiltinUnsafeValueBuffer:
            case swift::TypeKind::BuiltinUnknownObject:
            case swift::TypeKind::BuiltinBridgeObject:
            case swift::TypeKind::BuiltinVector:
            case swift::TypeKind::NameAlias:
                break;
            case swift::TypeKind::Paren:
                return CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).GetIndexOfChildWithName (name, omit_empty_base_classes);
            case swift::TypeKind::UnmanagedStorage:
            case swift::TypeKind::UnownedStorage:
            case swift::TypeKind::WeakStorage:
                return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).GetIndexOfChildWithName (name, omit_empty_base_classes);
            case swift::TypeKind::GenericTypeParam:
            case swift::TypeKind::AssociatedType:
            case swift::TypeKind::DependentMember:
            case swift::TypeKind::Optional:
            case swift::TypeKind::ImplicitlyUnwrappedOptional:
                break;
                
            case swift::TypeKind::Enum:
            case swift::TypeKind::BoundGenericEnum:
            {
                SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
                if (cached_enum_info)
                {
                    ConstString const_name(name);
                    const size_t num_sized_elements = cached_enum_info->GetNumElementsWithPayload();
                    for (size_t i=0; i<num_sized_elements; ++i)
                    {
                        if (cached_enum_info->GetElementWithPayloadAtIndex(i)->name == const_name)
                            return i;
                    }
                }
            }
                break;
                
            case swift::TypeKind::Dictionary:
            {
                swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
                
                if (t)
                    return CompilerType(GetASTContext(),t->getSinglyDesugaredType()).GetIndexOfChildWithName(name, omit_empty_base_classes);
                break;
            }
                
            case swift::TypeKind::Tuple:
            {
                swift::TupleType *tuple_type = swift_can_type->getAs<swift::TupleType>();
                if (tuple_type)
                {
                    uint32_t tuple_idx = StringConvert::ToUInt32(name, UINT32_MAX);
                    if (tuple_idx != UINT32_MAX)
                    {
                        if (tuple_idx < tuple_type->getNumElements())
                            return tuple_idx;
                    }
                }
            }
                // Fall through to struct/union/class case...
            case swift::TypeKind::Struct:
            case swift::TypeKind::Class:
            case swift::TypeKind::Protocol:
            case swift::TypeKind::ProtocolComposition:
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericStruct:
            {
                CachedMemberInfo *cached_member_info = GetCachedMemberInfo (type);
                if (cached_member_info)
                {
                    ConstString const_name(name);
                    const size_t num_members = cached_member_info->member_infos.size();
                    if (num_members > 0)
                    {
                        for (size_t i=0; i<num_members; ++i)
                        {
                            const MemberInfo &member_info = cached_member_info->member_infos[i];
                            if (member_info.name && member_info.member_type != MemberType::BaseClass)
                            {
                                if (const_name == member_info.name)
                                    return i;
                            }
                        }
                        // Check the base class name if  have one...
                        if (cached_member_info->member_infos[0].member_type == MemberType::BaseClass &&
                            cached_member_info->member_infos[0].name == const_name)
                        {
                            return 0;
                        }
                    }
                }
            }
                
                break;
                
            case swift::TypeKind::ExistentialMetatype:
            case swift::TypeKind::Metatype:
                break;
                
            case swift::TypeKind::Module:
            case swift::TypeKind::Archetype:
            case swift::TypeKind::Substituted:
            case swift::TypeKind::Function:
            case swift::TypeKind::GenericFunction:
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::ArraySlice:
                break;
            case swift::TypeKind::LValue:
            {
                CompilerType pointee_type (GetNonReferenceType(type));
                
                if (pointee_type.IsAggregateType ())
                {
                    return pointee_type.GetIndexOfChildWithName (name, omit_empty_base_classes);
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
        }
    }
    return UINT32_MAX;
}


size_t
SwiftASTContext::GetNumTemplateArguments (void* type)
{
    if (!type)
        return 0;
    
    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::UnboundGeneric:
        {
            swift::UnboundGenericType *unbound_generic_type = swift_can_type->getAs<swift::UnboundGenericType>();
            if (!unbound_generic_type)
                break;
            swift::NominalTypeDecl *nominal_type_decl = unbound_generic_type->getDecl();
            if (!nominal_type_decl)
                break;
            swift::GenericParamList *generic_param_list = nominal_type_decl->getGenericParams();
            if (!generic_param_list)
                break;
            return generic_param_list->getParams().size();
        }
            break;
        case swift::TypeKind::BoundGenericClass:
        case swift::TypeKind::BoundGenericStruct:
        case swift::TypeKind::BoundGenericEnum:
        {
            swift::BoundGenericType *bound_generic_type = swift_can_type->getAs<swift::BoundGenericType>();
            if (!bound_generic_type)
                break;
            return bound_generic_type->getGenericArgs().size();
        }
        case swift::TypeKind::PolymorphicFunction:
        {
            swift::PolymorphicFunctionType *polymorhpic_func_type = swift_can_type->getAs<swift::PolymorphicFunctionType>();
            if (!polymorhpic_func_type)
                break;
            return polymorhpic_func_type->getGenericParameters().size();
        }
            break;
        default:
            break;
    }
    
    return 0;
}

bool
SwiftASTContext::GetEnumTypeInfo (const CompilerType& type,
                                  uint32_t& num_payload_cases,
                                  uint32_t& num_nopayload_cases)
{
    if (llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::Enum:
            {
                swift::EnumType *enum_type = swift_can_type->getAs<swift::EnumType>();
                if (!enum_type)
                    break;
                swift::EnumDecl *enum_decl = enum_type->getDecl();
                if (!enum_decl)
                    break;
                auto range = enum_decl->getAllElements();
                auto iter = range.begin(), end = range.end();
                for (;iter != end;++iter)
                {
                    swift::EnumElementDecl *element_decl = *iter;
                    if (element_decl->hasArgumentType())
                        num_payload_cases++;
                    else
                        num_nopayload_cases++;
                }
                return true;
            }
            case swift::TypeKind::BoundGenericEnum:
            {
                swift::BoundGenericEnumType *bound_generic_enum_type = swift_can_type->getAs<swift::BoundGenericEnumType>();
                if (!bound_generic_enum_type)
                    break;
                swift::EnumDecl *enum_decl = bound_generic_enum_type->getDecl();
                if (!enum_decl)
                    break;
                auto range = enum_decl->getAllElements();
                auto iter = range.begin(), end = range.end();
                for (;iter != end;++iter)
                {
                    swift::EnumElementDecl *element_decl = *iter;
                    if (element_decl->hasArgumentType())
                        num_payload_cases++;
                    else
                        num_nopayload_cases++;
                }
                return true;
            }
            default:
                break;
        }
    }
    
    return false;
}

bool
SwiftASTContext::GetSelectedEnumCase (const CompilerType& type,
                                      const DataExtractor& data,
                                      ConstString *name,
                                      bool *has_payload,
                                      CompilerType* payload,
                                      bool *is_indirect)
{
    if (auto ast = llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem()))
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            default: break;
            case swift::TypeKind::Enum:
            case swift::TypeKind::BoundGenericEnum:
            {
                SwiftEnumDescriptor *cached_enum_info = ast->GetCachedEnumInfo (swift_can_type.getPointer());
                if (cached_enum_info)
                {
                    auto enum_elem_info = cached_enum_info->GetElementFromData(data);
                    if (enum_elem_info)
                    {
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
            }
                break;
        }
    }
    
    return false;
}

CompilerType
SwiftASTContext::GetTemplateArgument (void* type,
                                      size_t arg_idx,
                                      lldb::TemplateArgumentKind &kind)
{
    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        
        const swift::TypeKind type_kind = swift_can_type->getKind();
        switch (type_kind)
        {
            case swift::TypeKind::UnboundGeneric:
            {
                swift::UnboundGenericType *unbound_generic_type = swift_can_type->getAs<swift::UnboundGenericType>();
                if (!unbound_generic_type)
                    break;
                swift::NominalTypeDecl *nominal_type_decl = unbound_generic_type->getDecl();
                if (!nominal_type_decl)
                    break;
                swift::GenericParamList *generic_param_list = nominal_type_decl->getGenericParams();
                if (!generic_param_list)
                    break;
                if (arg_idx >= generic_param_list->getAllArchetypes().size())
                    break;
                kind = eTemplateArgumentKindType;
                return CompilerType(GetASTContext(),generic_param_list->getAllArchetypes()[arg_idx]);
            }
                break;
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericStruct:
            case swift::TypeKind::BoundGenericEnum:
            {
                swift::BoundGenericType *bound_generic_type = swift_can_type->getAs<swift::BoundGenericType>();
                if (!bound_generic_type)
                    break;
                const llvm::ArrayRef<swift::Substitution>& substitutions = bound_generic_type->getSubstitutions(nullptr,nullptr);
                if (arg_idx >= substitutions.size())
                    break;
                kind = eTemplateArgumentKindType;
                const swift::Substitution& substitution = substitutions[arg_idx];
                return CompilerType(GetASTContext(),substitution.getReplacement().getPointer());
            }
            case swift::TypeKind::PolymorphicFunction:
            {
                swift::PolymorphicFunctionType *polymorhpic_func_type = swift_can_type->getAs<swift::PolymorphicFunctionType>();
                if (!polymorhpic_func_type)
                    break;
                if (arg_idx >= polymorhpic_func_type->getGenericParameters().size())
                    break;
                kind = eTemplateArgumentKindType;
                return CompilerType(GetASTContext(),polymorhpic_func_type->getGenericParameters()[arg_idx]->getArchetype());
            }
                break;
            default:
                break;
        }
    }
    
    kind = eTemplateArgumentKindNull;
    return CompilerType ();
}

CompilerType
SwiftASTContext::GetTypeForFormatters (void* type)
{
    if (type)
        return StripRedundantParentheses(type);
    return CompilerType();
}

LazyBool
SwiftASTContext::ShouldPrintAsOneLiner (void* type, ValueObject* valobj)
{
    if (type)
    {
        CompilerType can_compiler_type(GetCanonicalType(type));
        if (IsImportedType(can_compiler_type, nullptr))
            return eLazyBoolNo;
    }
    if (valobj)
    {
        if (valobj->IsBaseClass())
            return eLazyBoolNo;
        if ((valobj->GetLanguageFlags() & LanguageFlags::eIsIndirectEnumCase) == LanguageFlags::eIsIndirectEnumCase)
            return eLazyBoolNo;
    }
    
    return eLazyBoolCalculate;
}

bool
SwiftASTContext::IsMeaninglessWithoutDynamicResolution (void* type)
{
    if (type)
    {
            swift::CanType swift_can_type (GetCanonicalSwiftType (type));
            
            const swift::TypeKind type_kind = swift_can_type->getKind();
            switch (type_kind)
            {
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

void
SwiftASTContext::DumpValue (void* type, ExecutionContext *exe_ctx,
                            Stream *s,
                            lldb::Format format,
                            const lldb_private::DataExtractor &data,
                            lldb::offset_t data_byte_offset,
                            size_t data_byte_size,
                            uint32_t bitfield_bit_size,
                            uint32_t bitfield_bit_offset,
                            bool show_types,
                            bool show_summary,
                            bool verbose,
                            uint32_t depth)
{
}




bool
SwiftASTContext::DumpTypeValue (void* type, Stream *s,
                                lldb::Format format,
                                const lldb_private::DataExtractor &data,
                                lldb::offset_t byte_offset,
                                size_t byte_size,
                                uint32_t bitfield_bit_size,
                                uint32_t bitfield_bit_offset,
                                ExecutionContextScope *exe_scope,
                                bool is_base_class)
{
    if (!type)
        return false;

    swift::CanType swift_can_type (GetCanonicalSwiftType (type));
    
    const swift::TypeKind type_kind = swift_can_type->getKind();
    switch (type_kind)
    {
        case swift::TypeKind::Error:
            break;
            
        case swift::TypeKind::Class:
        case swift::TypeKind::BoundGenericClass:
            // If we have a class that is in a variable then it is a pointer,
            // else if it is a base class, it has no value
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
        case swift::TypeKind::PolymorphicFunction:
        case swift::TypeKind::LValue:
        {
            uint32_t item_count = 1;
            // A few formats, we might need to modify our size and count for depending
            // on how we are trying to display the value...
            switch (format)
            {
                default:
                case eFormatBoolean:
                case eFormatBinary:
                case eFormatComplex:
                case eFormatCString:         // NULL terminated C strings
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
                    if (byte_size == 0)
                    {
                        byte_size = exe_scope->CalculateTarget()->GetArchitecture().GetAddressByteSize();
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
            return data.Dump (s,
                              byte_offset,
                              format,
                              byte_size,
                              item_count,
                              UINT32_MAX,
                              LLDB_INVALID_ADDRESS,
                              bitfield_bit_size,
                              bitfield_bit_offset,
                              exe_scope);
        }
            break;
        case swift::TypeKind::BuiltinVector:
        case swift::TypeKind::NameAlias:
            break;
        case swift::TypeKind::Paren:
            s->PutChar('(');
            CompilerType(GetASTContext(), llvm::cast<swift::ParenType>(swift_can_type.getPointer())->getCanonicalType().getPointer()).DumpTypeValue (s,
                                                                                                                                                 format,
                                                                                                                                                 data,
                                                                                                                                                 byte_offset,
                                                                                                                                                 byte_size,
                                                                                                                                                 bitfield_bit_size,
                                                                                                                                                 bitfield_bit_offset,
                                                                                                                                                 exe_scope,
                                                                                                                                                 is_base_class);
            s->PutChar(')');
            return true;
            
        case swift::TypeKind::Tuple:
            break;
            
        case swift::TypeKind::Dictionary:
        {
            swift::DictionaryType *t = llvm::dyn_cast<swift::DictionaryType>(swift_can_type.getPointer());
            
            if (t)
                return CompilerType(GetASTContext(),t->getSinglyDesugaredType()).DumpTypeValue (s,
                                                                                            format,
                                                                                            data,
                                                                                            byte_offset,
                                                                                            byte_size,
                                                                                            bitfield_bit_size,
                                                                                            bitfield_bit_offset,
                                                                                            exe_scope,
                                                                                            is_base_class);
            break;
        }
            
        case swift::TypeKind::UnmanagedStorage:
        case swift::TypeKind::UnownedStorage:
        case swift::TypeKind::WeakStorage:
            return CompilerType(GetASTContext(), swift_can_type->getAs<swift::ReferenceStorageType>()->getReferentType().getPointer()).DumpTypeValue (s,
                                                                                                                                                      format,
                                                                                                                                                      data,
                                                                                                                                                      byte_offset,
                                                                                                                                                      byte_size,
                                                                                                                                                      bitfield_bit_size,
                                                                                                                                                      bitfield_bit_offset,
                                                                                                                                                      exe_scope,
                                                                                                                                                      is_base_class);
        case swift::TypeKind::Enum:
        case swift::TypeKind::BoundGenericEnum:
        {
            SwiftEnumDescriptor *cached_enum_info = GetCachedEnumInfo (type);
            if (cached_enum_info)
            {
                auto enum_elem_info = cached_enum_info->GetElementFromData(data);
                if (enum_elem_info)
                    s->Printf("%s",enum_elem_info->name.GetCString());
                else
                {
                    lldb::offset_t ptr = 0;
                    if (data.GetByteSize())
                        s->Printf("<invalid> (0x%" PRIx8 ")", data.GetU8(&ptr));
                    else
                        s->Printf("<empty>");
                }
                return true;
            }
            else
                s->Printf("<unknown type>");
        }
            break;
            
            
        case swift::TypeKind::Optional:
        case swift::TypeKind::ImplicitlyUnwrappedOptional:
        case swift::TypeKind::Struct:
        case swift::TypeKind::Protocol:
        case swift::TypeKind::GenericTypeParam:
        case swift::TypeKind::AssociatedType:
        case swift::TypeKind::DependentMember:
            return false;
            
        case swift::TypeKind::ExistentialMetatype:
        case swift::TypeKind::Metatype:
        {
            return data.Dump (s,
                              byte_offset,
                              eFormatPointer,
                              byte_size,
                              1,
                              UINT32_MAX,
                              LLDB_INVALID_ADDRESS,
                              bitfield_bit_size,
                              bitfield_bit_offset,
                              exe_scope);
        }
            break;
            
        case swift::TypeKind::Module:
        case swift::TypeKind::Substituted:
        case swift::TypeKind::ArraySlice:
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
    }
    
    return 0;
}

bool
SwiftASTContext::IsImportedType (const CompilerType& type,
                                 CompilerType* original_type)
{
    bool success = false;
    
    if (llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem()))
    {
        do
        {
            swift::CanType swift_can_type (GetCanonicalSwiftType (type));
            swift::NominalType *nominal_type = swift_can_type->getAs<swift::NominalType>();
            if (!nominal_type)
                break;
            swift::NominalTypeDecl *nominal_type_decl = nominal_type->getDecl();
            if (nominal_type_decl && nominal_type_decl->hasClangNode())
            {
                const clang::Decl * clang_decl = nominal_type_decl->getClangDecl();
                if (!clang_decl)
                    break;
                success = true;
                if (!original_type)
                    break;
                
                if (const clang::ObjCInterfaceDecl* objc_interface_decl = llvm::dyn_cast<clang::ObjCInterfaceDecl>(clang_decl)) // ObjCInterfaceDecl is not a TypeDecl
                {
                    *original_type = CompilerType(&objc_interface_decl->getASTContext(), clang::QualType::getFromOpaquePtr(objc_interface_decl->getTypeForDecl()));
                }
                else if (const clang::TypeDecl* type_decl = llvm::dyn_cast<clang::TypeDecl>(clang_decl))
                {
                    *original_type = CompilerType(&type_decl->getASTContext(), clang::QualType::getFromOpaquePtr(type_decl->getTypeForDecl()));
                }
                else // TODO: any more cases that we care about?
                {
                    *original_type = CompilerType();
                }
                
            }
        } while (0);
    }

    return success;
}

bool
SwiftASTContext::IsImportedObjectiveCType (const CompilerType& type,
                                           CompilerType* original_type)
{
    bool success = false;
    
    if (llvm::dyn_cast_or_null<SwiftASTContext>(type.GetTypeSystem()))
    {
        CompilerType local_original_type;
        
        if (IsImportedType(type, &local_original_type))
        {
            if (local_original_type.IsValid())
            {
                ClangASTContext *clang_ast = llvm::dyn_cast_or_null<ClangASTContext>(local_original_type.GetTypeSystem());
                if (clang_ast && clang_ast->IsObjCObjectOrInterfaceType(local_original_type))
                {
                    if (original_type)
                        *original_type = local_original_type;
                    success = true;
                }
            }
        }
    }
    
    return success;
}

void
SwiftASTContext::DumpSummary (void* type, ExecutionContext *exe_ctx,
                              Stream *s,
                              const lldb_private::DataExtractor &data,
                              lldb::offset_t data_byte_offset,
                              size_t data_byte_size)
{
}

size_t
SwiftASTContext::ConvertStringToFloatValue (void * type,
                                            const char *s,
                                            uint8_t *dst,
                                            size_t dst_size)
{
    return 0;
}

void
SwiftASTContext::DumpTypeDescription (void * type)
{
    StreamFile s (stdout, false);
    DumpTypeDescription (type,
                         &s);
}

void
SwiftASTContext::DumpTypeDescription (void * type, Stream *s)
{
    DumpTypeDescription(type, s, false, true);
}

void
SwiftASTContext::DumpTypeDescription (void* type,
                                      bool print_help_if_available,
                                      bool print_extensions_if_available)
{
    StreamFile s (stdout, false);
    DumpTypeDescription (type,
                         &s,
                         print_help_if_available,
                         print_extensions_if_available);
}

static void
PrintSwiftNominalType (swift::NominalTypeDecl *nominal_type_decl,
                       Stream* s,
                       bool print_help_if_available,
                       bool print_extensions_if_available)
{
    if (nominal_type_decl && s)
    {
        std::string buffer;
        llvm::raw_string_ostream ostream(buffer);
        const swift::PrintOptions& print_options(SwiftASTContext::GetUserVisibleTypePrintingOptions(print_help_if_available));
        nominal_type_decl->print(ostream, print_options);
        ostream.flush();
        if (buffer.empty() == false)
            s->Printf("%s\n",buffer.c_str());
        if (print_extensions_if_available)
        {
            for(auto ext : nominal_type_decl->getExtensions())
            {
                if (ext)
                {
                    buffer.clear();
                    llvm::raw_string_ostream ext_ostream(buffer);
                    ext->print(ext_ostream, print_options);
                    ext_ostream.flush();
                    if (buffer.empty() == false)
                        s->Printf("%s\n",buffer.c_str());
                }
            }
        }
    }
}

void
SwiftASTContext::DumpTypeDescription (void* type,
                                      Stream *s,
                                      bool print_help_if_available,
                                      bool print_extensions_if_available)
{
    llvm::SmallVector<char, 1024> buf;
    llvm::raw_svector_ostream llvm_ostrm (buf);

    if (type)
    {
        swift::CanType swift_can_type (GetCanonicalSwiftType (type));
        switch (swift_can_type->getKind())
        {
            case swift::TypeKind::Module:
            {
                swift::ModuleType *module_type = swift_can_type->getAs<swift::ModuleType>();
                if (module_type)
                {
                    swift::ModuleDecl *module = module_type->getModule();
                    if (module)
                    {
                        llvm::SmallVector<swift::Decl*, 10> decls;
                        module->getDisplayDecls(decls);
                        for (swift::Decl *decl : decls)
                        {
                            swift::DeclKind kind = decl->getKind();
                            if (kind >= swift::DeclKind::First_TypeDecl &&
                                kind <= swift::DeclKind::Last_TypeDecl)
                            {
                                swift::TypeDecl *type_decl = llvm::dyn_cast_or_null<swift::TypeDecl>(decl);
                                if (type_decl)
                                {
                                    CompilerType clang_type(&module->getASTContext(),
                                                            type_decl->getType().getPointer());
                                    if (clang_type)
                                    {
                                        Flags clang_type_flags(clang_type.GetTypeInfo());
                                        if (clang_type_flags.AllSet(eTypeIsSwift | eTypeIsMetatype))
                                            clang_type = clang_type.GetInstanceType();
                                        DumpTypeDescription(clang_type.GetOpaqueQualType(),
                                                            s,
                                                            print_help_if_available,
                                                            print_extensions_if_available);
                                    }
                                }
                            }
                            else if (kind == swift::DeclKind::Func || kind == swift::DeclKind::Var)
                            {
                                std::string buffer;
                                llvm::raw_string_ostream stream(buffer);
                                decl->print(stream, SwiftASTContext::GetUserVisibleTypePrintingOptions(print_help_if_available));
                                stream.flush();
                                s->Printf("%s\n", buffer.c_str());
                            }
                            else if (kind == swift::DeclKind::Import)
                            {
                                swift::ImportDecl *import_decl = llvm::dyn_cast_or_null<swift::ImportDecl>(decl);
                                if (import_decl)
                                {
                                    switch (import_decl->getImportKind())
                                    {
                                        case swift::ImportKind::Module:
                                        {
                                            swift::Module *imported_module = import_decl->getModule();
                                            if (imported_module)
                                            {
                                                s->Printf("import %s\n", imported_module->getName().get());
                                            }
                                        }
                                            break;
                                        default:
                                        {
                                            for (swift::Decl* imported_decl : import_decl->getDecls())
                                            {
                                                // all of the non-module things you can import should be a ValueDecl
                                                if (swift::ValueDecl *imported_value_decl = llvm::dyn_cast_or_null<swift::ValueDecl>(imported_decl))
                                                {
                                                    if (swift::TypeBase *decl_type = imported_value_decl->getType().getPointer())
                                                    {
                                                        DumpTypeDescription(decl_type, s, print_help_if_available, print_extensions_if_available);
                                                    }
                                                }
                                            }
                                        }
                                            break;
                                    }
                                }
                            }
                        }
                    }
                }
                break;
            }
            case swift::TypeKind::Metatype:
            {
                s->PutCString ("metatype ");
                swift::MetatypeType *metatype_type = swift_can_type->getAs<swift::MetatypeType>();
                if (metatype_type)
                    DumpTypeDescription(metatype_type->getInstanceType().getPointer(), print_help_if_available, print_extensions_if_available);
            }
                break;
            case swift::TypeKind::UnboundGeneric:
            {
                swift::UnboundGenericType *unbound_generic_type = swift_can_type->getAs<swift::UnboundGenericType>();
                if (unbound_generic_type)
                {
                    swift::NominalTypeDecl* nominal_type_decl = unbound_generic_type->getDecl();
                    PrintSwiftNominalType(nominal_type_decl,
                                          s,
                                          print_help_if_available,
                                          print_extensions_if_available);
                }
            }
                break;
            case swift::TypeKind::PolymorphicFunction:
            case swift::TypeKind::GenericFunction:
            case swift::TypeKind::Function:
            {
                swift::AnyFunctionType *any_function_type = swift_can_type->getAs<swift::AnyFunctionType>();
                if (any_function_type)
                {
                    std::string buffer;
                    llvm::raw_string_ostream ostream(buffer);
                    const swift::PrintOptions& print_options(SwiftASTContext::GetUserVisibleTypePrintingOptions(print_help_if_available));
                    
                    any_function_type->print(ostream, print_options);
                    ostream.flush();
                    if (buffer.empty() == false)
                        s->Printf("%s\n",buffer.c_str());
                }
            }
                break;
            case swift::TypeKind::Tuple:
            {
                swift::TupleType *tuple_type = swift_can_type->getAs<swift::TupleType>();
                if (tuple_type)
                {
                    std::string buffer;
                    llvm::raw_string_ostream ostream(buffer);
                    const swift::PrintOptions& print_options(SwiftASTContext::GetUserVisibleTypePrintingOptions(print_help_if_available));
                    
                    tuple_type->print(ostream, print_options);
                    ostream.flush();
                    if (buffer.empty() == false)
                        s->Printf("%s\n",buffer.c_str());
                }
            }
            case swift::TypeKind::BoundGenericClass:
            case swift::TypeKind::BoundGenericEnum:
            case swift::TypeKind::BoundGenericStruct:
            {
                swift::BoundGenericType *bound_generic_type = swift_can_type->getAs<swift::BoundGenericType>();
                if (bound_generic_type)
                {
                    swift::NominalTypeDecl* nominal_type_decl = bound_generic_type->getDecl();
                    PrintSwiftNominalType(nominal_type_decl,
                                          s,
                                          print_help_if_available,
                                          print_extensions_if_available);
                }
            }
            case swift::TypeKind::BuiltinInteger:
            {
                swift::BuiltinIntegerType *builtin_integer_type = swift_can_type->getAs<swift::BuiltinIntegerType>();
                if (builtin_integer_type)
                    s->Printf("builtin integer type of width %u bits\n", builtin_integer_type->getWidth().getGreatestWidth());
                break;
            }
            case swift::TypeKind::BuiltinFloat:
            {
                swift::BuiltinFloatType *builtin_float_type = swift_can_type->getAs<swift::BuiltinFloatType>();
                if (builtin_float_type)
                    s->Printf("builtin floating-point type of width %u bits\n", builtin_float_type->getBitWidth());
                break;
            }
            default:
            {
                swift::NominalType* nominal_type = llvm::dyn_cast_or_null<swift::NominalType>(swift_can_type.getPointer());
                if (nominal_type)
                {
                    swift::NominalTypeDecl* nominal_type_decl = nominal_type->getDecl();
                    PrintSwiftNominalType(nominal_type_decl,
                                          s,
                                          print_help_if_available,
                                          print_extensions_if_available);
                }
            }
                break;
        }
        
        if (buf.size() > 0)
        {
            s->Write (buf.data(), buf.size());
        }
    }
}

TypeSP
SwiftASTContext::GetCachedType (const ConstString &mangled)
{
    TypeSP type_sp;
    if (m_swift_type_map.Lookup(mangled.GetCString(), type_sp))
        return type_sp;
    else
        return TypeSP();
}

void
SwiftASTContext::SetCachedType (const ConstString &mangled, const TypeSP &type_sp)
{
    m_swift_type_map.Insert(mangled.GetCString(), type_sp);
}

DWARFASTParser *
SwiftASTContext::GetDWARFParser ()
{
    if (!m_dwarf_ast_parser_ap)
        m_dwarf_ast_parser_ap.reset(new DWARFASTParserSwift(*this));
    return m_dwarf_ast_parser_ap.get();
}

SwiftASTContextForExpressions::SwiftASTContextForExpressions (Target &target) :
    SwiftASTContext (target.GetArchitecture().GetTriple().getTriple().c_str(), &target),
    m_persistent_state_up(new SwiftPersistentExpressionState)
{
}

UserExpression *
SwiftASTContextForExpressions::GetUserExpression (const char *expr,
                                                  const char *expr_prefix,
                                                  lldb::LanguageType language,
                                                  Expression::ResultType desired_type,
                                                  const EvaluateExpressionOptions &options)
{
    TargetSP target_sp = m_target_wp.lock();
    if (!target_sp)
        return nullptr;
    
    return new SwiftUserExpression(*target_sp.get(), expr, expr_prefix, language, desired_type, options);
}

PersistentExpressionState *
SwiftASTContextForExpressions::GetPersistentExpressionState()
{
    return m_persistent_state_up.get();
}
