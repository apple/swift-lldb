"""Test that conflicting symbols in different shared libraries work correctly"""

from __future__ import print_function


import os
import time
import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestConflictingSymbols(TestBase):

    mydir = TestBase.compute_mydir(__file__)
    NO_DEBUG_INFO_TESTCASE = True

    def setUp(self):
        TestBase.setUp(self)

        self.One_line = line_number('One/One.c', '// break here')
        self.Two_line = line_number('Two/Two.c', '// break here')
        self.main_line = line_number('main.c', '// break here')

        lldbutil.mkdir_p(self.getBuildArtifact("One"))
        lldbutil.mkdir_p(self.getBuildArtifact("Two"))

    def test_conflicting_symbols(self):
        self.build()
        exe = self.getBuildArtifact("a.out")
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Register our shared libraries for remote targets so they get
        # automatically uploaded
        environment = self.registerSharedLibrariesWithTarget(
            target, ['One', 'Two'])

        lldbutil.run_break_set_command(
            self, 'breakpoint set -f One.c -l %s' % (self.One_line))
        lldbutil.run_break_set_command(
            self, 'breakpoint set -f Two.c -l %s' % (self.Two_line))
        lldbutil.run_break_set_by_file_and_line(
            self, 'main.c', self.main_line, num_expected_locations=1, loc_exact=True)

        process = target.LaunchSimple(
            None, environment, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)

        # The stop reason of the thread should be breakpoint.
        self.expect("thread list", STOPPED_DUE_TO_BREAKPOINT,
                    substrs=['stopped',
                             'stop reason = breakpoint'])

        self.expect("breakpoint list -f", BREAKPOINT_HIT_ONCE,
                    substrs=[' resolved, hit count = 1'])

        # This should display correctly.
        self.expect(
            "expr (unsigned long long)conflicting_symbol",
            "Symbol from One should be found",
            substrs=[
                "11111"])

        self.runCmd("continue", RUN_SUCCEEDED)

        # The stop reason of the thread should be breakpoint.
        self.expect("thread list", STOPPED_DUE_TO_BREAKPOINT,
                    substrs=['stopped',
                             'stop reason = breakpoint'])

        self.expect("breakpoint list -f", BREAKPOINT_HIT_ONCE,
                    substrs=[' resolved, hit count = 1'])

        self.expect(
            "expr (unsigned long long)conflicting_symbol",
            "Symbol from Two should be found",
            substrs=[
                "22222"])

        self.runCmd("continue", RUN_SUCCEEDED)

        # The stop reason of the thread should be breakpoint.
        self.expect("thread list", STOPPED_DUE_TO_BREAKPOINT,
                    substrs=['stopped',
                             'stop reason = breakpoint'])

        self.expect("breakpoint list -f", BREAKPOINT_HIT_ONCE,
                    substrs=[' resolved, hit count = 1'])

        self.expect(
            "expr (unsigned long long)conflicting_symbol",
            "An error should be printed when symbols can't be ordered",
            error=True,
            substrs=[
                "Multiple internal symbols"])

    @expectedFailureAll(bugnumber="llvm.org/pr35043")
    def test_shadowed(self):
        self.build()
        exe = self.getBuildArtifact("a.out")
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Register our shared libraries for remote targets so they get
        # automatically uploaded
        environment = self.registerSharedLibrariesWithTarget(
            target, ['One', 'Two'])

        lldbutil.run_break_set_by_file_and_line(self, 'main.c', self.main_line)

        process = target.LaunchSimple(
            None, environment, self.get_process_working_directory())
        self.assertTrue(process, PROCESS_IS_VALID)

        # The stop reason of the thread should be breakpoint.
        self.expect("thread list", STOPPED_DUE_TO_BREAKPOINT,
                    substrs=['stopped',
                             'stop reason = breakpoint'])

        # As we are shadowing the conflicting symbol, there should be no
        # ambiguity in this expression.
        self.expect(
            "expr int conflicting_symbol = 474747; conflicting_symbol",
            substrs=[ "474747"])
