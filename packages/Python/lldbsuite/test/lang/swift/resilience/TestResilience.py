# TestResilience.py
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
Test that resilient APIs work regardless of the combination of library and executable
"""
import commands
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import os
import os.path
import time
import unittest2


def execute_command(command):
    # print '%% %s' % (command)
    (exit_status, output) = commands.getstatusoutput(command)
    # if output:
    #     print output
    # print 'status = %u' % (exit_status)
    return exit_status


class TestResilience(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.skipUnlessDarwin
    @decorators.swiftTest
    @decorators.skipIf(debug_info=decorators.no_match("dsym"))
    def test_cross_module_extension(self):
        """Test that LLDB can debug across resilient boundaries"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)

    def createSymlinks(self, exe_flavor, mod_flavor):
        execute_command("ln -sf " + self.getBuildArtifact("main." + exe_flavor) + " " + self.getBuildArtifact("main"))
        execute_command("ln -sf " + self.getBuildArtifact("main." + exe_flavor + ".dSYM") + " " + self.getBuildArtifact("main.dSYM"))

        execute_command("ln -sf " + self.getBuildArtifact("libmod." + exe_flavor + ".dylib") + " " + self.getBuildArtifact("libmod.dylib"))
        execute_command("ln -sf " + self.getBuildArtifact("libmod." + exe_flavor + ".dylib.dSYM") + " " + self.getBuildArtifact("libmod.dylib.dSYM"))

        execute_command("ln -sf " + self.getBuildArtifact("mod." + exe_flavor + ".swiftdoc") + " " + self.getBuildArtifact("mod.swiftdoc"))
        execute_command("ln -sf " + self.getBuildArtifact("mod." + exe_flavor + ".swiftmodule") + " " + self.getBuildArtifact("mod.swiftmodule"))

    def cleanupSymlinks(self):
        execute_command(
            "rm " +
            self.getBuildArtifact("main") + " " +
            self.getBuildArtifact("main.dSYM") + " " +
            self.getBuildArtifact("libmod.dylib") + " " +
            self.getBuildArtifact("libmod.dylib.dSYM") + " " +
            self.getBuildArtifact("mod.swiftdoc") + " " +
            self.getBuildArtifact("mod.swiftmodule"))

    def doTestWithFlavor(self, exe_flavor, mod_flavor):
        self.createSymlinks(exe_flavor, mod_flavor)

        exe_name = "main"
        exe_path = self.getBuildArtifact(exe_name)

        source_name = "main.swift"
        source_spec = lldb.SBFileSpec(source_name)

        # Create the target
        target = self.dbg.CreateTarget(exe_path)
        self.assertTrue(target, VALID_TARGET)

        breakpoint = target.BreakpointCreateBySourceRegex('break', source_spec)
        self.assertTrue(breakpoint.GetNumLocations() > 1, VALID_BREAKPOINT)

        process = target.LaunchSimple(None, None, os.getcwd())
        self.assertTrue(process, PROCESS_IS_VALID)

        threads = lldbutil.get_threads_stopped_at_breakpoint(
            process, breakpoint)

        self.assertTrue(len(threads) == 1)
        self.thread = threads[0]
        self.frame = self.thread.frames[0]
        self.assertTrue(self.frame, "Frame 0 is valid.")

        # FIXME: this should work with all flavors!
        if exe_flavor == "a":
            self.expect("target var global", DATA_TYPES_DISPLAYED_CORRECTLY,
                        substrs=["a = 1"])
        process.Continue()

        self.assertTrue(len(threads) == 1)
        self.thread = threads[0]
        self.frame = self.thread.frames[0]
        self.assertTrue(self.frame, "Frame 0 is valid.")
        
        # Try 'frame variable'
        var = self.frame.FindVariable("s")
        child = var.GetChildMemberWithName("a")
        lldbutil.check_variable(self, child, False, value="1")

        # Try the expression parser
        self.expect("expr s.a", DATA_TYPES_DISPLAYED_CORRECTLY, substrs=["1"])
        self.expect(
            "expr fA(s)",
            DATA_TYPES_DISPLAYED_CORRECTLY,
            substrs=["1"])

        process.Kill()

        self.cleanupSymlinks()

    def do_test(self):
        """Test that LLDB can debug across resilient boundaries"""

        for exe_flavor in ["a", "b"]:
            for mod_flavor in ["a", "b"]:
                self.doTestWithFlavor(exe_flavor, mod_flavor)

if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
