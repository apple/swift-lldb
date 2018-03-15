# TestSwiftProtocolTypes.py
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
Test support for protocol types
"""
import lldb
from lldbsuite.test.lldbtest import *
import lldbsuite.test.decorators as decorators
import lldbsuite.test.lldbutil as lldbutil
import unittest2


class TestSwiftProtocolTypes(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @decorators.swiftTest
    @decorators.skipIfOutOfTreeDebugserver
    def test_swift_protocol_types(self):
        """Test support for protocol types"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def do_test(self):
        """Tests that we can break and display simple types"""
        exe_name = "a.out"
        exe = self.getBuildArtifact(exe_name)

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

        self.expect("frame variable --raw-output --show-types loc2d",
                    substrs=['(PointUtils) loc2d =',
                             '(Builtin.RawPointer) payload_data_0 = 0x',
                             '(Builtin.RawPointer) payload_data_1 = 0x',
                             '(Builtin.RawPointer) payload_data_2 = 0x',
                             '(Builtin.RawPointer) instance_type = 0x',
                             '(Builtin.RawPointer) protocol_witness_0 = 0x'])

        self.expect("frame variable --dynamic-type run-target loc2d",
                    substrs=['Point2D) loc2d =',
                             'x = 1.25', 'y = 2.5'])

        self.expect("frame variable --raw-output --show-types loc3d",
                    substrs=['(PointUtils) loc3d =',
                             '(Builtin.RawPointer) payload_data_0 = 0x',
                             '(Builtin.RawPointer) payload_data_1 = 0x',
                             '(Builtin.RawPointer) payload_data_2 = 0x',
                             '(Builtin.RawPointer) instance_type = 0x',
                             '(Builtin.RawPointer) protocol_witness_0 = 0x'])

        self.expect(
            "frame variable --dynamic-type run-target loc3d",
            substrs=[
                'Point3D) loc3d = 0x',
                'x = 1.25',
                'y = 2.5',
                'z = 1.25'])

        self.expect("expression --raw-output --show-types -- loc2d",
                    substrs=['(PointUtils) $R',
                             '(Builtin.RawPointer) payload_data_0 = 0x',
                             '(Builtin.RawPointer) payload_data_1 = 0x',
                             '(Builtin.RawPointer) payload_data_2 = 0x',
                             '(Builtin.RawPointer) instance_type = 0x',
                             '(Builtin.RawPointer) protocol_witness_0 = 0x'])

        self.expect("expression --dynamic-type run-target -- loc2d",
                    substrs=['Point2D) $R',
                             'x = 1.25', 'y = 2.5'])

        self.expect("expression --raw-output --show-types -- loc3d",
                    substrs=['(PointUtils) $R',
                             '(Builtin.RawPointer) payload_data_0 = 0x',
                             '(Builtin.RawPointer) payload_data_1 = 0x',
                             '(Builtin.RawPointer) payload_data_2 = 0x',
                             '(Builtin.RawPointer) instance_type = 0x',
                             '(Builtin.RawPointer) protocol_witness_0 = 0x'])

        self.expect("expression --dynamic-type run-target -- loc3d",
                    substrs=['Point3D) $R', 'x = 1.25', 'y = 2.5', 'z = 1.25'])

if __name__ == '__main__':
    import atexit
    lldb.SBDebugger.Initialize()
    atexit.register(lldb.SBDebugger.Terminate)
    unittest2.main()
