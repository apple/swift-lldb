# TestMainExecutableResilient.py
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
"""
Test `@_implementationOnly import` in the main executable with a resilient library
"""
import commands
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil
import os
import os.path
import time
import unittest2

class TestMainExecutable(TestBase):

    mydir = TestBase.compute_mydir(__file__)
    
    @swiftTest
    def test_implementation_only_import_main_executable_resilient(self):
        """Test `@_implementationOnly import` in the main executable with a resilient library"""
        self.build()
        def cleanup():
            lldbutil.execute_command("make cleanup")
        self.addTearDownHook(cleanup)
        lldbutil.run_to_source_breakpoint(self, "break here", lldb.SBFileSpec("main.swift"))

        self.expect("fr var", substrs=[
            "(SomeLibrary.TwoInts) value = (first = 2, second = 3)",
            "(main.ContainsTwoInts) container = {\n  wrapped = (first = 2, second = 3)\n  other = 10\n}"])
        self.expect("e value", substrs=["(SomeLibrary.TwoInts)", "= (first = 2, second = 3)"])
        self.expect("e container", substrs=["(main.ContainsTwoInts)", "wrapped = (first = 2, second = 3)", "other = 10"])
        self.expect("e TwoInts(4, 5)", substrs=["(SomeLibrary.TwoInts)", "= (first = 4, second = 5)"])
    
    @swiftTest
    def test_implementation_only_import_main_executable_resilient_no_library_module(self):
        """Test `@_implementationOnly import` in the main executable with a resilient library, after removing the library's swiftmodule"""
        self.build()
        os.remove(os.path.join(self.getBuildDir(), "SomeLibrary.swiftmodule"))
        os.remove(os.path.join(self.getBuildDir(), "SomeLibrary.swiftinterface"))
        def cleanup():
            lldbutil.execute_command("make cleanup")
        self.addTearDownHook(cleanup)
        lldbutil.run_to_source_breakpoint(self, "break here", lldb.SBFileSpec("main.swift"))

        self.expect("fr var", substrs=[
            "value = <could not resolve type>",
            "(main.ContainsTwoInts) container = (other = 10)"])
        # FIXME: Ideally we would still show that 'value' exists but make it unusable somehow.
        self.expect("e value", error=True, substrs=["error: use of unresolved identifier 'value'"])
        # FIXME: This representation of 'container' is garbage because we've dropped a field.
        # The compiler should keep track of this.
        self.expect("e container", substrs=["(main.ContainsTwoInts)", "other = "])
        # This one's probably unavoidable, though.
        self.expect("e TwoInts(4, 5)", error=True, substrs=["error: use of unresolved identifier 'TwoInts'"])
