#ifndef TEST_H
#define TEST_H

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

#endif
