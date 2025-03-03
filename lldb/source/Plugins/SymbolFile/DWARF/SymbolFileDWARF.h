//===-- SymbolFileDWARF.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SymbolFileDWARF_SymbolFileDWARF_h_
#define SymbolFileDWARF_SymbolFileDWARF_h_

#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/Threading.h"

#include "lldb/Core/UniqueCStringMap.h"
#include "lldb/Core/dwarf.h"
#include "lldb/Expression/DWARFExpression.h"
#include "lldb/Symbol/DebugMacros.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/Utility/Flags.h"
#include "lldb/Utility/RangeMap.h"
#include "lldb/lldb-private.h"

#include "DWARFContext.h"
#include "DWARFDataExtractor.h"
#include "DWARFDefines.h"
#include "DWARFIndex.h"
#include "UniqueDWARFASTType.h"

// Forward Declarations for this DWARF plugin
class DebugMapModule;
class DWARFAbbreviationDeclaration;
class DWARFAbbreviationDeclarationSet;
class DWARFCompileUnit;
class DWARFDebugAbbrev;
class DWARFDebugAranges;
class DWARFDebugInfo;
class DWARFDebugInfoEntry;
class DWARFDebugLine;
class DWARFDebugRangesBase;
class DWARFDeclContext;
class DWARFFormValue;
class DWARFTypeUnit;
class SymbolFileDWARFDebugMap;
class SymbolFileDWARFDwo;
class SymbolFileDWARFDwp;

#define DIE_IS_BEING_PARSED ((lldb_private::Type *)1)

class SymbolFileDWARF : public lldb_private::SymbolFile,
                        public lldb_private::UserID {
public:
  friend class SymbolFileDWARFDebugMap;
  friend class SymbolFileDWARFDwo;
  friend class DebugMapModule;
  friend class DWARFCompileUnit;
  friend class DWARFDIE;
  friend class DWARFASTParserClang;

  // Static Functions
  static void Initialize();

  static void Terminate();

  static void DebuggerInitialize(lldb_private::Debugger &debugger);

  static lldb_private::ConstString GetPluginNameStatic();

  static const char *GetPluginDescriptionStatic();

  static lldb_private::SymbolFile *
  CreateInstance(lldb_private::ObjectFile *obj_file);

  static lldb_private::FileSpecList GetSymlinkPaths();

  // Constructors and Destructors

  SymbolFileDWARF(lldb_private::ObjectFile *ofile,
                  lldb_private::SectionList *dwo_section_list);

  ~SymbolFileDWARF() override;

  uint32_t CalculateAbilities() override;

  void InitializeObject() override;

  // Compile Unit function calls

  lldb::LanguageType
  ParseLanguage(lldb_private::CompileUnit &comp_unit) override;

  size_t ParseFunctions(lldb_private::CompileUnit &comp_unit) override;

  bool ParseLineTable(lldb_private::CompileUnit &comp_unit) override;

  bool ParseDebugMacros(lldb_private::CompileUnit &comp_unit) override;

  bool ParseSupportFiles(lldb_private::CompileUnit &comp_unit,
                         lldb_private::FileSpecList &support_files) override;

  bool ParseIsOptimized(lldb_private::CompileUnit &comp_unit) override;

  size_t ParseTypes(lldb_private::CompileUnit &comp_unit) override;

  bool ParseImportedModules(
      const lldb_private::SymbolContext &sc,
      std::vector<lldb_private::SourceModule> &imported_modules) override;

  size_t ParseBlocksRecursive(lldb_private::Function &func) override;

  size_t
  ParseVariablesForContext(const lldb_private::SymbolContext &sc) override;

  lldb_private::Type *ResolveTypeUID(lldb::user_id_t type_uid) override;
  llvm::Optional<ArrayInfo> GetDynamicArrayInfoForUID(
      lldb::user_id_t type_uid,
      const lldb_private::ExecutionContext *exe_ctx) override;

  bool CompleteType(lldb_private::CompilerType &compiler_type) override;

  lldb_private::Type *ResolveType(const DWARFDIE &die,
                                  bool assert_not_being_parsed = true,
                                  bool resolve_function_context = false);

  lldb_private::CompilerDecl GetDeclForUID(lldb::user_id_t uid) override;

  lldb_private::CompilerDeclContext
  GetDeclContextForUID(lldb::user_id_t uid) override;

  lldb_private::CompilerDeclContext
  GetDeclContextContainingUID(lldb::user_id_t uid) override;

  void
  ParseDeclsForContext(lldb_private::CompilerDeclContext decl_ctx) override;

  uint32_t ResolveSymbolContext(const lldb_private::Address &so_addr,
                                lldb::SymbolContextItem resolve_scope,
                                lldb_private::SymbolContext &sc) override;

  uint32_t
  ResolveSymbolContext(const lldb_private::FileSpec &file_spec, uint32_t line,
                       bool check_inlines,
                       lldb::SymbolContextItem resolve_scope,
                       lldb_private::SymbolContextList &sc_list) override;

  uint32_t
  FindGlobalVariables(lldb_private::ConstString name,
                      const lldb_private::CompilerDeclContext *parent_decl_ctx,
                      uint32_t max_matches,
                      lldb_private::VariableList &variables) override;

  uint32_t FindGlobalVariables(const lldb_private::RegularExpression &regex,
                               uint32_t max_matches,
                               lldb_private::VariableList &variables) override;

  uint32_t
  FindFunctions(lldb_private::ConstString name,
                const lldb_private::CompilerDeclContext *parent_decl_ctx,
                lldb::FunctionNameType name_type_mask, bool include_inlines,
                bool append, lldb_private::SymbolContextList &sc_list) override;

  uint32_t FindFunctions(const lldb_private::RegularExpression &regex,
                         bool include_inlines, bool append,
                         lldb_private::SymbolContextList &sc_list) override;

  void GetMangledNamesForFunction(
      const std::string &scope_qualified_name,
      std::vector<lldb_private::ConstString> &mangled_names) override;

  uint32_t
  FindTypes(lldb_private::ConstString name,
            const lldb_private::CompilerDeclContext *parent_decl_ctx,
            bool append, uint32_t max_matches,
            llvm::DenseSet<lldb_private::SymbolFile *> &searched_symbol_files,
            lldb_private::TypeMap &types) override;

  size_t FindTypes(const std::vector<lldb_private::CompilerContext> &context,
                   bool append, lldb_private::TypeMap &types) override;

  size_t GetTypes(lldb_private::SymbolContextScope *sc_scope,
                  lldb::TypeClass type_mask,
                  lldb_private::TypeList &type_list) override;

  lldb_private::TypeSystem *
  GetTypeSystemForLanguage(lldb::LanguageType language) override;

  lldb_private::CompilerDeclContext FindNamespace(
      lldb_private::ConstString name,
      const lldb_private::CompilerDeclContext *parent_decl_ctx) override;

  void PreloadSymbols() override;

  std::recursive_mutex &GetModuleMutex() const override;

  // PluginInterface protocol
  lldb_private::ConstString GetPluginName() override;

  uint32_t GetPluginVersion() override;

  const lldb_private::DWARFDataExtractor &get_debug_loc_data();
  const lldb_private::DWARFDataExtractor &get_debug_loclists_data();

  DWARFDebugAbbrev *DebugAbbrev();

  const DWARFDebugAbbrev *DebugAbbrev() const;

  DWARFDebugInfo *DebugInfo();

  const DWARFDebugInfo *DebugInfo() const;

  DWARFDebugRangesBase *GetDebugRanges();
  DWARFDebugRangesBase *GetDebugRngLists();

  const lldb_private::DWARFDataExtractor &DebugLocData();

  static bool SupportedVersion(uint16_t version);

  DWARFDIE
  GetDeclContextDIEContainingDIE(const DWARFDIE &die);

  bool
  HasForwardDeclForClangType(const lldb_private::CompilerType &compiler_type);

  lldb_private::CompileUnit *
  GetCompUnitForDWARFCompUnit(DWARFCompileUnit &dwarf_cu);

  virtual size_t GetObjCMethodDIEOffsets(lldb_private::ConstString class_name,
                                         DIEArray &method_die_offsets);

  bool Supports_DW_AT_APPLE_objc_complete_type(DWARFUnit *cu);

  lldb_private::DebugMacrosSP ParseDebugMacros(lldb::offset_t *offset);

  static DWARFDIE GetParentSymbolContextDIE(const DWARFDIE &die);

  virtual lldb::CompUnitSP ParseCompileUnit(DWARFCompileUnit &dwarf_cu);

  virtual lldb_private::DWARFExpression::LocationListFormat
  GetLocationListFormat() const;

  lldb::ModuleSP GetDWOModule(lldb_private::ConstString name);

  typedef std::map<lldb_private::ConstString, lldb::ModuleSP>
      ExternalTypeModuleMap;

  /// Return the list of Clang modules imported by this SymbolFile.
  const ExternalTypeModuleMap& getExternalTypeModules() const {
      return m_external_type_modules;
  }

  virtual DWARFDIE GetDIE(const DIERef &die_ref);

  DWARFDIE GetDIE(lldb::user_id_t uid);

  lldb::user_id_t GetUID(const DWARFBaseDIE &die) {
    return GetUID(die.GetDIERef());
  }

  lldb::user_id_t GetUID(const llvm::Optional<DIERef> &ref) {
    return ref ? GetUID(*ref) : LLDB_INVALID_UID;
  }

  lldb::user_id_t GetUID(DIERef ref);

  virtual std::unique_ptr<SymbolFileDWARFDwo>
  GetDwoSymbolFileForCompileUnit(DWARFUnit &dwarf_cu,
                                 const DWARFDebugInfoEntry &cu_die);

  // For regular SymbolFileDWARF instances the method returns nullptr,
  // for the instances of the subclass SymbolFileDWARFDwo
  // the method returns a pointer to the base compile unit.
  virtual DWARFCompileUnit *GetBaseCompileUnit() { return nullptr; }

  virtual llvm::Optional<uint32_t> GetDwoNum() { return llvm::None; }

  static bool
  DIEInDeclContext(const lldb_private::CompilerDeclContext *parent_decl_ctx,
                   const DWARFDIE &die);

  std::vector<lldb_private::CallEdge>
  ParseCallEdgesInFunction(UserID func_id) override;

  void Dump(lldb_private::Stream &s) override;

  void DumpClangAST(lldb_private::Stream &s) override;

  lldb_private::DWARFContext &GetDWARFContext() { return m_context; }

  lldb_private::FileSpec GetFile(DWARFUnit &unit, size_t file_idx);

protected:
  typedef llvm::DenseMap<const DWARFDebugInfoEntry *, lldb_private::Type *>
      DIEToTypePtr;
  typedef llvm::DenseMap<const DWARFDebugInfoEntry *, lldb::VariableSP>
      DIEToVariableSP;
  typedef llvm::DenseMap<const DWARFDebugInfoEntry *,
                         lldb::opaque_compiler_type_t>
      DIEToClangType;
  typedef llvm::DenseMap<lldb::opaque_compiler_type_t, lldb::user_id_t>
      ClangTypeToDIE;

  struct DWARFDataSegment {
    llvm::once_flag m_flag;
    lldb_private::DWARFDataExtractor m_data;
  };

  DISALLOW_COPY_AND_ASSIGN(SymbolFileDWARF);

  const lldb_private::DWARFDataExtractor &
  GetCachedSectionData(lldb::SectionType sect_type,
                       DWARFDataSegment &data_segment);

  virtual void LoadSectionData(lldb::SectionType sect_type,
                               lldb_private::DWARFDataExtractor &data);

  bool DeclContextMatchesThisSymbolFile(
      const lldb_private::CompilerDeclContext *decl_ctx);

  uint32_t CalculateNumCompileUnits() override;

  lldb::CompUnitSP ParseCompileUnitAtIndex(uint32_t index) override;

  lldb_private::TypeList &GetTypeList() override;

  virtual DWARFUnit *
  GetDWARFCompileUnit(lldb_private::CompileUnit *comp_unit);

  DWARFUnit *GetNextUnparsedDWARFCompileUnit(DWARFUnit *prev_cu);

  bool GetFunction(const DWARFDIE &die, lldb_private::SymbolContext &sc);

  lldb_private::Function *ParseFunction(lldb_private::CompileUnit &comp_unit,
                                        const DWARFDIE &die);

  size_t ParseBlocksRecursive(lldb_private::CompileUnit &comp_unit,
                              lldb_private::Block *parent_block,
                              const DWARFDIE &die,
                              lldb::addr_t subprogram_low_pc, uint32_t depth);

  size_t ParseTypes(const lldb_private::SymbolContext &sc, const DWARFDIE &die,
                    bool parse_siblings, bool parse_children);

  lldb::TypeSP ParseType(const lldb_private::SymbolContext &sc,
                         const DWARFDIE &die, bool *type_is_new);

  lldb_private::Type *ResolveTypeUID(const DWARFDIE &die,
                                     bool assert_not_being_parsed);

  lldb_private::Type *ResolveTypeUID(const DIERef &die_ref);

  lldb::VariableSP ParseVariableDIE(const lldb_private::SymbolContext &sc,
                                    const DWARFDIE &die,
                                    const lldb::addr_t func_low_pc);

  size_t ParseVariables(const lldb_private::SymbolContext &sc,
                        const DWARFDIE &orig_die,
                        const lldb::addr_t func_low_pc, bool parse_siblings,
                        bool parse_children,
                        lldb_private::VariableList *cc_variable_list = nullptr);

  bool ClassOrStructIsVirtual(const DWARFDIE &die);

  // Given a die_offset, figure out the symbol context representing that die.
  bool ResolveFunction(const DWARFDIE &die, bool include_inlines,
                       lldb_private::SymbolContextList &sc_list);

  virtual lldb::TypeSP
  FindDefinitionTypeForDWARFDeclContext(const DWARFDeclContext &die_decl_ctx);

  virtual lldb::TypeSP FindCompleteObjCDefinitionTypeForDIE(
      const DWARFDIE &die, lldb_private::ConstString type_name,
      bool must_be_implementation);

  lldb_private::Symbol *
  GetObjCClassSymbol(lldb_private::ConstString objc_class_name);

  lldb::TypeSP GetTypeForDIE(const DWARFDIE &die,
                             bool resolve_function_context = false);

  void SetDebugMapModule(const lldb::ModuleSP &module_sp) {
    m_debug_map_module_wp = module_sp;
  }

  SymbolFileDWARFDebugMap *GetDebugMapSymfile();

  DWARFDIE
  FindBlockContainingSpecification(const DIERef &func_die_ref,
                                   dw_offset_t spec_block_die_offset);

  DWARFDIE
  FindBlockContainingSpecification(const DWARFDIE &die,
                                   dw_offset_t spec_block_die_offset);

  virtual UniqueDWARFASTTypeMap &GetUniqueDWARFASTTypeMap();

  bool DIEDeclContextsMatch(const DWARFDIE &die1, const DWARFDIE &die2);

  bool ClassContainsSelector(const DWARFDIE &class_die,
                             lldb_private::ConstString selector);

  bool FixupAddress(lldb_private::Address &addr);

  typedef std::set<lldb_private::Type *> TypeSet;

  void GetTypes(const DWARFDIE &die, dw_offset_t min_die_offset,
                dw_offset_t max_die_offset, uint32_t type_mask,
                TypeSet &type_set);

  typedef lldb_private::RangeDataVector<lldb::addr_t, lldb::addr_t,
                                        lldb_private::Variable *>
      GlobalVariableMap;

  GlobalVariableMap &GetGlobalAranges();

  void UpdateExternalModuleListIfNeeded();

  virtual DIEToTypePtr &GetDIEToType() { return m_die_to_type; }

  virtual DIEToVariableSP &GetDIEToVariable() { return m_die_to_variable_sp; }

  virtual DIEToClangType &GetForwardDeclDieToClangType() {
    return m_forward_decl_die_to_clang_type;
  }

  virtual ClangTypeToDIE &GetForwardDeclClangTypeToDie() {
    return m_forward_decl_clang_type_to_die;
  }

  void BuildCuTranslationTable();
  llvm::Optional<uint32_t> GetDWARFUnitIndex(uint32_t cu_idx);

  struct DecodedUID {
    SymbolFileDWARF &dwarf;
    DIERef ref;
  };
  llvm::Optional<DecodedUID> DecodeUID(lldb::user_id_t uid);

  SymbolFileDWARFDwp *GetDwpSymbolFile();

  const lldb_private::FileSpecList &GetTypeUnitSupportFiles(DWARFTypeUnit &tu);

  lldb::ModuleWP m_debug_map_module_wp;
  SymbolFileDWARFDebugMap *m_debug_map_symfile;

  llvm::once_flag m_dwp_symfile_once_flag;
  std::unique_ptr<SymbolFileDWARFDwp> m_dwp_symfile;

  lldb_private::DWARFContext m_context;

  DWARFDataSegment m_data_debug_loc;
  DWARFDataSegment m_data_debug_loclists;

  // The unique pointer items below are generated on demand if and when someone
  // accesses them through a non const version of this class.
  std::unique_ptr<DWARFDebugAbbrev> m_abbr;
  std::unique_ptr<DWARFDebugInfo> m_info;
  std::unique_ptr<GlobalVariableMap> m_global_aranges_up;

  typedef std::unordered_map<lldb::offset_t, lldb_private::DebugMacrosSP>
      DebugMacrosMap;
  DebugMacrosMap m_debug_macros_map;

  ExternalTypeModuleMap m_external_type_modules;
  std::unique_ptr<lldb_private::DWARFIndex> m_index;
  bool m_fetched_external_modules : 1;
  lldb_private::LazyBool m_supports_DW_AT_APPLE_objc_complete_type;

  typedef std::set<lldb::user_id_t> DIERefSet;
  typedef llvm::StringMap<DIERefSet> NameToOffsetMap;
  NameToOffsetMap m_function_scope_qualified_name_map;
  std::unique_ptr<DWARFDebugRangesBase> m_ranges;
  std::unique_ptr<DWARFDebugRangesBase> m_rnglists;
  UniqueDWARFASTTypeMap m_unique_ast_type_map;
  DIEToTypePtr m_die_to_type;
  DIEToVariableSP m_die_to_variable_sp;
  DIEToClangType m_forward_decl_die_to_clang_type;
  ClangTypeToDIE m_forward_decl_clang_type_to_die;
  llvm::DenseMap<dw_offset_t, lldb_private::FileSpecList>
      m_type_unit_support_files;
  std::vector<uint32_t> m_lldb_cu_to_dwarf_unit;
};

#endif // SymbolFileDWARF_SymbolFileDWARF_h_
