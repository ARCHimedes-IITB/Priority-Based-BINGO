#include "dram_controller.h"

extern int l1d_prefetch_hit_at[4], dram_reads;

// initialized in main.cc
uint32_t DRAM_MTPS, DRAM_DBUS_RETURN_TIME, tRP, tRCD, tCAS;

void MEMORY_CONTROLLER::reset_remain_requests(PACKET_QUEUE *queue,
                                              uint32_t channel) {
    for (uint32_t i = 0; i < queue->SIZE; i++) {
        if (queue->entry[i].scheduled) {

            uint64_t op_addr = queue->entry[i].address;
            uint32_t op_cpu = queue->entry[i].cpu,
                     op_channel = dram_get_channel(op_addr),
                     op_rank = dram_get_rank(op_addr),
                     op_bank = dram_get_bank(op_addr),
                     op_row = dram_get_row(op_addr);

#ifdef DEBUG_PRINT
            // uint32_t op_column = dram_get_column(op_addr);
#endif

            // update open row
            if ((bank_request[op_channel][op_rank][op_bank].cycle_available -
                 tCAS) <= current_core_cycle[op_cpu])
                bank_request[op_channel][op_rank][op_bank].open_row = op_row;
            else
                bank_request[op_channel][op_rank][op_bank].open_row =
                    UINT32_MAX;

            // this bank is ready for another DRAM request
            bank_request[op_channel][op_rank][op_bank].request_index = -1;
            bank_request[op_channel][op_rank][op_bank].row_buffer_hit = 0;
            bank_request[op_channel][op_rank][op_bank].working = 0;
            bank_request[op_channel][op_rank][op_bank].cycle_available =
                current_core_cycle[op_cpu];
            if (bank_request[op_channel][op_rank][op_bank].is_write) {
                scheduled_writes[channel]--;
                bank_request[op_channel][op_rank][op_bank].is_write = 0;
            } else if (bank_request[op_channel][op_rank][op_bank].is_read) {
                scheduled_reads[channel]--;
                bank_request[op_channel][op_rank][op_bank].is_read = 0;
            }

            queue->entry[i].scheduled = 0;
            queue->entry[i].event_cycle = current_core_cycle[op_cpu];
        }
    }

    update_schedule_cycle(&RQ[channel]);
    update_schedule_cycle(&LOWER_PRIORITY_RQ[channel]);
    update_schedule_cycle(&WQ[channel]);
    update_process_cycle(&RQ[channel]);
    update_process_cycle(&LOWER_PRIORITY_RQ[channel]);
    update_process_cycle(&WQ[channel]);

    // The sanity check here has been removed, since
    // After resetting one of the read queues, we could still have
    // requests from the other queue outstanding.
    // Since operate() calls reset for both the read queues one after
    // another, this should not be a problem.
}

void MEMORY_CONTROLLER::operate() {
    // if (all_warmup_complete >= NUM_CPUS)
    //    log_file << RQ[0].occupancy << '\n';
    // First things first: retry outstanding promotions *before* taking
    // any scheduling or processing decisions.
    retry_outstanding_promotions();
    for (uint32_t i = 0; i < DRAM_CHANNELS; i++) {
        if ((write_mode[i] == 0) &&
            ((WQ[i].occupancy >= DRAM_WRITE_HIGH_WM) ||
             ((RQ[i].occupancy == 0) &&
              (WQ[i].occupancy > 0)))) { // use idle cycles to perform writes
            write_mode[i] = 1;

            // reset scheduled RQ requests
            reset_remain_requests(&RQ[i], i);
            // Also reset the LOWER_PRIORITY_RQ requests
            reset_remain_requests(&LOWER_PRIORITY_RQ[i], i);
            // add data bus turn-around time
            dbus_cycle_available[i] += DRAM_DBUS_TURN_AROUND_TIME;
        } else if (write_mode[i]) {

            if (WQ[i].occupancy == 0)
                write_mode[i] = 0;
            else if (RQ[i].occupancy && (WQ[i].occupancy < DRAM_WRITE_LOW_WM))
                write_mode[i] = 0;

            if (write_mode[i] == 0) {
                // reset scheduled WQ requests
                reset_remain_requests(&WQ[i], i);
                // add data bus turnaround time
                dbus_cycle_available[i] += DRAM_DBUS_TURN_AROUND_TIME;
            }
        }

        // handle write
        // schedule new entry
        if (write_mode[i] && (WQ[i].next_schedule_index < WQ[i].SIZE)) {
            if (WQ[i].next_schedule_cycle <=
                current_core_cycle[WQ[i].entry[WQ[i].next_schedule_index].cpu])
                schedule(&WQ[i]);
        }

        // process DRAM requests
        if (write_mode[i] && (WQ[i].next_process_index < WQ[i].SIZE)) {
            if (WQ[i].next_process_cycle <=
                current_core_cycle[WQ[i].entry[WQ[i].next_process_index].cpu])
                process(&WQ[i]);
        }

        // handle read
        bool low_prio_queue_preferred =
            (current_core_cycle[0] % (main_sched_ratio + low_sched_ratio) >=
             main_sched_ratio) &&
            (RQ[i].occupancy < queue_scheduling_watermark);

        if ((write_mode[i] == 0)) {
            // schedule DRAM requests

            bool can_schedule_main =
                (RQ[i].next_schedule_index < RQ[i].SIZE) &&
                (RQ[i].next_schedule_cycle <=
                 current_core_cycle
                     [RQ[i].entry[RQ[i].next_schedule_index].cpu]);
            bool can_schedule_low =
                (LOWER_PRIORITY_RQ[i].next_schedule_index <
                 LOWER_PRIORITY_RQ[i].SIZE) &&
                (LOWER_PRIORITY_RQ[i].next_schedule_cycle <=
                 current_core_cycle
                     [LOWER_PRIORITY_RQ[i]
                          .entry[LOWER_PRIORITY_RQ[i].next_schedule_index]
                          .cpu]);

            if (low_prio_queue_preferred) {
                if (can_schedule_low)
                    schedule(&LOWER_PRIORITY_RQ[i]);
                else if (can_schedule_main)
                    schedule(&RQ[i]);
            } else {
                if (can_schedule_main)
                    schedule(&RQ[i]);
                else if (can_schedule_low)
                    schedule(&LOWER_PRIORITY_RQ[i]);
            }

            // process DRAM requests
            bool can_process_main =
                (RQ[i].next_process_index < RQ[i].SIZE) &&
                (RQ[i].next_process_cycle <=
                 current_core_cycle[RQ[i].entry[RQ[i].next_process_index].cpu]);
            bool can_process_low =
                (LOWER_PRIORITY_RQ[i].next_process_index <
                 LOWER_PRIORITY_RQ[i].SIZE) &&
                (LOWER_PRIORITY_RQ[i].next_process_cycle <=
                 current_core_cycle
                     [LOWER_PRIORITY_RQ[i]
                          .entry[LOWER_PRIORITY_RQ[i].next_process_index]
                          .cpu]);

            if (low_prio_queue_preferred) {
                if (can_process_low)
                    process(&LOWER_PRIORITY_RQ[i]);
                else if (can_process_main)
                    process(&RQ[i]);
            } else {
                if (can_process_main)
                    process(&RQ[i]);
                else if (can_process_low)
                    process(&LOWER_PRIORITY_RQ[i]);
            }
        }
    }
}

void MEMORY_CONTROLLER::schedule(PACKET_QUEUE *queue) {
    uint64_t read_addr;
    uint32_t read_channel, read_rank, read_bank, read_row;
    uint8_t row_buffer_hit = 0;
    int oldest_index;
    uint64_t oldest_cycle;

restart:
    oldest_index = -1;
    oldest_cycle = UINT64_MAX;

    // If we have restarted, we may not be at next_schedule_cycle yet
    // However, next_schedule_index may *not* be zero, since at least for
    // now we can restart only if occupancy >= watermark > 1 to begin with
    // if (current_core_cycle[queue->entry[queue->next_schedule_index].cpu]
    //     < queue->next_schedule_cycle)
    //     return;

    // first, search for the oldest open row hit
    for (uint32_t i = 0; i < queue->SIZE; i++) {

        // already scheduled
        if (queue->entry[i].scheduled)
            continue;

        // empty entry
        read_addr = queue->entry[i].address;
        if (read_addr == 0)
            continue;

        read_channel = dram_get_channel(read_addr);
        read_rank = dram_get_rank(read_addr);
        read_bank = dram_get_bank(read_addr);

        // bank is busy
        if (bank_request[read_channel][read_rank][read_bank]
                .working) { // should we check this or not? how do we know if
                            // bank is busy or not for all requests in the
                            // queue?

            continue;
        }

        read_row = dram_get_row(read_addr);
        // read_column = dram_get_column(read_addr);

        // check open row
        if (bank_request[read_channel][read_rank][read_bank].open_row !=
            read_row)
            continue;

        // select the oldest entry
        if (queue->entry[i].event_cycle < oldest_cycle) {
            oldest_cycle = queue->entry[i].event_cycle;
            oldest_index = i;
            row_buffer_hit = 1;
        }
    }

    if (oldest_index == -1) { // no matching open_row (row buffer miss)

        oldest_cycle = UINT64_MAX;
        for (uint32_t i = 0; i < queue->SIZE; i++) {

            // already scheduled
            if (queue->entry[i].scheduled)
                continue;

            // empty entry
            read_addr = queue->entry[i].address;
            if (read_addr == 0)
                continue;

            // bank is busy
            read_channel = dram_get_channel(read_addr);
            read_rank = dram_get_rank(read_addr);
            read_bank = dram_get_bank(read_addr);
            if (bank_request[read_channel][read_rank][read_bank].working)
                continue;

            // select the oldest entry
            if (queue->entry[i].event_cycle < oldest_cycle) {
                oldest_cycle = queue->entry[i].event_cycle;
                oldest_index = i;
            }
        }
    }

    // At the RQ:
    // If the request is not a priority 1, and our queue is congested, kick the
    // request onto the lower priority queue or discard it, depending on its
    // priority. Also, restart the search for a request to service.
    if ((oldest_index) != -1 && queue->NAME == RQ[read_channel].NAME &&
        queue->occupancy >= queue_scheduling_watermark) {
        if (queue->entry[oldest_index].priority == 3) {
            // Reject the request and try again
            upper_level_dcache[queue->entry[oldest_index].cpu]->nack_request(
                &queue->entry[oldest_index]);
            queue->entry[oldest_index].address = 0;
            queue->occupancy--;
            update_schedule_cycle(&RQ[read_channel]);
            goto restart;
        } else if (queue->entry[oldest_index].priority == 2 &&
                   LOWER_PRIORITY_RQ[read_channel].occupancy <
                       LOWER_PRIORITY_RQ[read_channel].SIZE) {
            // Demote the request to the lower priority queue
            int index = -1;
            for (index = 0; index < int(LOWER_PRIORITY_RQ[read_channel].SIZE);
                 index++)
                if (LOWER_PRIORITY_RQ[read_channel].entry[index].address == 0)
                    break;
            LOWER_PRIORITY_RQ[read_channel].entry[index] =
                RQ[read_channel].entry[oldest_index];
            LOWER_PRIORITY_RQ[read_channel].occupancy++;
            queue->entry[oldest_index].address = 0;
            queue->occupancy--;
            update_schedule_cycle(&RQ[read_channel]);
            update_schedule_cycle(&LOWER_PRIORITY_RQ[read_channel]);
            goto restart;
        }
    }

    // at this point, the scheduler knows which bank to access and if the
    // request is a row buffer hit or miss
    if (oldest_index !=
        -1) { // scheduler might not find anything if all requests
              // are already scheduled or all banks are busy

        uint64_t LATENCY = 0;
        if (row_buffer_hit)
            LATENCY = tCAS;
        else
            LATENCY = tRP + tRCD + tCAS;

        uint64_t op_addr = queue->entry[oldest_index].address;
        uint32_t op_cpu = queue->entry[oldest_index].cpu,
                 op_channel = dram_get_channel(op_addr),
                 op_rank = dram_get_rank(op_addr),
                 op_bank = dram_get_bank(op_addr),
                 op_row = dram_get_row(op_addr);

        // this bank is now busy
        bank_request[op_channel][op_rank][op_bank].working = 1;
        bank_request[op_channel][op_rank][op_bank].working_type =
            queue->entry[oldest_index].type;
        bank_request[op_channel][op_rank][op_bank].cycle_available =
            current_core_cycle[op_cpu] + LATENCY;

        bank_request[op_channel][op_rank][op_bank].request_index = oldest_index;
        bank_request[op_channel][op_rank][op_bank].row_buffer_hit =
            row_buffer_hit;
        if (queue->is_WQ) {
            bank_request[op_channel][op_rank][op_bank].is_write = 1;
            bank_request[op_channel][op_rank][op_bank].is_read = 0;
            scheduled_writes[op_channel]++;
        } else {
            bank_request[op_channel][op_rank][op_bank].is_write = 0;
            bank_request[op_channel][op_rank][op_bank].is_read = 1;
            scheduled_reads[op_channel]++;
        }

        // update open row
        bank_request[op_channel][op_rank][op_bank].open_row = op_row;

        queue->entry[oldest_index].scheduled = 1;
        queue->entry[oldest_index].event_cycle =
            current_core_cycle[op_cpu] + LATENCY;

        update_schedule_cycle(queue);
        update_process_cycle(queue);
    }
}

void MEMORY_CONTROLLER::process(PACKET_QUEUE *queue) {
    uint32_t request_index = queue->next_process_index;

    // sanity check
    if (request_index == queue->SIZE)
        assert(0);

    uint8_t op_type = queue->entry[request_index].type;
    uint64_t op_addr = queue->entry[request_index].address;
    uint32_t op_cpu = queue->entry[request_index].cpu,
             op_channel = dram_get_channel(op_addr),
             op_rank = dram_get_rank(op_addr), op_bank = dram_get_bank(op_addr);

    // paid all DRAM access latency, data is ready to be processed
    if (bank_request[op_channel][op_rank][op_bank].cycle_available <=
        current_core_cycle[op_cpu]) {

        // check if data bus is available
        if (dbus_cycle_available[op_channel] <= current_core_cycle[op_cpu]) {

            if (queue->is_WQ) {
                // update data bus cycle time
                dbus_cycle_available[op_channel] =
                    current_core_cycle[op_cpu] + DRAM_DBUS_RETURN_TIME;

                if (bank_request[op_channel][op_rank][op_bank].row_buffer_hit)
                    queue->ROW_BUFFER_HIT++;
                else
                    queue->ROW_BUFFER_MISS++;

                // this bank is ready for another DRAM request
                bank_request[op_channel][op_rank][op_bank].request_index = -1;
                bank_request[op_channel][op_rank][op_bank].row_buffer_hit = 0;
                bank_request[op_channel][op_rank][op_bank].working = false;
                bank_request[op_channel][op_rank][op_bank].is_write = 0;
                bank_request[op_channel][op_rank][op_bank].is_read = 0;

                scheduled_writes[op_channel]--;
            } else {
                // update data bus cycle time
                dbus_cycle_available[op_channel] =
                    current_core_cycle[op_cpu] + DRAM_DBUS_RETURN_TIME;
                queue->entry[request_index].event_cycle =
                    dbus_cycle_available[op_channel];

                // send data back to the core cache hierarchy
                upper_level_dcache[op_cpu]->return_data(
                    &queue->entry[request_index]);

                if (bank_request[op_channel][op_rank][op_bank].row_buffer_hit)
                    queue->ROW_BUFFER_HIT++;
                else
                    queue->ROW_BUFFER_MISS++;

                // this bank is ready for another DRAM request
                bank_request[op_channel][op_rank][op_bank].request_index = -1;
                bank_request[op_channel][op_rank][op_bank].row_buffer_hit = 0;
                bank_request[op_channel][op_rank][op_bank].working = false;
                bank_request[op_channel][op_rank][op_bank].is_write = 0;
                bank_request[op_channel][op_rank][op_bank].is_read = 0;

                scheduled_reads[op_channel]--;
            }

            // remove the oldest entry
            queue->remove_queue(&queue->entry[request_index]);
            update_process_cycle(queue);
        } else { // data bus is busy, the available bank cycle time is
                 // fast-forwarded for faster simulation
            dbus_cycle_congested[op_channel] +=
                (dbus_cycle_available[op_channel] - current_core_cycle[op_cpu]);
            bank_request[op_channel][op_rank][op_bank].cycle_available =
                dbus_cycle_available[op_channel];
            dbus_congested[NUM_TYPES][NUM_TYPES]++;
            dbus_congested[NUM_TYPES][op_type]++;
            dbus_congested[bank_request[op_channel][op_rank][op_bank]
                               .working_type][NUM_TYPES]++;
            dbus_congested[bank_request[op_channel][op_rank][op_bank]
                               .working_type][op_type]++;
        }
    }
}

int MEMORY_CONTROLLER::add_rq(PACKET *packet) {
    // simply return read requests with dummy response before the warmup
    if (all_warmup_complete < NUM_CPUS) {
        if (packet->instruction)
            upper_level_icache[packet->cpu]->return_data(packet);
        if (packet->is_data)
            upper_level_dcache[packet->cpu]->return_data(packet);

        return -1;
    }

    if (packet->type == PREFETCH && packet->pf_origin_level == FILL_L1)
        l1d_prefetch_hit_at[3]++;
    dram_reads++;

    // check for the latest wirtebacks in the write queue
    uint32_t channel = dram_get_channel(packet->address);
    int wq_index = check_dram_queue(&WQ[channel], packet);
    if (wq_index != -1) {
        packet->data = WQ[channel].entry[wq_index].data;
        if (packet->instruction)
            upper_level_icache[packet->cpu]->return_data(packet);
        if (packet->is_data)
            upper_level_dcache[packet->cpu]->return_data(packet);
        //}

        ACCESS[1]++;
        HIT[1]++;

        WQ[channel].FORWARD++;
        RQ[channel].ACCESS++;
        // assert(0);

        return -1;
    }

    // check for duplicates in the read queue
    // Don't check for duplicates in the secondary queue for now to
    // avoid priority issues. Besides, duplicates should never occur
    // due to the MSHR at the LLC.
    int index = check_dram_queue(&RQ[channel], packet);
    if (index != -1)
        return index; // merged index

    // search for the empty index
    bool found_empty = false;
    int index_priority_2 = -1, index_priority_3 = -1;
    for (index = 0; index < DRAM_RQ_SIZE; index++) {
        if (RQ[channel].entry[index].address == 0) {
            found_empty = true;
            RQ[channel].entry[index] = *packet;
            RQ[channel].occupancy++;
            break;
        } else if (RQ[channel].entry[index].priority == 3)
            index_priority_3 = index;
        else if (RQ[channel].entry[index].priority == 2)
            index_priority_2 = index;
    }

    if (found_empty)
        update_schedule_cycle(&RQ[channel]);
    else {
        // The fact that this request was sent means that we must have
        // one of the following cases holding:
        // 1) We have a request of priority 3 outstanding. In this
        // case we must NACK this request and replace it with the incoming one.
        // 2) We have a request of priority 2 outstanding AND the incoming
        // request has priority 1 AND the lower priority queue has at least one
        // slot empty. In this case we kick the former into the lower priority
        // queue
        if (index_priority_3 != -1) {
            // NACK this packet and replace it with the incoming one.
            // occupancy stays the same.
            // Using the dcache here is OK, since all upper levels point to the
            // LLC.
            upper_level_dcache[RQ[channel].entry[index_priority_3].cpu]
                ->nack_request(&RQ[channel].entry[index_priority_3]);
            RQ[channel].entry[index] = *packet;
            update_schedule_cycle(&RQ[channel]);
        } else {
            // We must have (2) holding here.
            // Insert the packet at index_priority_2 into the lower priority
            // queue and replace it with the incoming packet. We should update
            // the schedule cycles of both queues.
            for (index = 0; index < int(LOWER_PRIORITY_RQ[channel].SIZE);
                 index++) {
                if (LOWER_PRIORITY_RQ[channel].entry[index].address == 0) {
                    LOWER_PRIORITY_RQ[channel].entry[index] = *packet;
                    LOWER_PRIORITY_RQ[channel].occupancy++;
                    break;
                }
            }
            LOWER_PRIORITY_RQ[channel].entry[index] =
                RQ[channel].entry[index_priority_2];
            LOWER_PRIORITY_RQ[channel].occupancy++;
            RQ[channel].entry[index_priority_2] = *packet;
            update_schedule_cycle(&RQ[channel]);
            update_schedule_cycle(&LOWER_PRIORITY_RQ[channel]);
        }
    }

    return -1;
}

int MEMORY_CONTROLLER::add_wq(PACKET *packet) {
    // simply drop write requests before the warmup
    if (all_warmup_complete < NUM_CPUS)
        return -1;

    // check for duplicates in the write queue
    uint32_t channel = dram_get_channel(packet->address);
    int index = check_dram_queue(&WQ[channel], packet);
    if (index != -1)
        return index; // merged index

    // search for the empty index
    for (index = 0; index < DRAM_WQ_SIZE; index++) {
        if (WQ[channel].entry[index].address == 0) {

            WQ[channel].entry[index] = *packet;
            WQ[channel].occupancy++;
            break;
        }
    }

    update_schedule_cycle(&WQ[channel]);

    return -1;
}

int MEMORY_CONTROLLER::add_pq(PACKET *packet) { return -1; }

void MEMORY_CONTROLLER::return_data(PACKET *packet) {}

void MEMORY_CONTROLLER::update_schedule_cycle(PACKET_QUEUE *queue) {
    // update next_schedule_cycle
    uint64_t min_cycle = UINT64_MAX;
    uint32_t min_index = queue->SIZE;
    for (uint32_t i = 0; i < queue->SIZE; i++) {
        if (queue->entry[i].address && (queue->entry[i].scheduled == 0) &&
            (queue->entry[i].event_cycle < min_cycle)) {
            min_cycle = queue->entry[i].event_cycle;
            min_index = i;
        }
    }

    queue->next_schedule_cycle = min_cycle;
    queue->next_schedule_index = min_index;
}

void MEMORY_CONTROLLER::update_process_cycle(PACKET_QUEUE *queue) {
    // update next_process_cycle
    uint64_t min_cycle = UINT64_MAX;
    uint32_t min_index = queue->SIZE;
    for (uint32_t i = 0; i < queue->SIZE; i++) {
        if (queue->entry[i].scheduled &&
            (queue->entry[i].event_cycle < min_cycle)) {
            min_cycle = queue->entry[i].event_cycle;
            min_index = i;
        }
    }

    queue->next_process_cycle = min_cycle;
    queue->next_process_index = min_index;
}

int MEMORY_CONTROLLER::check_dram_queue(PACKET_QUEUE *queue, PACKET *packet) {
    // search write queue
    for (uint32_t index = 0; index < queue->SIZE; index++) {
        if (queue->entry[index].address == packet->address)
            return index;
    }
    return -1;
}

uint32_t MEMORY_CONTROLLER::dram_get_channel(uint64_t address) {
    if (LOG2_DRAM_CHANNELS == 0)
        return 0;

    int shift = 0;

    return (uint32_t)(address >> shift) & (DRAM_CHANNELS - 1);
}

uint32_t MEMORY_CONTROLLER::dram_get_bank(uint64_t address) {
    if (LOG2_DRAM_BANKS == 0)
        return 0;

    int shift = LOG2_DRAM_CHANNELS;

    return (uint32_t)(address >> shift) & (DRAM_BANKS - 1);
}

uint32_t MEMORY_CONTROLLER::dram_get_column(uint64_t address) {
    if (LOG2_DRAM_COLUMNS == 0)
        return 0;

    int shift = LOG2_DRAM_BANKS + LOG2_DRAM_CHANNELS;

    return (uint32_t)(address >> shift) & (DRAM_COLUMNS - 1);
}

uint32_t MEMORY_CONTROLLER::dram_get_rank(uint64_t address) {
    if (LOG2_DRAM_RANKS == 0)
        return 0;

    int shift = LOG2_DRAM_COLUMNS + LOG2_DRAM_BANKS + LOG2_DRAM_CHANNELS;

    return (uint32_t)(address >> shift) & (DRAM_RANKS - 1);
}

uint32_t MEMORY_CONTROLLER::dram_get_row(uint64_t address) {
    if (LOG2_DRAM_ROWS == 0)
        return 0;

    int shift = LOG2_DRAM_RANKS + LOG2_DRAM_COLUMNS + LOG2_DRAM_BANKS +
                LOG2_DRAM_CHANNELS;

    return (uint32_t)(address >> shift) & (DRAM_ROWS - 1);
}

uint32_t MEMORY_CONTROLLER::get_occupancy(uint8_t queue_type,
                                          uint64_t address) {
    uint32_t channel = dram_get_channel(address);
    if (queue_type == 1)
        return RQ[channel].occupancy;
    else if (queue_type == 2)
        return WQ[channel].occupancy;

    return 0;
}

uint32_t MEMORY_CONTROLLER::get_size(uint8_t queue_type, uint64_t address) {
    uint32_t channel = dram_get_channel(address);
    if (queue_type == 1)
        return RQ[channel].SIZE;
    else if (queue_type == 2)
        return WQ[channel].SIZE;

    return 0;
}

void MEMORY_CONTROLLER::increment_WQ_FULL(uint64_t address) {
    uint32_t channel = dram_get_channel(address);
    WQ[channel].FULL++;
}

/**
 * check_availability_for_priority_load - Check whether we can accept a read
 * request of priority `priority`.
 * This function is required since the (occupancy != size) check under-estimates
 * availability of slots. In particular,
 * 1) For priority 1 requests, we can afford to discard priority 3 requests, or
 *    demote a priority 2 request to the lower priority queue, if it is not
 * full. 2) For a priority 2 request, we can afford to discard priority 3
 * requests. 3) For all priorities, we can afford to fill them into free slots
 * if they exist.
 *
 * Returns whether we can accept this request
 */
bool MEMORY_CONTROLLER::check_availability_for_priority_read(uint64_t address,
                                                             uint8_t priority) {
    uint32_t channel = dram_get_channel(address);
    // This function is only called for reads
    // Firstly, if we have a free slot, we can just return true
    if (RQ[channel].occupancy < RQ[channel].SIZE)
        return true;

    if (priority >= 3)
        return false;

    // AB: Should we have a separate occupancy value for each priority class
    // as well? That way we won't need this loop.
    bool have_a_2 = false, have_a_3 = false;
    for (int index = 0; index < DRAM_RQ_SIZE; index++) {
        if (RQ[channel].entry[index].priority == 3) {
            have_a_3 = true;
            // No need to check 2's
            break;
        } else if (RQ[channel].entry[index].priority == 2)
            have_a_2 = true;
    }

    if (have_a_3 || ((priority == 1) && have_a_2 &&
                     (LOWER_PRIORITY_RQ[channel].occupancy <
                      LOWER_PRIORITY_RQ[channel].SIZE)))
        return true;
    return false;
}

/**
 * increase_priority_by_index - Increase priority of the specified index of
 * the specified queue. Check the increase_priority queue for comments.
 * Returns the success value of the operation.
 */
void MEMORY_CONTROLLER::increase_priority_by_index(uint32_t channel, int index,
                                                   uint8_t new_priority,
                                                   bool lower_prio_q) {
    if (!lower_prio_q) {
        RQ[channel].entry[index].priority = new_priority;
        // Irrespective of whether this entry was at the head, there is no need
        // to call update_*_cycle here.
        return;
    } else if (RQ[channel].occupancy < DRAM_RQ_SIZE) {
        // We have space.
        // Find a free index first
        int free_index = -1;
        for (free_index = 0; free_index < DRAM_RQ_SIZE; free_index++) {
            if (RQ[channel].entry[free_index].address == 0)
                break;
        }

        bool scheduled = LOWER_PRIORITY_RQ[channel].entry[index].scheduled;

        // Fill in the entry from the lower queue, but mark the increased
        // priority.
        RQ[channel].entry[free_index] = LOWER_PRIORITY_RQ[channel].entry[index];
        RQ[channel].entry[free_index].priority = new_priority;
        RQ[channel].occupancy++;

        // Erase the entry from the lower priority queue
        LOWER_PRIORITY_RQ[channel].entry[index].address = 0;
        LOWER_PRIORITY_RQ[channel].occupancy--;

        // We changed the contents of both the queues. We should update either
        // the schedule or process cycles for both the queues. The latter is
        // because we may have just promoted a request that has been scheduled
        // but not processed. If the request hasn't been scheduled yet, updating
        // the scheduling cycles should suffice.
        if (scheduled) {
            // Change the index being served at the specific ChBaRa, since index
            // will now have to be replaced by free_index
            uint32_t rank =
                         dram_get_rank(RQ[channel].entry[free_index].address),
                     bank =
                         dram_get_bank(RQ[channel].entry[free_index].address);
            bank_request[channel][rank][bank].request_index = free_index;
            update_process_cycle(&RQ[channel]);
            update_process_cycle(&LOWER_PRIORITY_RQ[channel]);
        } else {
            update_schedule_cycle(&RQ[channel]);
            update_schedule_cycle(&LOWER_PRIORITY_RQ[channel]);
        }
    } else {
        // No space. Stash it away.
        // Note that the retry loop that happens every cycle *must* ensure RQ
        // has enough free space before calling the function. Else, this line
        // below will mess up the iteration used by the calling function, and
        // may lead to duplicates in the outstanding queue.
        pending_promotions[channel].push(std::make_pair(index, new_priority));
    }
}

/**
 * retry_outstanding_promotions - Retries the outstanding priority boosts.
 * Called every cycle, if there are outstanding boosts to complete.
 */
void MEMORY_CONTROLLER::retry_outstanding_promotions() {
    for (uint32_t channel = 0; channel < DRAM_CHANNELS; channel++) {
        while (!pending_promotions[channel].empty()) {
            // All the pending promotions are from the lower priority queue.
            // First, break if the main queue is full
            if (RQ[channel].occupancy >= DRAM_RQ_SIZE)
                break;

            // We have enough space: promote this.
            pair<int, uint8_t> p = pending_promotions[channel].front();
            pending_promotions[channel].pop();
            increase_priority_by_index(channel, p.first, p.second, true);
        }
    }
}

/**
 * increase_priority - Increase the priority of a previously dispatched request.
 * Since demand requests may be merged at the MSHR with an outstanding prefetch,
 * it is vital that the controller expose a method to update the priority of
 * requests.
 * If the main queue is full, we will instead insert it into the pending indices
 * set, so that the request may be promoted at the earliest possible time.
 * Refer to the comments in dram_controller.h for why this does not cause races.
 *
 * Note that this function will/should only be called for priority 2 requests,
 * since we may have discarded priority 3 requests. In the latter case however,
 * we would have informed the LLC of this, and hence that should not be a
 * problem. Further, all existing priority 3 requests will be in the main queue
 * and never in the lower priority queue.
 */
void MEMORY_CONTROLLER::increase_priority(PACKET *packet,
                                          uint8_t new_priority) {
    // We know (assume?) that at most one request with a given address exists
    // across both the queues because of the MSHR at the LLC.
    uint32_t channel = dram_get_channel(packet->address);
    // Walk the normal queue first
    int index = -1;
    bool low_prio_queue = false;
    for (index = 0; index < DRAM_RQ_SIZE; index++) {
        if (RQ[channel].entry[index].address == packet->address)
            break;
    }

    if (index == DRAM_RQ_SIZE) {
        for (index = 0; index < int(LOWER_PRIORITY_RQ[channel].SIZE); index++) {
            if (LOWER_PRIORITY_RQ[channel].entry[index].address ==
                packet->address)
                break;
        }
        if (index == int(LOWER_PRIORITY_RQ[channel].SIZE))
            index = -1;
        else
            low_prio_queue = true;
    }

    // This will also take care of cases where the main queue is full
    if (index != -1)
        increase_priority_by_index(channel, index, new_priority,
                                   low_prio_queue);
}
