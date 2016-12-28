/*
 * Copyright 2016 Iaroslav Zeigerman
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "vector_clock.h"
#include "network.h"
#include <string.h>

#define MEMBER_ID_ADDR_SIZE MEMBER_ID_SIZE - 4

static void vector_clock_create_member_id(const cluster_member_t *member, uint8_t *result) {
    // TODO: revisit this offset. It doesn't work properly with IPv6.
    pt_socklen_t addr_offset = member->address_len + 2; // skip length and family.
    uint32_t uid_network = PT_HTONL(member->uid);
    // copy the last 8 bytes of the address and port.
    memcpy(result, member->address + addr_offset, MEMBER_ID_ADDR_SIZE);
    // fill the remaining 4 bytes with member's uid.
    memcpy(result + MEMBER_ID_ADDR_SIZE, &uid_network, sizeof(uint32_t));
}

static int vector_clock_find_by_member_id(const vector_clock_t *clock, uint8_t *member_id) {
    for (int i = 0; i < clock->size; ++i) {
        if (memcmp(clock->records[i].member_id, member_id, MEMBER_ID_SIZE) == 0) return i;
    }
    return -1;
}

int vector_clock_init(vector_clock_t *clock) {
    if (clock == NULL) return -1;
    memset(clock, 0, sizeof(vector_clock_t));
    return 0;
}

int vector_clock_increment(vector_clock_t *clock, cluster_member_t *member) {
    uint8_t member_id[MEMBER_ID_SIZE];
    vector_clock_create_member_id(member, member_id);
    int idx = vector_clock_find_by_member_id(clock, member_id);
    if (idx < 0) {
        // insert or override the latest record with the new record.
        uint32_t new_idx = clock->current_idx;
        memcpy(clock->records[new_idx].member_id, member_id, MEMBER_ID_SIZE);
        clock->records[new_idx].sequence_number = 1;

        if (clock->size < MAX_VECTOR_SIZE) ++clock->size;
        if (++clock->current_idx >= MAX_VECTOR_SIZE) clock->current_idx = 0;
    } else {
        ++clock->records[idx].sequence_number;
    }
    return 0;
}

static vector_clock_comp_res_t vector_clock_resolve_comp_result(vector_clock_comp_res_t prev,
                                                                vector_clock_comp_res_t new) {
    return (prev != VC_EQUAL && new != prev) ? VC_CONFLICT : new;
}

vector_clock_comp_res_t vector_clock_compare(vector_clock_t *first, vector_clock_t *second) {
    // TODO: consider a better data structure to improve this algorithm performance.
    vector_clock_comp_res_t result = VC_EQUAL;

    uint32_t second_visited_idxs = 0;

    for (int i = 0; i < first->size; ++i) {
        int second_idx = vector_clock_find_by_member_id(second, first->records[i].member_id);
        second_visited_idxs |= (1 << second_idx);

        if (second_idx < 0) {
            result = vector_clock_resolve_comp_result(result, VC_AFTER);
        } else {
            uint32_t first_seq_num = first->records[i].sequence_number;
            uint32_t second_seq_num = second->records[second_idx].sequence_number;
            if (first_seq_num > second_seq_num) {
                result = vector_clock_resolve_comp_result(result, VC_AFTER);
            } else if (second_seq_num > first_seq_num) {
                result = vector_clock_resolve_comp_result(result, VC_BEFORE);
            }
        }
        if (result == VC_CONFLICT) return result;
    }

    uint32_t second_visited_mask = ((1 << second->size) - 1) & 0xFFFFFFFF;
    if ((second_visited_idxs ^ second_visited_mask) != 0) {
        // There are some records in the second clock that are missing
        // in the first one.
        result = vector_clock_resolve_comp_result(result, VC_BEFORE);
    }
    return result;
}