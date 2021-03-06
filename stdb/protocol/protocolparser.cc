/*!
 * \file protocolparser.cc
 */
#include "stdb/protocol/protocolparser.h"

#include <sstream>
#include <cassert>
#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include "stdb/common/logging.h"
#include "stdb/core/storage_api.h"

namespace stdb {
namespace protocol {

ProtocolParserError::ProtocolParserError(std::string line, size_t pos) : StreamError(line, pos) { }

DatabaseError::DatabaseError(const common::Status& status) : std::exception(), status(status) {
  error_msg = status.ToString();
}

const char* DatabaseError::what() const noexcept {
  return error_msg.c_str();
}

// ReadBuffer class //
ReadBuffer::ReadBuffer(const size_t buffer_size)
   : BUFFER_SIZE(buffer_size)
    , buffer_(buffer_size*N_BUF, 0)
    , rpos_(0)
    , wpos_(0)
    , cons_(0)
    , buffers_allocated_(0) { }

Byte ReadBuffer::get() {
  if (rpos_ == wpos_) {
    auto ctx = get_error_context("unexpected end of stream");
    throw ProtocolParserError(std::get<0>(ctx), std::get<1>(ctx));
  }
  return buffer_[rpos_++];
}

Byte ReadBuffer::pick() const {
  if (rpos_ == wpos_) {
    auto ctx = get_error_context("unexpected end of stream");
    throw ProtocolParserError(std::get<0>(ctx), std::get<1>(ctx));
  }
  return buffer_[rpos_];
}

bool ReadBuffer::is_eof() {
  return rpos_ == wpos_;
}

int ReadBuffer::read(Byte *buffer, size_t buffer_len) {
  assert(buffer_len < 0x100000000ul);
  u32 to_read = wpos_ - rpos_;
  to_read = std::min(static_cast<u32>(buffer_len), to_read);
  std::copy(buffer_.begin() + rpos_, buffer_.begin() + rpos_ + to_read, buffer);
  rpos_ += to_read;
  return static_cast<int>(to_read);
}

int ReadBuffer::read_line(Byte* buffer, size_t quota) {
  assert(quota < 0x100000000ul);
  u32 available = wpos_ - rpos_;
  auto to_read = std::min(static_cast<u32>(quota), available);
  for (u32 i = 0; i < to_read; i++) {
    Byte c = buffer_[rpos_ + i];
    buffer[i] = c;
    if (c == '\n') {
      // Stop iteration
      u32 bytes_copied = i + 1;
      rpos_ += bytes_copied;
      return static_cast<int>(bytes_copied);
    }
  }
  // No end of line found
  return -1*static_cast<int>(to_read);
}

void ReadBuffer::close() {
}

std::tuple<std::string, size_t> ReadBuffer::get_error_context(const char *error_message) const {
  // Get the frame: [...\r\n...\r\n...\r\n]
  auto origin = buffer_.data() + cons_;
  const Byte* stop = origin;
  int nlcnt = 0;
  const Byte* end = buffer_.data() + wpos_;
  while (stop < end) {
    if (*stop == '\n') {
      nlcnt++;
      if (nlcnt == 3) {
        break;
      }
    }
    stop++;
  }
  auto err = std::string(origin, stop);
  boost::algorithm::replace_all(err, "\r", "\\r");
  boost::algorithm::replace_all(err, "\n", "\\n");
  std::stringstream message;
  message << error_message << " - ";
  message << err;
  return std::make_tuple(message.str(), 0);
}

void ReadBuffer::consume() {
  assert(buffers_allocated_ == 0);  // Invariant check: buffer can be invalidated!
  cons_ = rpos_;
}

void ReadBuffer::discard() {
  assert(buffers_allocated_ == 0);  // Invariant check: buffer can be invalidated!
  rpos_ = cons_;
}

ReadBuffer::BufferT ReadBuffer::pull() {
  assert(buffers_allocated_ == 0);  // Invariant check: buffer will be invalidated after vector.resize!
  buffers_allocated_++;

  u32 sz = static_cast<u32>(buffer_.size()) - wpos_;  // because previous push can bring partially filled buffer
  if (sz < BUFFER_SIZE) {
    if ((cons_ + sz) > BUFFER_SIZE) {
      // Problem can be solved by rotating the buffer and asjusting wpos_, rpos_ and cons_
      std::copy(buffer_.begin() + cons_, buffer_.end(), buffer_.begin());
      wpos_ -= cons_;
      rpos_ -= cons_;
      cons_ = 0;
    } else {
      // Double the size of the buffer
      buffer_.resize(buffer_.size() * 2);
    }
  }
  Byte* ptr = buffer_.data() + wpos_;
  return ptr;
}

void ReadBuffer::push(ReadBuffer::BufferT, u32 size) {
  assert(buffers_allocated_ == 1);
  buffers_allocated_--;
  wpos_ += size;
}

// ProtocolParser class //
RESPProtocolParser::RESPProtocolParser(std::shared_ptr<DbSession> consumer)
    : done_(false)
    , rdbuf_(RDBUF_SIZE)
    , consumer_(consumer) { }

void RESPProtocolParser::start() {
  // LOG(INFO) << "Starting protocol parser";
}

bool RESPProtocolParser::parse_timestamp(RESPStream& stream, Sample& sample) {
  bool success = false;
  int bytes_read = 0;
  const size_t tsbuflen = 28;
  Byte tsbuf[tsbuflen];
  auto next = stream.next_type();
  switch(next) {
    case RESPStream::_AGAIN:
      return false;
    case RESPStream::INTEGER:
      std::tie(success, sample.timestamp) = stream.read_int();
      if (!success) {
        return false;
      }
      break;
    case RESPStream::STRING:
      std::tie(success, bytes_read) = stream.read_string(tsbuf, tsbuflen);
      if (!success) {
        return false;
      }
      tsbuf[bytes_read] = '\0';
      if (Utility::parse_timestamp(tsbuf, &sample).IsOk()) {
        break;
      }
      // Fail through on error
    case RESPStream::ARRAY:
    case RESPStream::BULK_STR:
    case RESPStream::ERROR:
    case RESPStream::_BAD:
      {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("unexpected parameter timestamp format");
        throw ProtocolParserError(msg, pos);
      }
  };
  return true;
}

bool RESPProtocolParser::update_dict(ParamId uid, const ParamId* row, int nvalues) {
  if (idmap_.count(uid) != 0) {
    return false;
  }
  for (int i = 0; i < nvalues; i++) {
    idmap_.insert(std::make_pair(uid, row[i]));
  }
  return true;
}

int RESPProtocolParser::read_dict(ParamId uid, ParamId* row, int nvalues) {
  auto pair = idmap_.equal_range(uid);
  int i = 0;
  for (auto it = pair.first; it != pair.second; it++) {
    if (i == nvalues) {
      break;
    }
    row[i] = it->second;
    i++;
  }
  return i;
}

bool RESPProtocolParser::parse_dict(RESPStream& stream) {
  const u32 nvalues = STDB_LIMITS_MAX_ROW_WIDTH;
  ParamId ids[nvalues];
  bool success;
  int bytes_read;
  int rowwidth = -1;
  const int buffer_len = RESPStream::STRING_LENGTH_MAX;
  Byte buffer[buffer_len] = {};

  while (true) {
    // read id
    auto next = stream.next_type();
    switch(next) {
      case RESPStream::_AGAIN:
        rdbuf_.discard();
        return false;
      case RESPStream::ARRAY: {
        // User provided the dictionary
        size_t arrsize;
        std::tie(success, arrsize) = stream.read_array_size();
        if (!success) {
          rdbuf_.discard();
          return false;
        }
        if (arrsize % 2 != 0) {
          std::string msg;
          size_t pos;
          std::tie(msg, pos) = rdbuf_.get_error_context("number of elements in the dictionary should be even");
          throw ProtocolParserError(msg, pos);
        }
        for (size_t i = 0; i < arrsize; i +=2 ) {
          next = stream.next_type();
          if (next == RESPStream::_AGAIN) {
            rdbuf_.discard();
            return false;
          } else if (next == RESPStream::STRING) {
            std::tie(success, bytes_read) = stream.read_string(buffer, buffer_len);
            if (!success) {
              rdbuf_.discard();
              return false;
            }
            rowwidth = consumer_->name_to_param_id_list(buffer, buffer + bytes_read, ids, nvalues);
            if (rowwidth <= 0) {
              std::string msg;
              size_t pos;
              std::tie(msg, pos) = rdbuf_.get_error_context("invalid series name format");
              throw ProtocolParserError(msg, pos);
            }
          } else {
            // Bad frame
            std::string msg;
            size_t pos;
            std::tie(msg, pos) = rdbuf_.get_error_context("unexpected series name format");
            BOOST_THROW_EXCEPTION(ProtocolParserError(msg, pos));
          }
          next = stream.next_type();
          if (next == RESPStream::_AGAIN) {
            rdbuf_.discard();
            return false;
          } else if (next == RESPStream::INTEGER) {
            ParamId uid;
            std::tie(success, uid) = stream.read_int();
            if (!success) {
              rdbuf_.discard();
              return false;
            }
            success = update_dict(uid, ids, rowwidth);
          } else {
            // Bad frame
            std::string msg;
            size_t pos;
            std::tie(msg, pos) = rdbuf_.get_error_context("unexpected series name format");
            throw ProtocolParserError(msg, pos);
          }
        }
        rdbuf_.consume();
        continue;
      }
      case RESPStream::BULK_STR:
      case RESPStream::ERROR:
      case RESPStream::INTEGER:
      case RESPStream::STRING:
      case RESPStream::_BAD:
        // We should parse message stream until we met something different.
        // The actual dictionary can be split into parts, so we shouldn't stop
        // to expect dictionary elements until we will see any other element
        // type (except array).
        rdbuf_.discard();
        return true;
    };
  }
}

int RESPProtocolParser::parse_ids(RESPStream& stream, ParamId* ids, int nvalues) {
  bool success;
  int bytes_read;
  int rowwidth = -1;
  const int buffer_len = RESPStream::STRING_LENGTH_MAX;
  Byte buffer[buffer_len] = {};
  ParamId uid = 0;
  // read id
  auto next = stream.next_type();
  switch (next) {
    case RESPStream::_AGAIN:
      rdbuf_.discard();
      return -1;
    case RESPStream::STRING:
      std::tie(success, bytes_read) = stream.read_string(buffer, buffer_len);
      if (!success) {
        rdbuf_.discard();
        return -1;
      }
      rowwidth = consumer_->name_to_param_id_list(buffer, buffer + bytes_read, ids, static_cast<u32>(nvalues));
      if (rowwidth <= 0) {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("invalid series name format");
        throw ProtocolParserError(msg, pos);
      }
      break;
    case RESPStream::INTEGER:
      std::tie(success, uid) = stream.read_int();
      if (!success) {
        rdbuf_.discard();
        return -1;
      }
      rowwidth = read_dict(uid, ids, nvalues);
      if (rowwidth <= 0) {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("invalid series name format");
        throw ProtocolParserError(msg, pos);
      }
      break;
    case RESPStream::ARRAY:
    case RESPStream::BULK_STR:
    case RESPStream::ERROR:
    case RESPStream::_BAD:
      // Bad frame
      {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("unexpected parameter id format");
        throw ProtocolParserError(msg, pos);
      }
  };
  return rowwidth;
}

bool RESPProtocolParser::parse_values(RESPStream&        stream,
                                      ParamId const* ids,
                                      double*            values,
                                      std::string*       events,
                                      int                nvalues) {
  const size_t buflen = 64;
  Byte buf[buflen];
  int bytes_read;
  int arrsize;
  bool success;
  auto parse_int_value = [&](int at) {
    std::tie(success, values[at]) = stream.read_int();
    if (!success) {
      return false;
    }
    return true;
  };
  auto parse_string_value = [&](int at) {
    std::tie(success, bytes_read) = stream.read_string(buf, buflen);
    if (!success) {
      return false;
    }
    if (bytes_read < 0) {
      std::string msg;
      size_t pos;
      std::tie(msg, pos) = rdbuf_.get_error_context("floating point value can't be that big");
      throw ProtocolParserError(msg, pos);
    }
    buf[bytes_read] = '\0';
    char* endptr = nullptr;
    values[at] = strtod(buf, &endptr);
    if (endptr - buf != bytes_read) {
      std::stringstream fmt;
      fmt << "can't parse double value: " << buf;
      std::string msg;
      size_t pos;
      std::tie(msg, pos) = rdbuf_.get_error_context(fmt.str().c_str());
      throw ProtocolParserError(msg, pos);
    }
    return true;
  };
  auto parse_event_value = [&](int at) {
    event_inp_buf_.resize(RESPStream::STRING_LENGTH_MAX);
    std::tie(success, bytes_read) = stream.read_string(event_inp_buf_.data(), event_inp_buf_.size());
    if (!success) {
      return false;
    }
    if (bytes_read < 0 || bytes_read >= STDB_LIMITS_MAX_EVENT_LEN) {
      std::string msg;
      size_t pos;
      std::tie(msg, pos) = rdbuf_.get_error_context("event value is too big");
      throw ProtocolParserError(msg, pos);
    }
    events[at].assign(event_inp_buf_.data(),
                      event_inp_buf_.data() + bytes_read);
    return true;
  };
  auto next = stream.next_type();
  switch(next) {
    case RESPStream::_AGAIN:
      return false;
    case RESPStream::INTEGER:
      if (nvalues == 1) {
        if (!parse_int_value(0)) {
          return false;
        }
      } else {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("array expected (bulk format), integer found");
        throw ProtocolParserError(msg, pos);
      }
      break;
    case RESPStream::STRING:
      // Single integer value returned
      if (nvalues == 1) {
        if (static_cast<i64>(ids[0]) > 0) {
          if (!parse_string_value(0)) {
            return false;
          }
        }
        else {
          if (!parse_event_value(0)) {
            return false;
          }
        }
      } else {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("array expected (bulk format), string found");
        throw ProtocolParserError(msg, pos);
      }
      break;
    case RESPStream::ARRAY:
      std::tie(success, arrsize) = stream.read_array_size();
      if (!success) {
        return false;
      }
      if (arrsize != nvalues) {
        std::string msg;
        size_t pos;
        const char* error;
        if (arrsize < nvalues) {
          error = "wrong array size, more values expected";
        } else {
          error = "wrong array size, less values expected";
        }
        std::tie(msg, pos) = rdbuf_.get_error_context(error);
        throw ProtocolParserError(msg, pos);
      }
      for (int i = 0; i < arrsize; i++) {
        next = stream.next_type();
        if (static_cast<i64>(ids[i]) > 0) {
          switch(next) {
            case RESPStream::_AGAIN:
              return false;
            case RESPStream::INTEGER:
              if (!parse_int_value(i)) {
                return false;
              }
              break;
            case RESPStream::STRING:
              if (!parse_string_value(i)) {
                return false;
              }
              break;
            case RESPStream::ARRAY:
            case RESPStream::BULK_STR:
            case RESPStream::ERROR:
            case RESPStream::_BAD: {
              // Bad frame
              std::string msg;
              size_t pos;
              std::tie(msg, pos) = rdbuf_.get_error_context("unexpected parameter value format");
              throw ProtocolParserError(msg, pos);
            }
          }
        } else {
          switch (next) {
            case RESPStream::STRING:
              if (!parse_event_value(i)) {
                return false;
              }
              break;
            case RESPStream::_AGAIN:
              return false;
            default: {
              std::string msg;
              size_t pos;
              std::tie(msg, pos) = rdbuf_.get_error_context("unexpected event format");
              throw ProtocolParserError(msg, pos);
            }
          }
        }
      }
      break;
    case RESPStream::BULK_STR:
    case RESPStream::ERROR:
    case RESPStream::_BAD:
      // Bad frame
      {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("unexpected parameter value format");
        throw ProtocolParserError(msg, pos);
      }
  };
  return true;
}

void RESPProtocolParser::worker() {
  // Buffer to read strings from
  int rowwidth = 0;
  // Data to read
  Sample sample;
  common::Status status;
  //
  RESPStream stream(&rdbuf_);
  // try to read dict
  while (true) {
    bool ok = parse_dict(stream);
    // Dict may be incomplete yet. In this case the method will return false and we
    // will need to call it once again. When done it will return true. On error it will
    // throw ProtocolParserError exception (the same way as all other parse_stuff methods).
    if (ok) break;
    else return;
  }
  while (true) {
    bool success;
    // read id
    rowwidth = parse_ids(stream, paramids_, STDB_LIMITS_MAX_ROW_WIDTH);
    if (rowwidth < 0) {
      rdbuf_.discard();
      return;
    }
    // read ts
    success = parse_timestamp(stream, sample);
    if (!success) {
      rdbuf_.discard();
      return;
    }
    success = parse_values(stream, paramids_, values_, events_, rowwidth);
    if (!success) {
      rdbuf_.discard();
      return;
    }
    rdbuf_.consume();

    sample.payload.type = PAYLOAD_FLOAT;
    sample.payload.size = sizeof(Sample);
    // Timestamp is initialized once and for all
    for (int i = 0; i < rowwidth; i++) {
      if (static_cast<i64>(paramids_[i]) > 0) {
        // Fast path
        sample.paramid = paramids_[i];
        sample.payload.float64 = values_[i];
        // LOG(INFO) << "value=" << values_[i];
        status = consumer_->write(sample);
        // Message processed and frame can be removed (if possible)
        if (status != common::Status::Ok()) {
          throw DatabaseError(status);
        }
      } else {
        size_t len = events_[i].size() + sizeof(Sample);
        Sample evt;
        evt.payload.type = PAYLOAD_EVENT;
        evt.payload.size = static_cast<u16>(len);  // len guaranteed to fit
        evt.timestamp = sample.timestamp;
        evt.paramid = paramids_[i];
        event_out_buf_.resize(len);
        auto pevt = reinterpret_cast<Sample*>(event_out_buf_.data());
        memcpy(pevt, &evt, sizeof(evt));
        memcpy(pevt->payload.data, events_[i].data(), events_[i].size());
        status = consumer_->write(*pevt);
        // Message processed and frame can be removed (if possible)
        if (status != common::Status::Ok()) {
          throw DatabaseError(status);
        }
      }
    }
  }
}

NullResponse RESPProtocolParser::parse_next(Byte* buffer, u32 sz) {
  static NullResponse response;
  rdbuf_.push(buffer, sz);
  worker();
  return response;
}

Byte* RESPProtocolParser::get_next_buffer() {
  return rdbuf_.pull();
}

void RESPProtocolParser::close() {
  done_ = true;
}

std::string RESPProtocolParser::error_repr(int kind, std::string const& err) const {
  switch (kind) {
    case ERR:
      return "-ERR " + err + "\r\n";
    case DB:
      return "-DB " + err + "\r\n";
    case PARSE:
      return "-PARSER " + err + "\r\n";
  };
  return "-UNKNOWN " + err + "\r\n";
}


//     OpenTSDB protocol      //
OpenTSDBProtocolParser::OpenTSDBProtocolParser(std::shared_ptr<DbSession> consumer)
    : done_(false)
    , rdbuf_(RDBUF_SIZE)
    , consumer_(consumer) { }

void OpenTSDBProtocolParser::start() { }

OpenTSDBResponse OpenTSDBProtocolParser::parse_next(Byte* buffer, u32 sz) {
  rdbuf_.push(buffer, sz);
  return worker();
}

Byte* OpenTSDBProtocolParser::get_next_buffer() {
  return rdbuf_.pull();
}

void OpenTSDBProtocolParser::close() {
  done_ = true;
}

enum CMD_PREF_LEN {
  PUT_LEN = 4,
  ROLLUP_LEN = 6,
  HISTOGRAM_LEN = 4,
  STATS_LEN = 5,
  VERSION_LEN = 7,
  HELP_LEN = 4,
  DROPCACHES_LEN = 10,
};

static bool is_put(const Byte* p) {
  static const u32 asciiput = 0x20747570;
  return *reinterpret_cast<const u32*>(p) == asciiput;
}

enum class OpenTSDBMessageType {
  PUT,
  ROLLUP,
  HISTOGRAM,
  STATS,
  VERSION,
  HELP,
  DROPCACHES,
  UNKNOWN,
};

static bool is_rollup(const Byte* p) {
  return std::equal(p, p + ROLLUP_LEN, "rollup");
}

static bool is_histogram(const Byte* p) {
  return std::equal(p, p + HISTOGRAM_LEN, "hist");
}

static bool is_stats(const Byte* p) {
  return std::equal(p, p + STATS_LEN, "stats");
}

static bool is_version(const Byte* p) {
  return std::equal(p, p + VERSION_LEN, "version");
}

static bool is_help(const Byte* p) {
  return std::equal(p, p + HELP_LEN, "help");
}

static bool is_dropcaches(const Byte* p) {
  return std::equal(p, p + DROPCACHES_LEN, "dropcaches");
}

static OpenTSDBMessageType message_dispatch(Byte* p, int len) {
  // Fast path
  if (len >= 4 && is_put(p)) {
    return OpenTSDBMessageType::PUT;
  } else if (len >= ROLLUP_LEN && is_rollup(p)) {
    return OpenTSDBMessageType::ROLLUP;
  } else if (len >= HISTOGRAM_LEN && is_histogram(p)) {
    return OpenTSDBMessageType::HISTOGRAM;
  } else if (len >= STATS_LEN && is_stats(p)) {
    return OpenTSDBMessageType::STATS;
  } else if (len >= VERSION_LEN && is_version(p)) {
    return OpenTSDBMessageType::VERSION;
  } else if (len >= HELP_LEN && is_help(p)) {
    return OpenTSDBMessageType::HELP;
  } else if (len >= DROPCACHES_LEN && is_dropcaches(p)) {
    return OpenTSDBMessageType::DROPCACHES;
  }
  return OpenTSDBMessageType::UNKNOWN;
}

/**
 * @brief Skip element of the space separated list
 * @return new pointer, adjusted length, and number of trailing spaces
 * @invariant quota >= ntrailing, ntrailing >= 1
 */
static std::tuple<Byte*, int, int> skip_element(Byte* buffer, int len) {
  int quota = len;
  Byte* p = buffer;
  // Skip element
  while (quota) {
    Byte c = *p;
    if (c == ' ' || c == '\n') {
      break;
    }
    p++;
    quota--;
  }
  // Skip space
  int ntrailing = 0;
  while (quota) {
    Byte c = *p;
    if (c != ' ' && c != '\n') {
      break;
    }
    p++;
    quota--;
    ntrailing++;
  }
  return std::make_tuple(p, quota, ntrailing);
}

static Timestamp from_unix_time(u64 ts) {
  static const boost::posix_time::ptime EPOCH = boost::posix_time::from_time_t(0);
  boost::posix_time::ptime t = boost::posix_time::from_time_t(static_cast<std::time_t>(ts));
  boost::posix_time::time_duration duration = t - EPOCH;
  auto ns = static_cast<Timestamp>(duration.total_nanoseconds());
  return ns;
}

OpenTSDBResponse OpenTSDBProtocolParser::worker() {
  static OpenTSDBResponse result;
  const size_t buffer_len = LIMITS_MAX_SNAME + 3 + 17 + 26;  // 3 space delimiters + 17 for value + 26 for timestampm
  Byte buffer[buffer_len];
  while (true) {
    Sample sample;
    common::Status status = common::Status::Ok();
    int len = rdbuf_.read_line(buffer, buffer_len);
    if (len <= 0) {
      // Buffer don't have a full PDU
      return result;
    }
    auto msgtype = message_dispatch(buffer, len);
    switch (msgtype) {
      case OpenTSDBMessageType::PUT:
        {
          // Convert 'put cpu.real 20141210T074343 3.12 host=machine1 region=NW'
          // to 'cpu.real 20141210T074343 3.12 host=machine1 region=NW'
          Byte* pend = buffer + len;
          Byte* pbuf = buffer;
          pbuf += 4;  // skip 'put '
          len  -= 4;
          while (*pbuf == ' ' && len > 0) {
            pbuf++;
            len--;
          }  // Skip redundant space characters

          // Convert 'cpu.real 20141210T074343 3.12 host=machine1 region=NW'
          // to 'cpu.real host=machine1 region=NW 20141210T074343 3.12'
          // using std::rotate:
          //  std::rotate(a, b, pend)
          //  where a = '20141210T074343 3.12 host=machine1 region=NW'
          //    and b = 'host=machine1 region=NW'

          // Skip metric name
          Byte* a;
          int quota = len;
          int nspaces = 0;
          std::tie(a, quota, nspaces) = skip_element(pbuf, quota);
          if (a == pend) {
            std::string msg;
            size_t pos;
            std::tie(msg, pos) = rdbuf_.get_error_context("put: illegal argument: not enough arguments (need least 4, got 0)");
            throw ProtocolParserError(msg, pos);
          }
          int metric_size = len - quota;

          // Skip timestamp and value
          Byte* b = a;
          int timestamp_size = quota;
          int timestamp_trailing = 0;
          std::tie(b, quota, timestamp_trailing) = skip_element(b, quota);
          timestamp_size -= quota;
          if (b == pend) {
            std::string msg;
            size_t pos;
            std::tie(msg, pos) = rdbuf_.get_error_context("put: illegal argument: not enough arguments (need least 4, got 1)");
            throw ProtocolParserError(msg, pos);
          }
          int value_size = quota;
          int value_trailing = 0;
          std::tie(b, quota, value_trailing) = skip_element(b, quota);
          value_size -= quota;
          if (b == pend) {
            std::string msg;
            size_t pos;
            std::tie(msg, pos) = rdbuf_.get_error_context("put: illegal argument: not enough arguments (need least 4, got 2)");
            throw ProtocolParserError(msg, pos);
          }
          // This is the size of the timestamp and value combined
          int name_size = quota + metric_size;
          int tags_trailing = 0;
          Byte const* c = b + quota - 1;
          while (c > b) {
            if (*c != ' ' && *c != '\n') {
              break;
            }
            c--;
            tags_trailing++;
          }

          // Rotate
          std::rotate(a, b, pend);

          // Buffer contains only one data point
          status = consumer_->series_to_param_id(pbuf, static_cast<u32>(name_size - tags_trailing), &sample);  // -1 because name_size includes space
          if (status != common::Status::Ok()) {
            std::string msg;
            size_t pos;
            std::tie(msg, pos) = rdbuf_.get_error_context("put: invalid series name format");
            throw ProtocolParserError(msg, pos);
          }

          pbuf += name_size;
          // try to parse as Unix timestamp first
          {
            bool err = false;
            Byte* endptr;
            const int eix = timestamp_size - timestamp_trailing;
            pbuf[eix] = '\0';  // timestamp_trailing can't be 0 or less
            auto result = strtoull(pbuf, &endptr, 10);
            pbuf[eix] = ' ';
            if (result == 0) {
              err = true;
            }
            if (result < 0xFFFFFFFF) {
              // If the Unix timestamp was sent, it will be less than 0xFFFFFFFF.
              // In this case we need to adjust the value.
              // If the value is larger than 0xFFFFFFFF, then the nanosecond timestamp
              // was passed. We don't need to do anything.
              // With this schema first 4.5 seconds of the nanosecond timestamp will be
              // treated as normal Unix timestamps.
              result = from_unix_time(result);
            }
            sample.timestamp = result;
            if (err) {
              // This is an extension of the OpenTSDB telnet protocol. If value can't be
              // interpreted as a Unix timestamp or as a nanosecond timestamp,
              // should try to parse it as a ISO-timestamp (because why not?).
              status = Utility::parse_timestamp(pbuf, &sample);
              if (status == common::Status::Ok()) {
                err = false;
              }
            }
            if (err) {
              std::string msg;
              size_t pos;
              std::tie(msg, pos) = rdbuf_.get_error_context("put: invalid timestamp format");
              throw ProtocolParserError(msg, pos);
            }
          }
          pbuf += timestamp_size;

          const int tix = value_size - value_trailing;
          pbuf[tix] = '\0';  // Replace space with 0-terminator
          char* endptr = nullptr;
          pbuf[tix] = ' ';
          double value = strtod(pbuf, &endptr);
          if (endptr - pbuf != (value_size - value_trailing)) {
            std::string msg;
            size_t pos;
            std::tie(msg, pos) = rdbuf_.get_error_context("put: bad floating point value");
            throw ProtocolParserError(msg, pos);
          }

          sample.payload.float64 = value;
          sample.payload.type = PAYLOAD_FLOAT;

          // Put value
          status = consumer_->write(sample);
          if (status != common::Status::Ok()) {
            throw DatabaseError(status);
          }

          rdbuf_.consume();
          break;
        }
      case OpenTSDBMessageType::STATS: {
        // Fake response
        OpenTSDBResponse stats("stdb.rpcs 1479600574 0 type=fake\n" );
        return stats;
      }
      case OpenTSDBMessageType::VERSION: {
        OpenTSDBResponse ver("net.opentsdb.tools BuildData built at revision a000000\n"
                             "STDB to TSD converter/n");
        return ver;
      }
      case OpenTSDBMessageType::UNKNOWN: {
        std::string msg;
        size_t pos;
        std::tie(msg, pos) = rdbuf_.get_error_context("unknown command: nosuchcommand.  Try `help'.");
        throw ProtocolParserError(msg, pos);
      }
      default:
        // Just ignore the rest of the commands
        continue;
    };  // endswitch
  }
  return result;
}

std::string OpenTSDBProtocolParser::error_repr(int kind, std::string const& err) const {
  switch (kind) {
    case ERR:
      return "error: " + err + "\n";
    case DB:
      return "database: " + err + "\n";
  };
  return err + "\n";
}

}  // namespace protocol
}  // namespace stdb
