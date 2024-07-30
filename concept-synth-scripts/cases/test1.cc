#include <algorithm>
#include <vector>

class X {
public:
    int attribute_1;
    bool operator==(const int i) {
        return this->attribute_1 == i;
    }
};

std::vector<X> v;

int main() {
    int elem = 12;
    bool isElementPresent = std::binary_search(
        v.begin(),
        v.end(),
        elem,
        [](const X &right, const X &left) { return right.attribute_1 < left.attribute_1; }
    );
}
