kernel saxpy(int* out, int* a, int* b, int n, int alpha) {
    int i = tid();
    if (i < n) {
        out[i] = alpha * a[i] + b[i];
    }
}
