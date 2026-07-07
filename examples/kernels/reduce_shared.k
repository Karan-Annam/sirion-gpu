kernel reduce_shared(int* out, int* a, int n) {
    __shared__ int buf[256];
    int t = tid();
    int lt = t - ctaid() * ntid();
    if (t < n) {
        buf[lt] = a[t];
    } else {
        buf[lt] = 0;
    }
    barrier();
    if (lt == 0) {
        int s = 0;
        for (int j = 0; j < ntid(); j = j + 1) {
            s = s + buf[j];
        }
        out[ctaid()] = s;
    }
}
