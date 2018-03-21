// REQUIRES: darwin

// RUN: rm -f %t.cmds %t
// RUN: echo 'breakpoint set -p "break here"' > %t.cmds
// RUN: echo 'run' >> %t.cmds

// RUN: echo 'language swift refcount Arg0' >> %t.cmds
// CHECK: refcount data: (strong = 0, weak = 1)

// RUN: %swiftc %s -g -Onone -o %t && %lldb -b -s %t.cmds -- %t | FileCheck %s

class Patatino {
  var Foo : String

  init(_ Foo : String) {
    self.Foo = Foo
  }
}

func g(_ Arg0 : Patatino) -> Patatino {
  return Arg0
}

func main() -> Patatino {
  var Arg0 = Patatino("Tinky")
  var Copy1 = Arg0
  var Copy2 = g(Copy1)
  return Copy2 // break here
}

main()
