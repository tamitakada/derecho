#pragma once

#include <derecho/config.h>
#include "../derecho_modes.hpp"
#include "../subgroup_info.hpp"
#include "connection_manager.hpp"
#include "derecho/conf/conf.hpp"
#include <derecho/mutils-serialization/SerializationMacros.hpp>
#include <derecho/mutils-serialization/SerializationSupport.hpp>
#include <derecho/rdmc/rdmc.hpp>
#include <derecho/sst/multicast.hpp>
#include <derecho/sst/sst.hpp>
#include "derecho_internal.hpp"
#include "derecho_sst.hpp"
#include "persistence_manager.hpp"

#include <spdlog/spdlog.h>

#include <assert.h>
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <queue>
#include <set>
#include <tuple>
#include <vector>

namespace derecho {

/**
 * The header for an individual multicast message, which will always be the
 * first sizeof(header) bytes in the message's data buffer.
 */
struct __attribute__((__packed__)) header {
    uint32_t    header_size;
    int32_t     index;
    uint64_t    timestamp;
    uint32_t    num_nulls;
    uint8_t     cooked_send;
    uint8_t     resv_b1;
    uint8_t     resv_b2;
    uint8_t     resv_b3;
    uint64_t    resv_q4;
};

/**
 * Bundles together a set of low-level parameters for configuring Derecho
 * subgroups and shards, mostly related to the way multicast messages are sent.
 */
struct DerechoParams : public mutils::ByteRepresentable {
    /**
     * The maximum size, in bytes, of an RDMC message. Controls the size of the
     * RDMA buffers allocated by RDMC. Headers and payload must fit within this.
     */
    uint64_t max_msg_size;
    /** The maximum size (in bytes) of a message sent in reply to an ordered_send RPC message. */
    uint64_t max_reply_msg_size;
    /** The maximum size (in bytes) of an SST Multicast message. */
    uint64_t sst_max_msg_size;
    /** The size of a single block for RDMC. */
    uint64_t block_size;
    /**
     * The number of messages that can be in-progress before further sends are
     * blocked. Controls the number of message buffers that are allocated.
     */
    unsigned int window_size;
    /** The number of milliseconds between heartbeat messages sent to detect failures. */
    unsigned int heartbeat_ms;
    /** The algorithm to use for RDMC (binomial, chain, sequential, or tree). */
    rdmc::send_algorithm rdmc_send_algorithm;
    /** The TCP port to use when transferring state to new members. */
    uint32_t state_transfer_port;

    static uint64_t compute_max_msg_size(
            const uint64_t max_payload_size,
            const uint64_t block_size,
            bool using_rdmc) {
        auto max_msg_size = max_payload_size + sizeof(header);
        if(using_rdmc) {
            if(max_msg_size % block_size != 0) {
                max_msg_size = (max_msg_size / block_size + 1) * block_size;
            }
        }
        return max_msg_size;
    }

    static rdmc::send_algorithm send_algorithm_from_string(const std::string& rdmc_send_algorithm_string) {
        if(rdmc_send_algorithm_string == "binomial_send") {
            return rdmc::send_algorithm::BINOMIAL_SEND;
        } else if(rdmc_send_algorithm_string == "chain_send") {
            return rdmc::send_algorithm::CHAIN_SEND;
        } else if(rdmc_send_algorithm_string == "sequential_send") {
            return rdmc::send_algorithm::SEQUENTIAL_SEND;
        } else if(rdmc_send_algorithm_string == "tree_send") {
            return rdmc::send_algorithm::TREE_SEND;
        } else {
            throw "wrong value for RDMC send algorithm: " + rdmc_send_algorithm_string + ". Check your config file.";
        }
    }

    DerechoParams(uint64_t max_payload_size,
                  uint64_t max_reply_payload_size,
                  uint64_t max_smc_payload_size,
                  uint64_t block_size,
                  unsigned int window_size,
                  unsigned int heartbeat_ms,
                  rdmc::send_algorithm rdmc_send_algorithm,
                  uint32_t state_transfer_port)
            : max_reply_msg_size(max_reply_payload_size + sizeof(header)),
              sst_max_msg_size(max_smc_payload_size + sizeof(header)),
              block_size(block_size),
              window_size(window_size),
              heartbeat_ms(heartbeat_ms),
              rdmc_send_algorithm(rdmc_send_algorithm),
              state_transfer_port(state_transfer_port) {
        //if this is initialized above, DerechoParams turns abstract. idk why.
        max_msg_size = compute_max_msg_size(max_payload_size, block_size,
                                            max_payload_size > max_smc_payload_size);
    }

    DerechoParams() {}

    /**
     * Constructs DerechoParams specifying subgroup metadata for specified profile.
     * @param profile Name of profile in the configuration file to use.
     * @return DerechoParams.
     */
    static DerechoParams from_profile(const std::string& profile) {
        // Use the profile string to search the configuration file for the appropriate
        // settings. If they do not exist, then we should utilize the defaults
        std::string prefix = "SUBGROUP/" + profile + "/";
        for(auto& field : Conf::subgroupProfileFields) {
            if(!hasCustomizedConfKey(prefix + field)) {
                std::cout << "key" << (prefix + field)
                          << " not found in SUBGROUP section of derecho conf. "
                             " Look at derecho-sample.cfg for more information."
                          << std::endl;
                throw profile + " derecho subgroup profile not found";
            }
        }

        uint64_t max_payload_size = getConfUInt64(prefix + Conf::subgroupProfileFields[0]);
        uint64_t max_reply_payload_size = getConfUInt64(prefix + Conf::subgroupProfileFields[1]);
        uint64_t max_smc_payload_size = getConfUInt64(prefix + Conf::subgroupProfileFields[2]);
        uint64_t block_size = getConfUInt64(prefix + Conf::subgroupProfileFields[3]);
        uint32_t window_size = getConfUInt32(prefix + Conf::subgroupProfileFields[4]);
        uint32_t timeout_ms = getConfUInt32(Conf::DERECHO_HEARTBEAT_MS);
        const std::string& algorithm = getConfString(prefix + Conf::subgroupProfileFields[5]);
        uint32_t state_transfer_port = getConfUInt32(Conf::DERECHO_STATE_TRANSFER_PORT);

        return DerechoParams{
                max_payload_size,
                max_reply_payload_size,
                max_smc_payload_size,
                block_size,
                window_size,
                timeout_ms,
                DerechoParams::send_algorithm_from_string(algorithm),
                state_transfer_port,
        };
    }

    DEFAULT_SERIALIZATION_SUPPORT(DerechoParams, max_msg_size, max_reply_msg_size,
                                  sst_max_msg_size, block_size, window_size,
                                  heartbeat_ms, rdmc_send_algorithm, state_transfer_port);
};

/**
 * Represents a block of memory used to store a message. This object contains
 * both the array of bytes in which the message is stored and the corresponding
 * RDMA memory region (which has registered that array of bytes as its buffer).
 * This is a move-only type, since memory regions can't be copied.
 */
struct MessageBuffer {
    std::unique_ptr<uint8_t[]> buffer;
    std::shared_ptr<rdma::memory_region> mr;

    MessageBuffer() {}
    MessageBuffer(size_t size) {
        if(size != 0) {
            buffer = std::unique_ptr<uint8_t[]>(new uint8_t[size]);
            mr = std::make_shared<rdma::memory_region>(buffer.get(), size);
        }
    }
    MessageBuffer(const MessageBuffer&) = delete;
    MessageBuffer(MessageBuffer&&) = default;
    MessageBuffer& operator=(const MessageBuffer&) = delete;
    MessageBuffer& operator=(MessageBuffer&&) = default;
};

/**
 * A structure containing an RDMC message (which consists of some bytes in a
 * registered memory region) and some associated metadata. Note that the
 * metadata (sender_id, index, etc.) is only stored locally, not sent over the
 * network with the message.
 */
struct RDMCMessage {
    /** The unique node ID of the message's sender. */
    uint32_t sender_id;
    /** The message's index (relative to other messages sent by that sender). */
    //long long int index;
    message_id_t index;
    /** The message's size in bytes. */
    long long unsigned int size;
    /** The MessageBuffer that contains the message's body. */
    MessageBuffer message_buffer;
};

struct SSTMessage {
    /** The unique node ID of the message's sender. */
    uint32_t sender_id;
    /** The message's index (relative to other messages sent by that sender). */
    int32_t index;
    /** The message's size in bytes. */
    long long unsigned int size;
    /** Pointer to the message */
    volatile uint8_t* buf;
};

/**
 * A collection of settings for a single subgroup that this node is a member of,
 * specifically the single shard within that subgroup that this node is a member
 * of (if the subgroup is sharded). This includes the same membership and
 * delivery-mode information found in the SubView for the shard, along with some
 * lower-level details needed specifically by MulticastGroup.
 */
struct SubgroupSettings {
    /** This node's shard number within the subgroup */
    uint32_t shard_num;
    /** This node's rank within its shard of the subgroup */
    uint32_t shard_rank;
    /** The members of this node's shard of the subgroup */
    std::vector<node_id_t> members;
    /** The "is_sender" flags for members of this node's shard of the subgroup */
    std::vector<int> senders;
    /** This node's sender rank within the shard (as defined by SubView::sender_rank_of) */
    int sender_rank;
    /** The offset of this node's num_received counter within the subgroup's SST section */
    uint32_t num_received_offset;
    /** The offset of this node's slot within the subgroup's SST section */
    uint32_t slot_offset;
    /** The index of the SST index used to track SMC messages in a specific subgroup */
    uint32_t index_offset;
    /** The operation mode of the shard */
    Mode mode;
    /** The multicast parameters for the shard */
    DerechoParams profile;
};

/**
 * Additional message-delivery-related callbacks needed by MulticastGroup that
 * are not in the user-facing set of callbacks defined in UserMessageCallbacks.
 */
struct MulticastGroupCallbacks {
    /** A function to be called upon receipt of a multicast RPC message */
    rpc_handler_t rpc_callback;
    /**
     * The callback for posting the upcoming version to be delivered in a
     * subgroup to Replicated<T>. Called just before delivering a message so
     * that the user code knows the current version being handled.
     */
    subgroup_post_next_version_func_t post_next_version_callback;
    /**
     * A callback to notify internal components that a new version has reached
     * global persistence (separate from the user-defined global persistence
     * callback in UserMessageCallbacks).
     */
    persistence_callback_t global_persistence_callback;
    /**
     * A callback to notify internal components that a new version has been
     * signed and verified on all replicas (separate from the user-defined
     * verification callback in UserMessageCallbacks).
     */
    verified_callback_t global_verified_callback;
};

/** Implements the low-level mechanics of tracking multicasts in a Derecho group,
 * using RDMC to deliver messages and SST to track their arrival and stability.
 * This class should only be used as part of a Group, since it does not know how
 * to handle failures. */
class MulticastGroup {
    friend class ViewManager;

private:
    /** vector of member id's */
    std::vector<node_id_t> members;
    /** inverse map of node_ids to sst_row */
    std::map<node_id_t, uint32_t> node_id_to_sst_index;
    /**  number of members */
    const unsigned int num_members;
    /** index of the local node in the members vector, which should also be its row index in the SST */
    const int member_index;
    /** Message-delivery event callbacks, supplied by the client, for "raw" sends */
    const UserMessageCallbacks callbacks;
    /** Other message-delivery event callbacks for internal components */
    const MulticastGroupCallbacks internal_callbacks;
    uint32_t total_num_subgroups;
    /** Maps subgroup IDs (for subgroups this node is a member of) to an immutable
     * set of configuration options for that subgroup. */
    const std::map<subgroup_id_t, SubgroupSettings> subgroup_settings_map;
    /** Used for synchronizing receives by RDMC and SST */
    std::vector<std::list<int32_t>> received_intervals;
    /** Maps subgroup IDs for which this node is a sender to the RDMC group it should use to send.
     * Constructed incrementally in create_rdmc_sst_groups(), so it can't be const.  */
    std::map<subgroup_id_t, uint32_t> subgroup_to_rdmc_group;
    /** Offset to add to member ranks to form RDMC group numbers. */
    uint16_t rdmc_group_num_offset;
    /** false if RDMC groups haven't been created successfully */
    bool rdmc_sst_groups_created = false;
    /** Stores message buffers not currently in use. Protected by
     * msg_state_mtx */
    std::map<uint32_t, std::vector<MessageBuffer>> free_message_buffers;

    /** Index to be used the next time get_sendbuffer_ptr is called.
     * When next_message is not none, then next_message.index = future_message_index-1 */
    std::vector<message_id_t> future_message_indices;

    /** next_message is the message that will be sent when send is called the next time.
     * It is std::nullopt when there is no message to send. */
    std::vector<std::optional<RDMCMessage>> next_sends;
    /** For each subgroup, indicates whether an SST Multicast send is currently in progress
     * (i.e. a thread is inside the send() method). This prevents multiple application threads
     * from calling send() simultaneously and causing a race condition. */
    std::map<uint32_t, bool> smc_send_in_progress;
    std::vector<uint32_t> committed_sst_index;
    std::vector<uint32_t> num_nulls_queued;
    std::vector<int32_t> first_null_index;
    /** Messages that are ready to be sent, but must wait until the current send finishes. */
    std::vector<std::queue<RDMCMessage>> pending_sends;
    /** Vector of messages that are currently being sent out using RDMC, or boost::none otherwise. */
    /** one per subgroup */
    std::vector<std::optional<RDMCMessage>> current_sends;

    /** Messages that are currently being received. */
    std::map<std::pair<subgroup_id_t, node_id_t>, RDMCMessage> current_receives;
    /** Receiver lambdas for shards that have only one member. */
    std::map<subgroup_id_t, std::function<void(uint8_t*, size_t)>> singleton_shard_receive_handlers;

    /** Messages that have finished sending/receiving but aren't yet globally stable.
     * Organized by [subgroup number] -> [sequence number] -> [message] */
    std::map<subgroup_id_t, std::map<message_id_t, RDMCMessage>> locally_stable_rdmc_messages;
    /** Same map as locally_stable_rdmc_messages, but for SST messages */
    std::map<subgroup_id_t, std::map<message_id_t, SSTMessage>> locally_stable_sst_messages;
    /** For each subgroup, the set of timestamps associated with currently-pending
     * (not yet delivered) messages. Used to compute the stability frontier. */
    std::map<subgroup_id_t, std::set<uint64_t>> pending_message_timestamps;
    /** Tracks the timestamps of messages that are currently being written to persistent storage */
    std::map<subgroup_id_t, std::map<message_id_t, uint64_t>> pending_persistence;
    /** Messages that are currently being written to persistent storage */
    std::map<subgroup_id_t, std::map<message_id_t, RDMCMessage>> non_persistent_messages;
    /** Messages that are currently being written to persistent storage */
    std::map<subgroup_id_t, std::map<message_id_t, SSTMessage>> non_persistent_sst_messages;

    /** The next message ID that can be delivered in each subgroup, indexed by subgroup number. */
    std::vector<message_id_t> next_message_to_deliver;
    /**
     * The minimum (persistent) version number that has finished persisting in
     * each subgroup, indexed by subgroup number.
     * We use atomic counters because they will be accessed by multiple threads:
     * 1) the predicate thread will update it on delivery
     * 2) Any threads holding a handle to Replicated<T> can read it throught get_global_persistence_frontier() and
     * wait_for_global_persistence_frontier()
     */
    std::vector<std::unique_ptr<std::atomic<persistent::version_t>>> minimum_persisted_version;
    mutable std::vector<std::condition_variable> minimum_persisted_cv;
    mutable std::vector<std::mutex> minimum_persisted_mtx; // for use with minimum_persisted_cv, It does not guard minimum_persisted_version
    /**
     * The minimum (persistent) version number that has had its signature verified
     * in each subgroup, indexed by subgroup number, if the signed log feature is
     * enabled. (If the features is disabled, this will stay at INVALID_VERSION).
     * for a similar reason as that for minimum_persisted_version, we use atomic counters.
     */
    std::vector<std::unique_ptr<std::atomic<persistent::version_t>>> minimum_verified_version;


    /**
     * store the delivered_version
     */
    std::vector<std::unique_ptr<std::atomic<persistent::version_t>>> delivered_version;

    std::recursive_mutex msg_state_mtx;
    std::condition_variable_any sender_cv;

    /** The time, in milliseconds, that a sender can wait to send a message before it is considered failed. */
    unsigned int sender_timeout;

    /** Indicates that the group is being destroyed. */
    std::atomic<bool> thread_shutdown{false};
    /** The background thread that sends messages with RDMC. */
    std::thread sender_thread;

    std::thread timeout_thread;

    /** The SST, shared between this group and its GMS. */
    std::shared_ptr<DerechoSST> sst;

    /** The SSTs for multicasts **/
    std::vector<std::unique_ptr<sst::multicast_group<DerechoSST>>> sst_multicast_group_ptrs;

    using pred_handle = typename sst::Predicates<DerechoSST>::pred_handle;
    std::list<pred_handle> receiver_pred_handles;
    std::list<pred_handle> stability_pred_handles;
    std::list<pred_handle> delivery_pred_handles;
    std::list<pred_handle> persistence_pred_handles;
    std::list<pred_handle> sender_pred_handles;

    std::vector<bool> last_transfer_medium;

    /** A reference to the PersistenceManager that lives in Group, used to
     * alert it when a new version needs to be persisted. */
    PersistenceManager& persistence_manager;

    pred_handle send_load_info_handle;
    pred_handle send_cache_models_info_handle;
    std::atomic<uint64_t> last_send_load_info_timeus;
    std::atomic<uint64_t> last_send_cache_models_info_timeus;

    /** Continuously waits for a new pending send, then sends it. This function
     * implements the sender thread. */
    void send_loop();

    /** Checks for failures when a sender reaches its timeout. This function
     * implements the timeout thread. */
    void check_failures_loop();

    bool create_rdmc_sst_groups();
    void initialize_sst_row();
    void register_predicates();

    /**
     * Delivers a single message to the application layer, either by invoking
     * an RPC function or by calling a global stability callback.
     * @param msg A reference to the message
     * @param subgroup_num The ID of the subgroup this message is in
     * @param version The version assigned to the message
     * @param msg_ts The timestamp of the message
     */
    void deliver_message(RDMCMessage& msg, const subgroup_id_t& subgroup_num,
                         const persistent::version_t& version, const uint64_t& msg_timestamp);

    /**
     * Same as the other deliver_message, but for the SSTMessage type
     * @param msg A reference to the message to deliver
     * @param subgroup_num The ID of the subgroup this message is in
     * @param version The version assigned to the message
     * @param msg_ts The timestamp of this message
     */
    void deliver_message(SSTMessage& msg, const subgroup_id_t& subgroup_num,
                         const persistent::version_t& version, const uint64_t& msg_timestamp);

    /**
     * Enqueues a single message for persistence with the persistence manager.
     * Note that this does not actually wait for the message to be persisted;
     * you must still post a persistence request with the persistence manager.
     * @param msg The message that should cause a new version to be registered
     * with PersistenceManager
     * @param subgroup_num The ID of the subgroup this message is in
     * @param version The version assigned to the message
     * @param msg_ts The timestamp of this message
     * @return true if a new version was created
     * false if the message is a null message
     */
    bool version_message(RDMCMessage& msg, const subgroup_id_t& subgroup_num,
                         const persistent::version_t& version, const uint64_t& msg_timestamp);
    /**
     * Same as the other version_message, but for the SSTMessage type.
     * @param msg The message that should cause a new version to be registered
     * with PersistenceManager
     * @param subgroup_num The ID of the subgroup this message is in
     * @param version The version assigned to the message
     * @param msg_ts The timestamp of this message
     * @return true if a new version was created
     * false if the message is a null message
     */
    bool version_message(SSTMessage& msg, const subgroup_id_t& subgroup_num,
                         const persistent::version_t& version, const uint64_t& msg_timestamp);

    uint32_t get_num_senders(const std::vector<int>& shard_senders) {
        uint32_t num = 0;
        for(const auto i : shard_senders) {
            if(i) {
                num++;
            }
        }
        return num;
    };

    int32_t resolve_num_received(int32_t index, uint32_t num_received_entry);

    /* Predicate functions for receiving and delivering messages, parameterized by subgroup.
     * register_predicates will create and bind one of these for each subgroup. */

    void delivery_trigger(subgroup_id_t subgroup_num, const SubgroupSettings& subgroup_settings,
                          const uint32_t num_shard_members, DerechoSST& sst);

    void sst_send_trigger(subgroup_id_t subgroup_num, const SubgroupSettings& subgroup_settings,
                          const uint32_t num_shard_members, DerechoSST& sst);

    void sst_receive_handler(subgroup_id_t subgroup_num, const SubgroupSettings& subgroup_settings,
                             const std::map<uint32_t, uint32_t>& shard_ranks_by_sender_rank,
                             uint32_t num_shard_senders, uint32_t sender_rank,
                             volatile uint8_t* data, uint64_t size);

    bool receiver_predicate(const SubgroupSettings& subgroup_settings,
                            const std::map<uint32_t, uint32_t>& shard_ranks_by_sender_rank,
                            uint32_t num_shard_senders, const DerechoSST& sst);

    void receiver_function(subgroup_id_t subgroup_num, const SubgroupSettings& subgroup_settings,
                           const std::map<uint32_t, uint32_t>& shard_ranks_by_sender_rank,
                           uint32_t num_shard_senders, DerechoSST& sst,
                           const std::function<void(uint32_t, volatile uint8_t*, uint32_t)>& sst_receive_handler_lambda);

    void update_min_persisted_num(subgroup_id_t subgroup_num, const SubgroupSettings& subgroup_settings,
                                  uint32_t num_shard_members, DerechoSST& sst);

    void update_min_verified_num(subgroup_id_t subgroup_num, const SubgroupSettings& subgroup_settings,
                                 uint32_t num_shard_members, DerechoSST& sst);

    // Internally used to automatically send a NULL message
    void get_buffer_and_send_auto_null(subgroup_id_t subgroup_num);
    /* Get a pointer into the current buffer, to write data into it before sending
     * Now this is a private function, called by send internally */
    uint8_t* get_sendbuffer_ptr(subgroup_id_t subgroup_num, long long unsigned int payload_size, bool cooked_send);

public:
    /**
     * Standard constructor for setting up a MulticastGroup for the first time.
     * @param members A list of node IDs of members in this group
     * @param my_node_id The rank (ID) of this node in the group
     * @param sst The SST this group will use; created by the GMS (membership
     * service) for this group.
     * @param callbacks A set of user-supplied functions to call when messages
     * have reached various levels of stability
     * @param internal_callbacks Some internal functions to call in response to
     * message events (documented in MulticastGroupCallbacks)
     * @param total_num_subgroups The total number of subgroups in this Derecho
     * Group
     * @param subgroup_settings_by_id A list of SubgroupSettings, one for each
     * subgroup this node belongs to, indexed by subgroup ID
     * @param sender_timeout
     * @param persistence_manager_ref A reference to the PersistenceManager
     * that will be used to persist received messages
     * @param already_failed (Optional) A Boolean vector indicating which
     * elements of _members are nodes that have already failed in this view
     */
    MulticastGroup(
            std::vector<node_id_t> members, node_id_t my_node_id,
            std::shared_ptr<DerechoSST> sst,
            UserMessageCallbacks callbacks,
            MulticastGroupCallbacks internal_callbacks,
            uint32_t total_num_subgroups,
            const std::map<subgroup_id_t, SubgroupSettings>& subgroup_settings_by_id,
            unsigned int sender_timeout,
            PersistenceManager& persistence_manager_ref,
            std::vector<char> already_failed = {});
    /** Constructor to initialize a new MulticastGroup from an old one,
     * preserving the same settings but providing a new list of members. */
    MulticastGroup(
            std::vector<node_id_t> members, node_id_t my_node_id,
            std::shared_ptr<DerechoSST> sst,
            MulticastGroup&& old_group,
            uint32_t total_num_subgroups,
            const std::map<subgroup_id_t, SubgroupSettings>& subgroup_settings_by_id,
            std::vector<char> already_failed = {});

    ~MulticastGroup();

    void deliver_messages_upto(const std::vector<int32_t>& max_indices_for_senders, subgroup_id_t subgroup_num, uint32_t num_shard_senders);
    /** Send now internally calls get_sendbuffer_ptr.
	The user function that generates the message is supplied to send */
    bool send(subgroup_id_t subgroup_num, long long unsigned int payload_size,
              const std::function<void(uint8_t* buf)>& msg_generator, bool cooked_send);

    /** Compute the global real-time stability frontier in nano seconds.
     */
    const uint64_t compute_global_stability_frontier(subgroup_id_t subgroup_num) const;

    /** Get the global persistence frontier version of local shard in a subgroup. The global persistence frontier
     *  version is the latest version which has been persisted by all shard members, meaning that this version can
     *  survive system restart.
     */
    const persistent::version_t get_global_persistence_frontier(subgroup_id_t subgroup_num) const;

    /** Wait until the global persistence frontier of local shard in a subgroup goes beyond a given version. If the
     * version specified is a future version (greater than the latest delivered version), this call will return
     * immediately with a false return value. Otherwise, wait if necessary and return true.
     */
    bool wait_for_global_persistence_frontier(subgroup_id_t subgroup_num, persistent::version_t version) const;

    /** Get the global verified version of local shard in a subgroup. The global verified frontier version is the latest
     * version which has been verified by all shard members.
     */
    const persistent::version_t get_global_verified_frontier(subgroup_id_t subgroup_num) const;

    /** Stops all sending and receiving in this group, in preparation for shutting it down. */
    void wedge();
    /** Debugging function; prints the current state of the SST to stdout. */
    void debug_print();

    /**
     * @return a map from subgroup ID to SubgroupSettings for only those subgroups
     * that this node belongs to.
     */
    const std::map<subgroup_id_t, SubgroupSettings>& get_subgroup_settings() {
        return subgroup_settings_map;
    }
    std::vector<uint32_t> get_shard_sst_indices(subgroup_id_t subgroup_num) const;

    /** Set the load in SST load_info column for this node's member_index.
     * this function is used by upper level application TIDE to update the local
     * load information to disseminate this to all nodes in the group
     * @param load      the updated load value to set in SST.load_info of this node
     */
    void set_load_info_entry(uint64_t load);

    /** Getter of load_info for a specific node
     * @param node_id   the node, for which to get its load info
     */
    uint64_t get_load_info(node_id_t node_id);

    /** Set the local models in cache information in SST cache_modelsfield for this node's member_index.
     * @param   cache_models     an encoded uint64_t value, where each bit represent if model exists in cache
     */
    void set_cache_models_info_entry(uint64_t cache_models);

    /** Getter of cache_models_info for a specific node
     * @param node_id   the node, for which to get its models in cache info
     */
    uint64_t get_cache_models_info(node_id_t node_id);
};
}  // namespace derecho
