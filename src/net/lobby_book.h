#pragma once
// The lobby's pilots and seats on the host (L4), with no network or game calls (unit tested). Seats are the members of
// the host's mission's player flight; each pilot (the host included) picks one, the host's book decides. HostCore
// guards it with its mission lock.
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "net/protocol.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

class LobbyBook {
public:
    static constexpr size_t kMax = 8;              // pilots, the host included
    static constexpr uint64_t kExpireMs = 15000;   // a client not heard of for this long has left (it asks every second)
    static constexpr uint32_t kHostTag = 0;        // the host's own row (client tags are never 0)

    /// <summary>The player flight's size for a new mission: seats past it are dropped, and pilots without a seat get
    /// the first free one (the host first, then in join order).</summary>
    void set_members(int members) {
        members_ = members < 0 ? 0 : members > 255 ? 255 : members;
        for (size_t i = 0; i < n_; ++i)
            if (p_[i].seat >= members_) p_[i].seat = -1;
        for (size_t i = 0; i < n_; ++i)
            if (p_[i].seat < 0) p_[i].seat = first_free();
    }
    int members() const { return members_; }

    /// <summary>The host's own row: its callsign, and a seat wish (-1: keep). Returns kLobbyOk or kLobbySeatTaken.</summary>
    uint8_t set_host(const char* callsign, int want_seat) {
        Pilot* h = find(kHostTag);
        if (!h) {
            if (n_ == kMax) return kLobbyFull;
            // The host is always the first row.
            for (size_t i = n_; i > 0; --i) p_[i] = p_[i - 1];
            ++n_;
            h = &p_[0];
            *h = Pilot{};
            h->tag = kHostTag;
            h->seat = first_free();
        }
        set_str(h->callsign, callsign);
        return wish(*h, want_seat);
    }

    /// <summary>A client's status at now_ms. A new client gets the first free seat. A seat wish is granted only for a
    /// free seat (and clears Ready); leaving drops the row. Returns a kLobby* result.</summary>
    uint8_t update(uint32_t tag, const char* callsign, bool ready, int want_seat, bool leaving, uint64_t now_ms) {
        if (tag == kHostTag) return kLobbyFull;
        Pilot* p = find(tag);
        if (leaving) {
            if (p) remove(static_cast<size_t>(p - p_));
            return kLobbyOk;
        }
        if (!p) {
            if (n_ == kMax) return kLobbyFull;
            p = &p_[n_++];
            *p = Pilot{};
            p->tag = tag;
            p->seat = first_free();
        }
        set_str(p->callsign, callsign);
        p->seen_ms = now_ms;
        const int before = p->seat;
        const uint8_t r = wish(*p, want_seat);
        p->ready = p->seat == before && ready && p->seat >= 0;  // a move clears Ready (the client clears its own too)
        return r;
    }

    /// <summary>Drops clients not heard of since now_ms - kExpireMs. Returns how many.</summary>
    int expire(uint64_t now_ms) {
        int n = 0;
        for (size_t i = n_; i > 0; --i) {
            const Pilot& p = p_[i - 1];
            if (p.tag != kHostTag && now_ms > p.seen_ms + kExpireMs) {
                remove(i - 1);
                ++n;
            }
        }
        return n;
    }

    int seat_of(uint32_t tag) const {
        const Pilot* p = find(tag);
        return p ? p->seat : -1;
    }
    int host_seat() const { return seat_of(kHostTag); }
    bool ready(uint32_t tag) const {
        const Pilot* p = find(tag);
        return p && p->ready;
    }
    /// <summary>Clients in the book (the host not counted).</summary>
    int clients() const {
        int n = 0;
        for (size_t i = 0; i < n_; ++i) n += p_[i].tag != kHostTag;
        return n;
    }

    /// <summary>The roster for one reader: the pilots (the host first, then in join order), then the free seats (flown by
    /// the AI), at most kLobbyRows. you: the reader's tag (kHostTag for the host's own view).</summary>
    size_t rows(LobbyRow* out, uint32_t you) const {
        size_t k = 0;
        for (size_t i = 0; i < n_ && k < static_cast<size_t>(kLobbyRows); ++i) {
            LobbyRow& r = out[k++];
            r = LobbyRow{};
            set_str(r.callsign, p_[i].callsign);
            r.seat = p_[i].seat >= 0 ? static_cast<uint8_t>(p_[i].seat) : kNoSeat;
            r.flags = static_cast<uint8_t>((p_[i].tag == kHostTag ? kLobbyRowHost : 0) | (p_[i].ready ? kLobbyRowReady : 0) |
                                           (p_[i].tag == you ? kLobbyRowYou : 0));
        }
        for (int m = 0; m < members_ && k < static_cast<size_t>(kLobbyRows); ++m) {
            if (held(m)) continue;
            LobbyRow& r = out[k++];
            r = LobbyRow{};
            r.seat = static_cast<uint8_t>(m);
        }
        return k;
    }

private:
    struct Pilot {
        uint32_t tag = 0;
        char callsign[16] = {};
        int seat = -1;
        bool ready = false;
        uint64_t seen_ms = 0;
    };

    // A seat wish (-1: none): granted if the seat exists and is free; a granted move clears Ready.
    uint8_t wish(Pilot& p, int want) {
        if (want < 0 || want == p.seat) return kLobbyOk;
        if (want >= members_ || held(want)) return kLobbySeatTaken;
        p.seat = want;
        p.ready = false;
        return kLobbyOk;
    }
    bool held(int seat) const {
        for (size_t i = 0; i < n_; ++i)
            if (p_[i].seat == seat) return true;
        return false;
    }
    int first_free() const {
        for (int m = 0; m < members_; ++m)
            if (!held(m)) return m;
        return -1;
    }
    Pilot* find(uint32_t tag) {
        for (size_t i = 0; i < n_; ++i)
            if (p_[i].tag == tag) return &p_[i];
        return nullptr;
    }
    const Pilot* find(uint32_t tag) const {
        for (size_t i = 0; i < n_; ++i)
            if (p_[i].tag == tag) return &p_[i];
        return nullptr;
    }
    void remove(size_t i) {
        for (size_t j = i + 1; j < n_; ++j) p_[j - 1] = p_[j];
        --n_;
    }

    Pilot p_[kMax];
    size_t n_ = 0;
    int members_ = 0;
};

}  // namespace LazyWorks::SF2Patch::net
