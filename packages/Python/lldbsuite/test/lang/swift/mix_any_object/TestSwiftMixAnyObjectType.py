# TestSwiftMixAnyObjectType.py
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
Test the AnyObject type in different combinations
"""
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import os
import unittest2


class TestSwiftMixAnyObjectType(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.skipUnlessDarwin
    @decorators.swiftTest
    def test_any_object_type(self):
        """Test the AnyObject type in different combinations"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def do_test(self):
        """Test the AnyObject type in different combinations"""
        exe_name = "a.out"
        exe = self.getBuildArtifact(exe_name)

        # Create the target
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Set the breakpoints
        breakpoint = target.BreakpointCreateBySourceRegex(
            '// break here', self.main_source_spec)
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

        self.expect(
            'frame variable -d run -- cls',
            substrs=['text = "Instance of MyClass"'])
        self.expect(
            'expr -d run -- cls',
            substrs=['text = "Instance of MyClass"'])

        self.expect(
            'frame variable -d run -- any',
            substrs=['text = "Instance of MyClass"'])
        self.expect(
            'expr -d run -- any',
            substrs=['text = "Instance of MyClass"'])

        self.expect(
            'frame variable -d run -- opt',
            substrs=['text = "Instance of MyClass"'])
        self.expect(
            'expr -d run -- opt',
            substrs=['text = "Instance of MyClass"'])

        self.expect(
            'frame variable -d run -- dict',
            substrs=[
                'key = "One"',
                'text = "Instance One"',
                'key = "Three"',
                'text = "Instance of MyClass"',
                'key = "Two"',
                'text = "Instance Two"'])
        self.expect(
            'expr -d run -- dict',
            substrs=[
                'key = "One"',
                'text = "Instance One"',
                'key = "Three"',
                'text = "Instance of MyClass"',
                'key = "Two"',
                'text = "Instance Two"'])

if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
