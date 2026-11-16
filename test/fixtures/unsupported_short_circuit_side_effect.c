int unsupported_short_circuit_side_effect(int x, int y) { if (x > 0 || (y = y + 1)) return 1; return 0; }
