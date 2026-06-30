#include <thread>
#include <iostream>

void hello() {
    std::cout << "hello world\n";
}

int main() {
    std::thread t(hello);
    t.join();
}