//===-- DWARFDebugInfo.h ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SymbolFileDWARF_DWARFDebugInfo_h_
#define SymbolFileDWARF_DWARFDebugInfo_h_

#include <map>
#include <vector>

#include "DWARFDIE.h"
#include "DWARFUnit.h"
#include "SymbolFileDWARF.h"
#include "lldb/Core/STLUtils.h"
#include "lldb/lldb-private.h"
#include "llvm/Support/Error.h"

namespace lldb_private {
class DWARFContext;
}

typedef std::multimap<const char *, dw_offset_t, CStringCompareFunctionObject>
    CStringToDIEMap;
typedef CStringToDIEMap::iterator CStringToDIEMapIter;
typedef CStringToDIEMap::const_iterator CStringToDIEMapConstIter;

class DWARFDebugInfo {
public:
  typedef dw_offset_t (*Callback)(SymbolFileDWARF *dwarf2Data,
                                  DWARFUnit *cu,
                                  DWARFDebugInfoEntry *die,
                                  const dw_offset_t next_offset,
                                  const uint32_t depth, void *userData);

  explicit DWARFDebugInfo(lldb_private::DWARFContext &context);
  void SetDwarfData(SymbolFileDWARF *dwarf2Data);

  size_t GetNumUnits();
  DWARFUnit *GetUnitAtIndex(lldb::user_id_t idx);
  DWARFUnit *GetUnitAtOffset(dw_offset_t cu_offset, uint32_t *idx_ptr = NULL);
  DWARFUnit *GetUnitContainingDIEOffset(dw_offset_t die_offset);
  DWARFUnit *GetUnit(const DIERef &die_ref);
  DWARFDIE GetDIEForDIEOffset(dw_offset_t die_offset);
  DWARFDIE GetDIE(const DIERef &die_ref);

  enum {
    eDumpFlag_Verbose = (1 << 0),  // Verbose dumping
    eDumpFlag_ShowForm = (1 << 1), // Show the DW_form type
    eDumpFlag_ShowAncestors =
        (1 << 2) // Show all parent DIEs when dumping single DIEs
  };

  llvm::Expected<DWARFDebugAranges &> GetCompileUnitAranges();

protected:
  static bool OffsetLessThanUnitOffset(dw_offset_t offset,
                                       const DWARFUnitSP &cu_sp);

  typedef std::vector<DWARFUnitSP> UnitColl;

  // Member variables
  SymbolFileDWARF *m_dwarf2Data;
  lldb_private::DWARFContext &m_context;
  UnitColl m_units;
  std::unique_ptr<DWARFDebugAranges>
      m_cu_aranges_up; // A quick address to compile unit table

private:
  // All parsing needs to be done partially any managed by this class as
  // accessors are called.
  void ParseUnitHeadersIfNeeded();

  uint32_t FindUnitIndex(dw_offset_t offset);

  DISALLOW_COPY_AND_ASSIGN(DWARFDebugInfo);
};

#endif // SymbolFileDWARF_DWARFDebugInfo_h_
