#include <functional>
#include <shared_mutex>
#include <mutex>
#include <list>
#include <utility>
#include <algorithm>
#include <vector>
#include <memory>
#include <atomic>

// resize is not possible, because when we create new buckets, 
// we destroy the old buckets and their mutexes.
// there can be some threads waiting on these mutexes when we destroyed them,
// causing undefined behaviour.

template<typename Key, typename Value, typename Hash=std::hash<Key>>
struct hashmap {
    struct bucket {
        using bucket_value = std::pair<Key, Value>;
        using bucket_data = std::list<bucket_value>;
        using bucket_iterator = typename bucket_data::iterator;
        using const_bucket_iterator = typename bucket_data::const_iterator;
        
        mutable std::shared_mutex mut;
        bucket_data data;

        const_bucket_iterator find_entry_for_const(const Key& key) const {
            return std::find_if(data.begin(), data.end(), [&] (const bucket_value& item) {
                return item.first == key;
            });
        }

        bucket_iterator find_entry_for(const Key& key) {
            return std::find_if(data.begin(), data.end(), [&] (const bucket_value& item) {
                return item.first == key;
            });
        }

        Value value_for(const Key& key, const Value& default_val) const {
            std::shared_lock lk(mut);
            auto it = find_entry_for_const(key);
            return it == data.end() ? default_val : it->second;
        }

        // return true if adds an element else false.
        bool add_or_update_mapping(const Key& key, const Value& value) {
            std::unique_lock lk(mut);
            auto it = find_entry_for(key);
            if (it == data.end()) {
                data.emplace_back(key, value);
                return true;
            } else {
                it->second = value;
                return false;
            }
        }

        // return true if adds an element else false.
        bool remove_mapping(const Key& key) {
            std::unique_lock lk(mut);
            auto it = find_entry_for(key);
            if (it == data.end()) return false;
            data.erase(it);
            return true;
        }
    };

    size_t num_buckets;
    std::vector<std::unique_ptr<bucket>> buckets;
    Hash hash;
    std::atomic<size_t> cnt{0};

    explicit hashmap(size_t n = 9929) : num_buckets(n), buckets(n) {
        for (size_t i = 0; i < num_buckets; i++) {
            buckets[i] = std::make_unique<bucket>();
        }
    }

    bucket& get_bucket(const Key& key) const {
        return *buckets[hash(key) % num_buckets];
    }

    Value value_for(const Key& key, const Value& default_val = Value()) const {
        return get_bucket(key).value_for(key, default_val);
    }

    void add_or_update_mapping(const Key& key, const Value& value) {
        cnt += get_bucket(key).add_or_update_mapping(key, value);
    }

    void remove_mapping(const Key& key) {
        cnt -= get_bucket(key).remove_mapping(key);
    }

    size_t size() const {
        return cnt.load();
    }
};
