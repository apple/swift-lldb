# TestDefiningOverloadedFunctions.py
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------------
"""
Tests that we can define overloaded functions in the expression parser/REPL
"""
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import os
import unittest2


class TestDefiningOverloadedFunctions(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.swiftTest
    def test_simple_overload_expressions(self):
        """Tests that we can define overloaded functions"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def check_expression(self, expression, expected_result, use_summary=True):
        value = self.frame.EvaluateExpression(expression)
        self.assertTrue(value.IsValid(), expression + "returned a valid value")
        if self.TraceOn():
            print value.GetSummary()
            print value.GetValue()
        if use_summary:
            answer = value.GetSummary()
        else:
            answer = value.GetValue()
        report_str = "%s expected: %s got: %s" % (
            expression, expected_result, answer)
        self.assertTrue(answer == expected_result, report_str)

    def do_test(self):
        """Test defining overloaded functions"""
        exe_name = "a.out"
        exe = self.getBuildArtifact(exe_name)

        # Create the target
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Set the breakpoints
        breakpoint = target.BreakpointCreateBySourceRegex(
            'Stop here to do your work.', self.main_source_spec)
        self.assertTrue(breakpoint.GetNumLocations() > 0, VALID_BREAKPOINT)

        # Launch the process, and do not stop at the entry point.
        process = target.LaunchSimple(None, None, os.getcwd())

        self.assertTrue(process, PROCESS_IS_VALID)

        # Frame #0 should be at our breakpoint.
        threads = lldbutil.get_threads_stopped_at_breakpoint(
            process, breakpoint)

        self.assertTrue(len(threads) == 1)
        self.thread = threads[0]
        self.frame = self.thread.frames[0]
        self.assertTrue(self.frame, "Frame 0 is valid.")

        # Here's the first function:
        value_obj = self.frame.EvaluateExpression(
            "func $overload(_ a: Int) -> Int { return 1 }\n 1")
        error = value_obj.GetError()
        self.assertTrue(error.Success())

        self.check_expression("$overload(10)", "1", False)

        # Here's the second function:
        value_obj = self.frame.EvaluateExpression(
            "func $overload(_ a: String) -> Int { return 2 } \n 1")
        error = value_obj.GetError()
        self.assertTrue(error.Success())

        self.check_expression('$overload(10)', '1', False)
        self.check_expression('$overload("some string")', '2', False)

if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
