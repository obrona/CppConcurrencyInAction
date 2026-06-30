#include <functional>
#include <shared_mutex>
#include <mutex>
#include <list>
#include <utility>
#include <algorithm>
#include <vector>
#include <memory>

template<typename Key, typename Value, typename Hash=std::hash<Key>>
struct hashmap {
    struct bucket {
        using bucket_value = std::pair<Key, Value>;
        using bucket_data = std::list<bucket_value>;
        using bucket_iterator = typename bucket_data::iterator;
        
        mutable std::shared_mutex mut;
        bucket_data data;

        bucket_iterator find_entry_for(const Key& key) const {
            return std::find_if(data.begin(), data.end(), [&] (const bucket_value& item) {
                return item.first == key;
            });
        }

        Value value_for(const Key& key, const Value& default_val) const {
            std::shared_lock lk(mut);
            auto it = find_entry_for(key);
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

    size_t num_buckets = 128; // minimum size is 128.
    std::vector<std::unique_ptr<bucket>> buckets;
    Hash hash;
    size_t cnt = 0;

    hashmap() {
        for (int i = 0; i < num_buckets; i++) {
            buckets[i] = make_unique(new bucket());
        }
    }

    bucket* get_bucket(const Key& key) {
        return hash(key) % 19;
    }

    Value value_for(const Key& key) {
        return get_bucket(key)->value_for(key);
    }

    void add_or_update_mapping(const Key& key, const Value& value) {
        cnt += get_bucket(key)->add_or_update_mapping(key, value);
    }

    void remove_mapping(const Key& key) {
        cnt += get_bucket(key)->remove_mapping(key);
    }    
};

int main() {}