#pragma once

#ifdef USE_VERBS_API
#include "derecho/sst/detail/verbs.hpp"
#else
#include "derecho/sst/detail/lf.hpp"
#endif

#include <atomic>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace sst {
class P2PConnectionManager;

enum MESSAGE_TYPE {
    P2P_REPLY = 0,
    P2P_REQUEST,
    RPC_REPLY
};

std::ostream& operator<<(std::ostream& os, MESSAGE_TYPE mt);

static const MESSAGE_TYPE p2p_message_types[] = {P2P_REPLY,
                                                 P2P_REQUEST,
                                                 RPC_REPLY};
static const uint8_t num_p2p_message_types = 3;

struct ConnectionParams {
    uint32_t window_sizes[num_p2p_message_types];
    uint32_t max_msg_sizes[num_p2p_message_types];
    uint64_t offsets[num_p2p_message_types];
};

/**
 * A pointer to a P2P message buffer, bundled with the sequence number of the
 * buffer as generated by get_sendbuffer_ptr(). This sequence number can be
 * used to send the message contained in the buffer once it is filled.
 */
struct P2PBufferHandle {
    uint8_t* buf_ptr;
    uint64_t seq_num;
};

class P2PConnection {
    const uint32_t my_node_id;
    const uint32_t remote_id;
    const ConnectionParams& connection_params;
    std::shared_ptr<spdlog::logger> rpc_logger;
    std::unique_ptr<volatile uint8_t[]> incoming_p2p_buffer;
    std::unique_ptr<volatile uint8_t[]> outgoing_p2p_buffer;
    std::unique_ptr<resources> res;
    std::map<MESSAGE_TYPE, std::atomic<uint64_t>> incoming_seq_nums_map, outgoing_seq_nums_map;
    uint64_t getOffsetSeqNum(MESSAGE_TYPE type, uint64_t seq_num);
    uint64_t getOffsetBuf(MESSAGE_TYPE type, uint64_t seq_num);

protected:
    friend class P2PConnectionManager;
    resources* get_res();
    uint32_t num_rdma_writes = 0;

public:
    P2PConnection(uint32_t my_node_id, uint32_t remote_id, uint64_t p2p_buf_size,
                  const ConnectionParams& connection_params);
    ~P2PConnection();

    /**
     * Returns the pair (pointer into an incoming message buffer, type of message)
     * if there is a new incoming message from the remote node, or std::nullopt if
     * there are no new messages.
     */
    std::optional<std::pair<uint8_t*, MESSAGE_TYPE>> probe();
    /**
     * Increments the incoming sequence number for the specified message type,
     * indicating that the caller is finished handling the current incoming
     * message of that type.
     */
    void increment_incoming_seq_num(MESSAGE_TYPE type);
    /**
     * Returns a MessageHandle containing a pointer to the beginning of the
     * next available message buffer for the specified message type and the
     * sequence number associated with that buffer, then increments the
     * outgoing message sequence number. If no message buffer is available,
     * returns std::nullopt and does not increment the outgoing sequence number.
     * @param type The message type, which identifies the buffer region to use.
     */
    std::optional<P2PBufferHandle> get_sendbuffer_ptr(MESSAGE_TYPE type);
    /**
     * Sends the message identified by the provided type and sequence number.
     * This may be used to send messages out of order (send a higher sequence
     * number before a lower sequence number), but messages will only be received
     * by the remote node in order of increasing sequence numbers.
     * @param type The type of message being sent, which identifies the buffer region to use.
     * @param sequence_num The sequence number of the buffer to send.
     */
    void send(MESSAGE_TYPE type, uint64_t sequence_num);

    /**
     * Get remote access key of a memory region
     * @param addr      memory region address
     * @return          remote access key
     * @throw           derecho::derecho_exception on failure
     */
    static uint64_t get_oob_memory_key(void* addr);

    /**
     * Register Out-of-band memory region
     * @param addr      The address of the memory region
     * @param size      The size in bytes of the memory region
     * @throw           derecho::derecho_exception on failure
     */
    static void register_oob_memory(void* addr, size_t size);

    /**
     * Deregister Out-of-band memory region
     * @param addr      The address of the memory region
     * @throw           derecho::derecho_exception on failure
     */
    static void deregister_oob_memory(void* addr);

    /**
     * oob write
     * @param iov
     * @param iovcnt
     * @param remote_dest_addr
     * @param rkey
     * @param size
     *
     * @throws derecho::derecho_exception on failure
     */
    void oob_remote_write(
            const struct iovec* iov, int iovcnt,
            void* remote_dest_addr, uint64_t rkey, size_t size);

    /**
     * oob read
     * @param iov
     * @param iovcnt
     * @param remote_src_addr
     * @param rkey
     * @param size
     *
     * @throws derecho::derecho_exception on failure
     */
    void oob_remote_read(
            const struct iovec* iov, int iovcnt,
            void* remote_src_addr, uint64_t rkey, size_t size);

    /**
     * oob send
     * @param iov
     * @param iovcnt
     *
     * @throws derecho::derecho_exception on failure
     *
     */
    void oob_send(
            const struct iovec* iov, int iovcnt);

    /**
     * oob recv
     * @param iov
     * @param iovcnt
     *
     * @throws derecho::derecho_exception on failure
     */
    void oob_recv(
            const struct iovec* iov, int iovcnt);

    /**
     * Wait for a non-blocking oob operation in the same thread.
     * IMPORTANT: We assume the order of events are ordered.
     * @param op        Operation
     * @param timeout_ms
     *                  timeout setting in milliseconds
     * @throw           derecho::derecho_exception on failure
     */
    void wait_for_oob_op(uint32_t op, uint64_t timeout_ms);

};
}  // namespace sst
