int supported_local(int input) {
  int local = input + 1;
  if (/*initialized_guard*/ local > 0) return 1;
  return 0;
}
