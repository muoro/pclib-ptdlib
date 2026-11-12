void loop_with_branch(int a, int b) {
    while (a <= 10) {
        if (b > a) {
            b = b - 1;
        } else {
            b = b + 1;
        }
        a = a + 1;
    }
}
