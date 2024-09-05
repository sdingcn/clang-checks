#include <fstream>
#define _A
#define _B
#define _C
#define _D
#define _E
#define _F
#define _G
#define _H
#define _I
#define _J

class C {
public:
    C() {}
    C(const C &c) {}
    C(C &&c) {}
    C &operator=(const C &c) { return *this; }
    C &operator=(C &&c) { return *this; }
    ~C() {}
    void f() {}
};

class D {};


void ff() {
    C c1;
    C c2 = c1;
std::fstream filestr_clang_move;filestr_clang_move.open("/Users/vidurmodgil/Desktop/Data/School/College/research/clang-checks/build/moves.txt", std::fstream::app | std::fstream::out);filestr_clang_move << "(/Users/vidurmodgil/Desktop/Data/School/College/research/clang-checks/move-analysis-scripts/test/test.h:28:12)" << std::endl;filestr_clang_move.close();
}
