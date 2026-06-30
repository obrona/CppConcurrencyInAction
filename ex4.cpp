#include <future>
#include <print>

int main() {
    std::packaged_task<int(int, int)> task([](int a, int b) {
        return a + b;
    });

    std::future<int> f = task.get_future();

    task(3, 4);  // execute the task — sets the internal promise

    std::println("{}", f.get());  // 7
}

