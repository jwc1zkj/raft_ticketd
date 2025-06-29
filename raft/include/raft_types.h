
#ifndef RAFT_DEFS_H_
#define RAFT_DEFS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Unique entry ids are mostly used for debugging and nothing else,
 * so there is little harm if they collide.
 */
typedef int raft_entry_id_t;

/**
 * Monotonic term counter.
 */
typedef long int raft_term_t;

/**
 * Monotonic log entry index.
 *
 * This is also used to as an entry count size type.
 */
typedef long int raft_index_t;

/**
 * Unique node identifier.
 */
typedef int raft_node_id_t;

#ifdef __cplusplus
}
#endif

#endif  /* RAFT_DEFS_H_ */
