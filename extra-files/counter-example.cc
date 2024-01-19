#include <utility>

struct S { void f() && {} };

template <typename T>
void f(T x) {
    std::move(x).f();
}

int main() {
    f(S{});
}
