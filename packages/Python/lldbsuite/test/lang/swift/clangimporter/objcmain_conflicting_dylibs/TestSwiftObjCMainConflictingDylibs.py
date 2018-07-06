# TestSwiftObjCMainConflictingDylibs.py
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2018 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------------

import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import os
import unittest2
import shutil

class TestSwiftObjCMainConflictingDylibs(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    def setUp(self):
        TestBase.setUp(self)

    @decorators.skipUnlessDarwin
    @decorators.swiftTest
    @decorators.add_test_categories(["swiftpr"])
    def test(self):
        # To ensure we hit the rebuild problem remove the cache to avoid caching.
        mod_cache = self.getBuildArtifact("my-clang-modules-cache")
        if os.path.isdir(mod_cache):
          shutil.rmtree(mod_cache)

        self.runCmd('settings set symbols.clang-modules-cache-path "%s"'
                    % mod_cache)
        # This test relies on particular failure modes of the 
        # expression parser in full mode.  To get that we need
        # to turn off the "frame var for simple expressions" mode
        # since it behaves better than plain expr in the face of
        # this sort of failure.
        self.runCmd('settings set target.frame-var-accelerates-simple-expr 0')
        self.build()
        exe_name = "a.out"
        exe = self.getBuildArtifact(exe_name)

        # Create the target
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Set the breakpoints
        bar_breakpoint = target.BreakpointCreateBySourceRegex(
            'break here', lldb.SBFileSpec('Bar.swift'))
        foo_breakpoint = target.BreakpointCreateBySourceRegex(
            'break here', lldb.SBFileSpec('Foo.swift'))

        process = target.LaunchSimple(None, None, os.getcwd())
        process.Continue()
        # Prime the module cache with the Foo variant.
        self.expect("fr var baz", "correct baz", substrs=["i_am_from_Foo"])
        self.assertTrue(os.path.isdir(mod_cache), "module cache exists")

        # Restart.
        process = target.LaunchSimple(None, None, os.getcwd())
        # This is failing because the Target-SwiftASTContext uses the
        # amalgamated target header search options from all dylibs.
        self.expect("p baz", "wrong baz", substrs=["i_am_from_Foo"])
        self.expect("fr var baz", "wrong baz", substrs=["i_am_from_Foo"])


        process.Continue()
        self.expect("p baz", "correct baz", substrs=["i_am_from_Foo"])
        self.expect("fr var baz", "correct baz", substrs=["i_am_from_Foo"])
        

if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
