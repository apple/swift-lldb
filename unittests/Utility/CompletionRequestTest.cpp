//===-- CompletionRequestTest.cpp -------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "lldb/Utility/CompletionRequest.h"
using namespace lldb_private;

TEST(CompletionRequest, Constructor) {
  std::string command = "a b";
  const unsigned cursor_pos = 2;
  Args args(command);
  const int arg_index = 1;
  const int arg_cursor_pos = 0;
  const int match_start = 2345;
  const int match_max_return = 12345;
  bool word_complete = false;
  StringList matches;
  CompletionRequest request(command, cursor_pos, args, arg_index,
                            arg_cursor_pos, match_start, match_max_return,
                            word_complete, matches);

  EXPECT_STREQ(request.GetRawLine().str().c_str(), command.c_str());
  EXPECT_EQ(request.GetRawCursorPos(), cursor_pos);
  EXPECT_EQ(request.GetCursorIndex(), arg_index);
  EXPECT_EQ(request.GetCursorCharPosition(), arg_cursor_pos);
  EXPECT_EQ(request.GetMatchStartPoint(), match_start);
  EXPECT_EQ(request.GetMaxReturnElements(), match_max_return);
  EXPECT_EQ(request.GetWordComplete(), word_complete);
  // This is the generated matches should be equal to our passed string list.
  EXPECT_EQ(&request.GetMatches(), &matches);
}
