#include <utility>

struct S { void f() && {} };

template <typename T>
void g(T x) {
    std::move(x).f();
}

int main() {
    g(S{});
}
