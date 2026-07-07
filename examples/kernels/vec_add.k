kernel vec_add(int* out, int* a, int* b, int n) {
    int i = tid();
    if (i < n) {
        out[i] = a[i] + b[i];
    }
}
