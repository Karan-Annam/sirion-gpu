kernel gen_checker(int* tex, int n) {
    int i = tid();
    if (i < n) {
        int x = i & 31;
        int y = i >> 5;
        int p = ((x >> 2) + (y >> 2)) & 1;
        if (p == 1) {
            tex[i] = 15786080;
        } else {
            tex[i] = 3166368;
        }
    }
}
