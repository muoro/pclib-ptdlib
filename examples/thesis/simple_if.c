/*
simple test 1.

Detailed CFG Visualization:

            B4 (Entry)
            |
            v
            B3 (x = x * 2; if(x > y))
           / \
          /   \
         v     v
    B2(T)     B1(F)
    y=y+5     y=y-5
        \     /
         \   /
          v v
          B0 (Exit)

Block Details:
- B4: Entry block
- B3: Assignment and condition evaluation
  * Condition: (x > y) after x = x * 2
  * True edge: B3T -> B2
  * False edge: B3F -> B1
- B2: Then branch (y = y + 5)
- B1: Else branch (y = y - 5)
- B0: Exit block (merge point)

Predicates:Conditions
- B3T: "((x * 2) > y)"
- B3F: "not((x * 2) > y)"

*/
void simple_if(int x, int y) {
    x = x * 2;
    if (x > y) {
        y = y + 5;
    } else {
        y = y - 5;
    }
}
