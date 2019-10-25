# Disclaimer

The swift-lldb repository is frozen and is preserved for historical purposes only.
Active development is now happening in the following repository: https://github.com/apple/llvm-project

# Swift Debugger and REPL

**Welcome to the Swift Debugger and REPL!**

Swift is a new, high performance systems programming language.  It has a clean
and modern syntax, offers seamless access to existing C and Objective-C
code and frameworks, and is memory safe (by default).

This repository covers the Swift Debugger and REPL support, built on
top of the LLDB Debugger.

# Building LLDB for Swift

To build LLDB for Swift, check out the swift repository and follow
the instruction listed there. You can build lldb passing the --lldb
flag to it. Example invocation:

```
mkdir myswift
cd myswift
git clone https://github.com/apple/swift.git swift
./swift/utils/update-checkout
./swift/utils/build-script -r --lldb
```

# Contribution Subtleties

The swift-lldb project enhances the core LLDB project developed under
the [LLVM Project][llvm]. Swift support in the debugger is added via
the existing source-level plugin infrastructure, isolated to files that
are newly introduced in the lldb-swift repository.

Files that come from the [core LLDB project][lldb] can be readily
identified by their use of the LLVM comment header.  As no local
changes should be made to any of these files, follow the standard
[guidance for upstream changes][upstream].

[lldb]: http://lldb.llvm.org "LLDB debugger"
[llvm]: http://llvm.org "The LLVM Project"
[upstream]: http://swift.org/contributing/#llvm-and-swift "Upstream LLVM changes"
