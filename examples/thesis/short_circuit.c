/* Short-circuit regression examples. Markers identify atomic replay sites. */
int short_circuit_or(int x, int y) {
  if (/*or_a*/ x > 0 || /*or_b*/ y > 5) return 1;
  return 0;
}

int short_circuit_and(int x, int y) {
  if (/*and_a*/ x > 0 && /*and_b*/ y > 5) return 1;
  return 0;
}

int short_circuit_mixed(int x, int y, int z) {
  if ((/*mixed_a*/ x > 0 || /*mixed_b*/ y > 5) && /*mixed_c*/ z < 3) return 1;
  return 0;
}

int short_circuit_substitution(int x, int y) {
  x = x + 1;
  y = y - 2;
  if (/*sub_a*/ x > 0 || /*sub_b*/ y > x) return 1;
  return 0;
}

int short_circuit_scalar(int left, int right) {
  if (/*scalar_a*/ left && /*scalar_b*/ right) return 1;
  return 0;
}

int short_circuit_contradiction(int x) {
  if (/*contradiction_a*/ x > 0 && /*contradiction_b*/ x < 0) return 1;
  return 0;
}

int short_circuit_true_or(int x) {
  if (/*true_or_a*/ 1 || /*true_or_b*/ x > 0) return 1;
  return 0;
}

int short_circuit_false_and(int x) {
  if (/*false_and_a*/ 0 && /*false_and_b*/ x > 0) return 1;
  return 0;
}

int short_circuit_while(int n, int stop) {
  while (/*while_a*/ n > 0 && /*while_b*/ stop == 0) {
    n = n - 1;
  }
  return n;
}

int short_circuit_for(int n, int stop) {
  for (; /*for_a*/ n > 0 && /*for_b*/ stop == 0; n = n - 1) {
    if (/*for_return*/ n == 2) return n;
  }
  return 0;
}

int short_circuit_arithmetic_contradiction(int x) {
  if (/*arithmetic_a*/ x > 0 && /*arithmetic_b*/ (0 - x) > 0) return 1;
  return 0;
}

/* Both outcomes of the final decision share a successor block. */
int short_circuit_or_empty(int x, int y) {
  if (/*or_empty_a*/ x > 0 || /*or_empty_b*/ y > 5) {}
  return 0;
}

int short_circuit_and_empty(int x, int y) {
  if (/*and_empty_a*/ x > 0 && /*and_empty_b*/ y > 5) {}
  return 0;
}
