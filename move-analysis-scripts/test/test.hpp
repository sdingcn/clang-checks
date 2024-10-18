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
}
