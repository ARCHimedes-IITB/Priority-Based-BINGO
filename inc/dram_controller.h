#ifndef DRAM_H
#define DRAM_H

#include <queue>

#include "memory_class.h"

// DRAM configuration
#define DRAM_CHANNEL_WIDTH 8 // 8B
#define DRAM_WQ_SIZE 64
#define DRAM_RQ_SIZE 64

#define tRP_DRAM_NANOSECONDS 12.5
#define tRCD_DRAM_NANOSECONDS 12.5
#define tCAS_DRAM_NANOSECONDS 12.5

// the data bus must wait this amount of time when switching between reads and
// writes, and vice versa
#define DRAM_DBUS_TURN_AROUND_TIME ((15 * CPU_FREQ) / 2000) // 7.5 ns
extern uint32_t DRAM_MTPS, DRAM_DBUS_RETURN_TIME;

// these values control when to send out a burst of writes
#define DRAM_WRITE_HIGH_WM ((DRAM_WQ_SIZE * 7) >> 3) // 7/8th
#define DRAM_WRITE_LOW_WM ((DRAM_WQ_SIZE * 3) >> 2)  // 6/8th
#define MIN_DRAM_WRITES_PER_SWITCH (DRAM_WQ_SIZE * 1 / 4)

// DRAM
class MEMORY_CONTROLLER : public MEMORY {
  public:
    const string NAME;

    DRAM_ARRAY dram_array[DRAM_CHANNELS][DRAM_RANKS][DRAM_BANKS];
    uint64_t dbus_cycle_available[DRAM_CHANNELS],
        dbus_cycle_congested[DRAM_CHANNELS],
        dbus_congested[NUM_TYPES + 1][NUM_TYPES + 1];
    uint64_t bank_cycle_available[DRAM_CHANNELS][DRAM_RANKS][DRAM_BANKS];
    uint8_t do_write, write_mode[DRAM_CHANNELS];
    uint32_t processed_writes, scheduled_reads[DRAM_CHANNELS],
        scheduled_writes[DRAM_CHANNELS];
    int fill_level;

    BANK_REQUEST bank_request[DRAM_CHANNELS][DRAM_RANKS][DRAM_BANKS];

    // queues
    PACKET_QUEUE WQ[DRAM_CHANNELS], RQ[DRAM_CHANNELS];
    // Priority mechanism
    PACKET_QUEUE LOWER_PRIORITY_RQ[DRAM_CHANNELS];

    // Though rare, it may happen that we want to promote a request which is in
    // the lower priority queue, but the main queue has no space. We take note
    // of these indices below. We will try to promote these every cycle.
    // Note that since the main queue is full, we will not be scheduling the
    // secondary queue at all until the main queue empties below a certain
    // threshold; hence, it is not possible that the entry at the corresponding
    // index of the secondary queue is serviced before we promote it.
    // Sidenote: After we merge a demand request with a prefetch, the MSHR entry
    // no longer lists it as a prefetch. Hence, we should *not* recieve multiple
    // promotion requests for the same index.
    // All indices here are of the lower priority queue since main queue
    // promotions are always feasible and will never be delayed. set just in
    // case we have some corner cases with re-addition of indices in the retry
    // loop
    std::queue<pair<int, uint8_t>> pending_promotions[DRAM_CHANNELS];

    // If the main queue occupancy is below this watermark, we schedule it and
    // the low priority queue in the ratio main_sched_ratio : low_sched_ratio.
    // Otherwise we schedule only the main queue [NOT implemented yet].
    uint32_t queue_scheduling_watermark;
    uint32_t main_sched_ratio, low_sched_ratio;

    // constructor
    MEMORY_CONTROLLER(string v1) : NAME(v1) {
        for (uint32_t i = 0; i < NUM_TYPES + 1; i++) {
            for (uint32_t j = 0; j < NUM_TYPES + 1; j++) {
                dbus_congested[i][j] = 0;
            }
        }
        do_write = 0;
        processed_writes = 0;
        for (uint32_t i = 0; i < DRAM_CHANNELS; i++) {
            dbus_cycle_available[i] = 0;
            dbus_cycle_congested[i] = 0;
            write_mode[i] = 0;
            scheduled_reads[i] = 0;
            scheduled_writes[i] = 0;

            for (uint32_t j = 0; j < DRAM_RANKS; j++) {
                for (uint32_t k = 0; k < DRAM_BANKS; k++)
                    bank_cycle_available[i][j][k] = 0;
            }

            WQ[i].NAME = "DRAM_WQ" + to_string(i);
            WQ[i].SIZE = DRAM_WQ_SIZE;
            WQ[i].entry = new PACKET[DRAM_WQ_SIZE];

            RQ[i].NAME = "DRAM_RQ" + to_string(i);
            RQ[i].SIZE = DRAM_RQ_SIZE;
            RQ[i].entry = new PACKET[DRAM_RQ_SIZE];

            // For now we shall assume that the LOWER_PRIOTIY queue has
            // half the size of the RQ. This should be generous enough.
            LOWER_PRIORITY_RQ[i].NAME = "DRAM_LOWER_PRIORITY_RQ" + to_string(i);
            LOWER_PRIORITY_RQ[i].SIZE = DRAM_RQ_SIZE / 2;
            LOWER_PRIORITY_RQ[i].entry = new PACKET[DRAM_RQ_SIZE / 2];

            main_sched_ratio = 2;
            low_sched_ratio = 1;
            queue_scheduling_watermark = 8;
            // queue_scheduling_watermark = (DRAM_RQ_SIZE*3)/4;
        }

        fill_level = FILL_DRAM;
        // log_file.open("results/other/occupancy.txt");
    };

    // destructor
    ~MEMORY_CONTROLLER(){
        // log_file.close();
    };

    // functions
    int add_rq(PACKET *packet), add_wq(PACKET *packet), add_pq(PACKET *packet);

    void return_data(PACKET *packet), operate(),
        increment_WQ_FULL(uint64_t address);

    uint32_t get_occupancy(uint8_t queue_type, uint64_t address),
        get_size(uint8_t queue_type, uint64_t address);

    void schedule(PACKET_QUEUE *queue), process(PACKET_QUEUE *queue),
        update_schedule_cycle(PACKET_QUEUE *queue),
        update_process_cycle(PACKET_QUEUE *queue),
        reset_remain_requests(PACKET_QUEUE *queue, uint32_t channel);

    uint32_t dram_get_channel(uint64_t address),
        dram_get_rank(uint64_t address), dram_get_bank(uint64_t address),
        dram_get_row(uint64_t address), dram_get_column(uint64_t address),
        drc_check_hit(uint64_t address, uint32_t cpu, uint32_t channel,
                      uint32_t rank, uint32_t bank, uint32_t row);

    uint64_t get_bank_earliest_cycle();

    int check_dram_queue(PACKET_QUEUE *queue, PACKET *packet);

    bool check_availability_for_priority_read(uint64_t address,
                                              uint8_t priority);
    void increase_priority(PACKET *packet, uint8_t new_priority);
    void increase_priority_by_index(uint32_t channel, int index,
                                    uint8_t new_priority, bool lower_prio_q);
    void retry_outstanding_promotions();

    // nack_request is a NOP
    void nack_request(PACKET *packet) {}
};

#endif
