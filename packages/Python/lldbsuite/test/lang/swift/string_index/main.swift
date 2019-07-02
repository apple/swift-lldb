let name = "patatino"
let index1 = name.firstIndex(of: "t")
let index_none = name.firstIndex(of: "Q")
let long_name = "sed avarissimi hominis cupiditati satisfacere posse"
let index2 = long_name.firstIndex(of: "h")
print() //%self.expect('frame var -d run-target -- index1', substrs=['(String.Index?) index1 = 2'])
        //%self.expect('frame var -d run-target -- index_none', substrs=['(String.Index?) index_none = nil'])
        //%self.expect('frame var -d run-target -- index2', substrs=['(String.Index?) index2 = 15'])
