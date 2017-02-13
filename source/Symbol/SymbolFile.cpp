//===-- SymbolFile.cpp ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/Symbol/SymbolFile.h"

#include "lldb/Core/Log.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/TypeMap.h"
#include "lldb/Symbol/TypeSystem.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/lldb-private.h"

using namespace lldb_private;

SymbolFile *SymbolFile::FindPlugin(ObjectFile *obj_file) {
  std::unique_ptr<SymbolFile> best_symfile_ap;
  if (obj_file != nullptr) {

    // We need to test the abilities of this section list. So create what it
    // would
    // be with this new obj_file.
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
      std::unique_ptr<SymbolFile> curr_symfile_ap(create_callback(obj_file));

      if (curr_symfile_ap.get()) {
        const uint32_t sym_file_abilities = curr_symfile_ap->GetAbilities();
        if (sym_file_abilities > best_symfile_abilities) {
          best_symfile_abilities = sym_file_abilities;
          best_symfile_ap.reset(curr_symfile_ap.release());
          // If any symbol file parser has all of the abilities, then
          // we should just stop looking.
          if ((kAllAbilities & sym_file_abilities) == kAllAbilities)
            break;
        }
      }
    }
    if (best_symfile_ap.get()) {
      // Let the winning symbol file parser initialize itself more
      // completely now that it has been chosen
      best_symfile_ap->InitializeObject();
    }
  }
  return best_symfile_ap.release();
}

TypeList *SymbolFile::GetTypeList() {
  if (m_obj_file)
    return m_obj_file->GetModule()->GetTypeList();
  return nullptr;
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
  return m_obj_file->GetType() == ObjectFile::eTypeJIT;
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
                                          uint32_t resolve_scope,
                                          SymbolContextList &sc_list) {
  return 0;
}

uint32_t SymbolFile::FindGlobalVariables(
    const ConstString &name, const CompilerDeclContext *parent_decl_ctx,
    bool append, uint32_t max_matches, VariableList &variables) {
  if (!append)
    variables.Clear();
  return 0;
}

uint32_t SymbolFile::FindGlobalVariables(const RegularExpression &regex,
                                         bool append, uint32_t max_matches,
                                         VariableList &variables) {
  if (!append)
    variables.Clear();
  return 0;
}

uint32_t SymbolFile::FindFunctions(const ConstString &name,
                                   const CompilerDeclContext *parent_decl_ctx,
                                   uint32_t name_type_mask,
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
    const SymbolContext &sc, const ConstString &name,
    const CompilerDeclContext *parent_decl_ctx, bool append,
    uint32_t max_matches,
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
