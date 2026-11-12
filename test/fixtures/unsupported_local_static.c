int unsupported_local_static(int input) { static int local = 0; if (local > input) return 1; return 0; }
