# TestREPLDictionary.py
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
"""Test that Dictionary work in the REPL."""

import os
import time
import unittest2
import lldb
import lldbsuite.test.lldbrepl as lldbrepl
import lldbsuite.test.decorators as decorators


class REPLDictionaryTestCase (lldbrepl.REPLTest):

    mydir = lldbrepl.REPLTest.compute_mydir(__file__)

    # rdar://37407231 -- This is flaky when run as a PR test.
    @decorators.swiftTest
    @decorators.no_debug_info_test
    @decorators.expectedFailureAll(
        oslist=['linux'],
        bugnumber="bugs.swift.org/SR-843")
    def testREPL(self):
        lldbrepl.REPLTest.testREPL(self)

    def doTest(self):
        self.sendline('[1:2, 2:3, 3:9]')
        self.expectall(
            patterns=[
                '\\$R0: \\[Int : Int\\]',
                'key = 2',
                'value = 3',
                'key = 3',
                'value = 9',
                'key = 1',
                'value = 2'])
        self.promptSync()
        self.command('$R0.count', patterns='\\$R1: Int = 3')
        self.sendline('var x = $R0')
        self.expectall(
            patterns=[
                'x: \\[Int : Int\\]',
                'key = 2',
                'value = 3',
                'key = 3',
                'value = 9',
                'key = 1',
                'value = 2'])
        self.promptSync()
        self.sendline('x.updateValue(8, forKey:4)')
        self.promptSync()
        self.sendline('x')
        self.expectall(
            patterns=[
                '\\[Int : Int\\]',
                'key = 2',
                'value = 3',
                'key = 3',
                'value = 9',
                'key = 1',
                'value = 2',
                'key = 4',
                'value = 8'])
        self.sendline('x[3] = 5')
        self.promptSync()
        self.sendline('x')
        self.expectall(
            patterns=[
                '\\[Int : Int\\]',
                'key = 2',
                'value = 3',
                'key = 3',
                'value = 5',
                'key = 1',
                'value = 2',
                'key = 4',
                'value = 8'])
        self.promptSync()
