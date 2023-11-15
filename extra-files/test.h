#ifndef TEST_H
#define TEST_H

template <typename T>
T f(T x) {
    T y = x + 1;
    T r = y * y;
    return r;
}

template <>
int f<int>(int x) {
    return 0;
}

#endif
