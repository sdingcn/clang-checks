#ifndef TEST_H
#define TEST_H

template <typename T, typename U>
T f(T a, U b) {
    b++;
    b++;
    return a + b;
}

template <>
int f<int, int>(int a, int b) {
    return a * b;
}

#endif
