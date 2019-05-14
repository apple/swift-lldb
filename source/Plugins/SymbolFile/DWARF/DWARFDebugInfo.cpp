//===-- DWARFDebugInfo.cpp --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolFileDWARF.h"

#include <algorithm>
#include <set>

#include "lldb/Host/PosixApi.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Utility/RegularExpression.h"
#include "lldb/Utility/Stream.h"

#include "DWARFCompileUnit.h"
#include "DWARFContext.h"
#include "DWARFDebugAranges.h"
#include "DWARFDebugInfo.h"
#include "DWARFDebugInfoEntry.h"
#include "DWARFFormValue.h"

using namespace lldb;
using namespace lldb_private;
using namespace std;

// Constructor
DWARFDebugInfo::DWARFDebugInfo(lldb_private::DWARFContext &context)
    : m_dwarf2Data(NULL), m_context(context), m_units(), m_cu_aranges_up() {}

// SetDwarfData
void DWARFDebugInfo::SetDwarfData(SymbolFileDWARF *dwarf2Data) {
  m_dwarf2Data = dwarf2Data;
  m_units.clear();
}

llvm::Expected<DWARFDebugAranges &> DWARFDebugInfo::GetCompileUnitAranges() {
  if (m_cu_aranges_up)
    return *m_cu_aranges_up;

  assert(m_dwarf2Data);

  m_cu_aranges_up = llvm::make_unique<DWARFDebugAranges>();
  const DWARFDataExtractor *debug_aranges_data =
      m_context.getOrLoadArangesData();
  if (debug_aranges_data) {
    llvm::Error error = m_cu_aranges_up->extract(*debug_aranges_data);
    if (error)
      return std::move(error);
  }

  // Make a list of all CUs represented by the arange data in the file.
  std::set<dw_offset_t> cus_with_data;
  for (size_t n = 0; n < m_cu_aranges_up->GetNumRanges(); n++) {
    dw_offset_t offset = m_cu_aranges_up->OffsetAtIndex(n);
    if (offset != DW_INVALID_OFFSET)
      cus_with_data.insert(offset);
  }

  // Manually build arange data for everything that wasn't in the
  // .debug_aranges table.
  const size_t num_units = GetNumUnits();
  for (size_t idx = 0; idx < num_units; ++idx) {
    DWARFUnit *cu = GetUnitAtIndex(idx);

    dw_offset_t offset = cu->GetOffset();
    if (cus_with_data.find(offset) == cus_with_data.end())
      cu->BuildAddressRangeTable(m_dwarf2Data, m_cu_aranges_up.get());
  }

  const bool minimize = true;
  m_cu_aranges_up->Sort(minimize);
  return *m_cu_aranges_up;
}

void DWARFDebugInfo::ParseUnitHeadersIfNeeded() {
  if (!m_units.empty())
    return;
  if (!m_dwarf2Data)
    return;

  lldb::offset_t offset = 0;
  const auto &debug_info_data = m_dwarf2Data->get_debug_info_data();

  while (debug_info_data.ValidOffset(offset)) {
    llvm::Expected<DWARFUnitSP> cu_sp = DWARFCompileUnit::extract(
        m_dwarf2Data, m_units.size(), debug_info_data, &offset);

    if (!cu_sp) {
      // FIXME: Propagate this error up.
      llvm::consumeError(cu_sp.takeError());
      return;
    }

    // If it didn't return an error, then it should be returning a valid Unit.
    assert(*cu_sp);

    m_units.push_back(*cu_sp);

    offset = (*cu_sp)->GetNextUnitOffset();
  }
}

size_t DWARFDebugInfo::GetNumUnits() {
  ParseUnitHeadersIfNeeded();
  return m_units.size();
}

DWARFUnit *DWARFDebugInfo::GetUnitAtIndex(user_id_t idx) {
  DWARFUnit *cu = NULL;
  if (idx < GetNumUnits())
    cu = m_units[idx].get();
  return cu;
}

bool DWARFDebugInfo::OffsetLessThanUnitOffset(dw_offset_t offset,
                                              const DWARFUnitSP &cu_sp) {
  return offset < cu_sp->GetOffset();
}

uint32_t DWARFDebugInfo::FindUnitIndex(dw_offset_t offset) {
  ParseUnitHeadersIfNeeded();

  // llvm::lower_bound is not used as for DIE offsets it would still return
  // index +1 and GetOffset() returning index itself would be a special case.
  auto pos = llvm::upper_bound(m_units, offset, OffsetLessThanUnitOffset);
  uint32_t idx = std::distance(m_units.begin(), pos);
  if (idx == 0)
    return DW_INVALID_OFFSET;
  return idx - 1;
}

DWARFUnit *DWARFDebugInfo::GetUnitAtOffset(dw_offset_t cu_offset,
                                           uint32_t *idx_ptr) {
  uint32_t idx = FindUnitIndex(cu_offset);
  DWARFUnit *result = GetUnitAtIndex(idx);
  if (result && result->GetOffset() != cu_offset) {
    result = nullptr;
    idx = DW_INVALID_INDEX;
  }
  if (idx_ptr)
    *idx_ptr = idx;
  return result;
}

DWARFUnit *DWARFDebugInfo::GetUnit(const DIERef &die_ref) {
  if (die_ref.cu_offset == DW_INVALID_OFFSET)
    return GetUnitContainingDIEOffset(die_ref.die_offset);
  else
    return GetUnitAtOffset(die_ref.cu_offset);
}

DWARFUnit *DWARFDebugInfo::GetUnitContainingDIEOffset(dw_offset_t die_offset) {
  uint32_t idx = FindUnitIndex(die_offset);
  DWARFUnit *result = GetUnitAtIndex(idx);
  if (result && !result->ContainsDIEOffset(die_offset))
    return nullptr;
  return result;
}

DWARFDIE
DWARFDebugInfo::GetDIEForDIEOffset(dw_offset_t die_offset) {
  DWARFUnit *cu = GetUnitContainingDIEOffset(die_offset);
  if (cu)
    return cu->GetDIE(die_offset);
  return DWARFDIE();
}

// GetDIE()
//
// Get the DIE (Debug Information Entry) with the specified offset.
DWARFDIE
DWARFDebugInfo::GetDIE(const DIERef &die_ref) {
  DWARFUnit *cu = GetUnit(die_ref);
  if (cu)
    return cu->GetDIE(die_ref.die_offset);
  return DWARFDIE(); // Not found
}

