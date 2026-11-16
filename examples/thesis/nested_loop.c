void nested_loop(int a, int b) {

    while (a <= 10) {         // B6
        while (b <= 10) {     // B5
            b = b + 1;
        }
        a = a + 1;
    }
}