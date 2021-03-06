/**
 * \file protocolparser.h
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef STDB_PROTOCOL_PROTOCOLPARSER_H_
#define STDB_PROTOCOL_PROTOCOLPARSER_H_

#include <cstdint>
#include <memory>
#include <queue>
#include <vector>
#include <unordered_map>

#include "stdb/common/basic.h"
#include "stdb/common/status.h"
#include "stdb/protocol/resp.h"
#include "stdb/protocol/stream.h"

namespace stdb {
//! Fwd
struct DbSession;

namespace protocol {

/** Protocol Data Unit */
struct PDU {
  std::shared_ptr<const Byte> buffer;  //< Pointer to buffer (buffer can be referenced by several PDU)
  u32 size;    //< Size of the buffer
  u32 pos;     //< Position in the buffer
  u32 cons;    //< Consumed part
};

struct ProtocolParserError : StreamError {
  ProtocolParserError(std::string line, size_t pos);
};

struct DatabaseError : std::exception {
  common::Status status;
  std::string error_msg;

  DatabaseError(const common::Status& status);
  virtual const char* what() const noexcept;
};

/** Protocol parser response.
 */
struct ProtocolParserResponse {
  virtual ~ProtocolParserResponse() = default;
  virtual bool is_available() const = 0;
  virtual std::string get_body() const = 0;
};

/** ChunkedWriter used by servers to acquire buffers.
 * Server should follow the protocol:
 * - pull buffer
 * - fill buffer with data
 * - push buffer
 * - pull next buffer
 * - etc...
 */
struct ChunkedWriter {
  typedef Byte* BufferT;
  virtual BufferT pull() = 0;
  virtual void push(BufferT buffer, u32 size) = 0;
};


/** This class should be used in conjunction with tcp-server class.
 * It allocates buffers for server and makes them available to parser.
 */
class ReadBuffer : public ByteStreamReader, public ChunkedWriter {
  enum {
    // This parameter defines initial buffer size as a number of BUFFER_SIZE regions.
    // Increasing this parameter will increase memory requirements. Decreasing this parameter
    // will increase amount of copying.
    N_BUF = 4,
  };
  const size_t BUFFER_SIZE;
  std::vector<Byte> buffer_;
  mutable u32 rpos_;   // Current read position
  mutable u32 wpos_;   // Current write position
  mutable u32 cons_;   // Consumed part of the buffer
  int buffers_allocated_; // Buffer counter (only one allocated buffer is allowed)

 public:
  ReadBuffer(const size_t buffer_size);

  // ByteStreamReader interface
  virtual Byte get() override;
  virtual Byte pick() const override;
  virtual bool is_eof() override;
  virtual int read(Byte *buffer, size_t buffer_len) override;
  virtual int read_line(Byte* buffer, size_t quota) override;
  virtual void close() override;
  virtual std::tuple<std::string, size_t> get_error_context(const char *error_message) const override;
  virtual void consume();
  virtual void discard();

  // BufferAllocator interface
  /** Get pointer to buffer. Size of the buffer is guaranteed to be at least
   * BUFFER_SIZE bytes.
   */
  virtual BufferT pull() override;
  /** Return previously acquired buffer.
  */
  virtual void push(BufferT, u32 size) override;
};

struct NullResponse : ProtocolParserResponse {
  virtual bool is_available() const {
    return false;
  }
  virtual std::string get_body() const {
    return std::string();
  }
};

/**
 * @brief RESP protocol parser
 * Implements two complimentary protocols:
 * - data point protocol
 * - row protocol
 *
 * DATA POINT PROTOCOL is used to insert individual data points. First line of
 * each PDU should contain a RESP string. This string is interpreted as a series name.
 * The second line should contain a RESP string that contains ISO8601 formatted
 * timestamp (only basic ISO8601 format supported). Alternatively, if the second line
 * contains a RESP integer, it's interpreted as a timestamp (number of nanoseconds since
 * epoch). The timestamp should be followed by the value. Value can be encoded using
 * RESP string or RESP integer. If the value is encoded as a string, it's interpreted as
 * a floating point number. If the value is an integer, the value is used as is.
 * Example:
 *     +balancers.memusage host=machine1 region=NW
 *     +20141210T074343.999999999
 *     :31
 *
 * ROW PROTOCOL is used to insert logically corelated data points using single PDU.
 * 'Logically corelated' means one particular thing: all data points share the set of
 * tags and a timestamp. First line of the PDU should contain a RESP string. This string
 * is interpreted as a compound series name. The second line should contain a RESP string
 * that contains ISO8601 formatted timestamp (only basic ISO8601 format supported).
 * Alternatively, if the second line contains a RESP integer, it's interpreted as a timestamp
 * (number of nanoseconds since epoch). The timestamp should be followed by the array of values.
 * The array of value is encoded using RESP array.
 * Example:
 *     +cpu.real|cpu.user|cpu.sys host=machine1 region=NW
 *     +20141210T074343
 *     *3
 *     +3.12
 *     +8.11
 *     +12.6
 *
 * Protocol data units of each protocol can be interleaved.
 *
 * DICTIONARY can be used to minimize the protocol footprint. User supplied dictionary maps
 * actual series names to unique integer ids. These ids can be used instead of series names.
 * Dictionary can be provided only before other messages in the session. A user that wants
 * to use a dictionary should start transmission with the dictionary.
 * The dictionary format is simple. It's composed of arrays (zero or more). Each array should
 * contain a set of name-id pairs (so the total number of array elements should always be even).
 * User provided id should be represented as integer.
 * Example:
 *     *4
 *     +balancers.memusage host=machine1
 *     :1
 *     +balancers.memusage host=machine2
 *     :2
 *     :1
 *     +20141210T074343
 *     :31
 *     :2
 *     +20141210T074343
 *     +12.01
 *     :1
 *     +20141210T074344
 *     :32
 *     :2
 *     +20141210T074344
 *     +12.02
 *
 * Here user provides the dictionary that contains two elements: `balancers.memusage host=machine1`
 * and `balancers.memusage host=machine2`. In the following messages (starts on line 6) the ids of
 * the seires are used instead of the actual series names (:1 and :2). Note that the id should be an
 * integer.
 *
 * The same approach can be used with the RAW PROTOCOL variant:
 *     *2
 *     +cpu.real|cpu.user|cpu.sys host=machine1
 *     :1
 *     :1
 *     +20141210T074343
 *     *3
 *     +3.12
 *     +8.11
 *     +12.6
 *
 * Note that the dictionary element can be a compound series name. The actual message starts on line
 * #3.
 */
class RESPProtocolParser {
  typedef std::unordered_multimap<ParamId, ParamId> SeriesIdMap;
  bool                               done_;
  ReadBuffer                         rdbuf_;
  std::shared_ptr<DbSession>         consumer_;
  SeriesIdMap                        idmap_;
  u64                                paramids_[STDB_LIMITS_MAX_ROW_WIDTH];
  double                             values_  [STDB_LIMITS_MAX_ROW_WIDTH];
  std::string                        events_  [STDB_LIMITS_MAX_ROW_WIDTH];
  std::vector<char>                  event_inp_buf_;
  std::vector<char>                  event_out_buf_;

  //! Process frames from queue
  void worker();

  //! Generate error message
  std::tuple<std::string, size_t> get_error_from_pdu(PDU const& pdu) const;

  bool parse_dict(RESPStream& stream);
  bool parse_timestamp(RESPStream& stream, Sample& sample);
  bool parse_values(RESPStream& stream, const ParamId *ids, double* values, std::string *events, int nvalues);
  int parse_ids(RESPStream& stream, ParamId* ids, int nvalues);
  /**
   * @brief Cache series id mapping
   * @param uid is a user supplied id
   * @param row is an array of series ids
   * @param nvalues is a size of the 'row' array
   * @return 'true' on success or 'false' on clash
   */
  bool update_dict(ParamId uid, const ParamId* row, int nvalues);
  int read_dict(ParamId uid, ParamId* row, int nvalues);
 
 public:
  enum {
    RDBUF_SIZE = 0x1000,  // 4KB
  };
  RESPProtocolParser(std::shared_ptr<DbSession> consumer);
  void start();
  NullResponse parse_next(Byte *buffer, u32 sz);
  void close();
  Byte* get_next_buffer();

  // Error representation
  enum {
    DB,
    ERR,
    PARSE,
  };

  /**
   * @brief Return error representation in OpenTSDB telnet protocol
   */
  std::string error_repr(int kind, std::string const& err) const;
};

struct OpenTSDBResponse : ProtocolParserResponse {
  bool is_set_;
  const char* body_;

  OpenTSDBResponse()
      : is_set_(false)
        , body_(nullptr) { }

  OpenTSDBResponse(const char* body)
      : is_set_(true)
        , body_(body) { }

  virtual bool is_available() const {
    return is_set_;
  }
  virtual std::string get_body() const {
    return body_;
  }
};

/**
 * @brief OpenTSDBProtocolParser class
 *
 * Implements OpenTSDB protocol. In this protocol PDU delimiter is a new line character.
 * Each line contains exactly one command. This parser supports only 'put' command.
 *
 * Example:
 *     put cpu.real 20141210T074343 3.12 host=machine1 region=NW
 *     put cpu.user 20141210T074343 8.11 host=machine1 region=NW
 *     put cpu.sys 20141210T074343 12.6 host=machine1 region=NW
 */
class OpenTSDBProtocolParser {
  bool                               done_;
  ReadBuffer                         rdbuf_;
  std::shared_ptr<DbSession>         consumer_;

  OpenTSDBResponse worker();

 public:
  enum {
    RDBUF_SIZE = 0x1000,  // 4KB
  };

  OpenTSDBProtocolParser(std::shared_ptr<DbSession> consumer);

  void start();
  OpenTSDBResponse parse_next(Byte *buffer, u32 sz);
  void close();
  Byte* get_next_buffer();

  // Error representation
  enum {
    DB,
    ERR,
    PARSE,
  };

  /**
   * @brief Return error representation in OpenTSDB telnet protocol
   */
  std::string error_repr(int kind, std::string const& err) const;
};

}  // namespace protocol
}  // namespace stdb

#endif  // STDB_PROTOCOL_PROTOCOLPARSER_H_
