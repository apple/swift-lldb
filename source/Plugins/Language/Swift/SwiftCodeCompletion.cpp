//===--- SwiftCodeCompletion.cpp - Code Completion for Swift ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SwiftCodeCompletion.h"

#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CodeCompletionCache.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Parse/DelayedParsingCallbacks.h"
#include "swift/Subsystems.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

using namespace llvm;
using namespace swift;
using namespace ide;

static std::string toInsertableString(CodeCompletionResult *Result) {
  using ChunkKind = CodeCompletionString::Chunk::ChunkKind;
  std::string Str;
  auto chunks = Result->getCompletionString()->getChunks();
  for (unsigned i = 0; i < chunks.size(); ++i) {
    auto outerChunk = chunks[i];

    // Consume the whole call parameter, keep track of which piece of the call
    // parameter we are in, and emit only pieces of the call parameter that
    // should be inserted into the code buffer.
    if (outerChunk.is(ChunkKind::CallParameterBegin)) {
      ++i;
      auto callParameterSection = ChunkKind::CallParameterBegin;
      bool hasParameterName = false;
      for (; i < chunks.size(); ++i) {
        auto innerChunk = chunks[i];

        // Exit this loop when we're at the end of the call parameter.
        if (innerChunk.endsPreviousNestedGroup(outerChunk.getNestingLevel())) {
          --i;
          break;
        }

        // Keep track of what part of the call parameter we are in.
        if (innerChunk.is(ChunkKind::CallParameterName) ||
            innerChunk.is(ChunkKind::CallParameterInternalName) ||
            innerChunk.is(ChunkKind::CallParameterColon) ||
            innerChunk.is(ChunkKind::CallParameterType) ||
            innerChunk.is(ChunkKind::CallParameterClosureType))
          callParameterSection = innerChunk.getKind();

        if (callParameterSection == ChunkKind::CallParameterName)
          hasParameterName = true;

        // Never emit these parts of the call parameter.
        if (callParameterSection == ChunkKind::CallParameterInternalName ||
            callParameterSection == ChunkKind::CallParameterType ||
            callParameterSection == ChunkKind::CallParameterClosureType)
          continue;

        // Do not emit a colon when the parameter is unnamed.
        if (!hasParameterName && callParameterSection == ChunkKind::CallParameterColon)
          continue;

        if (innerChunk.hasText() && !innerChunk.isAnnotation())
          Str += innerChunk.getText();
      }
      continue;
    }

    if (outerChunk.hasText() && !outerChunk.isAnnotation())
      Str += outerChunk.getText();
  }
  return Str;
}

static std::string toDisplayString(CodeCompletionResult *Result) {
  std::string Str;
  for (auto C : Result->getCompletionString()->getChunks()) {
    if (C.getKind() ==
        CodeCompletionString::Chunk::ChunkKind::BraceStmtWithCursor) {
      Str += ' ';
      continue;
    }
    if (!C.isAnnotation() && C.hasText()) {
      Str += C.getText();
      continue;
    }
    if (C.getKind() == CodeCompletionString::Chunk::ChunkKind::TypeAnnotation) {
      if (Result->getKind() == CodeCompletionResult::Declaration) {
        switch (Result->getAssociatedDeclKind()) {
        case CodeCompletionDeclKind::Module:
        case CodeCompletionDeclKind::PrecedenceGroup:
        case CodeCompletionDeclKind::Class:
        case CodeCompletionDeclKind::Struct:
        case CodeCompletionDeclKind::Enum:
          continue;

        case CodeCompletionDeclKind::EnumElement:
          Str += ": ";
          break;

        case CodeCompletionDeclKind::Protocol:
        case CodeCompletionDeclKind::TypeAlias:
        case CodeCompletionDeclKind::AssociatedType:
        case CodeCompletionDeclKind::GenericTypeParam:
        case CodeCompletionDeclKind::Constructor:
        case CodeCompletionDeclKind::Destructor:
          continue;

        case CodeCompletionDeclKind::Subscript:
        case CodeCompletionDeclKind::StaticMethod:
        case CodeCompletionDeclKind::InstanceMethod:
        case CodeCompletionDeclKind::PrefixOperatorFunction:
        case CodeCompletionDeclKind::PostfixOperatorFunction:
        case CodeCompletionDeclKind::InfixOperatorFunction:
        case CodeCompletionDeclKind::FreeFunction:
          Str += " -> ";
          break;

        case CodeCompletionDeclKind::StaticVar:
        case CodeCompletionDeclKind::InstanceVar:
        case CodeCompletionDeclKind::LocalVar:
        case CodeCompletionDeclKind::GlobalVar:
          Str += ": ";
          break;
        }
      } else {
        Str += ": ";
      }
      Str += C.getText();
    }
  }
  return Str;
}

namespace lldb_private {

class CodeCompletionConsumer : public SimpleCachingCodeCompletionConsumer {
  CompletionResponse &Response;

public:
  CodeCompletionConsumer(CompletionResponse &Response) : Response(Response) {}

  void handleResults(MutableArrayRef<CodeCompletionResult *> Results) override {
    CodeCompletionContext::sortCompletionResults(Results);
    for (auto *Result : Results) {
      Response.Matches.push_back(
          {toDisplayString(Result), toInsertableString(Result)});
    }
  }
};

/// Calculates completions at the end of `EnteredCode`.
static unsigned
doCodeCompletion(SourceFile &SF, StringRef EnteredCode,
                 CodeCompletionCallbacksFactory *CompletionCallbacksFactory) {
  ASTContext &Ctx = SF.getASTContext();
  DiagnosticTransaction DelayedDiags(Ctx.Diags);

  std::string AugmentedCode = EnteredCode.str();
  AugmentedCode += '\0';
  const unsigned BufferID =
      Ctx.SourceMgr.addMemBufferCopy(AugmentedCode, "<REPL Input>");

  const unsigned CodeCompletionOffset = AugmentedCode.size() - 1;

  Ctx.SourceMgr.setCodeCompletionPoint(BufferID, CodeCompletionOffset);

  const unsigned OriginalDeclCount = SF.Decls.size();

  PersistentParserState PersistentState(Ctx);
  std::unique_ptr<DelayedParsingCallbacks> DelayedCB(
      new CodeCompleteDelayedCallbacks(Ctx.SourceMgr.getCodeCompletionLoc()));
  bool Done;
  do {
    parseIntoSourceFile(SF, BufferID, &Done, nullptr, &PersistentState,
                        DelayedCB.get());
  } while (!Done);
  performTypeChecking(SF, PersistentState.getTopLevelContext(), None,
                      OriginalDeclCount);

  performDelayedParsing(&SF, PersistentState, CompletionCallbacksFactory);

  SF.Decls.resize(OriginalDeclCount);
  DelayedDiags.abort();

  return BufferID;
}

static void AddSourceFile(ASTContext &Ctx, ModuleDecl *Module,
                          SourceFileKind Kind) {
  Optional<unsigned> BufferID;
  SourceFile *SF = new (Ctx) SourceFile(
      *Module, Kind, BufferID, SourceFile::ImplicitModuleImportKind::Stdlib,
      /*Keep tokens*/ false);
  SF->ASTStage = SourceFile::TypeChecked;
  Module->addFile(*SF);
}

static SourceFile *GetSingleSourceFile(ModuleDecl *Module,
                                       SourceFileKind Kind) {
  SourceFile *Result = nullptr;
  for (auto *File : Module->getFiles()) {
    auto *SF = dyn_cast<SourceFile>(File);
    if (!SF)
      continue;
    if (SF->Kind != Kind)
      continue;
    assert(!Result && "multiple source files of the requested kind");
    Result = SF;
  }
  return Result;
}

CompletionResponse
SwiftCompleteCode(SwiftASTContext &SwiftCtx,
                  SwiftPersistentExpressionState &PersistentExpressionState,
                  StringRef EnteredCode) {
  Status Error;
  ASTContext &Ctx = *SwiftCtx.GetASTContext();

  // == Prepare a module and source files ==

  // Get or create the module that we do completions in.
  static ConstString CompletionsModuleName("completions");
  ModuleDecl *CompletionsModule =
      SwiftCtx.GetModule(CompletionsModuleName, Error);
  if (!CompletionsModule) {
    CompletionsModule = SwiftCtx.CreateModule(CompletionsModuleName, Error);
    if (!CompletionsModule)
      return CompletionResponse::error("could not make completions module");

    // This file accumulates all of the "hand imports" (imports that the user
    // made in previous executions). We also put the current entered code in
    // this file.
    AddSourceFile(Ctx, CompletionsModule, SourceFileKind::REPL);

    // We reset this file with the persistent decls every completion request.
    AddSourceFile(Ctx, CompletionsModule, SourceFileKind::Library);
  }

  // This file accumulates all of the "hand imports" (imports that the user
  // made in previous executions). We also put the current entered code in
  // this file.
  SourceFile *EnteredCodeFile =
      GetSingleSourceFile(CompletionsModule, SourceFileKind::REPL);
  assert(EnteredCodeFile);

  // Accumulate new hand imports into the file.
  {
    // First, construct a set of imports that we already have, so that we can
    // avoid duplicate imports.
    std::set<ModuleDecl *> ExistingImportSet;
    SmallVector<ModuleDecl::ImportedModule, 8> ExistingImports;
    EnteredCodeFile->getImportedModules(ExistingImports,
                                        ModuleDecl::ImportFilter::All);
    for (auto &ExistingImport : ExistingImports)
      ExistingImportSet.insert(std::get<1>(ExistingImport));

    // Next, add new imports into the file.
    SmallVector<SourceFile::ImportedModuleDesc, 8> NewImports;
    PersistentExpressionState.RunOverHandLoadedModules(
        [&](const ConstString ModuleName) -> bool {
          // Skip these modules, to prevent "TestSwiftCompletions.py" from
          // segfaulting.
          // TODO: Investigate why this happens.
          if (ModuleName == ConstString("SwiftOnoneSupport"))
            return true;
          if (ModuleName == ConstString("a"))
            return true;

          ModuleDecl *Module = SwiftCtx.GetModule(ModuleName, Error);
          if (!Module)
            return true;
          if (ExistingImportSet.find(Module) != ExistingImportSet.end())
            return true;
          NewImports.push_back(SourceFile::ImportedModuleDesc(
              std::make_pair(ModuleDecl::AccessPathTy(), Module),
              SourceFile::ImportOptions()));
        });
    EnteredCodeFile->addImports(NewImports);
  }

  // We reset this file with the persistent decls every completion request.
  SourceFile *PreviousDeclsFile =
      GetSingleSourceFile(CompletionsModule, SourceFileKind::Library);
  assert(PreviousDeclsFile);

  // Reset the decls to the persistent decls.
  {
    std::vector<Decl *> PersistentDecls;
    PersistentExpressionState.GetAllDecls(PersistentDecls);
    PreviousDeclsFile->Decls.clear();
    for (auto *PersistentDecl : PersistentDecls)
      PreviousDeclsFile->Decls.push_back(PersistentDecl);
    PreviousDeclsFile->clearLookupCache();
  }

  // `PreviousDeclsFile` might now contain decls that a re-defined in
  // `EnteredCode`. We want to remove these so that the completion results only
  // include results from the new definitions. To do this, we parse the
  // `EnteredCode` to get a list of decls and then remove these decls from
  // `PreviousDeclsFile`.
  {
    // Parse `EnteredCode` to populate `NewDecls`.
    DenseMap<Identifier, SmallVector<ValueDecl *, 1>> NewDecls;
    DiagnosticTransaction DelayedDiags(Ctx.Diags);
    std::string AugmentedCode = EnteredCode.str();
    AugmentedCode += '\0';
    const unsigned BufferID =
        Ctx.SourceMgr.addMemBufferCopy(AugmentedCode, "<REPL Input>");
    const unsigned OriginalDeclCount = EnteredCodeFile->Decls.size();
    PersistentParserState PersistentState(Ctx);
    bool Done;
    do {
      parseIntoSourceFile(*EnteredCodeFile, BufferID, &Done, nullptr,
                          &PersistentState, nullptr);
    } while (!Done);
    for (auto it = EnteredCodeFile->Decls.begin() + OriginalDeclCount;
         it != EnteredCodeFile->Decls.end(); it++)
      if (auto *NewValueDecl = dyn_cast<ValueDecl>(*it))
        NewDecls.FindAndConstruct(NewValueDecl->getBaseName().getIdentifier())
            .second.push_back(NewValueDecl);
    EnteredCodeFile->Decls.resize(OriginalDeclCount);
    DelayedDiags.abort();

    // Subtract `NewDecls` from the decls in `PreviousDeclsFile`.
    auto ContainedInNewDecls = [&](Decl *OldDecl) -> bool {
      auto *OldValueDecl = dyn_cast<ValueDecl>(OldDecl);
      if (!OldValueDecl)
        return false;
      auto NewDeclsLookup =
          NewDecls.find(OldValueDecl->getBaseName().getIdentifier());
      if (NewDeclsLookup == NewDecls.end())
        return false;
      for (auto *NewDecl : NewDeclsLookup->second)
        if (conflicting(NewDecl->getOverloadSignature(),
                        OldValueDecl->getOverloadSignature()))
          return true;
      return false;
    };
    PreviousDeclsFile->Decls.erase(
        std::remove_if(PreviousDeclsFile->Decls.begin(),
                       PreviousDeclsFile->Decls.end(), ContainedInNewDecls),
        PreviousDeclsFile->Decls.end());
    PreviousDeclsFile->clearLookupCache();
  }

  // == Compute the completions ==

  // Set up `Response` to collect results, and set up a callback handler that
  // puts the results into `Response`.
  CompletionResponse Response;
  CodeCompletionConsumer Consumer(Response);
  CodeCompletionCache CompletionCache;
  CodeCompletionContext CompletionContext(CompletionCache);
  std::unique_ptr<CodeCompletionCallbacksFactory> CompletionCallbacksFactory(
      ide::makeCodeCompletionCallbacksFactory(CompletionContext, Consumer));

  // Not sure what this first call to `doCodeCompletion` is for. It seems not to
  // return any results, but perhaps it prepares the buffer in some useful way
  // so that the next call works.
  const unsigned BufferID = doCodeCompletion(*EnteredCodeFile, EnteredCode,
                                             CompletionCallbacksFactory.get());

  // Now we tokenize it, and we treat the last token as a prefix for the
  // completion that we are looking for. We request completions for the code
  // with the last token removed. This gives us a bunch of completions that fit
  // in the context where the last token is, but these completions are not
  // filtered to match the prefix. So we filter them.
  auto Tokens = tokenize(Ctx.LangOpts, Ctx.SourceMgr, BufferID);
  if (!Tokens.empty() && Tokens.back().is(tok::code_complete))
    Tokens.pop_back();
  if (!Tokens.empty()) {
    Token &LastToken = Tokens.back();
    if (LastToken.is(tok::identifier) || LastToken.isKeyword()) {
      Response.Prefix = LastToken.getText();
      const unsigned Offset =
          Ctx.SourceMgr.getLocOffsetInBuffer(LastToken.getLoc(), BufferID);
      doCodeCompletion(*EnteredCodeFile, EnteredCode.substr(0, Offset),
                       CompletionCallbacksFactory.get());

      std::vector<CompletionMatch> FilteredMatches;
      for (auto &Match : Response.Matches) {
        if (!StringRef(Match.Insertable).startswith(Response.Prefix))
          continue;
        FilteredMatches.push_back(
            {Match.Display, Match.Insertable.substr(Response.Prefix.size())});
      }
      Response.Matches = FilteredMatches;
    }
  }

  return Response;
}

} // namespace lldb_private
