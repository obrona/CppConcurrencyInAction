#include <iostream>

int f(int a, int b) {
    return a * b;
}

int main() {
    int (*fp) (int, int) = f;
    std::cout << fp(2, 3) << '\n';
}