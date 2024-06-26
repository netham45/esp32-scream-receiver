#pragma once

extern bool is_underrun;
extern uint64_t received_packets;
extern uint64_t packet_buffer_size;
extern uint64_t packet_buffer_pos;
extern uint64_t target_buffer_size;

void setup_buffer();
bool push_chunk(uint8_t *chunk);
uint8_t *pop_chunk();
void setup_buffer();
