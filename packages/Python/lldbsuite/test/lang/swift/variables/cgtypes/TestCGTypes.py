# TestCGTypes.py
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
Test that we are able to properly format basic CG types
"""
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import os
import unittest2


class TestSwiftCoreGraphicsTypes(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.swiftTest
    @decorators.skipUnlessDarwin
    @decorators.add_test_categories(["swiftpr"])
    def test_swift_coregraphics_types(self):
        """Test that we are able to properly format basic CG types"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def do_test(self):
        """Test that we are able to properly format basic CG types"""
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
        self.thread = threads[0]
        self.frame = self.thread.frames[0]
        self.assertTrue(self.frame, "Frame 0 is valid.")

        self.expect('frame variable f', substrs=[' = 1'])
        self.expect('frame variable p', substrs=[' = (x = 1, y = 1)'])
        self.expect('frame variable r', substrs=[
            ' = (origin = (x = 0, y = 0), size = (width = 0, height = 0))'])

        self.expect('expr f', substrs=[' = 1'])
        self.expect('expr p', substrs=[' = (x = 1, y = 1)'])
        self.expect(
            'expr r',
            substrs=[' = (origin = (x = 0, y = 0), size = (width = 0, height = 0))'])

        self.expect('po f', substrs=['1.0'])
        self.expect('po p', substrs=['x : 1.0', 'y : 1.0'])
        self.expect(
            'po r',
            substrs=[
                'x : 0.0',
                'y : 0.0',
                'width : 0.0',
                'height : 0.0'])


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
