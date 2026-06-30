#include <vector>
#include <thread>
#include <stdexcept>
#include <print>
using namespace std;


template <typename T, typename Func>
T parallel_reduce(const vector<T>& arr, T init, Func f, int num_threads) {
    if (arr.size() == 0) return init;
    if (arr.size() < num_threads) throw invalid_argument("num of elems < num of threads");
    
    int d = arr.size() / num_threads;
    int r = arr.size() % num_threads;

    vector<T> results(num_threads);
    vector<thread> threads;

    for (int i = 0, p = 0; i < num_threads; i++) {
        int cnt = d + (i < r);
        threads.emplace_back([&arr, &results, i, d, p, cnt, init, &f] () {
            T store = init;
            for (int j = p; j < p + cnt; j++) {
                store = f(store, arr[j]);
            }
            results[i] = store;
        });
        p += cnt;
    }

    for (auto& t : threads) t.join();

   
    T store = init;
    for (T t : results) store = f(store, t);
    return store;
   
}

int main() {
    vector<long long> ans(100000);
    for (int i = 0; i < ans.size(); i++) ans[i] = i;

    auto res = parallel_reduce(ans, 0LL, [] (long long a, long long b) { return a + b; }, 10);
    println("{}", res);
}