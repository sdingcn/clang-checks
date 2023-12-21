namespace NS {

template <typename T>
void g(T x) {
    if (x < 0) {
        x++;
        g(x);
    }
}

template <>
void g<int>(int x) {
}

template <typename T, typename U, typename V, typename W>
void f(T t, U u, V* v, W w) {
    g(t);
    u.m(1, 2, true);
    if (w(true)) {}
}

}

int main() {
}
