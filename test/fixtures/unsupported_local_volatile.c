int unsupported_local_volatile(int input) { volatile int local = 1; if (local > input) return 1; return 0; }
