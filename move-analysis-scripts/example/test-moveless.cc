#include <vector>

void func() {
    std::vector<int> x(1000000, 0);
    std::vector<int> y = x;
}

int main() {
    for (int i = 0; i < 100; i++) {
        func();
    }
}
