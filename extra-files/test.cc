template <typename G>
void g(G x) {
    if (x < 0) {
        x++;
        g(x);
    }
}

template <>
void g<int>(int x) {
}

template <typename T, typename U, typename V, typename W>
void f(const T &t, U u, V v, W w) {
    g(t);
    u.field;
    u.fun();
    u.m1(1, t, true);
    u.m2(1, t, *v, true);
    if (w(true)) {}
}

int main() {
}
