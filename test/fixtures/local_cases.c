/* Locals must disappear from entry constraints and generated input records. */
#define LOCAL_OFFSET -40
#define LOCAL_LIMIT 5

int local_chain(int input) {
  int first = input + 1, second = first + 2;
  second = second + first;
  if (/*chain_guard*/ second > 10) return 1;
  return 0;
}

int local_snapshot(int input) {
  int saved = input;
  input = input + 1;
  if (/*snapshot_guard*/ saved == input) return 1;
  return 0;
}

int local_macro(int input) {
  const int value = input + LOCAL_OFFSET;
  if (/*macro_guard*/ value < LOCAL_LIMIT) return 1;
  return 0;
}

int local_branch(int input) {
  int result = 0;
  if (/*branch_first*/ input > 0) {
    int positive_value = input + 2;
    result = positive_value;
  } else {
    int negative_value = input - 2;
    result = negative_value;
  }
  if (/*branch_final*/ result > 1) return 1;
  return 0;
}

int local_loop(int n) {
  int remaining = n;
  while (/*loop_guard*/ remaining > 0) {
    int next_remaining = remaining - 1;
    remaining = next_remaining;
  }
  return remaining;
}

int local_for(int n) {
  for (int counter = n; /*for_guard*/ counter > 0; counter = counter - 1) {
    if (/*for_return*/ counter == 2) return counter;
  }
  return 0;
}
