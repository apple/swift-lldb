//===-- SymbolFile.cpp ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Symbol/SymbolFile.h"

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/CompileUnit.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/TypeMap.h"
#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/lldb-private.h"

#include <future>

using namespace lldb_private;
using namespace lldb;

void SymbolFile::PreloadSymbols() {
  // No-op for most implementations.
}

std::recursive_mutex &SymbolFile::GetModuleMutex() const {
  return GetObjectFile()->GetModule()->GetMutex();
}
ObjectFile *SymbolFile::GetMainObjectFile() {
  return m_obj_file->GetModule()->GetObjectFile();
}

SymbolFile *SymbolFile::FindPlugin(ObjectFile *obj_file) {
  std::unique_ptr<SymbolFile> best_symfile_up;
  if (obj_file != nullptr) {

    // We need to test the abilities of this section list. So create what it
    // would be with this new obj_file.
    lldb::ModuleSP module_sp(obj_file->GetModule());
    if (module_sp) {
      // Default to the main module section list.
      ObjectFile *module_obj_file = module_sp->GetObjectFile();
      if (module_obj_file != obj_file) {
        // Make sure the main object file's sections are created
        module_obj_file->GetSectionList();
        obj_file->CreateSections(*module_sp->GetUnifiedSectionList());
      }
    }

    // TODO: Load any plug-ins in the appropriate plug-in search paths and
    // iterate over all of them to find the best one for the job.

    uint32_t best_symfile_abilities = 0;

    SymbolFileCreateInstance create_callback;
    for (uint32_t idx = 0;
         (create_callback = PluginManager::GetSymbolFileCreateCallbackAtIndex(
              idx)) != nullptr;
         ++idx) {
      std::unique_ptr<SymbolFile> curr_symfile_up(create_callback(obj_file));

      if (curr_symfile_up) {
        const uint32_t sym_file_abilities = curr_symfile_up->GetAbilities();
        if (sym_file_abilities > best_symfile_abilities) {
          best_symfile_abilities = sym_file_abilities;
          best_symfile_up.reset(curr_symfile_up.release());
          // If any symbol file parser has all of the abilities, then we should
          // just stop looking.
          if ((kAllAbilities & sym_file_abilities) == kAllAbilities)
            break;
        }
      }
    }
    if (best_symfile_up) {
      // Let the winning symbol file parser initialize itself more completely
      // now that it has been chosen
      best_symfile_up->InitializeObject();
    }
  }
  return best_symfile_up.release();
}

TypeSystem *SymbolFile::GetTypeSystemForLanguage(lldb::LanguageType language) {
  TypeSystem *type_system =
      m_obj_file->GetModule()->GetTypeSystemForLanguage(language);
  if (type_system)
    type_system->SetSymbolFile(this);
  return type_system;
}

bool SymbolFile::ForceInlineSourceFileCheck() {
  // Force checking for inline breakpoint locations for any JIT object files.
  // If we have a symbol file for something that has been JIT'ed, chances
  // are we used "#line" directives to point to the expression code and this
  // means we will have DWARF line tables that have source implementation
  // entries that do not match the compile unit source (usually a memory buffer)
  // file. Returning true for JIT files means all breakpoints set by file and
  // line
  // will be found correctly.
  return m_objfile_sp->GetType() == ObjectFile::eTypeJIT;
}

bool SymbolFile::SetLimitSourceFileRange(const FileSpec &file,
                                         uint32_t first_line,
                                         uint32_t last_line) {
  if (file && first_line <= last_line) {
    m_limit_source_ranges.push_back(SourceRange(file, first_line, last_line));
    return true;
  }
  return false;
}

bool SymbolFile::SymbolContextShouldBeExcluded(const SymbolContext &sc,
                                               uint32_t actual_line) {
  if (!m_limit_source_ranges.empty()) {
    bool file_match = false;
    bool line_match = false;
    for (const auto &range : m_limit_source_ranges) {
      const auto &line_entry = sc.line_entry;
      if (range.file == line_entry.file) {
        file_match = true;
        if (range.first_line <= actual_line && actual_line <= range.last_line)
          line_match = true;
      }
    }
    if (file_match && !line_match)
      return true;
  }
  return false;
}

std::vector<lldb::DataBufferSP>
SymbolFile::GetASTData(lldb::LanguageType language) {
  // SymbolFile subclasses must add this functionality
  return std::vector<lldb::DataBufferSP>();
}

uint32_t SymbolFile::ResolveSymbolContext(const FileSpec &file_spec,
                                          uint32_t line, bool check_inlines,
                                          lldb::SymbolContextItem resolve_scope,
                                          SymbolContextList &sc_list) {
  return 0;
}

uint32_t
SymbolFile::FindGlobalVariables(ConstString name,
                                const CompilerDeclContext *parent_decl_ctx,
                                uint32_t max_matches, VariableList &variables) {
  return 0;
}

uint32_t SymbolFile::FindGlobalVariables(const RegularExpression &regex,
                                         uint32_t max_matches,
                                         VariableList &variables) {
  return 0;
}

uint32_t SymbolFile::FindFunctions(ConstString name,
                                   const CompilerDeclContext *parent_decl_ctx,
                                   lldb::FunctionNameType name_type_mask,
                                   bool include_inlines, bool append,
                                   SymbolContextList &sc_list) {
  if (!append)
    sc_list.Clear();
  return 0;
}

uint32_t SymbolFile::FindFunctions(const RegularExpression &regex,
                                   bool include_inlines, bool append,
                                   SymbolContextList &sc_list) {
  if (!append)
    sc_list.Clear();
  return 0;
}

void SymbolFile::GetMangledNamesForFunction(
    const std::string &scope_qualified_name,
    std::vector<ConstString> &mangled_names) {
  return;
}

uint32_t SymbolFile::FindTypes(
    ConstString name, const CompilerDeclContext *parent_decl_ctx,
    bool append, uint32_t max_matches,
    llvm::DenseSet<lldb_private::SymbolFile *> &searched_symbol_files,
    TypeMap &types) {
  if (!append)
    types.Clear();
  return 0;
}

size_t SymbolFile::FindTypes(const std::vector<CompilerContext> &context,
                             bool append, TypeMap &types) {
  if (!append)
    types.Clear();
  return 0;
}

void SymbolFile::AssertModuleLock() {
  // The code below is too expensive to leave enabled in release builds. It's
  // enabled in debug builds or when the correct macro is set.
#if defined(LLDB_CONFIGURATION_DEBUG)
  // We assert that we have to module lock by trying to acquire the lock from a
  // different thread. Note that we must abort if the result is true to
  // guarantee correctness.
  assert(std::async(std::launch::async,
                    [this] { return this->GetModuleMutex().try_lock(); })
                 .get() == false &&
         "Module is not locked");
#endif
}

uint32_t SymbolFile::GetNumCompileUnits() {
  std::lock_guard<std::recursive_mutex> guard(GetModuleMutex());
  if (!m_compile_units) {
    // Create an array of compile unit shared pointers -- which will each
    // remain NULL until someone asks for the actual compile unit information.
    m_compile_units.emplace(CalculateNumCompileUnits());
  }
  return m_compile_units->size();
}

CompUnitSP SymbolFile::GetCompileUnitAtIndex(uint32_t idx) {
  std::lock_guard<std::recursive_mutex> guard(GetModuleMutex());
  uint32_t num = GetNumCompileUnits();
  if (idx >= num)
    return nullptr;
  lldb::CompUnitSP &cu_sp = (*m_compile_units)[idx];
  if (!cu_sp)
    cu_sp = ParseCompileUnitAtIndex(idx);
  return cu_sp;
}

void SymbolFile::SetCompileUnitAtIndex(uint32_t idx, const CompUnitSP &cu_sp) {
  std::lock_guard<std::recursive_mutex> guard(GetModuleMutex());
  const size_t num_compile_units = GetNumCompileUnits();
  assert(idx < num_compile_units);
  (void)num_compile_units;

  // Fire off an assertion if this compile unit already exists for now. The
  // partial parsing should take care of only setting the compile unit
  // once, so if this assertion fails, we need to make sure that we don't
  // have a race condition, or have a second parse of the same compile
  // unit.
  assert((*m_compile_units)[idx] == nullptr);
  (*m_compile_units)[idx] = cu_sp;
}

Symtab *SymbolFile::GetSymtab() {
  std::lock_guard<std::recursive_mutex> guard(GetModuleMutex());
  if (m_symtab)
    return m_symtab;

  // Fetch the symtab from the main object file.
  m_symtab = GetMainObjectFile()->GetSymtab();

  // Then add our symbols to it.
  if (m_symtab)
    AddSymbols(*m_symtab);

  return m_symtab;
}

void SymbolFile::SectionFileAddressesChanged() {
  ObjectFile *module_objfile = GetMainObjectFile();
  ObjectFile *symfile_objfile = GetObjectFile();
  if (symfile_objfile != module_objfile)
    symfile_objfile->SectionFileAddressesChanged();
  if (m_symtab)
    m_symtab->SectionFileAddressesChanged();
}

void SymbolFile::Dump(Stream &s) {
  s.PutCString("Types:\n");
  m_type_list.Dump(&s, /*show_context*/ false);
  s.PutChar('\n');

  s.PutCString("Compile units:\n");
  if (m_compile_units) {
    for (const CompUnitSP &cu_sp : *m_compile_units) {
      // We currently only dump the compile units that have been parsed
      if (cu_sp)
        cu_sp->Dump(&s, /*show_context*/ false);
    }
  }
  s.PutChar('\n');
}

SymbolFile::RegisterInfoResolver::~RegisterInfoResolver() = default;
