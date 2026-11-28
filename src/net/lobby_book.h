#pragma once
// The lobby's pilots and seats on the host (L4), with no network or game calls (unit tested). Seats are the members of
// the host's mission's player flight; each pilot (the host included) picks one, the host's book decides. HostCore
// guards it with its mission lock.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "net/protocol.h"
#include "net/world_proto.h"

namespace LazyWorks::SF2Patch::net {

class LobbyBook {
public:
    static constexpr size_t kMax = 8;              // pilots, the host included
    static constexpr uint64_t kExpireMs = 20000;   // a client not heard of for this long has left (it asks every second)
    static constexpr uint32_t kHostTag = 0;        // the host's own row (client tags are never 0)
    static constexpr uint8_t kRefused = 0xff;      // update: not this sender's row (no answer is sent)

    /// <summary>The player flight's size for a new mission: seats past it are dropped, and pilots without a seat get
    /// the first free one (the host first, then in join order). Seats inside the flight are kept.</summary>
    /// <param name="host_member">The host's member in that mission, given to the host only if it has no seat yet and
    /// that seat is free (-1: none).</param>
    void set_members(int members, int host_member = -1) {
        members_ = members < 0 ? 0 : members > 255 ? 255 : members;
        for (size_t i = 0; i < n_; ++i)
            if (p_[i].seat >= members_) p_[i].seat = -1;
        Pilot* h = find(kHostTag);
        if (h && h->seat < 0 && host_member >= 0 && host_member < members_ && !held(host_member)) h->seat = host_member;
        if (h && h->seat < 0) h->seat = first_free();
        if (h && h->seat < 0 && members_ > 0) {
            // Every seat of the flight is a client's: the host flies, so the last client to join gives up its seat.
            for (size_t i = n_; i > 0; --i) {
                Pilot& p = p_[i - 1];
                if (p.tag != kHostTag && p.seat >= 0) {
                    h->seat = p.seat;
                    p.seat = -1;
                    p.ready = false;
                    break;
                }
            }
        }
        for (size_t i = 0; i < n_; ++i)
            if (p_[i].seat < 0) p_[i].seat = client_free();
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
            h->number = ++joined_;
        }
        clean_callsign(*h, callsign);
        return wish(*h, want_seat);
    }

    /// <summary>A client's status at now_ms. A new client gets the first free seat. A seat wish is granted only for a
    /// free seat (and clears Ready); leaving drops the row. Returns a kLobby* result, or kRefused.</summary>
    /// <param name="from">The sender's address (IP and port). A row belongs to the address that made it: another one
    /// presenting its tag is refused, and one address has one row. 0 binds nothing.</param>
    uint8_t update(uint32_t tag, const char* callsign, bool ready, int want_seat, bool leaving, uint64_t now_ms,
                   uint64_t from = 0) {
        if (tag == kHostTag) return kLobbyFull;
        Pilot* p = find(tag);
        if (p && p->from != from) return kRefused;
        if (!p && from)
            for (size_t i = 0; i < n_; ++i)
                if (p_[i].from == from) return kRefused;
        if (leaving) {
            if (p) remove(static_cast<size_t>(p - p_));
            return kLobbyOk;
        }
        if (!p) {
            if (n_ == kMax) return kLobbyFull;
            p = &p_[n_++];
            *p = Pilot{};
            p->tag = tag;
            p->from = from;
            p->seat = client_free();
            p->number = ++joined_;
        }
        clean_callsign(*p, callsign);
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
    /// <summary>Whether tag has a row made from this address.</summary>
    bool owns(uint32_t tag, uint64_t from) const {
        const Pilot* p = find(tag);
        return p && p->tag != kHostTag && p->from == from;
    }
    /// <summary>The client rows' tags and callsigns (the host's left out).</summary>
    size_t client_tags(uint32_t* tags, const char** callsigns, size_t cap) const {
        size_t k = 0;
        for (size_t i = 0; i < n_ && k < cap; ++i) {
            if (p_[i].tag == kHostTag) continue;
            tags[k] = p_[i].tag;
            callsigns[k] = p_[i].callsign;
            ++k;
        }
        return k;
    }
    /// <summary>The row of tag: its seat and when it was last heard of; false if there is none.</summary>
    bool info(uint32_t tag, int* seat, uint64_t* seen_ms) const {
        const Pilot* p = find(tag);
        if (!p) return false;
        *seat = p->seat;
        *seen_ms = p->seen_ms;
        return true;
    }
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

    /// <summary>The host's seat for a flight of `members` (the game thread, as the flight starts): a seat of that
    /// flight the book keeps for the host (it never flies a client's). -1 only without a host row or a flight.</summary>
    /// The book's flight (members()) and the clients' seats are left as they are unless the host needs one of them.
    int host_seat_for(int members) {
        Pilot* h = find(kHostTag);
        if (!h || members <= 0) return -1;
        if (h->seat >= 0 && h->seat < members) return h->seat;
        for (int m = 0; m < members; ++m) {
            if (!held(m)) {
                h->seat = m;
                return m;
            }
        }
        // Every seat of that flight is a client's: the last client to join gives up its seat.
        for (size_t i = n_; i > 0; --i) {
            Pilot& p = p_[i - 1];
            if (p.tag != kHostTag && p.seat >= 0 && p.seat < members) {
                h->seat = p.seat;
                p.seat = -1;
                p.ready = false;
                p.seat = client_free();
                return h->seat;
            }
        }
        return -1;
    }

    /// <summary>The roster for one reader: the pilots and the free seats (flown by the AI) in seat order, pilots without
    /// a seat last, at most kLobbyRows. you: the reader's tag (kHostTag for the host's own view).</summary>
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
        // Stable insertion sort by seat (kNoSeat, 255, sorts last).
        for (size_t i = 1; i < k; ++i) {
            const LobbyRow r = out[i];
            size_t j = i;
            for (; j > 0 && out[j - 1].seat > r.seat; --j) out[j] = out[j - 1];
            out[j] = r;
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
        uint64_t from = 0;
        int number = 0;  // join order, for a callsign that cannot be shown
    };

    // Printable ASCII only (the game's menu font, and a row with no callsign reads as a free seat); none left gives
    // "PILOT n".
    static void clean_callsign(Pilot& p, const char* in) {
        size_t k = 0;
        for (size_t i = 0; in && in[i] && k + 1 < sizeof(p.callsign); ++i) {
            const unsigned char c = static_cast<unsigned char>(in[i]);
            if (c >= 0x20 && c < 0x7f && (c != ' ' || k > 0)) p.callsign[k++] = static_cast<char>(c);
        }
        while (k > 0 && p.callsign[k - 1] == ' ') --k;
        p.callsign[k] = 0;
        if (!k) std::snprintf(p.callsign, sizeof(p.callsign), "PILOT %d", p.number);
    }

    // A seat wish (-1: none): granted if the seat exists and is free; a granted move clears Ready.
    uint8_t wish(Pilot& p, int want) {
        if (want < 0 || want == p.seat) return kLobbyOk;
        if (want >= members_ || held(want)) return kLobbySeatTaken;
        if (p.tag != kHostTag && host_needs(want)) return kLobbySeatTaken;  // the host's last seat
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
    // Whether seat is the last free one while the host (in the book) has none: kept for the host.
    bool host_needs(int seat) const {
        const Pilot* h = find(kHostTag);
        if (!h || h->seat >= 0) return false;
        for (int m = 0; m < members_; ++m)
            if (m != seat && !held(m)) return false;
        return true;
    }
    // A client's seat: the first free one, but never the host's last.
    int client_free() const {
        for (int m = 0; m < members_; ++m)
            if (!held(m) && !host_needs(m)) return m;
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
    int joined_ = 0;
    int members_ = 0;
};

}  // namespace LazyWorks::SF2Patch::net
