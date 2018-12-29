#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "rdmc/rdmc.h"
#include "rdmc/util.h"

#include "aggregate_latency.h"
#include "derecho/derecho.h"
#include "log_results.h"

using std::cout;
using std::endl;
using std::vector;
using namespace derecho;

std::unique_ptr<rdmc::barrier_group> universal_barrier_group;

struct exp_result {
    uint32_t num_nodes;
    long long unsigned int max_msg_size;
    uint32_t num_senders_selector;
    uint32_t delivery_mode;
    double latency;
    double stddev;

    void print(std::ofstream& fout) {
        fout << num_nodes << " " << max_msg_size << " "
	     << num_senders_selector << " "
             << delivery_mode << " " << latency << " "
             << stddev << endl;
    }
};

int main(int argc, char* argv[]) {
  if(argc < 4 || (argc > 4 && strcmp("--", argv[argc - 4]))) {
        cout << "Insufficient number of command line arguments" << endl;
        cout << "USAGE:" << argv[0] << "[ derecho-config-list -- ] num_nodes, num_senders_selector (0 - all senders, 1 - half senders, 2 - one sender), delivery_mode (0 - ordered mode, 1 - unordered mode)" << endl;
        return -1;
    }
    pthread_setname_np(pthread_self(), "latency_test");
    
    uint32_t num_nodes = std::stoi(argv[argc - 3]);
    const uint32_t num_senders_selector = std::stoi(argv[argc - 2]);
    const uint32_t delivery_mode = std::stoi(argv[argc - 1]);
    Conf::initialize(argc, argv);
    const uint64_t msg_size = getConfUInt64(CONF_DERECHO_MAX_PAYLOAD_SIZE);

    uint32_t num_messages = 1000;
    // only used by node 0
    vector<uint64_t> start_times(num_messages), end_times(num_messages);

    volatile bool done = false;
    uint32_t my_id;
    auto stability_callback = [&, num_delivered = 0u, time_index=0u](
                                      int32_t subgroup, uint32_t sender_id, long long int index,
                                      std::optional<std::pair<char*, long long int>> data,
                                      persistent::version_t ver) mutable {
        // DERECHO_LOG(sender_id, index, "complete_send");
        // cout << "Delivered a message: " << endl;
        ++num_delivered;
        if(sender_id == my_id) {
            end_times[time_index++] = get_time();
        }
        if(num_senders_selector == 0) {
            if(num_delivered == num_messages * num_nodes) {
                done = true;
            }
        } else if(num_senders_selector == 1) {
            if(num_delivered == num_messages * (num_nodes / 2)) {
                done = true;
            }
        } else {
            if(num_delivered == num_messages) {
                done = true;
            }
        }
    };

    Mode mode = Mode::ORDERED;
    if(delivery_mode) {
        mode = Mode::UNORDERED;
    }

    auto membership_function = [num_senders_selector, mode, num_nodes](const std::type_index& subgroup_type,
                                                                       const std::unique_ptr<View>& prev_view, View& curr_view) {
        //There will be only one subgroup (of type RawObject), so no need to check subgroup_type
        subgroup_shard_layout_t subgroup_vector(1);
        auto num_members = curr_view.members.size();
        // wait for all nodes to join the group
        if(num_members < num_nodes) {
            throw subgroup_provisioning_exception();
        }
        // all senders case
        if(num_senders_selector == 0) {
            // a call to make_subview without the sender information
            // defaults to all members sending
            subgroup_vector[0].emplace_back(curr_view.make_subview(curr_view.members, mode));
        } else {
            // configure the number of senders
            vector<int> is_sender(num_members, 1);
            // half senders case
            if(num_senders_selector == 1) {
                // mark members ranked 0 to num_members/2 as non-senders
                for(uint i = 0; i <= (num_members - 1) / 2; ++i) {
                    is_sender[i] = 0;
                }
            } else {
                // mark all members except the last ranked one as non-senders
                for(uint i = 0; i < num_members - 1; ++i) {
                    is_sender[i] = 0;
                }
            }
            // provide the sender information in a call to make_subview
            subgroup_vector[0].emplace_back(curr_view.make_subview(curr_view.members, mode, is_sender));
        }
        curr_view.next_unassigned_rank = curr_view.members.size();
        return subgroup_vector;
    };

    //Wrap the membership function in a SubgroupInfo
    SubgroupInfo one_raw_group(membership_function);

    Group<RawObject> managed_group(CallbackSet{stability_callback}, one_raw_group, nullptr,
                                                     std::vector<view_upcall_t>{},
                                                     &raw_object_factory);
    cout << "All nodes joined." << endl;

    auto group_members = managed_group.get_members();
    uint32_t my_rank = managed_group.get_my_rank();
    my_id = group_members[my_rank];

    Replicated<RawObject>& group_as_subgroup = managed_group.get_subgroup<RawObject>();
    auto send_all = [&]() {
        for(uint i = 0; i < num_messages; ++i) {
            group_as_subgroup.send(msg_size, [&](char* buf) {
                for(unsigned int j = 0; j < msg_size - 1; ++j) {
                    buf[j] = 'a' + (i % 26);
                }
                buf[msg_size - 1] = 0;
                start_times[i] = get_time();
                // DERECHO_LOG(my_rank, i, "start_send");
            });
        }
    };
    if(num_senders_selector == 0) {
        send_all();
    } else if(num_senders_selector == 1) {
        if(my_rank > (num_nodes - 1) / 2) {
            send_all();
        }
    } else {
        if(my_rank == num_nodes - 1) {
            send_all();
        }
    }

    while(!done) {
    }

    double avg_latency, avg_std_dev;
    if(num_senders_selector == 0 || (num_senders_selector == 1 && my_rank > (num_nodes - 1) / 2) || (num_senders_selector == 2 && my_rank == num_nodes - 1)) {
        uint64_t total_time = 0;
        double sum_of_square = 0.0f;
        double average_time = 0.0f;
        for(uint i = 0; i < num_messages; ++i) {
            total_time += end_times[i] - start_times[i];
            // cout << ((end_times[i] - start_times[i])/1000.0) << "us" << std::endl;
        }
        average_time = (total_time / num_messages);  // in nano seconds
        // calculate the standard deviation:
        for(uint i = 0; i < num_messages; ++i) {
            sum_of_square += (double)(end_times[i] - start_times[i] - average_time) * (end_times[i] - start_times[i] - average_time);
        }
        double std_dev = sqrt(sum_of_square / (num_messages - 1));
	// cout << "Average latency is: " << (average_time)/1000.0 << endl;
	std::tie(avg_latency, avg_std_dev) = aggregate_latency(group_members, my_id, (average_time / 1000.0), (std_dev / 1000.0));
    }
    else {
        std::tie(avg_latency, avg_std_dev) = aggregate_latency(group_members, my_id, 0.0, 0.0);
    }
    
    if(my_rank == 0) {
        log_results(exp_result{num_nodes, msg_size, num_senders_selector, delivery_mode, avg_latency, avg_std_dev}, "data_latency");
    }
    managed_group.barrier_sync();
    // flush_events();
    // for(int i = 100; i < num_messages - 100; i+= 5){
    // 	printf("%5.3f\n", (end_times[my_rank][i] - start_times[i]) * 1e-3);
    // }
    managed_group.leave();
}
