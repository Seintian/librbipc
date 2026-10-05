/**
 * @file test_unit_slot.c
 * @brief Unit tests for slot state machine and peer crash detection
 */

#include "rbipc.h"
#include "../src/rbipc_slot.h"
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>

static void test_slot_state_lifecycle(void) {
    rbipc_slot_t slots[4];
    rbipc_slot_init_table(slots, 4);

    for (uint32_t i = 0; i < 4; ++i) {
        assert(atomic_load(&slots[i].sequence) == i);
        assert(atomic_load(&slots[i].state) == RBIPC_SLOT_EMPTY);
        assert(atomic_load(&slots[i].producer_pid) == 0);
        assert(atomic_load(&slots[i].len) == 0);
    }

    /* Reserve slot 0 */
    rbipc_slot_mark_reserved(&slots[0], getpid());
    assert(atomic_load(&slots[0].state) == RBIPC_SLOT_RESERVED);
    assert(atomic_load(&slots[0].producer_pid) == (uint32_t)getpid());

    /* Commit slot 0 */
    rbipc_slot_commit(&slots[0], 0, 128);
    assert(atomic_load(&slots[0].state) == RBIPC_SLOT_COMMITTED);
    assert(atomic_load(&slots[0].sequence) == 1);
    assert(atomic_load(&slots[0].len) == 128);

    /* Release slot 0 */
    rbipc_slot_release(&slots[0], 0, 4);
    assert(atomic_load(&slots[0].state) == RBIPC_SLOT_EMPTY);
    assert(atomic_load(&slots[0].sequence) == 4);

    /* Test Poison transition */
    rbipc_slot_mark_reserved(&slots[1], getpid());
    assert(rbipc_slot_poison(&slots[1], 1) == true);
    assert(atomic_load(&slots[1].state) == RBIPC_SLOT_POISONED);
    assert(atomic_load(&slots[1].sequence) == 2);

    /* Poisoning already poisoned slot should return false */
    assert(rbipc_slot_poison(&slots[1], 1) == false);

    /* NULL safety */
    rbipc_slot_init_table(NULL, 0);
    rbipc_slot_mark_reserved(NULL, 0);
    rbipc_slot_commit(NULL, 0, 0);
    assert(rbipc_slot_poison(NULL, 0) == false);
    rbipc_slot_release(NULL, 0, 0);
}

static void test_peer_liveness(void) {
    /* Invalid PID */
    assert(rbipc_slot_is_peer_alive(0) == false);
    assert(rbipc_slot_is_peer_alive(-1) == false);

    /* Current process is definitely alive */
    assert(rbipc_slot_is_peer_alive(getpid()) == true);

    /* Dead process test */
    pid_t pid = fork();
    if (pid == 0) {
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    /* Now pid is dead */
    assert(rbipc_slot_is_peer_alive(pid) == false);
}

int main(void) {
    printf("[test_unit_slot] Running tests...\n");
    test_slot_state_lifecycle();
    test_peer_liveness();
    printf("[test_unit_slot] All tests passed!\n");
    return 0;
}
