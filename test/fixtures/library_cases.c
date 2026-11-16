/* Small regression cases for incremental PClib and PTDlib. */
int branchless(int input, int unused) {
  return input + 1;
}

int false_prefix(int x, int y, int unused) {
  if (x > 0) {
    y = y + 1;
  } else {
    y = y + 2;
  }
  if (y > 5) {
    return 1;
  }
  return 0;
}

int early_loop_exit(int n, int stop) {
  while (n > 0) {
    if (stop > 0) {
      return n;
    }
    n = n - 1;
  }
  return 0;
}

int large_model(int value, int unused) {
  if (value > 1000) {
    return 1;
  }
  return 0;
}

int flag_guard(int flag, int unused) {
  if (+flag) {
    return 1;
  }
  return 0;
}

int label_named(int B3T) {
  if (B3T > 0) {
    return 1;
  }
  return 0;
}
