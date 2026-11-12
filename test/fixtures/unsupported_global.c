int global_value;
int unsupported_global(int input) {
  if (input > global_value) return 1;
  return 0;
}
