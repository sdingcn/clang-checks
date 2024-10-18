#include "test.hpp"

void f1() {
    C c1;
    C c2 = c1;
}

void f2() {
    C c1;
    C c2 = c1;
    c1.f();
}

void f3() {
    D d1;
    D d2 = d1;
}

int main() {
}
