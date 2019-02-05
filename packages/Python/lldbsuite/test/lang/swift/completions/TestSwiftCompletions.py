# TestSwiftCompletions.py
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2019 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------------

import lldb
from lldbsuite.test.lldbtest import *
import unittest2


class TestSwiftCompletions(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.swiftTest
    @decorators.add_test_categories(["swiftpr"])
    def test_completions(self):
        self.build()
        self.do_test()

    def evaluate(self, code):
        # TODO(TF-107): Remove this workaround.
        code = '#sourceLocation(file: "test", line: 1)\n' + code

        result = self.target.EvaluateExpression(code, self.expr_opts)
        if result.error.type not in [lldb.eErrorTypeInvalid,
                                     lldb.eErrorTypeGeneric]:
          raise Exception("while evaluating:\n%s\n\nerror: %s" % (
              code, str(result.error)))

    def assertCompletions(self, code, expected_completions):
        sbcompletions = self.target.CompleteCode(self.swift_lang, None, code)
        completions = set()
        for i in range(sbcompletions.GetNumMatches()):
            completions.add(
                sbcompletions.GetMatchAtIndex(i).GetInsertable())
        self.assertTrue(completions == expected_completions,
                        "expected: %s, got: %s" % (str(expected_completions),
                                                   str(completions)))

    def assertDisplayCompletions(self, code, expected_completions):
        sbcompletions = self.target.CompleteCode(self.swift_lang, None, code)
        completions = set()
        for i in range(sbcompletions.GetNumMatches()):
            completions.add(
                sbcompletions.GetMatchAtIndex(i).GetDisplay())
        self.assertTrue(completions == expected_completions,
                        "expected: %s, got: %s" % (str(expected_completions),
                                                   str(completions)))

    def do_test(self):
        exe_name = "a.out"
        exe = self.getBuildArtifact(exe_name)

        self.expr_opts = lldb.SBExpressionOptions()
        self.swift_lang = lldb.SBLanguageRuntime.GetLanguageTypeFromString(
            "swift")
        self.expr_opts.SetLanguage(self.swift_lang)

        # REPL mode is necessary for previously-evaluated decls to persist.
        self.expr_opts.SetREPLMode(True)

        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)
        self.target = self.dbg.CreateTarget(exe)
        self.assertTrue(self.target, VALID_TARGET)
        self.breakpoint = self.target.BreakpointCreateBySourceRegex(
            "Set breakpoint here", self.main_source_spec)
        self.assertTrue(self.breakpoint.GetNumLocations() > 0, VALID_BREAKPOINT)
        self.process = self.target.LaunchSimple(None, None, os.getcwd())
        self.assertTrue(self.process, PROCESS_IS_VALID)

        # === Simple completions involving a custom struct ===

        self.evaluate("""
                      struct SimpleStruct {
                        let intfield: Int = 10
                        let strfield: String = "hello world"
                      }""")
        self.assertCompletions(
            """let s = SimpleSt""",
            set(["ruct"]))

        self.evaluate("""let simpleValue = SimpleStruct()""")
        self.assertCompletions(
            """simpleVa""",
            set(["lue"]))
        self.assertCompletions(
            """simpleValue.""",
            set(["intfield", "strfield", "self"]))

        # === Redefining a value ===

        self.evaluate("""
                      struct DifferentStruct {
                        let differentfield: Int = 1
                      }""")
        self.evaluate("""let replacedValue = SimpleStruct()""")
        # TODO: If you put the quotes that are after "replacedValue." on the
        # next line, then this completion request segfaults. Perhaps newlines
        # and/or whitespace confuse the completer? Investigate.
        self.assertCompletions(
            """
            let replacedValue = DifferentStruct()
            replacedValue.""",
            set(["differentfield", "self"]))
        self.evaluate("""let replacedValue = DifferentStruct()""")
        self.assertCompletions(
            """replacedValue.""",
            set(["differentfield", "self"]))

        # === Redefining a type ===

        self.evaluate("""
                      struct ReplacedStruct {
                        let field1: Int = 1
                      }
                      """)
        self.assertCompletions(
            """
            struct ReplacedStruct {
              let field2: Int = 2
            }
            ReplacedStruct().""",
            set(["field2", "self"]))
        self.evaluate("""
                      struct ReplacedStruct {
                        let field2: Int = 2
                      }
                      """)
        self.assertCompletions(
            """ReplacedStruct().""",
            set(["field2", "self"]))

        # === Redefining a func ===

        self.evaluate("""
                      func replacedFunc(arglabel: Int) {}
                      """)
        self.assertDisplayCompletions(
            """
            func replacedFunc(arglabel: Int) -> Int { return 42 }
            replacedFun""",
            set(["replacedFunc(arglabel: Int) -> Int"]))
        self.assertDisplayCompletions(
            """
            func replacedFunc(differentArglabel: Int) {}
            replacedFun""",
            set(["replacedFunc(arglabel: Int) -> Void",
                 "replacedFunc(differentArglabel: Int) -> Void"]))
        self.evaluate("""
                      func replacedFunc(arglabel: Int) -> Int { return 42 }
                      """)
        # Huh? Both versions of the func show up in the completion below. Let's
        # keep the test to document the current behavior, and we can figure out
        # later why it happens.
        self.assertDisplayCompletions(
            """replacedFun""",
            set(["replacedFunc(arglabel: Int) -> Void",
                 "replacedFunc(arglabel: Int) -> Int"]))
        self.evaluate("""
                      func replacedFunc(differentArglabel: Int) {}
                      """)
        self.assertDisplayCompletions(
            """replacedFun""",
            set(["replacedFunc(arglabel: Int) -> Void",
                 "replacedFunc(arglabel: Int) -> Int",
                 "replacedFunc(differentArglabel: Int) -> Void"]))

        # TODO: Test imports.
