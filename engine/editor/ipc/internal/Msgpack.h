// SPDX-License-Identifier: MIT
// Psynder editor IPC — minimal msgpack codec.
//
// Lane-private (under engine/editor/ipc/internal/). Implements only the
// subset we need to ship the protocol defined in protocol.psy: nil, bool,
// signed/unsigned ints (the smallest variant that fits is chosen on encode),
// f32, f64, str, bin, array, map. No ext types, no timestamps.
//
// Encoder API: Writer — append-only growing buffer.
// Decoder API: Reader — non-throwing, returns bool; all functions are
//   noexcept-correct for use in the engine's tools surface.

#pragma once

#include "core/Types.h"

#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psynder::editor::ipc::msgpack {

// ─── Writer ─────────────────────────────────────────────────────────────────
class Writer {
public:
    Writer() = default;

    std::vector<::psynder::u8>& buffer() noexcept { return buf_; }
    const std::vector<::psynder::u8>& buffer() const noexcept { return buf_; }
    ::psynder::usize size() const noexcept { return buf_.size(); }
    const ::psynder::u8* data() const noexcept { return buf_.data(); }

    void clear() noexcept { buf_.clear(); }
    void reserve(::psynder::usize n) { buf_.reserve(n); }

    void nil() { put1(0xC0); }
    void boolean(bool v) { put1(v ? 0xC3 : 0xC2); }

    // ── Signed / unsigned integers, smallest encoding selected ─────────────
    void u8_(::psynder::u8 v)   { if (v <= 0x7F) put1(v); else { put1(0xCC); put1(v); } }
    void u16_(::psynder::u16 v) { if (v <= 0xFF) u8_(static_cast<::psynder::u8>(v)); else { put1(0xCD); put_be16(v); } }
    void u32_(::psynder::u32 v) { if (v <= 0xFFFFu) u16_(static_cast<::psynder::u16>(v)); else { put1(0xCE); put_be32(v); } }
    void u64_(::psynder::u64 v) { if (v <= 0xFFFFFFFFull) u32_(static_cast<::psynder::u32>(v)); else { put1(0xCF); put_be64(v); } }

    void i8_(::psynder::i8 v) {
        if (v >= 0) {
            u8_(static_cast<::psynder::u8>(v));
        } else if (v >= -32) {
            put1(static_cast<::psynder::u8>(v));  // negative fixint 0xE0..0xFF
        } else {
            put1(0xD0); put1(static_cast<::psynder::u8>(v));
        }
    }
    void i16_(::psynder::i16 v) {
        if (v >= 0) {
            u16_(static_cast<::psynder::u16>(v));
        } else if (v >= -128) {
            i8_(static_cast<::psynder::i8>(v));
        } else {
            put1(0xD1); put_be16(static_cast<::psynder::u16>(v));
        }
    }
    void i32_(::psynder::i32 v) {
        if (v >= 0) {
            u32_(static_cast<::psynder::u32>(v));
        } else if (v >= -0x8000) {
            i16_(static_cast<::psynder::i16>(v));
        } else {
            put1(0xD2); put_be32(static_cast<::psynder::u32>(v));
        }
    }
    void i64_(::psynder::i64 v) {
        if (v >= 0) {
            u64_(static_cast<::psynder::u64>(v));
        } else if (v >= -0x80000000ll) {
            i32_(static_cast<::psynder::i32>(v));
        } else {
            put1(0xD3); put_be64(static_cast<::psynder::u64>(v));
        }
    }

    void f32_(::psynder::f32 v) {
        put1(0xCA);
        ::psynder::u32 bits = 0;
        std::memcpy(&bits, &v, 4);
        put_be32(bits);
    }
    void f64_(::psynder::f64 v) {
        put1(0xCB);
        ::psynder::u64 bits = 0;
        std::memcpy(&bits, &v, 8);
        put_be64(bits);
    }

    void str(std::string_view s) {
        const ::psynder::usize n = s.size();
        if (n <= 31) {
            put1(static_cast<::psynder::u8>(0xA0 | n));
        } else if (n <= 0xFF) {
            put1(0xD9); put1(static_cast<::psynder::u8>(n));
        } else if (n <= 0xFFFF) {
            put1(0xDA); put_be16(static_cast<::psynder::u16>(n));
        } else {
            put1(0xDB); put_be32(static_cast<::psynder::u32>(n));
        }
        append(reinterpret_cast<const ::psynder::u8*>(s.data()), n);
    }

    void bin(const ::psynder::u8* p, ::psynder::usize n) {
        if (n <= 0xFF) {
            put1(0xC4); put1(static_cast<::psynder::u8>(n));
        } else if (n <= 0xFFFF) {
            put1(0xC5); put_be16(static_cast<::psynder::u16>(n));
        } else {
            put1(0xC6); put_be32(static_cast<::psynder::u32>(n));
        }
        append(p, n);
    }

    void array_header(::psynder::usize n) {
        if (n <= 15) {
            put1(static_cast<::psynder::u8>(0x90 | n));
        } else if (n <= 0xFFFF) {
            put1(0xDC); put_be16(static_cast<::psynder::u16>(n));
        } else {
            put1(0xDD); put_be32(static_cast<::psynder::u32>(n));
        }
    }
    void map_header(::psynder::usize n) {
        if (n <= 15) {
            put1(static_cast<::psynder::u8>(0x80 | n));
        } else if (n <= 0xFFFF) {
            put1(0xDE); put_be16(static_cast<::psynder::u16>(n));
        } else {
            put1(0xDF); put_be32(static_cast<::psynder::u32>(n));
        }
    }

private:
    std::vector<::psynder::u8> buf_;

    void put1(::psynder::u8 b) { buf_.push_back(b); }
    void put_be16(::psynder::u16 v) {
        buf_.push_back(static_cast<::psynder::u8>(v >> 8));
        buf_.push_back(static_cast<::psynder::u8>(v));
    }
    void put_be32(::psynder::u32 v) {
        buf_.push_back(static_cast<::psynder::u8>(v >> 24));
        buf_.push_back(static_cast<::psynder::u8>(v >> 16));
        buf_.push_back(static_cast<::psynder::u8>(v >> 8));
        buf_.push_back(static_cast<::psynder::u8>(v));
    }
    void put_be64(::psynder::u64 v) {
        put_be32(static_cast<::psynder::u32>(v >> 32));
        put_be32(static_cast<::psynder::u32>(v));
    }
    void append(const ::psynder::u8* p, ::psynder::usize n) {
        if (n) buf_.insert(buf_.end(), p, p + n);
    }
};


// ─── Reader ─────────────────────────────────────────────────────────────────
class Reader {
public:
    Reader(const ::psynder::u8* data, ::psynder::usize size) noexcept
        : p_(data), end_(data + size) {}

    Reader(const std::vector<::psynder::u8>& v) noexcept
        : Reader(v.data(), v.size()) {}

    bool eof() const noexcept { return p_ >= end_; }
    ::psynder::usize remaining() const noexcept { return static_cast<::psynder::usize>(end_ - p_); }
    const ::psynder::u8* cursor() const noexcept { return p_; }

    // Peek next byte (the type tag).
    bool peek(::psynder::u8& tag) const noexcept {
        if (p_ >= end_) return false;
        tag = *p_;
        return true;
    }

    // Skip the next value (for forward-compat: a panel can ignore unknown
    // fields by walking past them).
    bool skip() noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        // Primitives — bool/nil/fixints handled by the dispatch below.
        if ((tag & 0x80) == 0) { ++p_; return true; }      // positive fixint
        if ((tag & 0xE0) == 0xE0) { ++p_; return true; }   // negative fixint
        if ((tag & 0xE0) == 0xA0) {                        // fixstr
            ::psynder::usize n = tag & 0x1Fu;
            ++p_;
            return advance(n);
        }
        if ((tag & 0xF0) == 0x90) {                        // fixarray
            ::psynder::u32 n = tag & 0x0Fu;
            ++p_;
            for (::psynder::u32 i = 0; i < n; ++i) if (!skip()) return false;
            return true;
        }
        if ((tag & 0xF0) == 0x80) {                        // fixmap
            ::psynder::u32 n = tag & 0x0Fu;
            ++p_;
            for (::psynder::u32 i = 0; i < n * 2; ++i) if (!skip()) return false;
            return true;
        }
        switch (tag) {
        case 0xC0: case 0xC2: case 0xC3: ++p_; return true; // nil / false / true
        case 0xCC: ++p_; return advance(1);
        case 0xCD: ++p_; return advance(2);
        case 0xCE: case 0xCA: ++p_; return advance(4);
        case 0xCF: case 0xCB: ++p_; return advance(8);
        case 0xD0: ++p_; return advance(1);
        case 0xD1: ++p_; return advance(2);
        case 0xD2: ++p_; return advance(4);
        case 0xD3: ++p_; return advance(8);
        case 0xD9: { ++p_; ::psynder::u8 n=0; if (!read_be8(n)) return false; return advance(n); }
        case 0xDA: { ++p_; ::psynder::u16 n=0; if (!read_be16(n)) return false; return advance(n); }
        case 0xDB: { ++p_; ::psynder::u32 n=0; if (!read_be32(n)) return false; return advance(n); }
        case 0xC4: { ++p_; ::psynder::u8 n=0; if (!read_be8(n)) return false; return advance(n); }
        case 0xC5: { ++p_; ::psynder::u16 n=0; if (!read_be16(n)) return false; return advance(n); }
        case 0xC6: { ++p_; ::psynder::u32 n=0; if (!read_be32(n)) return false; return advance(n); }
        case 0xDC: { ++p_; ::psynder::u16 n=0; if (!read_be16(n)) return false;
                     for (::psynder::u32 i = 0; i < n; ++i) if (!skip()) return false; return true; }
        case 0xDD: { ++p_; ::psynder::u32 n=0; if (!read_be32(n)) return false;
                     for (::psynder::u32 i = 0; i < n; ++i) if (!skip()) return false; return true; }
        case 0xDE: { ++p_; ::psynder::u16 n=0; if (!read_be16(n)) return false;
                     for (::psynder::u32 i = 0; i < n * 2u; ++i) if (!skip()) return false; return true; }
        case 0xDF: { ++p_; ::psynder::u32 n=0; if (!read_be32(n)) return false;
                     for (::psynder::u32 i = 0; i < n * 2u; ++i) if (!skip()) return false; return true; }
        default: return false;
        }
    }

    bool nil() noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag) || tag != 0xC0) return false;
        ++p_;
        return true;
    }

    bool boolean(bool& v) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        if (tag == 0xC2) { v = false; ++p_; return true; }
        if (tag == 0xC3) { v = true;  ++p_; return true; }
        return false;
    }

    // Unsigned integer decode, widening as needed.
    bool u8_(::psynder::u8& v)   noexcept { ::psynder::u64 x=0; if (!u64_(x) || x > 0xFFull) return false; v = static_cast<::psynder::u8>(x); return true; }
    bool u16_(::psynder::u16& v) noexcept { ::psynder::u64 x=0; if (!u64_(x) || x > 0xFFFFull) return false; v = static_cast<::psynder::u16>(x); return true; }
    bool u32_(::psynder::u32& v) noexcept { ::psynder::u64 x=0; if (!u64_(x) || x > 0xFFFFFFFFull) return false; v = static_cast<::psynder::u32>(x); return true; }
    bool u64_(::psynder::u64& v) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        // positive fixint
        if ((tag & 0x80) == 0) { v = tag; ++p_; return true; }
        switch (tag) {
        case 0xCC: { ++p_; ::psynder::u8 x=0; if (!read_be8(x)) return false; v = x; return true; }
        case 0xCD: { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; v = x; return true; }
        case 0xCE: { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; v = x; return true; }
        case 0xCF: { ++p_; ::psynder::u64 x=0; if (!read_be64(x)) return false; v = x; return true; }
        // Also accept non-negative signed values
        case 0xD0: { ++p_; ::psynder::u8 x=0; if (!read_be8(x)) return false; if (static_cast<::psynder::i8>(x) < 0) return false; v = x; return true; }
        case 0xD1: { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; if (static_cast<::psynder::i16>(x) < 0) return false; v = x; return true; }
        case 0xD2: { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; if (static_cast<::psynder::i32>(x) < 0) return false; v = x; return true; }
        case 0xD3: { ++p_; ::psynder::u64 x=0; if (!read_be64(x)) return false; if (static_cast<::psynder::i64>(x) < 0) return false; v = x; return true; }
        default: return false;
        }
    }

    bool i8_(::psynder::i8& v)   noexcept { ::psynder::i64 x=0; if (!i64_(x) || x < -128 || x > 127) return false; v = static_cast<::psynder::i8>(x); return true; }
    bool i16_(::psynder::i16& v) noexcept { ::psynder::i64 x=0; if (!i64_(x) || x < -32768 || x > 32767) return false; v = static_cast<::psynder::i16>(x); return true; }
    bool i32_(::psynder::i32& v) noexcept { ::psynder::i64 x=0; if (!i64_(x) || x < -0x80000000ll || x > 0x7FFFFFFFll) return false; v = static_cast<::psynder::i32>(x); return true; }
    bool i64_(::psynder::i64& v) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        // positive fixint
        if ((tag & 0x80) == 0) { v = tag; ++p_; return true; }
        // negative fixint
        if ((tag & 0xE0) == 0xE0) { v = static_cast<::psynder::i8>(tag); ++p_; return true; }
        switch (tag) {
        case 0xCC: { ++p_; ::psynder::u8 x=0; if (!read_be8(x)) return false; v = x; return true; }
        case 0xCD: { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; v = x; return true; }
        case 0xCE: { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; v = x; return true; }
        case 0xCF: { ++p_; ::psynder::u64 x=0; if (!read_be64(x)) return false; if (x > 0x7FFFFFFFFFFFFFFFull) return false; v = static_cast<::psynder::i64>(x); return true; }
        case 0xD0: { ++p_; ::psynder::u8 x=0; if (!read_be8(x)) return false; v = static_cast<::psynder::i8>(x); return true; }
        case 0xD1: { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; v = static_cast<::psynder::i16>(x); return true; }
        case 0xD2: { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; v = static_cast<::psynder::i32>(x); return true; }
        case 0xD3: { ++p_; ::psynder::u64 x=0; if (!read_be64(x)) return false; v = static_cast<::psynder::i64>(x); return true; }
        default: return false;
        }
    }

    bool f32_(::psynder::f32& v) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        if (tag == 0xCA) {
            ++p_; ::psynder::u32 bits=0; if (!read_be32(bits)) return false;
            std::memcpy(&v, &bits, 4); return true;
        }
        if (tag == 0xCB) {
            ::psynder::f64 d=0; if (!f64_(d)) return false;
            v = static_cast<::psynder::f32>(d); return true;
        }
        // Allow integer → float widening for robustness.
        ::psynder::i64 ix=0;
        if (i64_(ix)) { v = static_cast<::psynder::f32>(ix); return true; }
        return false;
    }
    bool f64_(::psynder::f64& v) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        if (tag == 0xCB) {
            ++p_; ::psynder::u64 bits=0; if (!read_be64(bits)) return false;
            std::memcpy(&v, &bits, 8); return true;
        }
        if (tag == 0xCA) {
            ::psynder::f32 f=0; if (!f32_(f)) return false;
            v = static_cast<::psynder::f64>(f); return true;
        }
        ::psynder::i64 ix=0;
        if (i64_(ix)) { v = static_cast<::psynder::f64>(ix); return true; }
        return false;
    }

    bool str(std::string& out) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        ::psynder::usize n = 0;
        if ((tag & 0xE0) == 0xA0) { n = tag & 0x1Fu; ++p_; }
        else if (tag == 0xD9) { ++p_; ::psynder::u8 x=0;  if (!read_be8(x))  return false; n = x; }
        else if (tag == 0xDA) { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; n = x; }
        else if (tag == 0xDB) { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; n = x; }
        else return false;
        if (remaining() < n) return false;
        out.assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return true;
    }

    bool bin(std::vector<::psynder::u8>& out) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        ::psynder::usize n = 0;
        if (tag == 0xC4)      { ++p_; ::psynder::u8 x=0;  if (!read_be8(x))  return false; n = x; }
        else if (tag == 0xC5) { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; n = x; }
        else if (tag == 0xC6) { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; n = x; }
        else return false;
        if (remaining() < n) return false;
        out.assign(p_, p_ + n);
        p_ += n;
        return true;
    }

    bool array_header(::psynder::u32& count) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        if ((tag & 0xF0) == 0x90) { count = tag & 0x0Fu; ++p_; return true; }
        if (tag == 0xDC) { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; count = x; return true; }
        if (tag == 0xDD) { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; count = x; return true; }
        return false;
    }

    bool map_header(::psynder::u32& count) noexcept {
        ::psynder::u8 tag = 0;
        if (!peek(tag)) return false;
        if ((tag & 0xF0) == 0x80) { count = tag & 0x0Fu; ++p_; return true; }
        if (tag == 0xDE) { ++p_; ::psynder::u16 x=0; if (!read_be16(x)) return false; count = x; return true; }
        if (tag == 0xDF) { ++p_; ::psynder::u32 x=0; if (!read_be32(x)) return false; count = x; return true; }
        return false;
    }

private:
    const ::psynder::u8* p_   = nullptr;
    const ::psynder::u8* end_ = nullptr;

    bool advance(::psynder::usize n) noexcept {
        if (remaining() < n) return false;
        p_ += n;
        return true;
    }
    bool read_be8(::psynder::u8& v) noexcept {
        if (remaining() < 1) return false;
        v = p_[0]; p_ += 1; return true;
    }
    bool read_be16(::psynder::u16& v) noexcept {
        if (remaining() < 2) return false;
        v = static_cast<::psynder::u16>((p_[0] << 8) | p_[1]); p_ += 2; return true;
    }
    bool read_be32(::psynder::u32& v) noexcept {
        if (remaining() < 4) return false;
        v = (::psynder::u32(p_[0]) << 24) | (::psynder::u32(p_[1]) << 16)
          | (::psynder::u32(p_[2]) <<  8) |  ::psynder::u32(p_[3]);
        p_ += 4; return true;
    }
    bool read_be64(::psynder::u64& v) noexcept {
        if (remaining() < 8) return false;
        ::psynder::u32 hi=0, lo=0;
        if (!read_be32(hi) || !read_be32(lo)) return false;
        v = (static_cast<::psynder::u64>(hi) << 32) | lo;
        return true;
    }
};


// ─── Free encode() / decode() helpers used by generated stubs ───────────────
// Primitives — uniform `encode(out, v)` / `decode(in, v)` overload set so
// the generated code can call them by name without knowing the concrete
// IDL type.

inline void encode(Writer& w, bool v)              { w.boolean(v); }
inline void encode(Writer& w, ::psynder::u8 v)     { w.u8_(v); }
inline void encode(Writer& w, ::psynder::u16 v)    { w.u16_(v); }
inline void encode(Writer& w, ::psynder::u32 v)    { w.u32_(v); }
inline void encode(Writer& w, ::psynder::u64 v)    { w.u64_(v); }
inline void encode(Writer& w, ::psynder::i8 v)     { w.i8_(v); }
inline void encode(Writer& w, ::psynder::i16 v)    { w.i16_(v); }
inline void encode(Writer& w, ::psynder::i32 v)    { w.i32_(v); }
inline void encode(Writer& w, ::psynder::i64 v)    { w.i64_(v); }
inline void encode(Writer& w, ::psynder::f32 v)    { w.f32_(v); }
inline void encode(Writer& w, ::psynder::f64 v)    { w.f64_(v); }
inline void encode(Writer& w, std::string_view v)  { w.str(v); }
inline void encode(Writer& w, const std::string& v){ w.str(v); }
inline void encode(Writer& w, const std::vector<::psynder::u8>& v) {
    w.bin(v.data(), v.size());
}
template <class T>
inline void encode(Writer& w, const std::vector<T>& v) {
    w.array_header(v.size());
    for (const auto& e : v) encode(w, e);
}

inline bool decode(Reader& r, bool& v)              { return r.boolean(v); }
inline bool decode(Reader& r, ::psynder::u8& v)     { return r.u8_(v); }
inline bool decode(Reader& r, ::psynder::u16& v)    { return r.u16_(v); }
inline bool decode(Reader& r, ::psynder::u32& v)    { return r.u32_(v); }
inline bool decode(Reader& r, ::psynder::u64& v)    { return r.u64_(v); }
inline bool decode(Reader& r, ::psynder::i8& v)     { return r.i8_(v); }
inline bool decode(Reader& r, ::psynder::i16& v)    { return r.i16_(v); }
inline bool decode(Reader& r, ::psynder::i32& v)    { return r.i32_(v); }
inline bool decode(Reader& r, ::psynder::i64& v)    { return r.i64_(v); }
inline bool decode(Reader& r, ::psynder::f32& v)    { return r.f32_(v); }
inline bool decode(Reader& r, ::psynder::f64& v)    { return r.f64_(v); }
inline bool decode(Reader& r, std::string& v)       { return r.str(v); }
inline bool decode(Reader& r, std::vector<::psynder::u8>& v) {
    return r.bin(v);
}
template <class T>
inline bool decode(Reader& r, std::vector<T>& v) {
    ::psynder::u32 n = 0;
    if (!r.array_header(n)) return false;
    v.clear();
    v.reserve(n);
    for (::psynder::u32 i = 0; i < n; ++i) {
        T e{};
        if (!decode(r, e)) return false;
        v.emplace_back(std::move(e));
    }
    return true;
}

}  // namespace psynder::editor::ipc::msgpack
