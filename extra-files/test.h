#ifndef TEST_H
#define TEST_H

template <typename T>
T g(T x) {
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
    return g(a) / h(b);
}

#endif
