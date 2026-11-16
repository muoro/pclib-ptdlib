void nested_if(int a, int b, int c) {
    a = a + 1;
    if (a < b) {
        b = b + 2;
        if (b < c) {
            c = c - 1;
        } else {
            c = c + 1;
        }
    }

    if (c < a) {
        a = a + b;
    }
}
