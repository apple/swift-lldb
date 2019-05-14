//===-- DWARFCompileUnit.cpp ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DWARFCompileUnit.h"

#include "SymbolFileDWARF.h"
#include "lldb/Utility/Stream.h"
#include "llvm/Object/Error.h"

using namespace lldb;
using namespace lldb_private;

DWARFCompileUnit::DWARFCompileUnit(SymbolFileDWARF *dwarf2Data,
                                   lldb::user_id_t uid)
    : DWARFUnit(dwarf2Data, uid) {}


llvm::Expected<DWARFUnitSP> DWARFCompileUnit::extract(
    SymbolFileDWARF *dwarf2Data, user_id_t uid,
    const DWARFDataExtractor &debug_info, lldb::offset_t *offset_ptr) {
  assert(debug_info.ValidOffset(*offset_ptr));

  // std::make_shared would require the ctor to be public.
  std::shared_ptr<DWARFCompileUnit> cu_sp(
      new DWARFCompileUnit(dwarf2Data, uid));

  cu_sp->m_offset = *offset_ptr;

  dw_offset_t abbr_offset;
  const DWARFDebugAbbrev *abbr = dwarf2Data->DebugAbbrev();
  if (!abbr)
    return llvm::make_error<llvm::object::GenericBinaryError>(
        "No debug_abbrev data");

  cu_sp->m_length = debug_info.GetDWARFInitialLength(offset_ptr);
  cu_sp->m_version = debug_info.GetU16(offset_ptr);

  if (cu_sp->m_version == 5) {
    cu_sp->m_unit_type = debug_info.GetU8(offset_ptr);
    cu_sp->m_addr_size = debug_info.GetU8(offset_ptr);
    abbr_offset = debug_info.GetDWARFOffset(offset_ptr);

    if (cu_sp->m_unit_type == llvm::dwarf::DW_UT_skeleton)
      cu_sp->m_dwo_id = debug_info.GetU64(offset_ptr);
  } else {
    abbr_offset = debug_info.GetDWARFOffset(offset_ptr);
    cu_sp->m_addr_size = debug_info.GetU8(offset_ptr);
  }

  bool length_OK = debug_info.ValidOffset(cu_sp->GetNextUnitOffset() - 1);
  bool version_OK = SymbolFileDWARF::SupportedVersion(cu_sp->m_version);
  bool abbr_offset_OK =
      dwarf2Data->get_debug_abbrev_data().ValidOffset(abbr_offset);
  bool addr_size_OK = (cu_sp->m_addr_size == 4) || (cu_sp->m_addr_size == 8);

  if (!length_OK)
    return llvm::make_error<llvm::object::GenericBinaryError>(
        "Invalid compile unit length");
  if (!version_OK)
    return llvm::make_error<llvm::object::GenericBinaryError>(
        "Unsupported compile unit version");
  if (!abbr_offset_OK)
    return llvm::make_error<llvm::object::GenericBinaryError>(
        "Abbreviation offset for compile unit is not valid");
  if (!addr_size_OK)
    return llvm::make_error<llvm::object::GenericBinaryError>(
        "Invalid compile unit address size");

  cu_sp->m_abbrevs = abbr->GetAbbreviationDeclarationSet(abbr_offset);
  if (!cu_sp->m_abbrevs)
    return llvm::make_error<llvm::object::GenericBinaryError>(
        "No abbrev exists at the specified offset.");

  return cu_sp;
}

void DWARFCompileUnit::Dump(Stream *s) const {
  s->Printf("0x%8.8x: Compile Unit: length = 0x%8.8x, version = 0x%4.4x, "
            "abbr_offset = 0x%8.8x, addr_size = 0x%2.2x (next CU at "
            "{0x%8.8x})\n",
            m_offset, m_length, m_version, GetAbbrevOffset(), m_addr_size,
            GetNextUnitOffset());
}

uint32_t DWARFCompileUnit::GetHeaderByteSize() const {
  if (m_version < 5)
    return 11;

  switch (m_unit_type) {
  case llvm::dwarf::DW_UT_compile:
  case llvm::dwarf::DW_UT_partial:
    return 12;
  case llvm::dwarf::DW_UT_skeleton:
  case llvm::dwarf::DW_UT_split_compile:
    return 20;
  case llvm::dwarf::DW_UT_type:
  case llvm::dwarf::DW_UT_split_type:
    return 24;
  }
  llvm_unreachable("invalid UnitType.");
}

const lldb_private::DWARFDataExtractor &DWARFCompileUnit::GetData() const {
  return m_dwarf->get_debug_info_data();
}
