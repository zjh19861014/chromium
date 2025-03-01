// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "encoding.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <stack>

namespace inspector_protocol_encoding {
namespace cbor {
namespace {
// Indicates the number of bits the "initial byte" needs to be shifted to the
// right after applying |kMajorTypeMask| to produce the major type in the
// lowermost bits.
static constexpr uint8_t kMajorTypeBitShift = 5u;
// Mask selecting the low-order 5 bits of the "initial byte", which is where
// the additional information is encoded.
static constexpr uint8_t kAdditionalInformationMask = 0x1f;
// Mask selecting the high-order 3 bits of the "initial byte", which indicates
// the major type of the encoded value.
static constexpr uint8_t kMajorTypeMask = 0xe0;
// Indicates the integer is in the following byte.
static constexpr uint8_t kAdditionalInformation1Byte = 24u;
// Indicates the integer is in the next 2 bytes.
static constexpr uint8_t kAdditionalInformation2Bytes = 25u;
// Indicates the integer is in the next 4 bytes.
static constexpr uint8_t kAdditionalInformation4Bytes = 26u;
// Indicates the integer is in the next 8 bytes.
static constexpr uint8_t kAdditionalInformation8Bytes = 27u;

// Encodes the initial byte, consisting of the |type| in the first 3 bits
// followed by 5 bits of |additional_info|.
constexpr uint8_t EncodeInitialByte(MajorType type, uint8_t additional_info) {
  return (static_cast<uint8_t>(type) << kMajorTypeBitShift) |
         (additional_info & kAdditionalInformationMask);
}

// TAG 24 indicates that what follows is a byte string which is
// encoded in CBOR format. We use this as a wrapper for
// maps and arrays, allowing us to skip them, because the
// byte string carries its size (byte length).
// https://tools.ietf.org/html/rfc7049#section-2.4.4.1
static constexpr uint8_t kInitialByteForEnvelope =
    EncodeInitialByte(MajorType::TAG, 24);
// The initial byte for a byte string with at most 2^32 bytes
// of payload. This is used for envelope encoding, even if
// the byte string is shorter.
static constexpr uint8_t kInitialByteFor32BitLengthByteString =
    EncodeInitialByte(MajorType::BYTE_STRING, 26);

// See RFC 7049 Section 2.2.1, indefinite length arrays / maps have additional
// info = 31.
static constexpr uint8_t kInitialByteIndefiniteLengthArray =
    EncodeInitialByte(MajorType::ARRAY, 31);
static constexpr uint8_t kInitialByteIndefiniteLengthMap =
    EncodeInitialByte(MajorType::MAP, 31);
// See RFC 7049 Section 2.3, Table 1; this is used for finishing indefinite
// length maps / arrays.
static constexpr uint8_t kStopByte =
    EncodeInitialByte(MajorType::SIMPLE_VALUE, 31);

// See RFC 7049 Section 2.3, Table 2.
static constexpr uint8_t kEncodedTrue =
    EncodeInitialByte(MajorType::SIMPLE_VALUE, 21);
static constexpr uint8_t kEncodedFalse =
    EncodeInitialByte(MajorType::SIMPLE_VALUE, 20);
static constexpr uint8_t kEncodedNull =
    EncodeInitialByte(MajorType::SIMPLE_VALUE, 22);
static constexpr uint8_t kInitialByteForDouble =
    EncodeInitialByte(MajorType::SIMPLE_VALUE, 27);

// See RFC 7049 Table 3 and Section 2.4.4.2. This is used as a prefix for
// arbitrary binary data encoded as BYTE_STRING.
static constexpr uint8_t kExpectedConversionToBase64Tag =
    EncodeInitialByte(MajorType::TAG, 22);

// Writes the bytes for |v| to |out|, starting with the most significant byte.
// See also: https://commandcenter.blogspot.com/2012/04/byte-order-fallacy.html
template <typename T>
void WriteBytesMostSignificantByteFirst(T v, std::vector<uint8_t>* out) {
  for (int shift_bytes = sizeof(T) - 1; shift_bytes >= 0; --shift_bytes)
    out->push_back(0xff & (v >> (shift_bytes * 8)));
}

// Extracts sizeof(T) bytes from |in| to extract a value of type T
// (e.g. uint64_t, uint32_t, ...), most significant byte first.
// See also: https://commandcenter.blogspot.com/2012/04/byte-order-fallacy.html
template <typename T>
T ReadBytesMostSignificantByteFirst(span<uint8_t> in) {
  assert(static_cast<std::size_t>(in.size()) >= sizeof(T));
  T result = 0;
  for (std::size_t shift_bytes = 0; shift_bytes < sizeof(T); ++shift_bytes)
    result |= T(in[sizeof(T) - 1 - shift_bytes]) << (shift_bytes * 8);
  return result;
}
}  // namespace

namespace internals {
// Reads the start of a token with definitive size from |bytes|.
// |type| is the major type as specified in RFC 7049 Section 2.1.
// |value| is the payload (e.g. for MajorType::UNSIGNED) or is the size
// (e.g. for BYTE_STRING).
// If successful, returns the number of bytes read. Otherwise returns -1.
int8_t ReadTokenStart(span<uint8_t> bytes, MajorType* type, uint64_t* value) {
  if (bytes.empty())
    return -1;
  uint8_t initial_byte = bytes[0];
  *type = MajorType((initial_byte & kMajorTypeMask) >> kMajorTypeBitShift);

  uint8_t additional_information = initial_byte & kAdditionalInformationMask;
  if (additional_information < 24) {
    // Values 0-23 are encoded directly into the additional info of the
    // initial byte.
    *value = additional_information;
    return 1;
  }
  if (additional_information == kAdditionalInformation1Byte) {
    // Values 24-255 are encoded with one initial byte, followed by the value.
    if (bytes.size() < 2)
      return -1;
    *value = ReadBytesMostSignificantByteFirst<uint8_t>(bytes.subspan(1));
    return 2;
  }
  if (additional_information == kAdditionalInformation2Bytes) {
    // Values 256-65535: 1 initial byte + 2 bytes payload.
    if (static_cast<std::size_t>(bytes.size()) < 1 + sizeof(uint16_t))
      return -1;
    *value = ReadBytesMostSignificantByteFirst<uint16_t>(bytes.subspan(1));
    return 3;
  }
  if (additional_information == kAdditionalInformation4Bytes) {
    // 32 bit uint: 1 initial byte + 4 bytes payload.
    if (static_cast<std::size_t>(bytes.size()) < 1 + sizeof(uint32_t))
      return -1;
    *value = ReadBytesMostSignificantByteFirst<uint32_t>(bytes.subspan(1));
    return 5;
  }
  if (additional_information == kAdditionalInformation8Bytes) {
    // 64 bit uint: 1 initial byte + 8 bytes payload.
    if (static_cast<std::size_t>(bytes.size()) < 1 + sizeof(uint64_t))
      return -1;
    *value = ReadBytesMostSignificantByteFirst<uint64_t>(bytes.subspan(1));
    return 9;
  }
  return -1;
}

// Writes the start of a token with |type|. The |value| may indicate the size,
// or it may be the payload if the value is an unsigned integer.
void WriteTokenStart(MajorType type,
                     uint64_t value,
                     std::vector<uint8_t>* encoded) {
  if (value < 24) {
    // Values 0-23 are encoded directly into the additional info of the
    // initial byte.
    encoded->push_back(EncodeInitialByte(type, /*additional_info=*/value));
    return;
  }
  if (value <= std::numeric_limits<uint8_t>::max()) {
    // Values 24-255 are encoded with one initial byte, followed by the value.
    encoded->push_back(EncodeInitialByte(type, kAdditionalInformation1Byte));
    encoded->push_back(value);
    return;
  }
  if (value <= std::numeric_limits<uint16_t>::max()) {
    // Values 256-65535: 1 initial byte + 2 bytes payload.
    encoded->push_back(EncodeInitialByte(type, kAdditionalInformation2Bytes));
    WriteBytesMostSignificantByteFirst<uint16_t>(value, encoded);
    return;
  }
  if (value <= std::numeric_limits<uint32_t>::max()) {
    // 32 bit uint: 1 initial byte + 4 bytes payload.
    encoded->push_back(EncodeInitialByte(type, kAdditionalInformation4Bytes));
    WriteBytesMostSignificantByteFirst<uint32_t>(static_cast<uint32_t>(value),
                                                 encoded);
    return;
  }
  // 64 bit uint: 1 initial byte + 8 bytes payload.
  encoded->push_back(EncodeInitialByte(type, kAdditionalInformation8Bytes));
  WriteBytesMostSignificantByteFirst<uint64_t>(value, encoded);
}
}  // namespace internals

// =============================================================================
// Detecting CBOR content
// =============================================================================

uint8_t InitialByteForEnvelope() {
  return kInitialByteForEnvelope;
}
uint8_t InitialByteFor32BitLengthByteString() {
  return kInitialByteFor32BitLengthByteString;
}
bool IsCBORMessage(span<uint8_t> msg) {
  return msg.size() >= 6 && msg[0] == InitialByteForEnvelope() &&
         msg[1] == InitialByteFor32BitLengthByteString();
}

// =============================================================================
// Encoding invidiual CBOR items
// =============================================================================

uint8_t EncodeTrue() {
  return kEncodedTrue;
}
uint8_t EncodeFalse() {
  return kEncodedFalse;
}
uint8_t EncodeNull() {
  return kEncodedNull;
}

uint8_t EncodeIndefiniteLengthArrayStart() {
  return kInitialByteIndefiniteLengthArray;
}

uint8_t EncodeIndefiniteLengthMapStart() {
  return kInitialByteIndefiniteLengthMap;
}

uint8_t EncodeStop() {
  return kStopByte;
}

void EncodeInt32(int32_t value, std::vector<uint8_t>* out) {
  if (value >= 0) {
    internals::WriteTokenStart(MajorType::UNSIGNED, value, out);
  } else {
    uint64_t representation = static_cast<uint64_t>(-(value + 1));
    internals::WriteTokenStart(MajorType::NEGATIVE, representation, out);
  }
}

void EncodeString16(span<uint16_t> in, std::vector<uint8_t>* out) {
  uint64_t byte_length = static_cast<uint64_t>(in.size_bytes());
  internals::WriteTokenStart(MajorType::BYTE_STRING, byte_length, out);
  // When emitting UTF16 characters, we always write the least significant byte
  // first; this is because it's the native representation for X86.
  // TODO(johannes): Implement a more efficient thing here later, e.g.
  // casting *iff* the machine has this byte order.
  // The wire format for UTF16 chars will probably remain the same
  // (least significant byte first) since this way we can have
  // golden files, unittests, etc. that port easily and universally.
  // See also:
  // https://commandcenter.blogspot.com/2012/04/byte-order-fallacy.html
  for (const uint16_t two_bytes : in) {
    out->push_back(two_bytes);
    out->push_back(two_bytes >> 8);
  }
}

void EncodeString8(span<uint8_t> in, std::vector<uint8_t>* out) {
  internals::WriteTokenStart(MajorType::STRING,
                             static_cast<uint64_t>(in.size_bytes()), out);
  out->insert(out->end(), in.begin(), in.end());
}

void EncodeFromLatin1(span<uint8_t> latin1, std::vector<uint8_t>* out) {
  for (std::ptrdiff_t ii = 0; ii < latin1.size(); ++ii) {
    if (latin1[ii] <= 127)
      continue;
    // If there's at least one non-ASCII char, convert to UTF8.
    std::vector<uint8_t> utf8(latin1.begin(), latin1.begin() + ii);
    for (; ii < latin1.size(); ++ii) {
      if (latin1[ii] <= 127) {
        utf8.push_back(latin1[ii]);
      } else {
        // 0xC0 means it's a UTF8 sequence with 2 bytes.
        utf8.push_back((latin1[ii] >> 6) | 0xc0);
        utf8.push_back((latin1[ii] | 0x80) & 0xbf);
      }
    }
    EncodeString8(SpanFromVector(utf8), out);
    return;
  }
  EncodeString8(latin1, out);
}

void EncodeFromUTF16(span<uint16_t> utf16, std::vector<uint8_t>* out) {
  // If there's at least one non-ASCII char, encode as STRING16 (UTF16).
  for (uint16_t ch : utf16) {
    if (ch <= 127)
      continue;
    EncodeString16(utf16, out);
    return;
  }
  // It's all US-ASCII, strip out every second byte and encode as UTF8.
  internals::WriteTokenStart(MajorType::STRING,
                             static_cast<uint64_t>(utf16.size()), out);
  out->insert(out->end(), utf16.begin(), utf16.end());
}

void EncodeBinary(span<uint8_t> in, std::vector<uint8_t>* out) {
  out->push_back(kExpectedConversionToBase64Tag);
  uint64_t byte_length = static_cast<uint64_t>(in.size_bytes());
  internals::WriteTokenStart(MajorType::BYTE_STRING, byte_length, out);
  out->insert(out->end(), in.begin(), in.end());
}

// A double is encoded with a specific initial byte
// (kInitialByteForDouble) plus the 64 bits of payload for its value.
constexpr std::ptrdiff_t kEncodedDoubleSize = 1 + sizeof(uint64_t);

// An envelope is encoded with a specific initial byte
// (kInitialByteForEnvelope), plus the start byte for a BYTE_STRING with a 32
// bit wide length, plus a 32 bit length for that string.
constexpr std::ptrdiff_t kEncodedEnvelopeHeaderSize = 1 + 1 + sizeof(uint32_t);

void EncodeDouble(double value, std::vector<uint8_t>* out) {
  // The additional_info=27 indicates 64 bits for the double follow.
  // See RFC 7049 Section 2.3, Table 1.
  out->push_back(kInitialByteForDouble);
  union {
    double from_double;
    uint64_t to_uint64;
  } reinterpret;
  reinterpret.from_double = value;
  WriteBytesMostSignificantByteFirst<uint64_t>(reinterpret.to_uint64, out);
}

// =============================================================================
// cbor::EnvelopeEncoder - for wrapping submessages
// =============================================================================

void EnvelopeEncoder::EncodeStart(std::vector<uint8_t>* out) {
  assert(byte_size_pos_ == 0);
  out->push_back(kInitialByteForEnvelope);
  out->push_back(kInitialByteFor32BitLengthByteString);
  byte_size_pos_ = out->size();
  out->resize(out->size() + sizeof(uint32_t));
}

bool EnvelopeEncoder::EncodeStop(std::vector<uint8_t>* out) {
  assert(byte_size_pos_ != 0);
  // The byte size is the size of the payload, that is, all the
  // bytes that were written past the byte size position itself.
  uint64_t byte_size = out->size() - (byte_size_pos_ + sizeof(uint32_t));
  // We store exactly 4 bytes, so at most INT32MAX, with most significant
  // byte first.
  if (byte_size > std::numeric_limits<uint32_t>::max())
    return false;
  for (int shift_bytes = sizeof(uint32_t) - 1; shift_bytes >= 0;
       --shift_bytes) {
    (*out)[byte_size_pos_++] = 0xff & (byte_size >> (shift_bytes * 8));
  }
  return true;
}

// =============================================================================
// cbor::NewCBOREncoder - for encoding from a streaming parser
// =============================================================================

namespace {
class CBOREncoder : public StreamingParserHandler {
 public:
  CBOREncoder(std::vector<uint8_t>* out, Status* status)
      : out_(out), status_(status) {
    *status_ = Status();
  }

  void HandleMapBegin() override {
    envelopes_.emplace_back();
    envelopes_.back().EncodeStart(out_);
    out_->push_back(kInitialByteIndefiniteLengthMap);
  }

  void HandleMapEnd() override {
    out_->push_back(kStopByte);
    assert(!envelopes_.empty());
    envelopes_.back().EncodeStop(out_);
    envelopes_.pop_back();
  }

  void HandleArrayBegin() override {
    envelopes_.emplace_back();
    envelopes_.back().EncodeStart(out_);
    out_->push_back(kInitialByteIndefiniteLengthArray);
  }

  void HandleArrayEnd() override {
    out_->push_back(kStopByte);
    assert(!envelopes_.empty());
    envelopes_.back().EncodeStop(out_);
    envelopes_.pop_back();
  }

  void HandleString8(span<uint8_t> chars) override {
    EncodeString8(chars, out_);
  }

  void HandleString16(span<uint16_t> chars) override {
    EncodeFromUTF16(chars, out_);
  }

  void HandleBinary(span<uint8_t> bytes) override { EncodeBinary(bytes, out_); }

  void HandleDouble(double value) override { EncodeDouble(value, out_); }

  void HandleInt32(int32_t value) override { EncodeInt32(value, out_); }

  void HandleBool(bool value) override {
    // See RFC 7049 Section 2.3, Table 2.
    out_->push_back(value ? kEncodedTrue : kEncodedFalse);
  }

  void HandleNull() override {
    // See RFC 7049 Section 2.3, Table 2.
    out_->push_back(kEncodedNull);
  }

  void HandleError(Status error) override {
    assert(!error.ok());
    *status_ = error;
    out_->clear();
  }

 private:
  std::vector<uint8_t>* out_;
  std::vector<EnvelopeEncoder> envelopes_;
  Status* status_;
};
}  // namespace

std::unique_ptr<StreamingParserHandler> NewCBOREncoder(
    std::vector<uint8_t>* out,
    Status* status) {
  return std::unique_ptr<StreamingParserHandler>(new CBOREncoder(out, status));
}

// =============================================================================
// cbor::CBORTokenizer - for parsing individual CBOR items
// =============================================================================

CBORTokenizer::CBORTokenizer(span<uint8_t> bytes) : bytes_(bytes) {
  ReadNextToken(/*enter_envelope=*/false);
}
CBORTokenizer::~CBORTokenizer() {}

CBORTokenTag CBORTokenizer::TokenTag() const {
  return token_tag_;
}

void CBORTokenizer::Next() {
  if (token_tag_ == CBORTokenTag::ERROR_VALUE ||
      token_tag_ == CBORTokenTag::DONE)
    return;
  ReadNextToken(/*enter_envelope=*/false);
}

void CBORTokenizer::EnterEnvelope() {
  assert(token_tag_ == CBORTokenTag::ENVELOPE);
  ReadNextToken(/*enter_envelope=*/true);
}

Status CBORTokenizer::Status() const {
  return status_;
}

int32_t CBORTokenizer::GetInt32() const {
  assert(token_tag_ == CBORTokenTag::INT32);
  // The range checks happen in ::ReadNextToken().
  return static_cast<uint32_t>(
      token_start_type_ == MajorType::UNSIGNED
          ? token_start_internal_value_
          : -static_cast<int64_t>(token_start_internal_value_) - 1);
}

double CBORTokenizer::GetDouble() const {
  assert(token_tag_ == CBORTokenTag::DOUBLE);
  union {
    uint64_t from_uint64;
    double to_double;
  } reinterpret;
  reinterpret.from_uint64 = ReadBytesMostSignificantByteFirst<uint64_t>(
      bytes_.subspan(status_.pos + 1));
  return reinterpret.to_double;
}

span<uint8_t> CBORTokenizer::GetString8() const {
  assert(token_tag_ == CBORTokenTag::STRING8);
  auto length = static_cast<std::ptrdiff_t>(token_start_internal_value_);
  return bytes_.subspan(status_.pos + (token_byte_length_ - length), length);
}

span<uint8_t> CBORTokenizer::GetString16WireRep() const {
  assert(token_tag_ == CBORTokenTag::STRING16);
  auto length = static_cast<std::ptrdiff_t>(token_start_internal_value_);
  return bytes_.subspan(status_.pos + (token_byte_length_ - length), length);
}

span<uint8_t> CBORTokenizer::GetBinary() const {
  assert(token_tag_ == CBORTokenTag::BINARY);
  auto length = static_cast<std::ptrdiff_t>(token_start_internal_value_);
  return bytes_.subspan(status_.pos + (token_byte_length_ - length), length);
}

span<uint8_t> CBORTokenizer::GetEnvelopeContents() const {
  assert(token_tag_ == CBORTokenTag::ENVELOPE);
  auto length = static_cast<std::ptrdiff_t>(token_start_internal_value_);
  return bytes_.subspan(status_.pos + kEncodedEnvelopeHeaderSize, length);
}

void CBORTokenizer::ReadNextToken(bool enter_envelope) {
  if (enter_envelope) {
    status_.pos += kEncodedEnvelopeHeaderSize;
  } else {
    status_.pos =
        status_.pos == Status::npos() ? 0 : status_.pos + token_byte_length_;
  }
  status_.error = Error::OK;
  if (status_.pos >= bytes_.size()) {
    token_tag_ = CBORTokenTag::DONE;
    return;
  }
  switch (bytes_[status_.pos]) {
    case kStopByte:
      SetToken(CBORTokenTag::STOP, 1);
      return;
    case kInitialByteIndefiniteLengthMap:
      SetToken(CBORTokenTag::MAP_START, 1);
      return;
    case kInitialByteIndefiniteLengthArray:
      SetToken(CBORTokenTag::ARRAY_START, 1);
      return;
    case kEncodedTrue:
      SetToken(CBORTokenTag::TRUE_VALUE, 1);
      return;
    case kEncodedFalse:
      SetToken(CBORTokenTag::FALSE_VALUE, 1);
      return;
    case kEncodedNull:
      SetToken(CBORTokenTag::NULL_VALUE, 1);
      return;
    case kExpectedConversionToBase64Tag: {  // BINARY
      int8_t bytes_read = internals::ReadTokenStart(
          bytes_.subspan(status_.pos + 1), &token_start_type_,
          &token_start_internal_value_);
      int64_t token_byte_length = 1 + bytes_read + token_start_internal_value_;
      if (-1 == bytes_read || token_start_type_ != MajorType::BYTE_STRING ||
          status_.pos + token_byte_length > bytes_.size()) {
        SetError(Error::CBOR_INVALID_BINARY);
        return;
      }
      SetToken(CBORTokenTag::BINARY,
               static_cast<std::ptrdiff_t>(token_byte_length));
      return;
    }
    case kInitialByteForDouble: {  // DOUBLE
      if (status_.pos + kEncodedDoubleSize > bytes_.size()) {
        SetError(Error::CBOR_INVALID_DOUBLE);
        return;
      }
      SetToken(CBORTokenTag::DOUBLE, kEncodedDoubleSize);
      return;
    }
    case kInitialByteForEnvelope: {  // ENVELOPE
      if (status_.pos + kEncodedEnvelopeHeaderSize > bytes_.size()) {
        SetError(Error::CBOR_INVALID_ENVELOPE);
        return;
      }
      // The envelope must be a byte string with 32 bit length.
      if (bytes_[status_.pos + 1] != kInitialByteFor32BitLengthByteString) {
        SetError(Error::CBOR_INVALID_ENVELOPE);
        return;
      }
      // Read the length of the byte string.
      token_start_internal_value_ = ReadBytesMostSignificantByteFirst<uint32_t>(
          bytes_.subspan(status_.pos + 2));
      // Make sure the payload is contained within the message.
      if (token_start_internal_value_ + kEncodedEnvelopeHeaderSize +
              status_.pos >
          static_cast<std::size_t>(bytes_.size())) {
        SetError(Error::CBOR_INVALID_ENVELOPE);
        return;
      }
      auto length = static_cast<std::ptrdiff_t>(token_start_internal_value_);
      SetToken(CBORTokenTag::ENVELOPE, kEncodedEnvelopeHeaderSize + length);
      return;
    }
    default: {
      span<uint8_t> remainder =
          bytes_.subspan(status_.pos, bytes_.size() - status_.pos);
      assert(!remainder.empty());
      int8_t token_start_length = internals::ReadTokenStart(
          remainder, &token_start_type_, &token_start_internal_value_);
      bool success = token_start_length != -1;
      switch (token_start_type_) {
        case MajorType::UNSIGNED:  // INT32.
          if (!success || std::numeric_limits<int32_t>::max() <
                              token_start_internal_value_) {
            SetError(Error::CBOR_INVALID_INT32);
            return;
          }
          SetToken(CBORTokenTag::INT32, token_start_length);
          return;
        case MajorType::NEGATIVE:  // INT32.
          if (!success ||
              std::numeric_limits<int32_t>::min() >
                  -static_cast<int64_t>(token_start_internal_value_) - 1) {
            SetError(Error::CBOR_INVALID_INT32);
            return;
          }
          SetToken(CBORTokenTag::INT32, token_start_length);
          return;
        case MajorType::STRING: {  // STRING8.
          if (!success || remainder.size() < static_cast<int64_t>(
                                                 token_start_internal_value_)) {
            SetError(Error::CBOR_INVALID_STRING8);
            return;
          }
          auto length =
              static_cast<std::ptrdiff_t>(token_start_internal_value_);
          SetToken(CBORTokenTag::STRING8, token_start_length + length);
          return;
        }
        case MajorType::BYTE_STRING: {  // STRING16.
          if (!success ||
              remainder.size() <
                  static_cast<int64_t>(token_start_internal_value_) ||
              // Must be divisible by 2 since UTF16 is 2 bytes per character.
              token_start_internal_value_ & 1) {
            SetError(Error::CBOR_INVALID_STRING16);
            return;
          }
          auto length =
              static_cast<std::ptrdiff_t>(token_start_internal_value_);
          SetToken(CBORTokenTag::STRING16, token_start_length + length);
          return;
        }
        case MajorType::ARRAY:
        case MajorType::MAP:
        case MajorType::TAG:
        case MajorType::SIMPLE_VALUE:
          SetError(Error::CBOR_UNSUPPORTED_VALUE);
          return;
      }
    }
  }
}

void CBORTokenizer::SetToken(CBORTokenTag token_tag,
                             std::ptrdiff_t token_byte_length) {
  token_tag_ = token_tag;
  token_byte_length_ = token_byte_length;
}

void CBORTokenizer::SetError(Error error) {
  token_tag_ = CBORTokenTag::ERROR_VALUE;
  status_.error = error;
}

// =============================================================================
// cbor::ParseCBOR - for receiving streaming parser events for CBOR messages
// =============================================================================

namespace {
// When parsing CBOR, we limit recursion depth for objects and arrays
// to this constant.
static constexpr int kStackLimit = 300;

// Below are three parsing routines for CBOR, which cover enough
// to roundtrip JSON messages.
bool ParseMap(int32_t stack_depth,
              CBORTokenizer* tokenizer,
              StreamingParserHandler* out);
bool ParseArray(int32_t stack_depth,
                CBORTokenizer* tokenizer,
                StreamingParserHandler* out);
bool ParseValue(int32_t stack_depth,
                CBORTokenizer* tokenizer,
                StreamingParserHandler* out);

void ParseUTF16String(CBORTokenizer* tokenizer, StreamingParserHandler* out) {
  std::vector<uint16_t> value;
  span<uint8_t> rep = tokenizer->GetString16WireRep();
  for (std::ptrdiff_t ii = 0; ii < rep.size(); ii += 2)
    value.push_back((rep[ii + 1] << 8) | rep[ii]);
  out->HandleString16(span<uint16_t>(value.data(), value.size()));
  tokenizer->Next();
}

bool ParseUTF8String(CBORTokenizer* tokenizer, StreamingParserHandler* out) {
  assert(tokenizer->TokenTag() == CBORTokenTag::STRING8);
  out->HandleString8(tokenizer->GetString8());
  tokenizer->Next();
  return true;
}

bool ParseValue(int32_t stack_depth,
                CBORTokenizer* tokenizer,
                StreamingParserHandler* out) {
  if (stack_depth > kStackLimit) {
    out->HandleError(
        Status{Error::CBOR_STACK_LIMIT_EXCEEDED, tokenizer->Status().pos});
    return false;
  }
  // Skip past the envelope to get to what's inside.
  if (tokenizer->TokenTag() == CBORTokenTag::ENVELOPE)
    tokenizer->EnterEnvelope();
  switch (tokenizer->TokenTag()) {
    case CBORTokenTag::ERROR_VALUE:
      out->HandleError(tokenizer->Status());
      return false;
    case CBORTokenTag::DONE:
      out->HandleError(Status{Error::CBOR_UNEXPECTED_EOF_EXPECTED_VALUE,
                              tokenizer->Status().pos});
      return false;
    case CBORTokenTag::TRUE_VALUE:
      out->HandleBool(true);
      tokenizer->Next();
      return true;
    case CBORTokenTag::FALSE_VALUE:
      out->HandleBool(false);
      tokenizer->Next();
      return true;
    case CBORTokenTag::NULL_VALUE:
      out->HandleNull();
      tokenizer->Next();
      return true;
    case CBORTokenTag::INT32:
      out->HandleInt32(tokenizer->GetInt32());
      tokenizer->Next();
      return true;
    case CBORTokenTag::DOUBLE:
      out->HandleDouble(tokenizer->GetDouble());
      tokenizer->Next();
      return true;
    case CBORTokenTag::STRING8:
      return ParseUTF8String(tokenizer, out);
    case CBORTokenTag::STRING16:
      ParseUTF16String(tokenizer, out);
      return true;
    case CBORTokenTag::BINARY: {
      out->HandleBinary(tokenizer->GetBinary());
      tokenizer->Next();
      return true;
    }
    case CBORTokenTag::MAP_START:
      return ParseMap(stack_depth + 1, tokenizer, out);
    case CBORTokenTag::ARRAY_START:
      return ParseArray(stack_depth + 1, tokenizer, out);
    default:
      out->HandleError(
          Status{Error::CBOR_UNSUPPORTED_VALUE, tokenizer->Status().pos});
      return false;
  }
}

// |bytes| must start with the indefinite length array byte, so basically,
// ParseArray may only be called after an indefinite length array has been
// detected.
bool ParseArray(int32_t stack_depth,
                CBORTokenizer* tokenizer,
                StreamingParserHandler* out) {
  assert(tokenizer->TokenTag() == CBORTokenTag::ARRAY_START);
  tokenizer->Next();
  out->HandleArrayBegin();
  while (tokenizer->TokenTag() != CBORTokenTag::STOP) {
    if (tokenizer->TokenTag() == CBORTokenTag::DONE) {
      out->HandleError(
          Status{Error::CBOR_UNEXPECTED_EOF_IN_ARRAY, tokenizer->Status().pos});
      return false;
    }
    if (tokenizer->TokenTag() == CBORTokenTag::ERROR_VALUE) {
      out->HandleError(tokenizer->Status());
      return false;
    }
    // Parse value.
    if (!ParseValue(stack_depth, tokenizer, out))
      return false;
  }
  out->HandleArrayEnd();
  tokenizer->Next();
  return true;
}

// |bytes| must start with the indefinite length array byte, so basically,
// ParseArray may only be called after an indefinite length array has been
// detected.
bool ParseMap(int32_t stack_depth,
              CBORTokenizer* tokenizer,
              StreamingParserHandler* out) {
  assert(tokenizer->TokenTag() == CBORTokenTag::MAP_START);
  out->HandleMapBegin();
  tokenizer->Next();
  while (tokenizer->TokenTag() != CBORTokenTag::STOP) {
    if (tokenizer->TokenTag() == CBORTokenTag::DONE) {
      out->HandleError(
          Status{Error::CBOR_UNEXPECTED_EOF_IN_MAP, tokenizer->Status().pos});
      return false;
    }
    if (tokenizer->TokenTag() == CBORTokenTag::ERROR_VALUE) {
      out->HandleError(tokenizer->Status());
      return false;
    }
    // Parse key.
    if (tokenizer->TokenTag() == CBORTokenTag::STRING8) {
      if (!ParseUTF8String(tokenizer, out))
        return false;
    } else if (tokenizer->TokenTag() == CBORTokenTag::STRING16) {
      ParseUTF16String(tokenizer, out);
    } else {
      out->HandleError(
          Status{Error::CBOR_INVALID_MAP_KEY, tokenizer->Status().pos});
      return false;
    }
    // Parse value.
    if (!ParseValue(stack_depth, tokenizer, out))
      return false;
  }
  out->HandleMapEnd();
  tokenizer->Next();
  return true;
}
}  // namespace

void ParseCBOR(span<uint8_t> bytes, StreamingParserHandler* out) {
  if (bytes.empty()) {
    out->HandleError(Status{Error::CBOR_NO_INPUT, 0});
    return;
  }
  if (bytes[0] != kInitialByteForEnvelope) {
    out->HandleError(Status{Error::CBOR_INVALID_START_BYTE, 0});
    return;
  }
  CBORTokenizer tokenizer(bytes);
  if (tokenizer.TokenTag() == CBORTokenTag::ERROR_VALUE) {
    out->HandleError(tokenizer.Status());
    return;
  }
  // We checked for the envelope start byte above, so the tokenizer
  // must agree here, since it's not an error.
  assert(tokenizer.TokenTag() == CBORTokenTag::ENVELOPE);
  tokenizer.EnterEnvelope();
  if (tokenizer.TokenTag() != CBORTokenTag::MAP_START) {
    out->HandleError(
        Status{Error::CBOR_MAP_START_EXPECTED, tokenizer.Status().pos});
    return;
  }
  if (!ParseMap(/*stack_depth=*/1, &tokenizer, out))
    return;
  if (tokenizer.TokenTag() == CBORTokenTag::DONE)
    return;
  if (tokenizer.TokenTag() == CBORTokenTag::ERROR_VALUE) {
    out->HandleError(tokenizer.Status());
    return;
  }
  out->HandleError(Status{Error::CBOR_TRAILING_JUNK, tokenizer.Status().pos});
}
}  // namespace cbor

namespace json {

// =============================================================================
// json::NewJSONEncoder - for encoding streaming parser events as JSON
// =============================================================================

namespace {
// Prints |value| to |out| with 4 hex digits, most significant chunk first.
void PrintHex(uint16_t value, std::string* out) {
  for (int ii = 3; ii >= 0; --ii) {
    int four_bits = 0xf & (value >> (4 * ii));
    out->append(1, four_bits + ((four_bits <= 9) ? '0' : ('a' - 10)));
  }
}

// In the writer below, we maintain a stack of State instances.
// It is just enough to emit the appropriate delimiters and brackets
// in JSON.
enum class Container {
  // Used for the top-level, initial state.
  NONE,
  // Inside a JSON object.
  MAP,
  // Inside a JSON array.
  ARRAY
};
class State {
 public:
  explicit State(Container container) : container_(container) {}
  void StartElement(std::string* out) {
    assert(container_ != Container::NONE || size_ == 0);
    if (size_ != 0) {
      char delim = (!(size_ & 1) || container_ == Container::ARRAY) ? ',' : ':';
      out->append(1, delim);
    }
    ++size_;
  }
  Container container() const { return container_; }

 private:
  Container container_ = Container::NONE;
  int size_ = 0;
};

constexpr char kBase64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz0123456789+/";

void Base64Encode(const span<uint8_t>& in, std::string* out) {
  // The following three cases are based on the tables in the example
  // section in https://en.wikipedia.org/wiki/Base64. We process three
  // input bytes at a time, emitting 4 output bytes at a time.
  std::ptrdiff_t ii = 0;

  // While possible, process three input bytes.
  for (; ii + 3 <= in.size(); ii += 3) {
    uint32_t twentyfour_bits = (in[ii] << 16) | (in[ii + 1] << 8) | in[ii + 2];
    out->push_back(kBase64Table[(twentyfour_bits >> 18)]);
    out->push_back(kBase64Table[(twentyfour_bits >> 12) & 0x3f]);
    out->push_back(kBase64Table[(twentyfour_bits >> 6) & 0x3f]);
    out->push_back(kBase64Table[twentyfour_bits & 0x3f]);
  }
  if (ii + 2 <= in.size()) {  // Process two input bytes.
    uint32_t twentyfour_bits = (in[ii] << 16) | (in[ii + 1] << 8);
    out->push_back(kBase64Table[(twentyfour_bits >> 18)]);
    out->push_back(kBase64Table[(twentyfour_bits >> 12) & 0x3f]);
    out->push_back(kBase64Table[(twentyfour_bits >> 6) & 0x3f]);
    out->push_back('=');  // Emit padding.
    return;
  }
  if (ii + 1 <= in.size()) {  // Process a single input byte.
    uint32_t twentyfour_bits = (in[ii] << 16);
    out->push_back(kBase64Table[(twentyfour_bits >> 18)]);
    out->push_back(kBase64Table[(twentyfour_bits >> 12) & 0x3f]);
    out->push_back('=');  // Emit padding.
    out->push_back('=');  // Emit padding.
  }
}

// Implements a handler for JSON parser events to emit a JSON string.
class JSONEncoder : public StreamingParserHandler {
 public:
  JSONEncoder(const Platform* platform, std::string* out, Status* status)
      : platform_(platform), out_(out), status_(status) {
    *status_ = Status();
    state_.emplace(Container::NONE);
  }

  void HandleMapBegin() override {
    if (!status_->ok())
      return;
    assert(!state_.empty());
    state_.top().StartElement(out_);
    state_.emplace(Container::MAP);
    out_->append("{");
  }

  void HandleMapEnd() override {
    if (!status_->ok())
      return;
    assert(state_.size() >= 2 && state_.top().container() == Container::MAP);
    state_.pop();
    out_->append("}");
  }

  void HandleArrayBegin() override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    state_.emplace(Container::ARRAY);
    out_->append("[");
  }

  void HandleArrayEnd() override {
    if (!status_->ok())
      return;
    assert(state_.size() >= 2 && state_.top().container() == Container::ARRAY);
    state_.pop();
    out_->append("]");
  }

  void HandleString16(span<uint16_t> chars) override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    out_->append("\"");
    for (const uint16_t ch : chars) {
      if (ch == '"') {
        out_->append("\\\"");
      } else if (ch == '\\') {
        out_->append("\\\\");
      } else if (ch == '\b') {
        out_->append("\\b");
      } else if (ch == '\f') {
        out_->append("\\f");
      } else if (ch == '\n') {
        out_->append("\\n");
      } else if (ch == '\r') {
        out_->append("\\r");
      } else if (ch == '\t') {
        out_->append("\\t");
      } else if (ch >= 32 && ch <= 126) {
        out_->append(1, ch);
      } else {
        out_->append("\\u");
        PrintHex(ch, out_);
      }
    }
    out_->append("\"");
  }

  void HandleString8(span<uint8_t> chars) override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    out_->append("\"");
    for (std::ptrdiff_t ii = 0; ii < chars.size(); ++ii) {
      uint8_t c = chars[ii];
      if (c == '"') {
        out_->append("\\\"");
      } else if (c == '\\') {
        out_->append("\\\\");
      } else if (c == '\b') {
        out_->append("\\b");
      } else if (c == '\f') {
        out_->append("\\f");
      } else if (c == '\n') {
        out_->append("\\n");
      } else if (c == '\r') {
        out_->append("\\r");
      } else if (c == '\t') {
        out_->append("\\t");
      } else if (c >= 32 && c <= 126) {
        out_->append(1, c);
      } else if (c < 32) {
        out_->append("\\u");
        PrintHex(static_cast<uint16_t>(c), out_);
      } else {
        // Inspect the leading byte to figure out how long the utf8
        // byte sequence is; while doing this initialize |codepoint|
        // with the first few bits.
        // See table in: https://en.wikipedia.org/wiki/UTF-8
        // byte one is 110x xxxx -> 2 byte utf8 sequence
        // byte one is 1110 xxxx -> 3 byte utf8 sequence
        // byte one is 1111 0xxx -> 4 byte utf8 sequence
        uint32_t codepoint;
        int num_bytes_left;
        if ((c & 0xe0) == 0xc0) {  // 2 byte utf8 sequence
          num_bytes_left = 1;
          codepoint = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {  // 3 byte utf8 sequence
          num_bytes_left = 2;
          codepoint = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {  // 4 byte utf8 sequence
          codepoint = c & 0x07;
          num_bytes_left = 3;
        } else {
          continue;  // invalid leading byte
        }

        // If we have enough bytes in our input, decode the remaining ones
        // belonging to this Unicode character into |codepoint|.
        if (ii + num_bytes_left > chars.size())
          continue;
        while (num_bytes_left > 0) {
          c = chars[++ii];
          --num_bytes_left;
          // Check the next byte is a continuation byte, that is 10xx xxxx.
          if ((c & 0xc0) != 0x80)
            continue;
          codepoint = (codepoint << 6) | (c & 0x3f);
        }

        // Disallow overlong encodings for ascii characters, as these
        // would include " and other characters significant to JSON
        // string termination / control.
        if (codepoint < 0x7f)
          continue;
        // Invalid in UTF8, and can't be represented in UTF16 anyway.
        if (codepoint > 0x10ffff)
          continue;

        // So, now we transcode to UTF16,
        // using the math described at https://en.wikipedia.org/wiki/UTF-16,
        // for either one or two 16 bit characters.
        if (codepoint < 0xffff) {
          out_->append("\\u");
          PrintHex(static_cast<uint16_t>(codepoint), out_);
          continue;
        }
        codepoint -= 0x10000;
        // high surrogate
        out_->append("\\u");
        PrintHex(static_cast<uint16_t>((codepoint >> 10) + 0xd800), out_);
        // low surrogate
        out_->append("\\u");
        PrintHex(static_cast<uint16_t>((codepoint & 0x3ff) + 0xdc00), out_);
      }
    }
    out_->append("\"");
  }

  void HandleBinary(span<uint8_t> bytes) override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    out_->append("\"");
    Base64Encode(bytes, out_);
    out_->append("\"");
  }

  void HandleDouble(double value) override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    // JSON cannot represent NaN or Infinity. So, for compatibility,
    // we behave like the JSON object in web browsers: emit 'null'.
    if (!std::isfinite(value)) {
      out_->append("null");
      return;
    }
    std::unique_ptr<char[]> str_value = platform_->DToStr(value);

    // DToStr may fail to emit a 0 before the decimal dot. E.g. this is
    // the case in base::NumberToString in Chromium (which is based on
    // dmg_fp). So, much like
    // https://cs.chromium.org/chromium/src/base/json/json_writer.cc
    // we probe for this and emit the leading 0 anyway if necessary.
    const char* chars = str_value.get();
    if (chars[0] == '.') {
      out_->append("0");
    } else if (chars[0] == '-' && chars[1] == '.') {
      out_->append("-0");
      ++chars;
    }
    out_->append(chars);
  }

  void HandleInt32(int32_t value) override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    out_->append(std::to_string(value));
  }

  void HandleBool(bool value) override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    out_->append(value ? "true" : "false");
  }

  void HandleNull() override {
    if (!status_->ok())
      return;
    state_.top().StartElement(out_);
    out_->append("null");
  }

  void HandleError(Status error) override {
    assert(!error.ok());
    *status_ = error;
    out_->clear();
  }

 private:
  const Platform* platform_;
  std::string* out_;
  Status* status_;
  std::stack<State> state_;
};
}  // namespace

std::unique_ptr<StreamingParserHandler> NewJSONEncoder(const Platform* platform,
                                                       std::string* out,
                                                       Status* status) {
  return std::unique_ptr<StreamingParserHandler>(
      new JSONEncoder(platform, out, status));
}

// =============================================================================
// json::ParseJSON - for receiving streaming parser events for JSON.
// =============================================================================

namespace {
const int kStackLimit = 300;

enum Token {
  ObjectBegin,
  ObjectEnd,
  ArrayBegin,
  ArrayEnd,
  StringLiteral,
  Number,
  BoolTrue,
  BoolFalse,
  NullToken,
  ListSeparator,
  ObjectPairSeparator,
  InvalidToken,
  NoInput
};

const char* const kNullString = "null";
const char* const kTrueString = "true";
const char* const kFalseString = "false";

template <typename Char>
class JsonParser {
 public:
  JsonParser(const Platform* platform, StreamingParserHandler* handler)
      : platform_(platform), handler_(handler) {}

  void Parse(const Char* start, std::size_t length) {
    start_pos_ = start;
    const Char* end = start + length;
    const Char* tokenEnd;
    ParseValue(start, end, &tokenEnd, 0);
    if (tokenEnd != end) {
      HandleError(Error::JSON_PARSER_UNPROCESSED_INPUT_REMAINS, tokenEnd);
    }
  }

 private:
  bool CharsToDouble(const uint16_t* chars,
                     std::size_t length,
                     double* result) {
    std::string buffer;
    buffer.reserve(length + 1);
    for (std::size_t ii = 0; ii < length; ++ii) {
      bool is_ascii = !(chars[ii] & ~0x7F);
      if (!is_ascii)
        return false;
      buffer.push_back(static_cast<char>(chars[ii]));
    }
    return platform_->StrToD(buffer.c_str(), result);
  }

  bool CharsToDouble(const uint8_t* chars, std::size_t length, double* result) {
    std::string buffer(reinterpret_cast<const char*>(chars), length);
    return platform_->StrToD(buffer.c_str(), result);
  }

  static bool ParseConstToken(const Char* start,
                              const Char* end,
                              const Char** token_end,
                              const char* token) {
    // |token| is \0 terminated, it's one of the constants at top of the file.
    while (start < end && *token != '\0' && *start++ == *token++) {
    }
    if (*token != '\0')
      return false;
    *token_end = start;
    return true;
  }

  static bool ReadInt(const Char* start,
                      const Char* end,
                      const Char** token_end,
                      bool allow_leading_zeros) {
    if (start == end)
      return false;
    bool has_leading_zero = '0' == *start;
    int length = 0;
    while (start < end && '0' <= *start && *start <= '9') {
      ++start;
      ++length;
    }
    if (!length)
      return false;
    if (!allow_leading_zeros && length > 1 && has_leading_zero)
      return false;
    *token_end = start;
    return true;
  }

  static bool ParseNumberToken(const Char* start,
                               const Char* end,
                               const Char** token_end) {
    // We just grab the number here. We validate the size in DecodeNumber.
    // According to RFC4627, a valid number is: [minus] int [frac] [exp]
    if (start == end)
      return false;
    Char c = *start;
    if ('-' == c)
      ++start;

    if (!ReadInt(start, end, &start, /*allow_leading_zeros=*/false))
      return false;
    if (start == end) {
      *token_end = start;
      return true;
    }

    // Optional fraction part
    c = *start;
    if ('.' == c) {
      ++start;
      if (!ReadInt(start, end, &start, /*allow_leading_zeros=*/true))
        return false;
      if (start == end) {
        *token_end = start;
        return true;
      }
      c = *start;
    }

    // Optional exponent part
    if ('e' == c || 'E' == c) {
      ++start;
      if (start == end)
        return false;
      c = *start;
      if ('-' == c || '+' == c) {
        ++start;
        if (start == end)
          return false;
      }
      if (!ReadInt(start, end, &start, /*allow_leading_zeros=*/true))
        return false;
    }

    *token_end = start;
    return true;
  }

  static bool ReadHexDigits(const Char* start,
                            const Char* end,
                            const Char** token_end,
                            int digits) {
    if (end - start < digits)
      return false;
    for (int i = 0; i < digits; ++i) {
      Char c = *start++;
      if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'f') ||
            ('A' <= c && c <= 'F')))
        return false;
    }
    *token_end = start;
    return true;
  }

  static bool ParseStringToken(const Char* start,
                               const Char* end,
                               const Char** token_end) {
    while (start < end) {
      Char c = *start++;
      if ('\\' == c) {
        if (start == end)
          return false;
        c = *start++;
        // Make sure the escaped char is valid.
        switch (c) {
          case 'x':
            if (!ReadHexDigits(start, end, &start, 2))
              return false;
            break;
          case 'u':
            if (!ReadHexDigits(start, end, &start, 4))
              return false;
            break;
          case '\\':
          case '/':
          case 'b':
          case 'f':
          case 'n':
          case 'r':
          case 't':
          case 'v':
          case '"':
            break;
          default:
            return false;
        }
      } else if ('"' == c) {
        *token_end = start;
        return true;
      }
    }
    return false;
  }

  static bool SkipComment(const Char* start,
                          const Char* end,
                          const Char** comment_end) {
    if (start == end)
      return false;

    if (*start != '/' || start + 1 >= end)
      return false;
    ++start;

    if (*start == '/') {
      // Single line comment, read to newline.
      for (++start; start < end; ++start) {
        if (*start == '\n' || *start == '\r') {
          *comment_end = start + 1;
          return true;
        }
      }
      *comment_end = end;
      // Comment reaches end-of-input, which is fine.
      return true;
    }

    if (*start == '*') {
      Char previous = '\0';
      // Block comment, read until end marker.
      for (++start; start < end; previous = *start++) {
        if (previous == '*' && *start == '/') {
          *comment_end = start + 1;
          return true;
        }
      }
      // Block comment must close before end-of-input.
      return false;
    }

    return false;
  }

  static bool IsSpaceOrNewLine(Char c) {
    // \v = vertial tab; \f = form feed page break.
    return c == ' ' || c == '\n' || c == '\v' || c == '\f' || c == '\r' ||
           c == '\t';
  }

  static void SkipWhitespaceAndComments(const Char* start,
                                        const Char* end,
                                        const Char** whitespace_end) {
    while (start < end) {
      if (IsSpaceOrNewLine(*start)) {
        ++start;
      } else if (*start == '/') {
        const Char* comment_end;
        if (!SkipComment(start, end, &comment_end))
          break;
        start = comment_end;
      } else {
        break;
      }
    }
    *whitespace_end = start;
  }

  static Token ParseToken(const Char* start,
                          const Char* end,
                          const Char** tokenStart,
                          const Char** token_end) {
    SkipWhitespaceAndComments(start, end, tokenStart);
    start = *tokenStart;

    if (start == end)
      return NoInput;

    switch (*start) {
      case 'n':
        if (ParseConstToken(start, end, token_end, kNullString))
          return NullToken;
        break;
      case 't':
        if (ParseConstToken(start, end, token_end, kTrueString))
          return BoolTrue;
        break;
      case 'f':
        if (ParseConstToken(start, end, token_end, kFalseString))
          return BoolFalse;
        break;
      case '[':
        *token_end = start + 1;
        return ArrayBegin;
      case ']':
        *token_end = start + 1;
        return ArrayEnd;
      case ',':
        *token_end = start + 1;
        return ListSeparator;
      case '{':
        *token_end = start + 1;
        return ObjectBegin;
      case '}':
        *token_end = start + 1;
        return ObjectEnd;
      case ':':
        *token_end = start + 1;
        return ObjectPairSeparator;
      case '0':
      case '1':
      case '2':
      case '3':
      case '4':
      case '5':
      case '6':
      case '7':
      case '8':
      case '9':
      case '-':
        if (ParseNumberToken(start, end, token_end))
          return Number;
        break;
      case '"':
        if (ParseStringToken(start + 1, end, token_end))
          return StringLiteral;
        break;
    }
    return InvalidToken;
  }

  static int HexToInt(Char c) {
    if ('0' <= c && c <= '9')
      return c - '0';
    if ('A' <= c && c <= 'F')
      return c - 'A' + 10;
    if ('a' <= c && c <= 'f')
      return c - 'a' + 10;
    assert(false);  // Unreachable.
    return 0;
  }

  static bool DecodeString(const Char* start,
                           const Char* end,
                           std::vector<uint16_t>* output) {
    if (start == end)
      return true;
    if (start > end)
      return false;
    output->reserve(end - start);
    while (start < end) {
      uint16_t c = *start++;
      // If the |Char| we're dealing with is really a byte, then
      // we have utf8 here, and we need to check for multibyte characters
      // and transcode them to utf16 (either one or two utf16 chars).
      if (sizeof(Char) == sizeof(uint8_t) && c >= 0x7f) {
        // Inspect the leading byte to figure out how long the utf8
        // byte sequence is; while doing this initialize |codepoint|
        // with the first few bits.
        // See table in: https://en.wikipedia.org/wiki/UTF-8
        // byte one is 110x xxxx -> 2 byte utf8 sequence
        // byte one is 1110 xxxx -> 3 byte utf8 sequence
        // byte one is 1111 0xxx -> 4 byte utf8 sequence
        uint32_t codepoint;
        int num_bytes_left;
        if ((c & 0xe0) == 0xc0) {  // 2 byte utf8 sequence
          num_bytes_left = 1;
          codepoint = c & 0x1f;
        } else if ((c & 0xf0) == 0xe0) {  // 3 byte utf8 sequence
          num_bytes_left = 2;
          codepoint = c & 0x0f;
        } else if ((c & 0xf8) == 0xf0) {  // 4 byte utf8 sequence
          codepoint = c & 0x07;
          num_bytes_left = 3;
        } else {
          return false;  // invalid leading byte
        }

        // If we have enough bytes in our inpput, decode the remaining ones
        // belonging to this Unicode character into |codepoint|.
        if (start + num_bytes_left > end)
          return false;
        while (num_bytes_left > 0) {
          c = *start++;
          --num_bytes_left;
          // Check the next byte is a continuation byte, that is 10xx xxxx.
          if ((c & 0xc0) != 0x80)
            return false;
          codepoint = (codepoint << 6) | (c & 0x3f);
        }

        // Disallow overlong encodings for ascii characters, as these
        // would include " and other characters significant to JSON
        // string termination / control.
        if (codepoint < 0x7f)
          return false;
        // Invalid in UTF8, and can't be represented in UTF16 anyway.
        if (codepoint > 0x10ffff)
          return false;

        // So, now we transcode to UTF16,
        // using the math described at https://en.wikipedia.org/wiki/UTF-16,
        // for either one or two 16 bit characters.
        if (codepoint < 0xffff) {
          output->push_back(codepoint);
          continue;
        }
        codepoint -= 0x10000;
        output->push_back((codepoint >> 10) + 0xd800);    // high surrogate
        output->push_back((codepoint & 0x3ff) + 0xdc00);  // low surrogate
        continue;
      }
      if ('\\' != c) {
        output->push_back(c);
        continue;
      }
      if (start == end)
        return false;
      c = *start++;

      if (c == 'x') {
        // \x is not supported.
        return false;
      }

      switch (c) {
        case '"':
        case '/':
        case '\\':
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case 'v':
          c = '\v';
          break;
        case 'u':
          c = (HexToInt(*start) << 12) + (HexToInt(*(start + 1)) << 8) +
              (HexToInt(*(start + 2)) << 4) + HexToInt(*(start + 3));
          start += 4;
          break;
        default:
          return false;
      }
      output->push_back(c);
    }
    return true;
  }

  void ParseValue(const Char* start,
                  const Char* end,
                  const Char** value_token_end,
                  int depth) {
    if (depth > kStackLimit) {
      HandleError(Error::JSON_PARSER_STACK_LIMIT_EXCEEDED, start);
      return;
    }
    const Char* token_start;
    const Char* token_end;
    Token token = ParseToken(start, end, &token_start, &token_end);
    switch (token) {
      case NoInput:
        HandleError(Error::JSON_PARSER_NO_INPUT, token_start);
        return;
      case InvalidToken:
        HandleError(Error::JSON_PARSER_INVALID_TOKEN, token_start);
        return;
      case NullToken:
        handler_->HandleNull();
        break;
      case BoolTrue:
        handler_->HandleBool(true);
        break;
      case BoolFalse:
        handler_->HandleBool(false);
        break;
      case Number: {
        double value;
        if (!CharsToDouble(token_start, token_end - token_start, &value)) {
          HandleError(Error::JSON_PARSER_INVALID_NUMBER, token_start);
          return;
        }
        if (value >= std::numeric_limits<int32_t>::min() &&
            value <= std::numeric_limits<int32_t>::max() &&
            static_cast<int32_t>(value) == value)
          handler_->HandleInt32(static_cast<int32_t>(value));
        else
          handler_->HandleDouble(value);
        break;
      }
      case StringLiteral: {
        std::vector<uint16_t> value;
        bool ok = DecodeString(token_start + 1, token_end - 1, &value);
        if (!ok) {
          HandleError(Error::JSON_PARSER_INVALID_STRING, token_start);
          return;
        }
        handler_->HandleString16(span<uint16_t>(value.data(), value.size()));
        break;
      }
      case ArrayBegin: {
        handler_->HandleArrayBegin();
        start = token_end;
        token = ParseToken(start, end, &token_start, &token_end);
        while (token != ArrayEnd) {
          ParseValue(start, end, &token_end, depth + 1);
          if (error_)
            return;

          // After a list value, we expect a comma or the end of the list.
          start = token_end;
          token = ParseToken(start, end, &token_start, &token_end);
          if (token == ListSeparator) {
            start = token_end;
            token = ParseToken(start, end, &token_start, &token_end);
            if (token == ArrayEnd) {
              HandleError(Error::JSON_PARSER_UNEXPECTED_ARRAY_END, token_start);
              return;
            }
          } else if (token != ArrayEnd) {
            // Unexpected value after list value. Bail out.
            HandleError(Error::JSON_PARSER_COMMA_OR_ARRAY_END_EXPECTED,
                        token_start);
            return;
          }
        }
        handler_->HandleArrayEnd();
        break;
      }
      case ObjectBegin: {
        handler_->HandleMapBegin();
        start = token_end;
        token = ParseToken(start, end, &token_start, &token_end);
        while (token != ObjectEnd) {
          if (token != StringLiteral) {
            HandleError(Error::JSON_PARSER_STRING_LITERAL_EXPECTED,
                        token_start);
            return;
          }
          std::vector<uint16_t> key;
          if (!DecodeString(token_start + 1, token_end - 1, &key)) {
            HandleError(Error::JSON_PARSER_INVALID_STRING, token_start);
            return;
          }
          handler_->HandleString16(span<uint16_t>(key.data(), key.size()));
          start = token_end;

          token = ParseToken(start, end, &token_start, &token_end);
          if (token != ObjectPairSeparator) {
            HandleError(Error::JSON_PARSER_COLON_EXPECTED, token_start);
            return;
          }
          start = token_end;

          ParseValue(start, end, &token_end, depth + 1);
          if (error_)
            return;
          start = token_end;

          // After a key/value pair, we expect a comma or the end of the
          // object.
          token = ParseToken(start, end, &token_start, &token_end);
          if (token == ListSeparator) {
            start = token_end;
            token = ParseToken(start, end, &token_start, &token_end);
            if (token == ObjectEnd) {
              HandleError(Error::JSON_PARSER_UNEXPECTED_MAP_END, token_start);
              return;
            }
          } else if (token != ObjectEnd) {
            // Unexpected value after last object value. Bail out.
            HandleError(Error::JSON_PARSER_COMMA_OR_MAP_END_EXPECTED,
                        token_start);
            return;
          }
        }
        handler_->HandleMapEnd();
        break;
      }

      default:
        // We got a token that's not a value.
        HandleError(Error::JSON_PARSER_VALUE_EXPECTED, token_start);
        return;
    }

    SkipWhitespaceAndComments(token_end, end, value_token_end);
  }

  void HandleError(Error error, const Char* pos) {
    assert(error != Error::OK);
    if (!error_) {
      handler_->HandleError(Status{error, pos - start_pos_});
      error_ = true;
    }
  }

  const Char* start_pos_ = nullptr;
  bool error_ = false;
  const Platform* platform_;
  StreamingParserHandler* handler_;
};
}  // namespace

void ParseJSON(const Platform* platform,
               span<uint8_t> chars,
               StreamingParserHandler* handler) {
  JsonParser<uint8_t> parser(platform, handler);
  parser.Parse(chars.data(), chars.size());
}

void ParseJSON(const Platform* platform,
               span<uint16_t> chars,
               StreamingParserHandler* handler) {
  JsonParser<uint16_t> parser(platform, handler);
  parser.Parse(chars.data(), chars.size());
}
}  // namespace json
}  // namespace inspector_protocol_encoding
