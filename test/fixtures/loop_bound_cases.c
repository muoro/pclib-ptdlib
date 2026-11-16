void bounded_nested(int a, int b) {
  while (a <= 10) {
    while (b <= 10) {
      b = b + 1;
    }
    a = a + 1;
  }
}

void bounded_sequential(int a, int b) {
  while (a <= 10) {
    a = a + 1;
  }
  while (b <= 10) {
    b = b + 1;
  }
}

int bounded_jumps(int n, int run, int stop) {
  for (; n > 0 || run > 0; n = n - 1) {
    if (stop > 1)
      return n;
    if (stop > 0)
      break;
    continue;
  }
  return 0;
}

void bounded_inner_break(int a, int b) {
  while (a > 0) {
    while (b > 0) {
      if (b > 3)
        break;
      b = b + 1;
    }
    a = a - 1;
  }
}

void bounded_nested_then_if(int a, int b) {
  while (a <= 10) {
    while (b <= 10) {
      b = b + 1;
    }
    a = a + 1;
  }
  if (a > 10) {
    a = 0;
  }
}

void bounded_nested_decisions(int a, int b, int x) {
  while (a <= 10) {
    if (x > 0) {
      x = x - 1;
    } else {
      x = x + 1;
    }
    while (b <= 10) {
      if (b < x) {
        b = b + 1;
      } else {
        b = b + 2;
      }
    }
    a = a + 1;
  }
}
