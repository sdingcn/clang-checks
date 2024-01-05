template <typename T> void a(T x) { x + 1; }
template <typename T> void b(T x) { a(x); }
template <typename T> void c(T x) { a(x); }
template <typename T> void d(T x) { b(x); c(x); }

int main() {}
