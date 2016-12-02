# TestSwiftFrameworkSearchPaths.py
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
Tests that we can import modules located using
target.swift-framework-search-paths
"""

import re
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import unittest2

class TestSwiftFrameworkSearchPaths(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    def setUp(self):
        TestBase.setUp(self)
        system([["make", "_framework"]])
        self.addTearDownHook(lambda: system([["make", "cleanup"]]))
        
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)
    
    @decorators.swiftTest
    @decorators.skipUnlessDarwin
    def test_swift_framework_search_paths(self):
        """
        Tests that we can import modules located using
        target.swift-framework-search-paths
        """
        
        # Build and run the dummy target
        self.build()
        
        exe_name = "a.out"
        exe = os.path.join(os.getcwd(), exe_name)

        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        a_breakpoint = target.BreakpointCreateBySourceRegex(
            'break here', self.main_source_spec)
        self.assertTrue(a_breakpoint.GetNumLocations() > 0, VALID_BREAKPOINT)

        process = target.LaunchSimple(None, None, os.getcwd())
        self.assertTrue(process, PROCESS_IS_VALID)

        threads = lldbutil.get_threads_stopped_at_breakpoint(
            process, a_breakpoint)

        self.assertTrue(len(threads) == 1)
        self.thread = threads[0]
        self.frame = self.thread.frames[0]
        self.assertTrue(self.frame, "Frame 0 is valid.")
        

        # Add the framework dir to the swift-framework-search-paths
        self.runCmd(
            "settings append target.swift-framework-search-paths .")
        
        # Check that we can find and import the Framework
        self.runCmd("e import Framework")
        
        # Check that we can call the function defined in the Framework
        self.expect("e plusTen(10)", r"\(Int\) \$R0 = 20")


if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
