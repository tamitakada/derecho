#pragma once

/**
 * @file    group.hpp
 * @brief   Declaration of the `Group<>` class template.
 */

#include <derecho/config.h>
#include <derecho/conf/conf.hpp>
#include <derecho/tcp/tcp.hpp>
#include "derecho_exception.hpp"
#include "detail/derecho_internal.hpp"
#include "detail/persistence_manager.hpp"
#include "detail/rpc_manager.hpp"
#include "detail/view_manager.hpp"
#include "notification.hpp"
#include "replicated.hpp"
#include "subgroup_info.hpp"

#include <mutils-containers/KindMap.hpp>
#include <mutils-containers/TypeMap2.hpp>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <iostream>
#include <list>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

/**
 * @brief   The umbrella namespace for derecho.
 */
namespace derecho {
/**
 * The function that implements index_of_type, which is separate to hide the
 * "counter" template parameter (an implementation detail only used to maintain
 * state across recursive calls).
 */
template <uint32_t counter, typename TargetType, typename FirstType, typename... RestTypes>
constexpr uint32_t index_of_type_impl() {
    if constexpr(std::is_same<TargetType, FirstType>::value)
        return counter;
    else
        return index_of_type_impl<counter + 1, TargetType, RestTypes...>();
}

/**
 * A compile-time "function" that computes the index of a type within a template
 * parameter pack of types. The value of this constant is equal to the index of
 * TargetType within TypePack. (The compiler will spew template errors if
 * TargetType is not actually in TypePack).
 * @tparam TargetType The type to search for in the parameter pack
 * @tparam TypePack The template parameter pack that should be searched
 */
template <typename TargetType, typename... TypePack>
constexpr inline uint32_t index_of_type = index_of_type_impl<0, TargetType, TypePack...>();

/**
 * A type-trait-like template that provides a True member "value" if TargetType
 * matches some type in TypePack (according to std::is_same), or provides a
 * False member "value" if TargetType does not match anything in TypePack.
 */
template <typename TargetType, typename... TypePack>
using contains = std::integral_constant<bool, (std::is_same<TargetType, TypePack>::value || ...)>;

//Type alias for a sparse-vector of Replicated, otherwise KindMap can't understand it's a template
template <typename T>
using replicated_index_map = std::map<uint32_t, Replicated<T>>;

/**
 * Type-erased base class for Group that provides the public API functions
 * (get_subgroup, get_nonmember_subgroup, etc.). Each method is implemented
 * by dynamically casting "this" to a GroupProjection of the correct subgroup
 * type and then forwarding the call to GroupProjection.
 */
class _Group {
private:
protected:
    virtual uint32_t get_index_of_type(const std::type_info&) const = 0;

public:
    virtual ~_Group() = default;

    template <typename SubgroupType>
    auto& get_subgroup(uint32_t subgroup_index = 0);

    template <typename SubgroupType>
    uint32_t get_subgroup_max_payload_size(uint32_t subgroup_index = 0);

    template <typename SubgroupType>
    auto& get_nonmember_subgroup(uint32_t subgroup_num = 0);

    template <typename SubgroupType>
    ExternalClientCallback<SubgroupType>& get_client_callback(uint32_t subgroup_index = 0);

    template <typename SubgroupType>
    std::size_t get_number_of_shards(uint32_t subgroup_index = 0);

    template <typename SubgroupType>
    uint32_t get_num_subgroups();

    template <typename SubgroupType>
    std::vector<std::vector<node_id_t>> get_subgroup_members(uint32_t subgroup_index = 0);

    template <typename SubgroupType>
    std::vector<std::vector<IpAndPorts>> get_subgroup_member_addresses(uint32_t subgroup_index = 0);

    virtual node_id_t get_my_id() = 0;

    virtual node_id_t get_rpc_caller_id() = 0;

    virtual uint64_t get_max_p2p_request_payload_size() = 0;

    virtual uint64_t get_max_p2p_reply_payload_size() = 0;

};

/**
 * A base class for Group that provides the subgroup-related API functions
 * for only a single subgroup type; thus it is a projection of the
 * Group<ReplicatedTypes...> onto a single ReplicatedType. Group will inherit
 * from one copy of this class for each ReplicatedType in its parameter pack.
 *
 * @tparam ReplicatedType The subgroup type that this GroupProjection can operate on
 */
template <typename ReplicatedType>
class GroupProjection : public virtual _Group {
protected:
    virtual void set_replicated_pointer(std::type_index, uint32_t, void**) = 0;
    virtual void set_peer_caller_pointer(std::type_index, uint32_t, void**) = 0;
    virtual void set_external_client_pointer(std::type_index, uint32_t, void**) = 0;
    virtual ViewManager& get_view_manager() = 0;

public:
    Replicated<ReplicatedType>& get_subgroup(uint32_t subgroup_index = 0);
    uint32_t get_subgroup_max_payload_size(uint32_t subgroup_index = 0);
    PeerCaller<ReplicatedType>& get_nonmember_subgroup(uint32_t subgroup_index = 0);
    ExternalClientCallback<ReplicatedType>& get_client_callback(uint32_t subgroup_index = 0);
    std::vector<std::vector<node_id_t>> get_subgroup_members(uint32_t subgroup_index = 0);
    std::vector<std::vector<IpAndPorts>> get_subgroup_member_addresses(uint32_t subgroup_index = 0);
    std::size_t get_number_of_shards(uint32_t subgroup_index = 0);
    uint32_t get_num_subgroups();
};

/**
 * A base class that user-defined replicated object types (i.e. the T in
 * Replicated<T>) can inherit from to get access to a type-erased pointer to
 * the Group object that manages them. This pointer can be used to call some of
 * Group's public API methods, such as get_subgroup() and get_subgroup_members().
 */
class GroupReference {
public:
    /** A pointer to the Group that contains the replicated object */
    _Group* group;
    uint32_t subgroup_index;
    void set_group_pointers(_Group* group, uint32_t subgroup_index) {
        this->group = group;
        this->subgroup_index = subgroup_index;
    }
};

/**
 * The top-level object for creating a Derecho group. This implements the group
 * management service (GMS) features and contains a MulticastGroup instance that
 * manages the actual sending and tracking of messages within the group.
 * @tparam ReplicatedTypes The types of user-provided objects that will represent
 * state and RPC functions for subgroups of this group.
 */
template <typename... ReplicatedTypes>
class Group : public virtual _Group, public GroupProjection<ReplicatedTypes>... {
public:
    //Functions implementing GroupProjection<ReplicatedTypes>...
    void set_replicated_pointer(std::type_index type, uint32_t subgroup_index, void** returned_replicated_ptr) override;
    void set_peer_caller_pointer(std::type_index type, uint32_t subgroup_index, void** returned_peercaller_ptr) override;
    void set_external_client_pointer(std::type_index type, uint32_t subgroup_index, void** returned_external_client_ptr) override;

protected:
    uint32_t get_index_of_type(const std::type_info&) const override;
    ViewManager& get_view_manager() override;

private:
    using pred_handle = sst::Predicates<DerechoSST>::pred_handle;

    //Type alias for a sparse-vector of PeerCaller
    template <typename T>
    using peer_caller_index_map = std::map<uint32_t, PeerCaller<T>>;

    template <typename T>
    using external_client_callback_map = std::map<uint32_t, ExternalClientCallback<T>>;

    const node_id_t my_id;
    /**
     * A list (possibly empty) of user-provided deserialization contexts that are
     * needed to help deserialize the Replicated Objects. These should be passed
     * into the from_bytes method whenever from_bytes is called on a Replicated
     * Object. They are stored as raw pointers, even though it is more dangerous,
     * because Weijia found that trying to store a shared_ptr here complicated
     * things: the deserialization context is generally a big object containing
     * the group handle; however, the group handle need to hold a shared pointer
     * to the object, which causes a dependency loop and results in an object
     * indirectly holding a shared pointer to its self.
     */
    std::vector<DeserializationContext*> user_deserialization_context;

    /** Persist the objects. Once persisted, persistence_manager updates the SST
     * so that the persistent progress is known by group members. */
    PersistenceManager persistence_manager;
    /** Contains all state related to managing Views, including the
     * MulticastGroup and SST (since those change when the view changes). */
    ViewManager view_manager;
    /** Contains all state related to receiving and handling RPC function
     * calls for any Replicated objects implemented by this group. */
    rpc::RPCManager rpc_manager;
    /** Maps a type to the Factory for that type. */
    mutils::KindMap<Factory, ReplicatedTypes...> factories;
    /**
     * Maps each type T to a map of (index -> Replicated<T>) for that type's
     * subgroup(s). If this node is not a member of a subgroup for a type, the
     * map will have no entry for that type and index. (Instead, peer_callers
     * will have an entry for that type-index pair). If this node is a member
     * of a subgroup, the Replicated<T> will refer to the one shard that this
     * node belongs to.
     */
    mutils::KindMap<replicated_index_map, ReplicatedTypes...> replicated_objects;
    /**
     * Maps each type T to a map of (index -> PeerCaller<T>) for the
     * subgroup(s) of that type that this node is not a member of. The
     * PeerCaller for subgroup i of type T can be used to contact any member
     * of any shard of that subgroup, so shards are not indexed.
     */
    mutils::KindMap<peer_caller_index_map, ReplicatedTypes...> peer_callers;
    /**
     * Same thing as peer_callers, but with ExternalClientCallback<T>
     */
    mutils::KindMap<external_client_callback_map, ReplicatedTypes...> external_client_callbacks;
    /**
     * Alternate view of the Replicated<T>s, indexed by subgroup ID. The entry
     * at index X is a pointer to the Replicated<T> for this node's shard of
     * subgroup X, if this node is a member of subgroup X. (There is no entry
     * if this node is not a member of the subgroup). The pointers are the
     * abstract base type ReplicatedObject so that they can be used by
     * components like ViewManager that don't know the parameter types (T), and
     * consequently they can't be used to call ordered_send. Although these
     * pointers are never null during normal operation (they are simply
     * observers for objects that already exist in replicated_objects), it is
     * NOT safe to use them outside of view_manager's predicates without owning
     * a read lock on curr_view: a pointer may become temporarily null during a
     * view change if the Replicated<T> it points to is reconstructed by the view
     * change, and the map entry might disappear entirely if this node is
     * removed from the subgroup.
     */
    std::map<subgroup_id_t, ReplicatedObject*> objects_by_subgroup_id;

    /**
     * Updates the state of the replicated objects that correspond to subgroups
     * identified in the provided map, by receiving serialized state from the
     * shard leader whose ID is paired with that subgroup ID.
     * @param subgroups_and_leaders Pairs of (subgroup ID, leader's node ID) for
     * subgroups that need to have their state initialized from the leader.
     */
    void receive_objects(const std::set<std::pair<subgroup_id_t, node_id_t>>& subgroups_and_leaders);

    /** Base case for new_view_callback_per_type with an empty parameter pack, does nothing */
    template <typename... Empty>
    std::enable_if_t<0 == sizeof...(Empty)> new_view_callback_per_type(const View&){};

    /**
     * A helper method for new_view_callback that unpacks the Group's template
     * parameter pack and uses each type to access replicated_objects. This is
     * the only way to iterate through the KindMap.
     */
    template <typename FirstType, typename... RestTypes>
    void new_view_callback_per_type(const View& new_view);

    /**
     * A method that matches ViewManager's view_upcall_t interface, to be called
     * by ViewManager after a new view has finished being installed. Forwards
     * the new-view notification to each Replicated<T>, in case the user-provided
     * object (T) wants to receive it.
     *
     * @param new_view The new View
     */
    void new_view_callback(const View& new_view);

    /** Constructor helper that wires together the component objects of Group. */
    void set_up_components();

    /**
     * Base case for the construct_objects template. Note that the neat "varargs
     * trick" (defining construct_objects(...) as the base case) doesn't work
     * because varargs can't match const references, and will force a copy
     * constructor on View. So std::enable_if is the only way to match an empty
     * template pack.
     */
    template <typename... Empty>
    std::enable_if_t<0 == sizeof...(Empty),
                     std::set<std::pair<subgroup_id_t, node_id_t>>>
    construct_objects(const View&, const vector_int64_2d&, bool) {
        return std::set<std::pair<subgroup_id_t, node_id_t>>();
    }

    /**
     * Constructor helper that unpacks this Group's template parameter pack.
     * Constructs Replicated<T> wrappers for each object being replicated,
     * using the corresponding Factory<T> saved in Group::factories. If this
     * node is not a member of the subgroup for a type T, an "empty" Replicated<T>
     * will be constructed with no corresponding object. If this node is joining
     * an existing group and there was a previous leader for its shard of a
     * subgroup, an "empty" Replicated<T> will also be constructed for that
     * subgroup, since all object state will be received from the shard leader.
     *
     * @param curr_view A reference to the current view as reported by View_manager
     * @param old_shard_leaders The array of old shard leaders for each subgroup
     * (indexed by subgroup ID), which will contain -1 if there is no previous
     * leader for that shard.
     * @param in_restart True if this node is in restart mode (in which case this
     * function may be called multiple times if the restart view is aborted)
     * @return The set of subgroup IDs that are un-initialized because this node is
     * joining an existing group and needs to receive initial object state, paired
     * with the ID of the node that should be contacted to receive that state.
     */
    template <typename FirstType, typename... RestTypes>
    std::set<std::pair<subgroup_id_t, node_id_t>> construct_objects(
            const View& curr_view, const vector_int64_2d& old_shard_leaders,
            bool in_restart);

public:
    /**
     * Constructor that starts or joins a Derecho group. Whether this node acts
     * as the leader of a new group or joins an existing group is determined by
     * the Derecho configuration file (loaded by the conf module).
     *
     * @param callbacks The set of callback functions for message delivery
     * events in this group.
     * @param subgroup_info The set of functions that define how membership in
     * each subgroup and shard will be determined in this group.
     * @param deserialization_context A list of pointers to deserialization
     * contexts, which are objects needed to help deserialize user-provided
     * Replicated Object classes. These context pointers will be provided in
     * the DeserializationManager argument to from_bytes any time a Replicated
     * Object is deserialized, e.g. during state transfer. The calling
     * application is responsible for keeping these objects alive during the
     * lifetime of the Group, since Group does not own the pointers.
     * @param _view_upcalls A list of functions to be called when the group
     * experiences a View-Change event (optional).
     * @param factories A variable number of Factory functions, one for each
     * template parameter of Group, providing a way to construct instances of
     * each Replicated Object
     */
    Group(const UserMessageCallbacks& callbacks,
          const SubgroupInfo& subgroup_info,
          const std::vector<DeserializationContext*>& deserialization_context,
          std::vector<view_upcall_t> _view_upcalls = {},
          Factory<ReplicatedTypes>... factories);

    /**
     * Constructor that starts or joins a Derecho group. Whether this node acts
     * as the leader of a new group or joins an existing group is determined by
     * the Derecho configuration file (loaded by the conf module).
     *
     * @param subgroup_info The set of functions that define how membership in
     * each subgroup and shard will be determined in this group.
     * @param factories A variable number of Factory functions, one for each
     * template parameter of Group, providing a way to construct instances of
     * each Replicated Object
     */
    Group(const SubgroupInfo& subgroup_info, Factory<ReplicatedTypes>... factories);

    ~Group();

    /**
     * Gets the "handle" for the subgroup of the specified type and index,
     * which is a Replicated<T>, assuming this node is a member of the desired
     * subgroup. The Replicated<T> will contain the replicated state of an
     * object of type T (if it has any state) and be usable to send multicasts
     * to this node's shard of the subgroup.
     *
     * @param subgroup_index The index of the subgroup within the set of
     * subgroups that replicate the same type of object. Defaults to 0, so
     * if there is only one subgroup of type T, it can be retrieved with
     * get_subgroup<T>();
     * @tparam SubgroupType The object type identifying the subgroup
     * @return A reference to a Replicated<SubgroupType>
     * @throws subgroup_provisioning_exception If there are no subgroups because
     * the current View is inadequately provisioned
     * @throws invalid_subgroup_exception If this node is not a member of the
     * requested subgroup.
     */
    template <typename SubgroupType>
    Replicated<SubgroupType>& get_subgroup(uint32_t subgroup_index = 0);

    /**
     * Gets the "handle" for a subgroup of the specified type and index,
     * assuming this node is not a member of the subgroup. The returned
     * PeerCaller can be used to make peer-to-peer RPC calls to a specific
     * member of the subgroup.
     *
     * @param subgroup_index The index of the subgroup within the set of
     * subgroups that replicate the same type of object.
     * @tparam SubgroupType The object type identifying the subgroup
     * @return A reference to the PeerCaller for this subgroup
     * @throws invalid_subgroup_exception If this node is actually a member of
     * the requested subgroup, or if no such subgroup exists
     */
    template <typename SubgroupType>
    PeerCaller<SubgroupType>& get_nonmember_subgroup(uint32_t subgroup_index = 0);

    /**
     * Get an ExternalClientCallback object that can be used to send P2P messages
     * to external clients of a specific subgroup
     * @tparam SubgroupType The subgroup type
     * @param subgroup_index The index of the subgroup within SubgroupType
     * @return An ExternalClientCallback that can send notifications to external
     * clients of the requested subgroup
     */
    template <typename SubgroupType>
    ExternalClientCallback<SubgroupType>& get_client_callback(uint32_t subgroup_index = 0);

    /**
     * Get a ShardIterator object that can be used to send P2P messages to every
     * shard within a specific subgroup.
     * @tparam SubgroupType The subgroup type to communicate with
     * @param subgroup_index The index of the subgroup within SubgroupType
     * to communicate with
     * @return A ShardIterator that will contact one member of each shard in
     * the subgroup identified by (SubgroupType, subgroup_index)
     */
    template <typename SubgroupType>
    ShardIterator<SubgroupType> get_shard_iterator(uint32_t subgroup_index = 0);

    /**
     * Causes this node to cleanly leave the group by setting itself to "failed."
     * @param group_shutdown True if all nodes in the group are going to leave.
     */
    void leave(bool group_shutdown = true);

    /** @returns a vector listing the nodes that are currently members of the group. */
    std::vector<node_id_t> get_members();

    /**
     * Returns a vector listing the IP addresses (and Derecho ports) of nodes that
     * are currently members of the group, in the same order as the node ID list
     * returned by get_members(). Although the list includes the set of Derecho ports
     * for each member, note that Derecho has already made connections to the other
     * nodes on each of these ports, so they should not be used by the caller to
     * initiate a new connection.
     */
    std::vector<IpAndPorts> get_member_addresses();

    /**
     * Returns the number of subgroups of the specified type. This information
     * is also in the configuration file or SubgroupInfo function, but this method
     * is provided for convenience.
     * @tparam SubgroupType the subgroup type
     * @return The number of subgroups of type SubgroupType
     */
    template <typename SubgroupType>
    uint32_t get_num_subgroups();

    /**
     * Gets a list of the nodes currently assigned to the subgroup of the
     * specified type and index, organized by shard. The outer vector has an
     * entry for each shard in the subgroup, and the vector at each position
     * contains the IDs of the nodes in that shard.
     * @tparam SubgroupType the subgroup type
     * @param subgroup_index The index of the subgroup (of the same type)
     * @return A vector of vectors, where the outer index represents a shard
     * number, and the inner index counts individual nodes in that shard.
     */
    template <typename SubgroupType>
    std::vector<std::vector<node_id_t>> get_subgroup_members(uint32_t subgroup_index = 0);

    /**
     * Gets a list of IP addresses of nodes currently assigned to the subgroup
     * of the specified type and index, organized by shard. This has the same
     * structure as the vector returned by get_subgroup_members() but with IP
     * address-and-port structures in place of node IDs.
     * @tparam SubgroupType the subgroup type
     * @param subgroup_index The index of the subgroup (of the same type)
     * @return A vector of vectors, where the outer index represents a shard
     * number, and the inner index counts individual nodes in that shard.
     */
    template <typename SubgroupType>
    std::vector<std::vector<IpAndPorts>> get_subgroup_member_addresses(uint32_t subgroup_index = 0);

    /** @returns the order of this node in the sequence of members of the group */
    std::int32_t get_my_rank();

    /** @returns the ID of local node */
    node_id_t get_my_id() override;

    /** @returns the id of the lastest rpc caller, only valid when called from an RPC handler */
    node_id_t get_rpc_caller_id() override;

    /** @returns the maximal allowed p2p request payload size */
    uint64_t get_max_p2p_request_payload_size() override;

    /** @returns the maximal allowed p2p reply payload size */
    uint64_t get_max_p2p_reply_payload_size() override;

    /**
     * @returns the shard number that this node is a member of in the specified
     * subgroup (by subgroup type and index), or -1 if this node is not a member
     * of any shard in the specified subgroup.
     * @tparam SubgroupType the subgroup type
     */
    template <typename SubgroupType>
    std::int32_t get_my_shard(uint32_t subgroup_index = 0);

    /**
     * Lists the subgroup index(es) that this node is a member of for the
     * specified subgroup type. Note that a node may be a member of more than
     * one subgroup of the same type. If this node is not a member of any
     * subgroups of this type, the returned vector will be empty.
     * @tparam SubgroupType The type of subgroup to check for membership in
     * @return a vector of subgroup indexes, or an empty vector if this node
     * is not a member of any subgroup of type SubgroupType
     */
    template <typename SubgroupType>
    std::vector<uint32_t> get_my_subgroup_indexes();

    
    /** Set the load of this member to the load_info column in SST.
     * @param load   the updated load value to set in SST.load_info of this node
     */
    void set_my_load_info(uint64_t load);
    /** Get the load_info of a specific node in the group.
     * @param node_id   the node, for which to get its load info
     */
    uint64_t get_load_info(node_id_t node_id);

    /** Set the local models in cache information in SST cache_models field for this node's member_index.
     * @param cache_models  an encoded uint64_t value, where each bit represent if model exists in cache
     */
    void set_my_cache_models_info(uint64_t cache_models);
    /** Get the models_in_cache_info of all members in the group.
     * @param node_id  the node, for which to get its models in cache info
     */
    uint64_t get_cache_models_info(node_id_t node_id);


    /** Reports to the GMS that the given node has failed. */
    void report_failure(const node_id_t who);
    /** Waits until all members of the group have called this function. */
    void barrier_sync();
    void debug_print_status() const;

    /**
     * @brief Register Out-of-band memory region
     *
     * @param addr      The address of the memory region
     * @param size      The size in bytes of the memory region
     *
     * @throw           derecho::derecho_exception on failure
     */
    void register_oob_memory(void* addr, size_t size);

    /**
     * @brief Register Out-of-band memory region with extended arguments
     * Please check fi_mr_regattr documentation for more explanation:
     * https://ofiwg.github.io/libfabric/v1.12.1/man/fi_mr.3.html
     * 
     * @param addr      The address of the memory region
     * @param size      The size in bytes of the memory region
     * @param attr      The memory attribute
     *
     * @throw           derecho::derecho_exception on failure
     */
    void register_oob_memory_ex(void* addr, size_t size, const memory_attribute_t& attr);

    /**
     * Get the Out-of-band memory region's remote access key
     *
     * @param[in]   addr    The address of the memory region
     *
     * @return      The remote access key
     * @throw       derecho::derecho_exception if the address does not fall in any memory region
     */
    uint64_t get_oob_memory_key(void* addr);

    /**
     * @brief Deregister Out-of-band memory region
     *
     * @param[in]   addr    The address of the memory region, which has been used for register_oob_memory
     * @throw       derecho::derecho_exception on failure
     */
    void deregister_oob_memory(void* addr);
};

} /* namespace derecho */

#include "detail/group_impl.hpp"
