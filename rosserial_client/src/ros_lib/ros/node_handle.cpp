/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2011, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials provided
 *    with the distribution.
 * * Neither the name of Willow Garage, Inc. nor the names of its
 *    contributors may be used to endorse or promote prducts derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "node_handle.h"

#include <stdio.h>

#include "msg_receiver.h"
#include "node_output.h"
#include "publisher.h"
#include "rosserial_ids.h"
#include "service_server.h"
#include "subscriber.h"
#include "hardware.h"

#include "std_msgs/Time.h"
#include "rosserial_msgs/TopicInfo.h"
#include "rosserial_msgs/Log.h"
#include "rosserial_msgs/RequestParam.h"

namespace ros {

using rosserial_msgs::TopicInfo;

NodeHandle::NodeHandle(Hardware* hardware)
    : hardware_(hardware),
      node_output_(hardware),
      connected_(false),
      param_received_(false),
      time_sync_start_(0),
      time_sync_end_(0),
      state_(STATE_FIRST_FF),
      remaining_data_bytes_(0),
      topic_(0),
      data_index_(0),
      checksum_(0),
      invalid_size_error_count_(0),
      checksum_error_count_(0),
      malformed_message_error_count_(0),
      total_receivers_(0) {}

Hardware* NodeHandle::getHardware() {
  return hardware_;
}

void NodeHandle::logdebug(const char* msg) {
  log(rosserial_msgs::Log::DEBUG, msg);
}

void NodeHandle::loginfo(const char* msg) {
  log(rosserial_msgs::Log::INFO, msg);
}

void NodeHandle::logwarn(const char* msg) {
  log(rosserial_msgs::Log::WARN, msg);
}

void NodeHandle::logerror(const char* msg) {
  log(rosserial_msgs::Log::ERROR, msg);
}

void NodeHandle::logfatal(const char* msg) {
  log(rosserial_msgs::Log::FATAL, msg);
}

bool NodeHandle::registerReceiver(MsgReceiver* receiver) {
  if (total_receivers_ >= kMaxSubscribers) {
    return false;
  }
  receivers[total_receivers_] = receiver;
  receiver->setId(100 + total_receivers_);
  total_receivers_++;
  return true;
}

void NodeHandle::reset() {
  state_ = STATE_FIRST_FF;
  remaining_data_bytes_ = 0;
  topic_ = 0;
  data_index_ = 0;
  checksum_ = 0;
}

int NodeHandle::spinOnce() {
  unsigned long current_time = hardware_->time();

  if (connected_) {
    // Connection times out after kConnectionTimeout milliseconds without a
    // time sync.
    if (current_time - time_sync_end_ > kConnectionTimeout) {
      connected_ = false;
      time_sync_start_ = 0;
      reset();
    }
    // Sync time every kSyncPeriod milliseconds.
    if (current_time - time_sync_end_ > kSyncPeriod) {
      requestTimeSync();
    }
  }

  int byte_count;
  for (byte_count = 0; byte_count < kMaxBytesPerSpin; byte_count++) {
    int input_byte = hardware_->read();
    if (input_byte < 0) {
      break;
    }
    checksum_ += input_byte;
    switch (state_) {
      case STATE_FIRST_FF:
        if (input_byte == 0xff) {
          state_ = STATE_SECOND_FF;
        } else {
          state_error_count_++;
          reset();
        }
        break;
      case STATE_SECOND_FF:
        if (input_byte == 0xff) {
          state_ = STATE_TOPIC_LOW;
        } else {
          state_error_count_++;
          reset();
        }
        break;
      case STATE_TOPIC_LOW:
        // This is the first byte to be included in the checksum.
        checksum_ = input_byte;
        topic_ = input_byte;
        state_ = STATE_TOPIC_HIGH;
        break;
      case STATE_TOPIC_HIGH:
        topic_ += input_byte << 8;
        state_ = STATE_SIZE_LOW;
        break;
      case STATE_SIZE_LOW:
        remaining_data_bytes_ = input_byte;
        state_ = STATE_SIZE_HIGH;
        break;
      case STATE_SIZE_HIGH:
        remaining_data_bytes_ += static_cast<uint16_t>(input_byte) << 8;
        if (remaining_data_bytes_ == 0) {
          state_ = STATE_CHECKSUM;
        } else if (remaining_data_bytes_ <= kInputSize) {
          state_ = STATE_MESSAGE;
        } else {
          // Protect against buffer overflow.
          reset();
          ++invalid_size_error_count_;
        }
        break;
      case STATE_MESSAGE:
        message_in[data_index_++] = input_byte;
        remaining_data_bytes_--;
        if (remaining_data_bytes_ == 0) {
          state_ = STATE_CHECKSUM;
        }
        break;
      case STATE_CHECKSUM:
        if ((checksum_ % 256) == 255) {
          if (topic_ == TOPIC_NEGOTIATION) {
            requestTimeSync();
            negotiateTopics();
          } else if (topic_ == TopicInfo::ID_TIME) {
            completeTimeSync(message_in);
            connected_ = true;
          } else if (topic_ == TopicInfo::ID_PARAMETER_REQUEST) {
            if (req_param_resp.deserialize(message_in, kInputSize) >= 0) {
              param_received_ = true;
            }
          } else if (topic_ >= 100 && topic_ - 100 < kMaxSubscribers &&
                     receivers[topic_ - 100] != 0) {
            bool success = receivers[topic_ - 100]->receive(message_in, data_index_);
            if (!success) {
              ++malformed_message_error_count_;
            }
          } else {
            ++checksum_error_count_;
          }
        }
        reset();
        break;
      default:;
        reset();
        break;
    }
  }
  return byte_count;
}

int NodeHandle::getInvalidSizeErrorCount() const {
  return invalid_size_error_count_;
}

int NodeHandle::getChecksumErrorCount() const {
  return checksum_error_count_;
}

int NodeHandle::getStateErrorCount() const {
  return state_error_count_;
}

int NodeHandle::getMalformedMessageErrorCount() const {
  return malformed_message_error_count_;
}

void NodeHandle::requestTimeSync() {
  if (time_sync_start_ > 0) {
    // A time sync request is already in flight.
    return;
  }
  time_sync_start_ = hardware_->time();
  // TODO(damonkohler): Why publish an empty message here?
  std_msgs::Time time;
  node_output_.publish(rosserial_msgs::TopicInfo::ID_TIME, &time);
}

void NodeHandle::completeTimeSync(unsigned char* data) {
  // TODO(damonkohler): Use micros() for higher precision?
  time_sync_end_ = hardware_->time();
  unsigned long offset = (time_sync_end_ - time_sync_start_) / 2;
  std_msgs::Time time;
  if (time.deserialize(data, kInputSize) < 0) {
    return;
  }
  sync_time_ = time.data;
  sync_time_ += Duration::fromMillis(offset);
  time_sync_start_ = 0;
  char message[40];
  snprintf(message, 40, "Time: %lu %lu", sync_time_.sec, sync_time_.nsec);
  logdebug(message);
}

Time NodeHandle::now() const {
  unsigned long offset = hardware_->time() - time_sync_end_;
  return sync_time_ + Duration::fromMillis(offset);
}

bool NodeHandle::advertise(Publisher& publisher) {
  // TODO(damonkohler): Pull out a publisher registry or keep track of
  // the next available ID.
  for (int i = 0; i < kMaxPublishers; i++) {
    if (publishers[i] == 0) {  // empty slot
      publishers[i] = &publisher;
      publisher.setId(i + 100 + kMaxSubscribers);
      publisher.setNodeOutput(&node_output_);
      return true;
    }
  }
  return false;
}

void NodeHandle::negotiateTopics() {
  rosserial_msgs::TopicInfo topic_info;
  // Slots are allocated sequentially and contiguously. We can break
  // out early.
  for (int i = 0; i < kMaxPublishers && publishers[i] != 0; i++) {
    topic_info.topic_id = publishers[i]->getId();
    topic_info.topic_name = const_cast<char*>(publishers[i]->getTopicName());
    topic_info.message_type = const_cast<char*>(publishers[i]->getMessageType());
    node_output_.publish(TOPIC_PUBLISHERS, &topic_info);
  }
  for (int i = 0; i < kMaxSubscribers && receivers[i] != 0; i++) {
    topic_info.topic_id = receivers[i]->getId();
    topic_info.topic_name = const_cast<char*>(receivers[i]->getTopicName());
    topic_info.message_type = const_cast<char*>(receivers[i]->getMessageType());
    node_output_.publish(TOPIC_SUBSCRIBERS, &topic_info);
  }
}

void NodeHandle::log(char byte, const char* msg) {
  rosserial_msgs::Log l;
  l.level = byte;
  l.msg = const_cast<char*>(msg);
  this->node_output_.publish(rosserial_msgs::TopicInfo::ID_LOG, &l);
}

bool NodeHandle::requestParam(const char* name, int time_out) {
  param_received_ = false;
  rosserial_msgs::RequestParamRequest req;
  req.name  = const_cast<char*>(name);
  node_output_.publish(TopicInfo::ID_PARAMETER_REQUEST, &req);
  unsigned long start_time = hardware_->time();
  while(!param_received_) {
    spinOnce();
    if (hardware_->time() - start_time > time_out) {
      return false;
    }
  }
  return true;
}

bool NodeHandle::getParam(const char* name, int* param, int length) {
  if (requestParam(name) && length == req_param_resp.ints_length) {
    for (int i = 0; i < length; i++) {
      param[i] = req_param_resp.ints[i];
    }
    return true;
  }
  return false;
}

bool NodeHandle::getParam(const char* name, float* param, int length) {
  if (requestParam(name) && length == req_param_resp.floats_length) {
    for (int i = 0; i < length; i++) {
      param[i] = req_param_resp.floats[i];
    }
    return true;
  }
  return false;
}

bool NodeHandle::getParam(const char* name, char** param, int length) {
  if (requestParam(name) && length == req_param_resp.strings_length) {
    for (int i = 0; i < length; i++) {
      strcpy(param[i], req_param_resp.strings[i]);
    }
    return true;
  }
  return false;
}

bool NodeHandle::connected() {
  return connected_;
}

}  // namespace ros
