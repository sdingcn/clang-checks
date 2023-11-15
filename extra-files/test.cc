#include "test.h"

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

struct ID {
    ID operator + (int x) { return ID(); }
    ID operator * (const ID &id) { return ID(); }
};

void h(int x) {}

template <typename T>
void g(T x) {
    T y(x);
    x + 1;
    x = 1;
    f(x);
    h(x);
}

int main() {
    n::m::C c1, c2;
    c1 = c2;
    f(1);
    f(10LL);
    f(0.5);
    f(ID());
}
