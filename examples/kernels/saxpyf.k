kernel saxpyf(float* out, float* x, float* y, int n, float a) {
    int i = tid();
    if (i < n) {
        out[i] = a * x[i] + y[i];
    }
}
