kernel strided_sum(int* out, int* a, int n, int k) {
    int i = tid();
    if (i < n) {
        int s = 0;
        int j = 0;
        while (j < k) {
            s = s + a[i * k + j];
            j = j + 1;
        }
        out[i] = s;
    }
}
