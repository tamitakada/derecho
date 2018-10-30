/**
 * @file smart_membership_function_test.cpp
 *
 * @date May 9, 2017
 * @author edward
 */

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <typeindex>
#include <vector>

#include "derecho/derecho.h"
#include "initialize.h"
#include "conf/conf.hpp"

/*
 * The Eclipse CDT parser crashes if it tries to expand the REGISTER_RPC_FUNCTIONS
 * macro, probably because there are too many layers of variadic argument expansion.
 * This definition makes the RPC macros no-ops when the CDT parser tries to expand
 * them, which allows it to continue syntax-highlighting the rest of the file.
 */
#ifdef __CDT_PARSER__
#define REGISTER_RPC_FUNCTIONS(...)
#define RPC_NAME(...) 0ULL
#endif

class Cache : public mutils::ByteRepresentable {
    std::map<std::string, std::string> cache_map;

public:
    void put(const std::string& key, const std::string& value) {
        cache_map[key] = value;
    }
    std::string get(const std::string& key) {
        return cache_map[key];
    }
    bool contains(const std::string& key) {
        return cache_map.find(key) != cache_map.end();
    }
    bool invalidate(const std::string& key) {
        auto key_pos = cache_map.find(key);
        if(key_pos == cache_map.end()) {
            return false;
        }
        cache_map.erase(key_pos);
        return true;
    }
    REGISTER_RPC_FUNCTIONS(Cache, put, get, contains, invalidate);

    Cache() : cache_map() {}
    Cache(const std::map<std::string, std::string>& cache_map) : cache_map(cache_map) {}

    DEFAULT_SERIALIZATION_SUPPORT(Cache, cache_map);
};

class LoadBalancer : public mutils::ByteRepresentable {
    std::vector<std::pair<std::string, std::string>> key_ranges_by_shard;

public:
    //I can't think of any RPC methods this class needs, but it can't be a Replicated Object without an RPC method
    void dummy() {}

    REGISTER_RPC_FUNCTIONS(LoadBalancer, dummy);

    LoadBalancer() : LoadBalancer({{"a", "i"}, {"j", "r"}, {"s", "z"}}) {}
    LoadBalancer(const std::vector<std::pair<std::string, std::string>>& key_ranges_by_shard)
            : key_ranges_by_shard(key_ranges_by_shard) {}

    DEFAULT_SERIALIZATION_SUPPORT(LoadBalancer, key_ranges_by_shard);
};

using std::cout;
using std::endl;

int main(int argc, char** argv) {
    derecho::node_id_t node_id;
    derecho::ip_addr my_ip;
    derecho::ip_addr leader_ip;

    query_node_info(node_id, my_ip, leader_ip);

    //Derecho message parameters
    //Where do these come from? What do they mean? Does the user really need to supply them?
    long long unsigned int max_msg_size = 100;
    long long unsigned int block_size = 100000;
    const long long unsigned int sst_max_msg_size = (max_msg_size < 17000 ? max_msg_size : 0);
    derecho::DerechoParams derecho_params{max_msg_size, sst_max_msg_size, block_size};

    derecho::message_callback_t stability_callback{};
    derecho::CallbackSet callback_set{stability_callback, {}};

    auto load_balancer_factory = [](PersistentRegistry*) { return std::make_unique<LoadBalancer>(); };
    auto cache_factory = [](PersistentRegistry*) { return std::make_unique<Cache>(); };

    derecho::SubgroupAllocationPolicy load_balancer_policy = derecho::one_subgroup_policy(derecho::even_sharding_policy(1, 3));
    derecho::SubgroupAllocationPolicy cache_policy = derecho::one_subgroup_policy(derecho::even_sharding_policy(3, 3));
    derecho::SubgroupInfo subgroup_info({{std::type_index(typeid(LoadBalancer)), derecho::DefaultSubgroupAllocator(load_balancer_policy)},
                                         {std::type_index(typeid(Cache)), derecho::DefaultSubgroupAllocator(cache_policy)}},
                                        keys_as_list(subgroup_info.subgroup_membership_functions));

    std::unique_ptr<derecho::Group<LoadBalancer, Cache>> group;
    if(my_ip == leader_ip) {
        group = std::make_unique<derecho::Group<LoadBalancer, Cache>>(
                node_id, my_ip, callback_set, subgroup_info, derecho_params,
                std::vector<derecho::view_upcall_t>{},
                load_balancer_factory, cache_factory);
    } else {
        group = std::make_unique<derecho::Group<LoadBalancer, Cache>>(
                node_id, my_ip, leader_ip, callback_set, subgroup_info,
                std::vector<derecho::view_upcall_t>{},
                load_balancer_factory, cache_factory);
    }
    cout << "Finished constructing/joining Group" << endl;

    if(node_id == 1) {
        derecho::ExternalCaller<Cache>& cache_handle = group->get_nonmember_subgroup<Cache>();
        derecho::node_id_t who = 3;
        std::this_thread::sleep_for(std::chrono::seconds(1));
        derecho::rpc::QueryResults<std::string> cache_results = cache_handle.p2p_query<RPC_NAME(get)>(who, "6");
        std::string response = cache_results.get().get(who);
        cout << " Response from node " << who << ":" << response << endl;
    }
    if(node_id > 2) {
        derecho::Replicated<Cache>& cache_handle = group->get_subgroup<Cache>();
        std::stringstream string_builder;
        string_builder << "Node " << node_id << "'s things";
        cache_handle.ordered_send<RPC_NAME(put)>(std::to_string(node_id), string_builder.str());
    }

    std::cout << "Reached end of main(), entering infinite loop so program doesn't exit" << std::endl;
    while(true) {
    }
}
