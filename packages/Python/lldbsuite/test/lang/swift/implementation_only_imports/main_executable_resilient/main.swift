@_implementationOnly import SomeLibrary

struct ContainsTwoInts {
  var wrapped: TwoInts
  var other: Int
}

func test(_ value: TwoInts) {
  let container = ContainsTwoInts(wrapped: value, other: 10)
  return // break here
}

test(TwoInts(2, 3))
