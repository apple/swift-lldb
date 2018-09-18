// main.swift
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
// -----------------------------------------------------------------------------
func foo(_ x: Int, y: Int, z: Int) -> Int {
  return x + y - z
}

func main() {
  print(foo(5, y: 31, z: 12))
}

main()
