# TestSwiftBool.py
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
Test that we can inspect Swift Bools - they are 8 bit entities with only the
LSB significant.  Make sure that works.
"""
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import unittest2


class TestSwiftBool(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.swiftTest
    def test_swift_bool(self):
        """Test that we can inspect various Swift bools"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def do_test(self):
        """Tests that we can break and display simple types"""
        exe_name = "a.out"
        exe = os.path.join(os.getcwd(), exe_name)

        # Create the target
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Set the breakpoints
        breakpoint = target.BreakpointCreateBySourceRegex(
            'Set breakpoint here', self.main_source_spec)
        self.assertTrue(breakpoint.GetNumLocations() > 0, VALID_BREAKPOINT)

        # Launch the process, and do not stop at the entry point.
        process = target.LaunchSimple(None, None, os.getcwd())

        self.assertTrue(process, PROCESS_IS_VALID)

        # Frame #0 should be at our breakpoint.
        threads = lldbutil.get_threads_stopped_at_breakpoint(
            process, breakpoint)

        self.assertTrue(len(threads) == 1)
        thread = threads[0]
        
        frame = thread.frames[0]
        self.assertTrue(frame.IsValid(), "Couldn't get a frame.")
        
        true_vars = ["reg_true", "odd_true", "odd_true_works", "odd_false_works"]
        for name in true_vars:
            var = frame.FindVariable(name)
            summary = var.GetSummary()
            self.assertTrue(summary == "true", "%s should be true, was: %s"%(name, summary))

        false_vars = ["reg_false", "odd_false"]
        for name in false_vars:
            var = frame.FindVariable(name)
            summary = var.GetSummary()
            self.assertTrue(summary == "false", "%s should be false, was: %s"%(name, summary))


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
