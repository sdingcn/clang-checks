struct C {
	C() {}
	C(const C &c) {}
	C(C &&c) {}
	C &operator = (const C &c) {}
	C &operator = (C &&c) {}
	~C() {}
};

int main() {
	C c1, c2;
	c1 = c2;
}
