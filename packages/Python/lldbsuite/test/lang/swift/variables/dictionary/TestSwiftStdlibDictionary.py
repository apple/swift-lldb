# TestSwiftStdlibDictionary.py
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
Tests that we properly vend synthetic children for Swift.Dictionary
"""
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import os
import unittest2


class TestSwiftStdlibDictionary(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.swiftTest
    @decorators.add_test_categories(["swiftpr"])
    def test_swift_stdlib_dictionary(self):
        """Tests that we properly vend synthetic children for Swift.Dictionary"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def get_variable(self, name):
        var = self.frame.FindVariable(
            name).GetDynamicValue(lldb.eDynamicCanRunTarget)
        var.SetPreferSyntheticValue(True)
        return var

    def find_dictionary_entry(
            self,
            vdict,
            key_summary=None,
            key_value=None,
            value_summary=None,
            value_value=None,
            fail_on_missing=True):
        self.assertTrue(vdict.IsValid(), "invalid Dictionary")
        count = vdict.GetNumChildren()
        found = False
        for i in range(0, count):
            child = vdict.GetChildAtIndex(i)
            if child.IsValid():
                key = child.GetChildMemberWithName("key")
                value = child.GetChildMemberWithName("value")

                key_match = False
                value_match = False

                if key_value:
                    if key.GetValue() == key_value:
                        key_match = True
                elif key_summary:
                    if key.GetSummary() == key_summary:
                        key_match = True

                if value_value:
                    if value.GetValue() == value_value:
                        value_match = True
                elif value_summary:
                    if value.GetSummary() == value_summary:
                        value_match = True

                if key_match and value_match:
                    found = True
                    break

        if key_value:
            key_str = str(key_value)
        else:
            key_str = str(key_summary)

        if value_value:
            value_str = str(value_value)
        else:
            value_str = str(value_summary)

        if fail_on_missing:
            self.assertTrue(
                found, ("could not find an expected child for '%s':'%s'" %
                        (key_str, value_str)))
        else:
            self.assertFalse(
                found, ("found a not expected child for '%s':'%s'" %
                        (key_str, value_str)))

    def do_test(self):
        """Tests that we properly vend synthetic children for Swift.Dictionary"""
        exe_name = "a.out"
        exe = os.path.join(os.getcwd(), exe_name)

        # Create the target
        target = self.dbg.CreateTarget(exe)
        self.assertTrue(target, VALID_TARGET)

        # Set the breakpoints
        breakpoint = target.BreakpointCreateBySourceRegex(
            'break here', self.main_source_spec)
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

        # This is the function to remove the custom formats in order to have a
        # clean slate for the next test case.
        def cleanup():
            self.runCmd("type summary delete a.Wrapper", check=False)

        # Execute the cleanup function during test case tear down.
        self.addTearDownHook(cleanup)

        self.runCmd('type summary add a.Wrapper -s ${var.value%S}')

        for i in range(0, 100):
            self.find_dictionary_entry(
                self.get_variable("d"),
                key_value=str(i),
                value_summary='"%s"' % (i * 2 + 1))

        self.runCmd('expression d.removeValue(forKey: 34)')
        self.find_dictionary_entry(
            self.get_variable("d"),
            key_value=34,
            value_summary='"43"',
            fail_on_missing=False)

if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
