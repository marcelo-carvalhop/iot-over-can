#ifndef NODE_TYPES_H
#define NODE_TYPES_H

/* =========================================================
 * ESTADOS LOGICOS DO NO
 * ========================================================= */

enum NodeState {
  STATE_IDLE,
  STATE_GATEWAY,
  STATE_JOINING,
  STATE_ELECTION,
  STATE_RECOVERING,
  STATE_LEADER,
  STATE_FOLLOWER,
  STATE_FAULT
};

#endif
