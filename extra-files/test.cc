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

int main() {
    n::m::C c1, c2;
    c1 = c2;
    f(5);
    f(10LL);
    f(0.5);
    f(ID());
}
