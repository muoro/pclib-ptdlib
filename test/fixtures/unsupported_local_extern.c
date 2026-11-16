int unsupported_local_extern(int input) { extern int local; if (local > input) return 1; return 0; }
