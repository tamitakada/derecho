#pragma once

#include <derecho/config.h>
#include "../derecho_type_definitions.hpp"
#include <derecho/sst/sst.hpp>
#include "derecho_internal.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>

namespace derecho {

using sst::SSTField;
using sst::SSTFieldVector;

/**
 * Represents a proposal to either add or remove a node from the View.
 * Includes the ID of the leader who proposed the change, as well as a flag
 * indicating whether this is the last proposed change in a View (used when
 * the leader changes to mark the end of the previous leader's proposals).
 * Although node IDs are technically 32-bit ints, in practice they should never
 * be longer than 16 bits, so we can pack both IDs into 32 bits here.
 */
struct ChangeProposal {
    uint16_t leader_id;
    uint16_t change_id;
    bool end_of_view;
};

/** "Constructor" for ChangeProposals, as a free function so they are still POD. */
inline ChangeProposal make_change_proposal(uint16_t leader_id, uint16_t change_id) {
    return ChangeProposal{leader_id, change_id, false};
}


/**
 * ViewManager and MulticastGroup will share the same SST for efficiency. This
 * class defines all the fields in this SST.
 */
class DerechoSST : public sst::SST<DerechoSST> {
public:
    // MulticastGroup members, related only to tracking message delivery
    /**
     * Sequence numbers are interpreted like a row-major pair:
     * (sender, index) becomes sender + num_members * index.
     * Since the global order is round-robin, the correct global order of
     * messages becomes a consecutive sequence of these numbers: with 4
     * senders, we expect to receive (0,0), (1,0), (2,0), (3,0), (0,1),
     * (1,1), ... which is 0, 1, 2, 3, 4, 5, ....
     *
     * This variable is the highest sequence number that has been received
     * in-order by this node at each subgroup; if a node updates seq_num[i],
     * it has received all messages up to seq_num in the global round-robin
     * order for subgroup i.
     */
    SSTFieldVector<message_id_t> seq_num;
    /**
     * This represents the highest sequence number that has been delivered
     * at this node for each subgroup; delivered_num[i] is the latest delivered
     * message for subgroup i. Messages are only delivered once stable
     * (received by all), so delivered_num[i] >= seq_num[i].
     */
    SSTFieldVector<message_id_t> delivered_num;
    /**
     * This contains this node's signature over the latest update that has been
     * delivered locally, if signatures are enabled. The vector is really an
     * array of arrays: there is one entry for each subgroup (just like
     * delivered_num), and each entry is an array of bytes of a constant size
     * (the length of a signature). The signature for subgroup i is at
     * signatures[i * signature_length].
     */
    SSTFieldVector<unsigned char> signatures;
    /**
     * This represents the highest persistent version number that has been
     * persisted to disk at this node, if persistence is enabled. This is
     * updated by the PersistenceManager, and contains one entry for each
     * subgroup.
     */
    SSTFieldVector<persistent::version_t> persisted_num;
    /**
     * This represents the highest persistent version number that has a
     * signature in its log at this node, if any persistent fields have
     * signatures enabled. Since signatures are added to log entries at
     * the same time as persistence, this is usually equal to persisted_num,
     * but it may lag behind if there are persistent fields that do not have
     * signatures enabled (hence there may be persistent versions with no
     * corresponding signature). Contains one entry per subgroup.
     */
    SSTFieldVector<persistent::version_t> signed_num;
    /**
     * This represents the highest persistent version number for which this
     * node has verified a signature from all other nodes in the subgroup, if
     * any persistent fields have signatures enabled. This is updated by the
     * PersistenceManager, and contains one entry per subgroup. It will
     * generally lag behind persisted_num, since updates are only verified once
     * they have been signed locally.
     */
    SSTFieldVector<persistent::version_t> verified_num;

    // Group management service members, related only to handling view changes
    /** View ID associated with this SST. VIDs monotonically increase as views change. */
    SSTField<int32_t> vid;
    /**
     * Array of same length as View::members, where each bool represents
     * whether the corresponding member is suspected to have failed
     */
    SSTFieldVector<bool> suspected;
    /**
     * An array of the same length as View::members, containing a list of
     * proposed changes to the view that have not yet been installed. The number
     * of valid elements is num_changes - num_installed, which should never exceed
     * View::num_members/2.
     * If request i is a Join, changes[i] is not in current View's members.
     * If request i is a Departure, changes[i] is in current View's members.
     */
    SSTFieldVector<ChangeProposal> changes;
    /**
     * If changes[i] is a Join, joiner_ips[i] is the IP address of the joining
     * node, packed into an unsigned int in network byte order. This
     * representation is necessary because SST doesn't support variable-length
     * strings.
     */
    SSTFieldVector<uint32_t> joiner_ips;
    /** joiner_xxx_ports are the port numbers for the joining nodes. */
    SSTFieldVector<uint16_t> joiner_gms_ports;
    SSTFieldVector<uint16_t> joiner_state_transfer_ports;
    SSTFieldVector<uint16_t> joiner_sst_ports;
    SSTFieldVector<uint16_t> joiner_rdmc_ports;
    SSTFieldVector<uint16_t> joiner_external_ports;
    /**
     * How many changes to the view have been proposed. Monotonically increases.
     * num_changes - num_committed is the number of pending changes, which should never
     * exceed the number of members in the current view. If num_changes == num_committed
     * == num_installed, no changes are pending.
     */
    SSTField<int> num_changes;
    /** How many proposed view changes have reached the commit point. */
    SSTField<int> num_committed;
    /**
     * How many proposed changes have been seen. Incremented by a member
     * to acknowledge that it has seen a proposed change.
     */
    SSTField<int> num_acked;
    /**
     * How many previously proposed view changes have been installed in the
     * current view. Monotonically increases, lower bound on num_committed.
     */
    SSTField<int> num_installed;
    /**
     * Local count of number of received messages by sender. For each subgroup,
     * there is a range of num_shard_senders entries in this array, and entry k
     * in that range represents the number of messages received from sender k.
     * (Thus, it's really an array of arrays, with one array per subgroup).
     * Each subgroup has a num_received_offset that indicates where its range
     * begins in this array.
     */
    SSTFieldVector<int32_t> num_received;
    /**
     * Set after calling rdmc::wedged(), reports that this member is wedged.
     * Must be after num_received!
     */
    SSTField<bool> wedged;
    /**
     * Indicates the number of messages to accept from each sender (of each
     * subgroup) in the current view change. Just like num_received, each
     * subgroup has its own range of entries in this array, starting at that
     * subgroup's num_received_offset and consisting of one entry per sender.
     */
    SSTFieldVector<int> global_min;
    /**
     * Array indicating whether each shard leader (indexed by subgroup number)
     * has published a global_min for the current view change
     */
    SSTFieldVector<bool> global_min_ready;
    /** for SST multicast */
    SSTFieldVector<uint8_t> slots;
    SSTFieldVector<int32_t> num_received_sst;
    SSTFieldVector<int32_t> index;

    /** to check for failures - used by the thread running check_failures_loop in derecho_group **/
    SSTFieldVector<uint64_t> local_stability_frontier;

    /** to signal a graceful exit */
    SSTField<bool> rip;

    /**
     * Application field: For TIDE scheduler to multicast the load information.
     * For each member  each int represents
     * the loading information(queue length) of the member.
     */
    SSTField<uint64_t> load_info;
    SSTField<uint64_t> cache_models_info; 

    /**
     * Constructs an SST, and initializes the GMS fields to "safe" initial values
     * (0, false, etc.). Initializing the MulticastGroup fields is left to MulticastGroup.
     * @param   parameters          The SST parameters, which will be forwarded to the
     *                      standard SST constructor.
     * @param   num_subgroups       Number of the subgroups
     * @param   signature_size      Size of the signature
     * @param   num_received_size
     * @param   slot_size
     * @param   index_field_size
     */
    DerechoSST(const sst::SSTParams& parameters, uint32_t num_subgroups, uint32_t signature_size, uint32_t num_received_size, uint64_t slot_size, uint32_t index_field_size)
            : sst::SST<DerechoSST>(this, parameters),
              seq_num(num_subgroups),
              delivered_num(num_subgroups),
              signatures(num_subgroups * signature_size),
              persisted_num(num_subgroups),
              signed_num(num_subgroups),
              verified_num(num_subgroups),
              suspected(parameters.members.size()),
              changes(100 + parameters.members.size()),  // The extra 100 entries allows for more joins at startup, when the group is very small
              joiner_ips(100 + parameters.members.size()),
              joiner_gms_ports(100 + parameters.members.size()),
              joiner_state_transfer_ports(100 + parameters.members.size()),
              joiner_sst_ports(100 + parameters.members.size()),
              joiner_rdmc_ports(100 + parameters.members.size()),
              joiner_external_ports(100 + parameters.members.size()),
              num_received(num_received_size),
              global_min(num_received_size),
              global_min_ready(num_subgroups),
              slots(slot_size),
              num_received_sst(num_received_size),
              index(index_field_size),
              local_stability_frontier(num_subgroups){
        SSTInit(seq_num, delivered_num, signatures,
                persisted_num, signed_num, verified_num,
                vid, suspected, changes, joiner_ips,
                joiner_gms_ports, joiner_state_transfer_ports, joiner_sst_ports, joiner_rdmc_ports, joiner_external_ports,
                num_changes, num_committed, num_acked, num_installed,
                num_received, wedged, global_min, global_min_ready,
                slots, num_received_sst, index, local_stability_frontier, rip, load_info, cache_models_info);
        // Once superclass constructor has finished, table entries can be initialized
        for(unsigned int row = 0; row < get_num_rows(); ++row) {
            vid[row] = 0;
            for(size_t i = 0; i < suspected.size(); ++i) {
                suspected[row][i] = false;
            }
            for(size_t i = 0; i < changes.size(); ++i) {
                changes[row][i].leader_id = 0;
                changes[row][i].change_id = 0;
                changes[row][i].end_of_view = false;
            }
            for(size_t i = 0; i < global_min_ready.size(); ++i) {
                global_min_ready[row][i] = false;
            }
            for(size_t i = 0; i < global_min.size(); ++i) {
                global_min[row][i] = 0;
            }
            memset(const_cast<uint32_t*>(joiner_ips[row]), 0, joiner_ips.size());
            memset(const_cast<uint16_t*>(joiner_gms_ports[row]), 0, joiner_gms_ports.size());
            memset(const_cast<uint16_t*>(joiner_state_transfer_ports[row]), 0, joiner_state_transfer_ports.size());
            memset(const_cast<uint16_t*>(joiner_sst_ports[row]), 0, joiner_sst_ports.size());
            memset(const_cast<uint16_t*>(joiner_rdmc_ports[row]), 0, joiner_rdmc_ports.size());
            memset(const_cast<uint16_t*>(joiner_external_ports[row]), 0, joiner_external_ports.size());
            num_changes[row] = 0;
            num_committed[row] = 0;
            num_installed[row] = 0;
            num_acked[row] = 0;
            wedged[row] = false;
            // start off local_stability_frontier with the current time
            auto current_time_ns = get_walltime();
            for(size_t i = 0; i < local_stability_frontier.size(); ++i) {
                local_stability_frontier[row][i] = current_time_ns;
            }
            rip[row] = false;
            cache_models_info[row] = 0;
            load_info[row] = 0;
        }
    }

    /**
     * Initializes the local row of this SST based on the specified row of the
     * previous View's SST. Copies num_changes, num_committed, and num_acked,
     * adds num_changes_installed to the previous value of num_installed, copies
     * (num_changes - num_changes_installed) elements of changes, and initializes
     * the other SST fields to 0/false.
     * @param old_sst The SST instance to copy data from
     * @param row The target row in that SST instance (from which data will be copied)
     * @param num_changes_installed The number of changes that were applied
     * when changing from the previous view to this one
     */
    void init_local_row_from_previous(const DerechoSST& old_sst, const int row, const int num_changes_installed);

    /**
     * Copies currently proposed changes and the various counter values associated
     * with them to the local row from some other row (i.e. the group leader's row).
     * @param other_row The row to copy values from.
     */
    void init_local_change_proposals(const int other_row);

    /**
     * Pushes the entire local SST row except the SMC slots.
     */
    void push_row_except_slots();

    /**
     * Creates a string representation of the local row (not the whole table).
     * This should be converted to an ostream operator<< to follow standards.
     */
    std::string to_string() const;
};

namespace gmssst {

/**
 * Thread-safe setter for DerechoSST members; ensures there is a
 * std::atomic_signal_fence after writing the value.
 * @param e A reference to a member of GMSTableRow.
 * @param value The value to set that reference to.
 */
template <typename Elem>
void set(volatile Elem& e, const Elem& value) {
    e = value;
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

/**
 * Thread-safe setter for DerechoSST members; ensures there is a
 * std::atomic_signal_fence after writing the value.
 * @param e A reference to a member of GMSTableRow.
 * @param value The value to set that reference to.
 */
template <typename Elem>
void set(volatile Elem& e, volatile const Elem& value) {
    e = value;
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

/**
 * Thread-safe setter for DerechoSST members that are arrays; takes a lock
 * before running memcpy, and then ensures there is an atomic_signal_fence.
 * The first {@code length} members of {@code value} are copied to {@code array}.
 * @param array A pointer to the first element of an array that should be set
 * to {@code value}, obtained by calling SSTFieldVector::operator[]
 * @param value A pointer to the first element of an array to read values from
 * @param length The number of array elements to copy
 */
template <typename Elem>
void set(volatile Elem* array, volatile Elem* value, const size_t length) {
    static thread_local std::mutex set_mutex;
    {
        std::lock_guard<std::mutex> lock(set_mutex);
        memcpy(const_cast<Elem*>(array), const_cast<Elem*>(value),
               length * sizeof(Elem));
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
}
/**
 * Thread-safe setter for DerechoSST members that are arrays; takes a lock
 * before running memcpy, and then ensures there is an atomic_signal_fence.
 * This version copies the entire array, and assumes both arrays are the same
 * length.
 *
 * @param e A reference to an array-type member of GMSTableRow
 * @param value The array whose contents should be copied to this member
 */
template <typename Arr, size_t Len>
void set(volatile Arr (&e)[Len], const volatile Arr (&value)[Len]) {
    static thread_local std::mutex set_mutex;
    {
        std::lock_guard<std::mutex> lock(set_mutex);
        memcpy(const_cast<Arr(&)[Len]>(e), const_cast<const Arr(&)[Len]>(value),
               Len * sizeof(Arr));
        // copy_n just plain doesn't work, claiming that its argument types are
        // "not assignable"
        //        std::copy_n(const_cast<const Arr (&)[Len]>(value), Len,
        //        const_cast<Arr (&)[Len]>(e));
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

/**
 * Thread-safe setter for DerechoSST members that are arrays; takes a lock
 * before running memcpy, and then ensures there is an atomic_signal_fence.
 * This version only copies the first num elements of the source array.
 * @param dst
 * @param src
 * @param num
 */
template <size_t L1, size_t L2, typename Arr>
void set(volatile Arr (&dst)[L1], const volatile Arr (&src)[L2], const size_t& num) {
    static thread_local std::mutex set_mutex;
    {
        std::lock_guard<std::mutex> lock(set_mutex);
        memcpy(const_cast<Arr(&)[L2]>(dst), const_cast<const Arr(&)[L1]>(src),
               num * sizeof(Arr));
    }
    std::atomic_signal_fence(std::memory_order_acq_rel);
}

void set(volatile ChangeProposal& member, const ChangeProposal& value);

void set(volatile char* string_array, const std::string& value);

void increment(volatile int& member);

bool equals(const volatile char& string_array, const std::string& value);

}  // namespace gmssst

}  // namespace derecho
