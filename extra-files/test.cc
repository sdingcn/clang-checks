namespace NS {

template <typename V>
V g(V x) {
    return x + 1;
}

template <>
int g<int>(int x) {
    return x - 1;
}

int h(int x) {
    return x * 2;
}

template <typename T, typename U>
T f(T a, U b) {
    a++;
    h(0);
    return g(a) * h(b);
}

template <typename T>
T recursion(T x) {
    if (x <= 1) {
        return 1;
    } else {
        T y = x - 1;
        return x * recursion(y);
    }
}

template <typename Callable>
void apply(Callable f) {
    f(0);
}

template <typename ...T>
void many(T ...x) {
}

}

int main() {
    NS::f(1, 1);
}
