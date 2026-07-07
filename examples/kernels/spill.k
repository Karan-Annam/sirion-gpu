kernel spill(int* out, int n) {
    int i = tid();
    if (i < n) {
        int a = i + 1;
        int b = a * 2;
        int c = b + 3;
        int d = c * 2;
        int e = d + 5;
        int f = e * 2;
        int g = f + 7;
        int h = g * 2;
        int p = h + 9;
        int q = p * 2;
        int r = q + 11;
        int s = r * 2;
        int t = s + 13;
        int u = t * 2;
        int v = u + a + b + c + d + e + f + g + h + p + q + r + s + t;
        out[i] = v;
    }
}
