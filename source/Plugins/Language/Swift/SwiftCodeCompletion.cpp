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

CompletionResponse SwiftCompleteCode(SourceFile &SF, StringRef EnteredCode) {
  ASTContext &Ctx = SF.getASTContext();

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
  const unsigned BufferID =
      doCodeCompletion(SF, EnteredCode, CompletionCallbacksFactory.get());

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
      doCodeCompletion(SF, EnteredCode.substr(0, Offset),
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
