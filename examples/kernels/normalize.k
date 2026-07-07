func float sq(float v) {
    return v * v;
}

kernel normalize(float* out, float* x, float* y, int n) {
    int i = tid();
    if (i < n) {
        float r = rsqrtf(sq(x[i]) + sq(y[i]));
        out[i] = x[i] * r;
    }
}
