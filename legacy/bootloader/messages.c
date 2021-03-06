/*
 * This file is part of the Trezor project, https://trezor.io/
 *
 * Copyright (C) 2014 Pavol Rusnak <stick@satoshilabs.com>
 *
 * This library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>

#include "memzero.h"
#include "messages.h"
#include "util.h"

#include "messages.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"

#include "messages-management.pb.h"

static uint8_t msg_in_buffer[MSG_IN_SIZE];

struct MessagesMap_t {
  char type;  // n = normal, d = debug
  char dir;   // i = in, o = out
  uint16_t msg_id;
  const pb_msgdesc_t *fields;
  void (*process_func)(const void *ptr);
};

static const struct MessagesMap_t MessagesMap[] = {
#include "messages_map.h"
    // end
    {0, 0, 0, 0, 0}};

#include "messages_map_limits.h"

const pb_msgdesc_t *MessageFields(char type, char dir, uint16_t msg_id) {
  const struct MessagesMap_t *m = MessagesMap;
  while (m->type) {
    if (type == m->type && dir == m->dir && msg_id == m->msg_id) {
      return m->fields;
    }
    m++;
  }
  return 0;
}

void MessageProcessFunc(char type, char dir, uint16_t msg_id, void *ptr) {
  const struct MessagesMap_t *m = MessagesMap;
  while (m->type) {
    if (type == m->type && dir == m->dir && msg_id == m->msg_id) {
      m->process_func(ptr);
      return;
    }
    m++;
  }
}

static uint32_t msg_out_start = 0;
uint32_t msg_out_end = 0;
static uint32_t msg_out_cur = 0;
uint8_t msg_out[MSG_OUT_SIZE];

static inline void msg_out_append(uint8_t c) {
  if (msg_out_cur == 0) {
    msg_out[msg_out_end * 64] = '?';
    msg_out_cur = 1;
  }
  msg_out[msg_out_end * 64 + msg_out_cur] = c;
  msg_out_cur++;
  if (msg_out_cur == 64) {
    msg_out_cur = 0;
    msg_out_end = (msg_out_end + 1) % (MSG_OUT_SIZE / 64);
  }
}

static inline void msg_out_pad(void) {
  if (msg_out_cur == 0) return;
  while (msg_out_cur < 64) {
    msg_out[msg_out_end * 64 + msg_out_cur] = 0;
    msg_out_cur++;
  }
  msg_out_cur = 0;
  msg_out_end = (msg_out_end + 1) % (MSG_OUT_SIZE / 64);
}

static bool pb_callback_out(pb_ostream_t *stream, const uint8_t *buf,
                            size_t count) {
  (void)stream;
  for (size_t i = 0; i < count; i++) {
    msg_out_append(buf[i]);
  }
  return true;
}
volatile bool decode_flag = false;
bool msg_write_common(char type, uint16_t msg_id, const void *msg_ptr) {
  const pb_msgdesc_t *fields = MessageFields(type, 'o', msg_id);
  if (!fields) {  // unknown message
    return false;
  }

  size_t len = 0;
  decode_flag = pb_get_encoded_size(&len, fields, msg_ptr);
  if (!decode_flag) {
    return false;
  }

  void (*append)(uint8_t) = NULL;
  bool (*pb_callback)(pb_ostream_t *, const uint8_t *, size_t);

  if (type == 'n') {
    append = msg_out_append;
    pb_callback = pb_callback_out;
  } else {
    return false;
  }

  append('#');
  append('#');
  append((msg_id >> 8) & 0xFF);
  append(msg_id & 0xFF);
  append((len >> 24) & 0xFF);
  append((len >> 16) & 0xFF);
  append((len >> 8) & 0xFF);
  append(len & 0xFF);
  pb_ostream_t stream = {pb_callback, 0, SIZE_MAX, 0, 0};
  bool status = pb_encode(&stream, fields, msg_ptr);
  if (type == 'n') {
    msg_out_pad();
  }

  return status;
}

enum {
  READSTATE_IDLE,
  READSTATE_READING,
};

void msg_process(char type, uint16_t msg_id, const pb_msgdesc_t *fields,
                 uint8_t *msg_raw, uint32_t msg_size) {
  static uint8_t *msg_data = msg_in_buffer;
  memzero(msg_data, sizeof(msg_in_buffer));
  pb_istream_t stream = pb_istream_from_buffer(msg_raw, msg_size);
  bool status = pb_decode(&stream, fields, msg_data);
  if (status) {
    MessageProcessFunc(type, 'i', msg_id, msg_data);
  } else {
    // fsm_sendFailure(FailureType_Failure_DataError, stream.errmsg);
  }
}

void msg_read_common(char type, const uint8_t *buf, uint32_t len) {
  static char read_state = READSTATE_IDLE;
  static uint8_t msg_in[MSG_IN_SIZE];
  static uint16_t msg_id = 0xFFFF;
  static uint32_t msg_size = 0;
  static uint32_t msg_pos = 0;
  static const pb_msgdesc_t *fields = 0;

  if (len != 64) return;

  if (read_state == READSTATE_IDLE) {
    if (buf[0] != '?' || buf[1] != '#' ||
        buf[2] != '#') {  // invalid start - discard
      return;
    }
    msg_id = (buf[3] << 8) + buf[4];
    msg_size =
        ((uint32_t)buf[5] << 24) + (buf[6] << 16) + (buf[7] << 8) + buf[8];

    fields = MessageFields(type, 'i', msg_id);
    if (!fields) {  // unknown message
      // fsm_sendFailure(FailureType_Failure_UnexpectedMessage,
      //                 _("Unknown message"));
      return;
    }
    if (msg_size > MSG_IN_SIZE) {  // message is too big :(
      // fsm_sendFailure(FailureType_Failure_DataError, _("Message too big"));
      return;
    }

    read_state = READSTATE_READING;

    memcpy(msg_in, buf + 9, len - 9);
    msg_pos = len - 9;
  } else if (read_state == READSTATE_READING) {
    if (buf[0] != '?') {  // invalid contents
      read_state = READSTATE_IDLE;
      return;
    }
    /* raw data starts at buf + 1 with len - 1 bytes */
    buf++;
    len = MIN(len - 1, MSG_IN_SIZE - msg_pos);

    memcpy(msg_in + msg_pos, buf, len);
    msg_pos += len;
  }

  if (msg_pos >= msg_size) {
    msg_process(type, msg_id, fields, msg_in, msg_size);
    msg_pos = 0;
    read_state = READSTATE_IDLE;
  }
}

const uint8_t *msg_out_data(void) {
  if (msg_out_start == msg_out_end) return 0;
  uint8_t *data = msg_out + (msg_out_start * 64);
  msg_out_start = (msg_out_start + 1) % (MSG_OUT_SIZE / 64);
  return data;
}
