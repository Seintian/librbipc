/**
 * @file test_unit_cpp_compat.cpp
 * @brief C++20/C++23 Native Interoperability & Concurrency Verification Test.
 *
 * Validates that all public headers (rbipc.h, rbipc_ipc.h, rbipc_pubsub.h)
 * can be consumed seamlessly from ISO C++ without language extensions,
 * compiler warnings, or layout discrepancies.
 */

#include "rbipc.h"
#include "rbipc_ipc.h"
#include "rbipc_pubsub.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Enforce layout invariants in C++
static_assert(sizeof(rbipc_slot_t) == RBIPC_CACHE_LINE, "rbipc_slot_t size mismatch in C++");
static_assert(alignof(rbipc_slot_t) == RBIPC_CACHE_LINE, "rbipc_slot_t alignment mismatch in C++");
static_assert(sizeof(rbipc_shm_header_t) % RBIPC_CACHE_LINE == 0, "rbipc_shm_header_t size mismatch in C++");
static_assert(alignof(rbipc_shm_header_t) == RBIPC_CACHE_LINE, "rbipc_shm_header_t alignment mismatch in C++");

int main() {
    std::cout << "[test_unit_cpp_compat] Testing C++ public API integration..." << std::endl;

    const std::string ring_name = "/rbipc_cpp_compat_test";
    const size_t capacity = 64;
    const uint32_t slot_size = 256;

    // Clean up any stale SHM segment
    rbipc_destroy(ring_name.c_str());

    // 1. Create ring buffer
    rbipc_ring_t *ring = nullptr;
    int rc = rbipc_create(ring_name.c_str(), capacity, slot_size, &ring);
    assert(rc == RBIPC_OK && ring != nullptr);

    // 2. Test SPSC payload passing across C++ threads
    constexpr int msg_count = 1000;
    std::thread consumer_thread([ring]() {
        for (int i = 0; i < msg_count; ++i) {
            char buf[256] = {};
            uint32_t len = 0;
            int ret = rbipc_read_timeout(ring, buf, sizeof(buf), &len, 1000000000ULL);
            assert(ret == RBIPC_OK);
            assert(len == sizeof(int));
            int val = 0;
            std::memcpy(&val, buf, sizeof(int));
            assert(val == i);
        }
    });

    for (int i = 0; i < msg_count; ++i) {
        int val = i;
        int ret = rbipc_write_timeout(ring, &val, sizeof(val), 1000000000ULL);
        assert(ret == RBIPC_OK);
    }

    consumer_thread.join();
    std::cout << "  [cpp_spsc] Transmitted " << msg_count << " messages across C++ threads successfully." << std::endl;

    // 3. Test PubSub subscriber integration in C++
    rbipc_sub_t *sub = nullptr;
    rc = rbipc_sub_create(ring, &sub);
    assert(rc == RBIPC_OK && sub != nullptr);

    const char pub_msg[] = "Hello from C++ PubSub!";
    rc = rbipc_write(ring, pub_msg, static_cast<uint32_t>(sizeof(pub_msg)));
    assert(rc == RBIPC_OK);

    const void *sub_data = nullptr;
    uint32_t sub_len = 0;
    uint64_t sub_ticket = 0;
    rc = rbipc_sub_poll(sub, &sub_data, &sub_len, &sub_ticket);
    assert(rc == RBIPC_OK && sub_data != nullptr);
    assert(std::string(static_cast<const char*>(sub_data)) == pub_msg);

    rbipc_sub_destroy(sub);
    std::cout << "  [cpp_pubsub] Multicast broadcast validated." << std::endl;

    // 4. Close & destroy ring
    rc = rbipc_detach(ring);
    assert(rc == RBIPC_OK);
    rc = rbipc_destroy(ring_name.c_str());
    assert(rc == RBIPC_OK);

    std::cout << "[test_unit_cpp_compat] All C++ compatibility checks passed successfully." << std::endl;
    return 0;
}
