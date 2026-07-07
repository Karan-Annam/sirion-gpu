kernel hist(int* bins, int* data, int n) {
    int i = tid();
    if (i < n) {
        int v = data[i] & 15;
        atomic_add(bins, v, 1);
    }
}
