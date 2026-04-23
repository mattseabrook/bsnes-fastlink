// Compiler-rt builtins for 128-bit integer conversions (missing on Windows clang-cl)
typedef __int128 ti_int;
typedef unsigned __int128 tu_int;

ti_int __fixdfti(double a) {
    return (ti_int)a;
}

tu_int __fixunsdfti(double a) {
    if (a < 0.0) return 0;
    return (tu_int)a;
}
