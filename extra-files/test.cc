#include "test.h"

#if 0
namespace n {
namespace m {

struct C {
    C() {}
    C(const C &c) {}
    C(C &&c) {}
    C &operator = (const C &c) { return *this; }
    C &operator = (C &&c) { return *this; }
    ~C() {}
};

}
}

void h(int x) {}
void h(long long x) {}

template <typename T>
void g(T x) {
    T y(x);
    x + 1;
    x = 1;
    x++;
    f(x);
    h(x);
}

template <typename T>
void obj(T x) {
    x.f;
    x.dump();
}

struct ID {
    ID () {}
    ID (int x) {}
    ID operator + (int x) { return ID(); }
    ID operator + (const ID &id) { return ID(); }
};

template <typename T>
struct C {
    C() {}
    template <typename U>
    void g(U x) { return f(x, x); }
};
#endif

int main() {
/*
    n::m::C c1, c2;
    c1 = c2;
*/
    f(1, 1);
}
