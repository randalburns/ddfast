// Scalar ports of the parts of DDS that decide how many positions a search visits: quick
// tricks, later tricks, second-hand quick tricks, and the move-ordering weights. The solver
// (solver.cpp) uses them for its bounds and move ordering at the upper levels of the search.
//
// The three sections marked below are taken from DDS (https://github.com/dds-bridge/dds,
// Apache-2.0, Copyright 2006-2014 Bo Haglund, 2014-2018 Bo Haglund and Soren Hein) with only
// mechanical changes: the solver context is replaced by the small types defined here, and
// results go to Pos::win_ranks. The logic and layout are DDS's, kept as they are so that the
// port can be checked against DDS position by position. The code before and after those
// sections was written for ddfast (first for vecdds, an earlier experiment).
#pragma once

#include <algorithm>
#include <cstring>

#include <cstdint>

namespace ddfast {
namespace port {

constexpr int DDS_HANDS = 4;
constexpr int DDS_SUITS = 4;
constexpr int DDS_NOTRUMP = 4;
constexpr int MAXNODE = 1;
constexpr int MINNODE = 0;

// Seats are N E S W; North-South maximize.
inline int node_type(int hand) { return (hand & 1) == 0 ? MAXNODE : MINNODE; }

inline constexpr int lho[4] = {1, 2, 3, 0};
inline constexpr int rho[4] = {3, 0, 1, 2};
inline constexpr int partner[4] = {2, 3, 0, 1};
// Ranks are 2..14; a holding has bit rank - 2 set.
inline constexpr unsigned short bit_map_rank[16] = {0x0000, 0x0000, 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020,
                                                    0x0040, 0x0080, 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000};

// A holding split into groups of touching cards, lowest group first.
struct MoveGroupType {
  int last_group_;   // index of the top group, -1 for an empty holding
  int rank_[7];      // the top rank of each group
  int sequence_[7];  // the group's cards below its top card
  int fullseq_[7];   // all of the group's cards
  int gap_[7];       // the ranks between the group and the one below it
};

struct Tables {
  int highest[8192], lowest[8192], count[8192];
  char rel[8192][15];  // rel[holding][rank]: 1 for the top card of the holding, 2 for the next, ...
  MoveGroupType group[8192];
  Tables() {
    highest[0] = lowest[0] = 0;
    for (int a = 0; a < 8192; a++) {
      count[a] = __builtin_popcount(a);
      std::memset(rel[a], 0, 15);
      if (!a) continue;
      highest[a] = 33 - __builtin_clz(a);
      lowest[a] = __builtin_ctz(a) + 2;
      char ordinal = 0;
      for (int rank = 14; rank >= 2; rank--)
        if (a & bit_map_rank[rank]) rel[a][rank] = ++ordinal;
    }
    static const int topside[15] = {0x0000, 0x0000, 0x0000, 0x0001, 0x0003, 0x0007, 0x000f, 0x001f,
                                    0x003f, 0x007f, 0x00ff, 0x01ff, 0x03ff, 0x07ff, 0x0fff};
    static const int botside[15] = {0xffff, 0xffff, 0x1ffe, 0x1ffc, 0x1ff8, 0x1ff0, 0x1fe0, 0x1fc0,
                                    0x1f80, 0x1f00, 0x1e00, 0x1c00, 0x1800, 0x1000, 0x0000};
    group[0] = MoveGroupType{};
    group[0].last_group_ = -1;
    group[1] = MoveGroupType{};
    group[1].rank_[0] = 2;
    group[1].fullseq_[0] = 1;
    int top_bit = 1, next_bit = 0, top_no = 2;
    for (int ris = 2; ris < 8192; ris++) {
      if (ris >= (top_bit << 1)) {
        next_bit = top_bit;
        top_bit <<= 1;
        top_no++;
      }
      MoveGroupType& m = group[ris];
      m = group[ris ^ top_bit];
      if (ris & next_bit) {  // the new top card extends the top group
        const int g = m.last_group_;
        m.rank_[g]++;
        m.sequence_[g] |= next_bit;
        m.fullseq_[g] |= top_bit;
      } else {               // it starts a new group
        const int g = ++m.last_group_;
        m.rank_[g] = top_no;
        m.sequence_[g] = 0;
        m.fullseq_[g] = top_bit;
        m.gap_[g] = g == 0 ? 0 : topside[top_no] & botside[m.rank_[g - 1]];
      }
    }
  }
};
inline const Tables& tables() {
  static const Tables t;
  return t;
}
inline const int* const highest_rank = tables().highest;
inline const int* const lowest_rank = tables().lowest;
inline const int* const count_table = tables().count;
inline const char (*const rel_rank)[15] = tables().rel;
inline const MoveGroupType* const group_data = tables().group;

struct HighCardType {
  int rank;  // 0 if there is no such card
  int hand;  // -1 if there is no such card
};
struct MoveType {
  int suit;
  int rank;
  int sequence;  // the cards the move stands for, below its own
  int weight;
};

// The position as DDS's bounds and weights read it.
struct Pos {
  unsigned short rank_in_suit[4][4];  // holdings now
  unsigned short aggr[4];             // all hands' cards of each suit now
  unsigned char length[4][4];
  HighCardType winner[4], second_best[4];  // top two cards of each suit at the start of the trick
  HighCardType third[4];                   // the third-best card of each suit now
  int tricks_max;                          // tricks North-South have
  unsigned short win_ranks[4];             // out: the cards a bound depends on
  MoveType lead;                           // second hand: the card led
};
inline const HighCardType& third_best(const Pos& tpos, int suit) { return tpos.third[suit]; }

struct HeuristicContext {
  const Pos& tpos;
  const MoveType& best_move;
  const MoveType& best_move_tt;
  MoveType* mply;
  int num_moves;
  int last_num_moves;
  const int trump;
  int suit;
  const int curr_trick;
  const int curr_hand;
  const int lead_hand;
  const int lead_suit;
  int removed_ranks[4] = {0};
  int move1_rank = 0, high1 = 0, move1_suit = 0;  // the card winning after two plays, and who played it
  int move2_rank = 0, move2_suit = 0, high2 = 0;  // ... after three plays
  int lead0_rank = 0;
};

enum class WeightCase : int {
  Nt0 = 0, Trump0 = 1,
  NtNotVoid1 = 4, TrumpNotVoid1 = 5, NtVoid1 = 6, TrumpVoid1 = 7,
  NtNotVoid2 = 8, TrumpNotVoid2 = 9, NtVoid2 = 10, TrumpVoid2 = 11,
  CombinedNotVoid3 = 12, CombinedNotVoid3Trump = 13, NtVoid3 = 14, TrumpVoid3 = 15,
};

// ---------------------------------------------------------------------------------------
// Quick tricks (DDS quick_tricks.cpp).

inline auto next_quick_trick_suit(int suit, int trump) -> int
{
    if ((trump != DDS_NOTRUMP) && (suit == trump))
    {
        if (trump == 0)
            return 1;
        return 0;
    }

    suit++;
    if ((trump != DDS_NOTRUMP) && (suit == trump))
        suit++;
    return suit;
}


inline int QtricksLeadHandNT(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    int& lhoTrumpRanks,
    int& rhoTrumpRanks,
    const bool commPartner,
    const int commSuit,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    const int trump,
    int& res);

inline int QtricksLeadHandTrump(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    const int lhoTrumpRanks,
    const int rhoTrumpRanks,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    int& res);

inline int QuickTricksPartnerHand(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    const int lhoTrumpRanks,
    const int rhoTrumpRanks,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    const int commSuit,
    const int commRank,
    int& res);

inline int QuickTricksPartnerHandTrump(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    const int lhoTrumpRanks,
    const int rhoTrumpRanks,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    const int commSuit,
    const int commRank,
    int& res);

inline int QuickTricksPartnerHandNT(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    const int commSuit,
    const int commRank,
    int& res);


/**
 * @brief Analyze the number of quick tricks available for a given position.
 *
 * This function estimates the number of immediate tricks that can be taken
 * from the current position, hand, depth, target, and trump suit using double dummy logic.
 *
 * @param tpos Position state
 * @param hand Current hand
 * @param depth Current search depth
 * @param target Target number of tricks
 * @param trump Trump suit
 * @param result Reference to store if the target is achievable
 * @param ctx Solver context
 * @return Number of quick tricks found
 */
inline int QuickTricks(
    Pos& tpos,
    const int hand,
    const int depth,
    const int target,
    const int trump,
    bool& result)
{
    int suit, commRank = 0, commSuit = -1;
    int res;
    int lhoTrumpRanks = 0, rhoTrumpRanks = 0;
    int cutoff, lowestQtricks = 0;

    result = true;
    int qtricks = 0;

    if (node_type(hand) == MAXNODE)
        cutoff = target - tpos.tricks_max;
    else
        cutoff = tpos.tricks_max - target + (depth >> 2) + 2;

    bool commPartner = false;
    const unsigned short (* ris)[DDS_SUITS] = tpos.rank_in_suit;
    const unsigned char (* len)[DDS_SUITS] = tpos.length;
    HighCardType const * winner = tpos.winner;

    for (int s = 0; s < DDS_SUITS; s++)
    {
        if ((trump != DDS_NOTRUMP) && (trump != s))
        {
            /* Trump game, and we lead a non-trump suit */
            if (winner[s].hand == partner[hand])
            {
                /* Partner has winning card */
                if (ris[hand][s] != 0 && /* Own hand has card */
                        (((ris[lho[hand]][s] != 0) || /* LHO not void */
                            (ris[lho[hand]][trump] == 0)) && /* LHO no trump */
             ((ris[rho[hand]][s] != 0) || /* RHO not void */
                            (ris[rho[hand]][trump] == 0)))) /* RHO no trump */
                {
                    commPartner = true;
                    commSuit = s;
                    commRank = winner[s].rank;
                    break;
                }
            }
            else if ((tpos.second_best[s].hand == partner[hand]) &&
               (winner[s].hand == hand) &&
               (len[hand][s] >= 2) &&
               (len[partner[hand]][s] >= 2))
            {
                /* Can cross to partner's card: Type Kx opposite Ax */
                if (((ris[lho[hand]][s] != 0) || /* LHO not void */
             (ris[lho[hand]][trump] == 0)) /* LHO no trump */
                        && ((ris[rho[hand]][s] != 0) || /* RHO not void */
                                (ris[rho[hand]][trump] == 0))) /* RHO no trump */
                {
                    commPartner = true;
                    commSuit = s;
                    commRank = tpos.second_best[s].rank;
                    break;
                }
            }
        }
        else if (trump == DDS_NOTRUMP)
        {
            if (winner[s].hand == partner[hand])
            {
                /* Partner has winning card in NT */
                if (ris[hand][s] != 0) /* Own hand has card */
                {
                    commPartner = true;
                    commSuit = s;
                    commRank = winner[s].rank;
                    break;
                }
            }
            else if ((tpos.second_best[s].hand == partner[hand]) &&
               (winner[s].hand == hand) &&
               (len[hand][s] >= 2) &&
               (len[partner[hand]][s] >= 2))
            {
                /* Can cross to partner's card: Type Kx opposite Ax */
                commPartner = true;
                commSuit = s;
                commRank = tpos.second_best[s].rank;
                break;
            }
        }
    }

    if ((trump != DDS_NOTRUMP) && (!commPartner) &&
            (ris[hand][trump] != 0) &&
            (winner[trump].hand == partner[hand]))
    {
        /* Communication in trump suit */
        commPartner = true;
        commSuit = trump;
        commRank = winner[trump].rank;
    }

    if (trump != DDS_NOTRUMP)
    {
        suit = trump;
        lhoTrumpRanks = len[lho[hand]][trump];
        rhoTrumpRanks = len[rho[hand]][trump];
    }
    else
        suit = 0;

    do
    {
        int countOwn = len[hand][suit];
        int countLho = len[lho[hand]][suit];
        int countRho = len[rho[hand]][suit];
        int countPart = len[partner[hand]][suit];
        int opps = countLho | countRho;

        if (!opps && (countPart == 0)) [[unlikely]]
        {
            if (countOwn == 0) [[unlikely]]
            {
                /* Continue with next suit. */
                suit = next_quick_trick_suit(suit, trump);
                continue;
            }

            /* Long tricks when only leading hand have cards in the suit. */
            if ((trump != DDS_NOTRUMP) && (trump != suit))
            {
                if ((lhoTrumpRanks == 0) && (rhoTrumpRanks == 0)) [[unlikely]]
                {
                    qtricks += countOwn;
                    if (qtricks >= cutoff)
                        return qtricks;
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
                else [[likely]]
                {
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
            }
            else [[likely]]
            {
                qtricks += countOwn;
                if (qtricks >= cutoff)
                    return qtricks;

                suit = next_quick_trick_suit(suit, trump);
                continue;
            }
        }
        else [[likely]]
        {
            if (!opps && (trump != DDS_NOTRUMP) && (suit == trump)) [[unlikely]]
            {
                /* The partner but not the opponents have cards in
           the trump suit. */

                int sum = std::max(countOwn, countPart);
                for (int s = 0; s < DDS_SUITS; s++)
                {
                    if ((sum > 0) &&
                            (s != trump) &&
                            (countOwn >= countPart) &&
                            (len[hand][s] > 0) &&
                            (len[partner[hand]][s] == 0))
                    {
                        sum++;
                        break;
                    }
                }
                /* If the additional trick by ruffing causes a cutoff.
           (qtricks not incremented.) */
                if (sum >= cutoff)
                    return sum;
            }
            else if (!opps) [[unlikely]]
            {
                /* The partner but not the opponents have cards in the suit. */
                int sum = std::min(countOwn, countPart);
                if (trump == DDS_NOTRUMP)
                {
                    if (sum >= cutoff)
                        return sum;
                }
                else if ((suit != trump) &&
                 (lhoTrumpRanks == 0) &&
                 (rhoTrumpRanks == 0))
                {
                    if (sum >= cutoff)
                        return sum;
                }
            }

            if (commPartner)
            {
                if (!opps && (countOwn == 0)) [[unlikely]]
                {
                    if ((trump != DDS_NOTRUMP) && (trump != suit))
                    {
                        if ((lhoTrumpRanks == 0) && (rhoTrumpRanks == 0)) [[unlikely]]
                        {
                            qtricks += countPart;
                            tpos.win_ranks[commSuit] |=
                                bit_map_rank[commRank];

                            if (qtricks >= cutoff)
                                return qtricks;

                            suit = next_quick_trick_suit(suit, trump);
                            continue;
                        }
                        else [[likely]]
                        {
                            suit = next_quick_trick_suit(suit, trump);
                            continue;
                        }
                    }
                    else [[likely]]
                    {
                        qtricks += countPart;
                        tpos.win_ranks[commSuit] |=
                            bit_map_rank[commRank];

                        if (qtricks >= cutoff)
                            return qtricks;

                        suit = next_quick_trick_suit(suit, trump);
                        continue;
                    }
                }
                else [[likely]]
                {
                    if (!opps && (trump != DDS_NOTRUMP) && (suit == trump)) [[unlikely]]
                    {
                        int sum = std::max(countOwn, countPart);
                        for (int s = 0; s < DDS_SUITS; s++)
                        {
                            if ((sum > 0) &&
                                    (s != trump) &&
                                    (countOwn <= countPart) &&
                                    (len[partner[hand]][s] > 0) &&
                                    (len[hand][s] == 0))
                            {
                                sum++;
                                break;
                            }
                        }
                        if (sum >= cutoff)
                        {
                            tpos.win_ranks[commSuit] |=
                                bit_map_rank[commRank];
                            return sum;
                        }
                    }
                    else if (!opps) [[unlikely]]
                    {
                        int sum = std::min(countOwn, countPart);
                        if (trump == DDS_NOTRUMP)
                        {
                            if (sum >= cutoff)
                                return sum;
                        }
                        else if ((suit != trump) &&
                     (lhoTrumpRanks == 0) &&
                     (rhoTrumpRanks == 0))
                        {
                            if (sum >= cutoff)
                                return sum;
                        }
                    }
                }
            }
        }

        if (winner[suit].rank == 0) [[unlikely]]
        {
            suit = next_quick_trick_suit(suit, trump);
            continue;
        }

        if (winner[suit].hand == hand)
        {
            if ((trump != DDS_NOTRUMP) && (trump != suit))
            {
                qtricks = QtricksLeadHandTrump(hand, tpos, cutoff, depth,
                    countLho, countRho, lhoTrumpRanks, rhoTrumpRanks,
                    countOwn, countPart, suit, qtricks, res);

                if (res == 1)
                    return qtricks;
                else if (res == 2)
                {
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
            }
            else
            {
                qtricks = QtricksLeadHandNT(hand, tpos, cutoff, depth,
                    countLho, countRho, lhoTrumpRanks, rhoTrumpRanks,
                    commPartner, commSuit, countOwn, countPart,
                    suit, qtricks, trump, res);

                if (res == 1)
                    return qtricks;
                else if (res == 2)
                {
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
            }
        }

        /* It was not possible to take a quick trick by own winning
       card in the suit */
        else
        {
            /* Partner winning card? */
            if (winner[suit].hand == partner[hand])
            {
                /* Winner found at partner*/
                if (commPartner)
                {
                    /* There is communication with the partner */
                    if ((trump != DDS_NOTRUMP) && (trump != suit))
                    {
                        qtricks = QuickTricksPartnerHandTrump(hand, tpos,
                            cutoff, depth, countLho, countRho,
                            lhoTrumpRanks, rhoTrumpRanks, countOwn,
                            countPart, suit, qtricks, commSuit, commRank, res);

                        if (res == 1)
                            return qtricks;
                        else if (res == 2)
                        {
                            suit = next_quick_trick_suit(suit, trump);
                            continue;
                        }
                    }
                    else
                    {
                        qtricks = QuickTricksPartnerHandNT(hand, tpos, cutoff,
                            depth, countLho, countRho, countOwn, countPart,
                            suit, qtricks, commSuit, commRank, res);

                        if (res == 1)
                            return qtricks;
                        else if (res == 2)
                        {
                            suit = next_quick_trick_suit(suit, trump);
                            continue;
                        }
                    }
                }
            }
        }
        if ((trump != DDS_NOTRUMP) && (suit != trump) &&
                (countOwn > 0) && (lowestQtricks == 0) &&
                ((qtricks == 0) ||
         ((winner[suit].hand != hand) &&
                    (winner[suit].hand != partner[hand]) &&
                    (winner[trump].hand != hand) &&
                    (winner[trump].hand != partner[hand]))))
        {
            if ((countPart == 0) && (len[partner[hand]][trump] > 0))
            {
                if (((countRho > 0) || (len[rho[hand]][trump] == 0)) &&
                        ((countLho > 0) || (len[lho[hand]][trump] == 0)))
                {
                    lowestQtricks = 1;
                    if (1 >= cutoff)
                        return 1;
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
                else if ((countRho == 0) && (countLho == 0))
                {
                    if ((ris[lho[hand]][trump] |
               ris[rho[hand]][trump]) <
                            ris[partner[hand]][trump])
                    {
                        lowestQtricks = 1;

                        int rr = highest_rank[ris[partner[hand]][trump]];
                        if (rr != 0)
                        {
                            tpos.win_ranks[trump] |= bit_map_rank[rr];
                            if (1 >= cutoff)
                                return 1;
                        }
                    }
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
                else if (countLho == 0)
                {
                    if (ris[lho[hand]][trump] <
                            ris[partner[hand]][trump])
                    {
                        lowestQtricks = 1;
                        for (int rr = 14; rr >= 2; rr--)
                        {
                            if ((ris[partner[hand]][trump] & bit_map_rank[rr]) != 0)
                            {
                                tpos.win_ranks[trump] |= bit_map_rank[rr];
                                break;
                            }
                        }
                        if (1 >= cutoff)
                            return 1;
                    }
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
                else if (countRho == 0)
                {
                    if (ris[rho[hand]][trump] <
                            ris[partner[hand]][trump])
                    {
                        lowestQtricks = 1;
                        for (int rr = 14; rr >= 2; rr--)
                        {
                            if ((ris[partner[hand]][trump] & bit_map_rank[rr]) != 0)
                            {
                                tpos.win_ranks[trump] |= bit_map_rank[rr];
                                break;
                            }
                        }
                        if (1 >= cutoff)
                            return 1;
                    }
                    suit = next_quick_trick_suit(suit, trump);
                    continue;
                }
            }
        }

        if (qtricks >= cutoff)
            return qtricks;

        suit = next_quick_trick_suit(suit, trump);
    }
    while (suit <= 3);

    if (qtricks == 0)
    {
        if ((trump == DDS_NOTRUMP) || (winner[trump].hand == -1))
        {
            for (int ss = 0; ss < DDS_SUITS; ss++)
            {
                if (winner[ss].hand == -1)
                    continue;
                if (len[hand][ss] > 0)
                {
                    tpos.win_ranks[ss] = bit_map_rank[winner[ss].rank];
                }
            }

            if (node_type(hand) != MAXNODE)
                cutoff = target - tpos.tricks_max;
            else
            {
                cutoff = tpos.tricks_max - target + (depth >> 2) + 2;
            }

            if (1 >= cutoff)
                return 0;
        }
    }

    result = false;
    return qtricks;
}


inline int QtricksLeadHandTrump(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    const int lhoTrumpRanks,
    const int rhoTrumpRanks,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    int& res)
{
    /* res=0 Continue with same suit.
     res=1 Cutoff.
     res=2 Continue with next suit. */

    res = 1;
    int qt = qtricks;
    if (((countLho != 0) ||
       (lhoTrumpRanks == 0)) &&
            ((countRho != 0) || (rhoTrumpRanks == 0)))
    {
        tpos.win_ranks[suit] |=
            bit_map_rank[tpos.winner[suit].rank];
        qt++;
        if (qt >= cutoff)
            return qt;

        if ((countLho <= 1) &&
                (countRho <= 1) &&
                (countPart <= 1) &&
                (lhoTrumpRanks == 0) &&
                (rhoTrumpRanks == 0))
        {
            qt += countOwn - 1;
            if (qt >= cutoff)
                return qt;
            res = 2;
            return qt;
        }
    }

    if (tpos.second_best[suit].hand == hand)
    {
        if ((lhoTrumpRanks == 0) && (rhoTrumpRanks == 0))
        {
            tpos.win_ranks[suit] |=
                bit_map_rank[tpos.second_best[suit].rank];
            qt++;
            if (qt >= cutoff)
                return qt;
            if ((countLho <= 2) && (countRho <= 2) && (countPart <= 2))
            {
                qt += countOwn - 2;
                if (qt >= cutoff)
                    return qt;
                res = 2;
                return qt;
            }
        }
    }
    else if ((tpos.second_best[suit].hand == partner[hand])
           && (countOwn > 1) && (countPart > 1))
    {
        /* Second best at partner and suit length of own
       hand and partner > 1 */
        if ((lhoTrumpRanks == 0) && (rhoTrumpRanks == 0))
        {
            tpos.win_ranks[suit] |=
                bit_map_rank[tpos.second_best[suit].rank];
            qt++;
            if (qt >= cutoff)
                return qt;
            if ((countLho <= 2) &&
                    (countRho <= 2) &&
                    ((countPart <= 2) || (countOwn <= 2)))
            {
                qt += std::max(countOwn - 2, countPart - 2);
                if (qt >= cutoff)
                    return qt;
                res = 2;
                return qt;
            }
        }
    }
    res = 0;
    return qt;
}

inline int QtricksLeadHandNT(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    int& lhoTrumpRanks,
    int& rhoTrumpRanks,
    const bool commPartner,
    const int commSuit,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    const int trump,
    int& res)
{
    /* res=0 Continue with same suit.
     res=1 Cutoff.
     res=2 Continue with next suit. */

    res = 1;
    int qt = qtricks;
    tpos.win_ranks[suit] |=
        bit_map_rank[tpos.winner[suit].rank];

    qt++;
    if (qt >= cutoff)
        return qt;
    if ((trump == suit) && ((!commPartner) || (suit != commSuit)))
    {
        lhoTrumpRanks = std::max(0, lhoTrumpRanks - 1);
        rhoTrumpRanks = std::max(0, rhoTrumpRanks - 1);
    }

    if ((countLho <= 1) && (countRho <= 1) && (countPart <= 1))
    {
        qt += countOwn - 1;
        if (qt >= cutoff)
            return qt;
        res = 2;
        return qt;
    }

    if (tpos.second_best[suit].hand == hand)
    {
        tpos.win_ranks[suit] |=
            bit_map_rank[tpos.second_best[suit].rank];
        qt++;
        if (qt >= cutoff)
            return qt;
        if ((trump == suit) && ((!commPartner) || (suit != commSuit)))
        {
            lhoTrumpRanks = std::max(0, lhoTrumpRanks - 1);
            rhoTrumpRanks = std::max(0, rhoTrumpRanks - 1);
        }
        if ((countLho <= 2) && (countRho <= 2) && (countPart <= 2))
        {
            qt += countOwn - 2;
            if (qt >= cutoff)
                return qt;
            res = 2;
            return qt;
        }
    }
    else if ((tpos.second_best[suit].hand == partner[hand])
           && (countOwn > 1) && (countPart > 1))
    {
        /* Second best at partner and suit length of own
       hand and partner > 1 */
        tpos.win_ranks[suit] |=
            bit_map_rank[tpos.second_best[suit].rank];
        qt++;
        if (qt >= cutoff)
            return qt;
        if ((trump == suit) && ((!commPartner) || (suit != commSuit)))
        {
            lhoTrumpRanks = std::max(0, lhoTrumpRanks - 1);
            rhoTrumpRanks = std::max(0, rhoTrumpRanks - 1);
        }
        if ((countLho <= 2) &&
                (countRho <= 2) &&
                ((countPart <= 2) || (countOwn <= 2)))
        {
            qt += std::max(countOwn - 2, countPart - 2);
            if (qt >= cutoff)
                return qt;
            res = 2;
            return qt;
        }
    }

    res = 0;
    return qt;
}


inline int QuickTricksPartnerHandTrump(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    const int lhoTrumpRanks,
    const int rhoTrumpRanks,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    const int commSuit,
    const int commRank,
    int& res)
{
    /* res=0 Continue with same suit.
     res=1 Cutoff.
     res=2 Continue with next suit. */

    res = 1;
    int qt = qtricks;
    if (((countLho != 0) || (lhoTrumpRanks == 0)) &&
            ((countRho != 0) || (rhoTrumpRanks == 0)))
    {
        tpos.win_ranks[suit] |=
            bit_map_rank[tpos.winner[suit].rank];

        tpos.win_ranks[commSuit] |= bit_map_rank[commRank];

        qt++; /* A trick can be taken */
        if (qt >= cutoff)
            return qt;
        if ((countLho <= 1) &&
                (countRho <= 1) &&
                (countOwn <= 1) &&
                (lhoTrumpRanks == 0) &&
                (rhoTrumpRanks == 0))
        {
            qt += countPart - 1;
            if (qt >= cutoff)
                return qt;
            res = 2;
            return qt;
        }
    }

    if (tpos.second_best[suit].hand == partner[hand])
    {
        /* Second best found in partners hand */
        if ((lhoTrumpRanks == 0) && (rhoTrumpRanks == 0))
        {
            /* Opponents have no trump */
            tpos.win_ranks[suit] |=
                bit_map_rank[tpos.second_best[suit].rank];

            tpos.win_ranks[commSuit] |= bit_map_rank[commRank];
            qt++;
            if (qt >= cutoff)
                return qt;
            if ((countLho <= 2) && (countRho <= 2) && (countOwn <= 2))
            {
                qt += countPart - 2;
                if (qt >= cutoff)
                    return qt;
                res = 2;
                return qt;
            }
        }
    }
    else if ((tpos.second_best[suit].hand == hand) &&
           (countPart > 1) &&
           (countOwn > 1))
    {
        /* Second best found in own hand and suit lengths of own hand
       and partner > 1*/

        if ((lhoTrumpRanks == 0) && (rhoTrumpRanks == 0))
        {
            /* Opponents have no trump */
            tpos.win_ranks[suit] |=
                bit_map_rank[tpos.second_best[suit].rank];

            tpos.win_ranks[commSuit] |= bit_map_rank[commRank];

            qt++;
            if (qt >= cutoff)
                return qt;
            if ((countLho <= 2) &&
                    (countRho <= 2) &&
                    ((countOwn <= 2) || (countPart <= 2)))
            {
                qt += std::max(countPart - 2, countOwn - 2);
                if (qt >= cutoff)
                    return qt;
                res = 2;
                return qt;
            }
        }
    }
    else if ((suit == commSuit) &&
           (tpos.second_best[suit].hand == lho[hand]) &&
           ((countLho >= 2) || (lhoTrumpRanks == 0)) &&
           ((countRho >= 2) || (rhoTrumpRanks == 0)))
    {
        if (third_best(tpos, suit).hand == partner[hand])
        {
            tpos.win_ranks[suit] |= bit_map_rank[
                static_cast<int>(third_best(tpos, suit).rank) ];

            tpos.win_ranks[commSuit] |= bit_map_rank[commRank];

            qt++;
            if (qt >= cutoff)
                return qt;
            if ((countOwn <= 2) &&
                    (countLho <= 2) &&
                    (countRho <= 2) &&
                    (lhoTrumpRanks == 0) &&
                    (rhoTrumpRanks == 0))
            {
                qt += countPart - 2;
                if (qt >= cutoff)
                    return qt;
            }
        }
    }
    res = 0;
    return qt;
}


inline int QuickTricksPartnerHandNT(
    const int hand,
    Pos& tpos,
    const int cutoff,
    const int depth,
    const int countLho,
    const int countRho,
    const int countOwn,
    const int countPart,
    const int suit,
    const int qtricks,
    const int commSuit,
    const int commRank,
    int& res)
{
    res = 1;
    int qt = qtricks;

    tpos.win_ranks[suit] |=
        bit_map_rank[tpos.winner[suit].rank];

    tpos.win_ranks[commSuit] |= bit_map_rank[commRank];

    qt++;
    if (qt >= cutoff)
        return qt;
    if ((countLho <= 1) && (countRho <= 1) && (countOwn <= 1))
    {
        qt += countPart - 1;
        if (qt >= cutoff)
            return qt;
        res = 2;
        return qt;
    }

    if (tpos.second_best[suit].hand == partner[hand])
    {
        /* Second best found in partners hand */
        tpos.win_ranks[suit] |=
            bit_map_rank[tpos.second_best[suit].rank];

        qt++;
        if (qt >= cutoff)
            return qt;
        if ((countLho <= 2) && (countRho <= 2) && (countOwn <= 2))
        {
            qt += countPart - 2;
            if (qt >= cutoff)
                return qt;
            res = 2;
            return qt;
        }
    }
    else if ((tpos.second_best[suit].hand == hand)
           && (countPart > 1) && (countOwn > 1))
    {
        /* Second best found in own hand and own and
       partner's suit length > 1 */
        tpos.win_ranks[suit] |=
            bit_map_rank[tpos.second_best[suit].rank];

        qt++;
        if (qt >= cutoff)
            return qt;
        if ((countLho <= 2) &&
                (countRho <= 2) &&
                ((countOwn <= 2) || (countPart <= 2)))
        {
            qt += std::max(countPart - 2, countOwn - 2);
            if (qt >= cutoff)
                return qt;
            res = 2;
            return qt;
        }
    }
    else if ((suit == commSuit) &&
           (tpos.second_best[suit].hand == lho[hand]))
    {
        if (third_best(tpos, suit).hand == partner[hand])
        {
            tpos.win_ranks[suit] |= bit_map_rank[
                static_cast<int>(third_best(tpos, suit).rank) ];
            qt++;
            if (qt >= cutoff)
                return qt;
            if ((countOwn <= 2) && (countLho <= 2) && (countRho <= 2))
            {
                qt += countPart - 2;
                if (qt >= cutoff)
                    return qt;
            }
        }
    }
    res = 0;
    return qt;
}


inline bool QuickTricksSecondHand(
    Pos& tpos,
    const int hand,
    const int depth,
    const int target,
    const int trump)
{
    if (depth == -1000)
        return false;

    int ss = tpos.lead.suit;
    unsigned short (*ris)[DDS_SUITS] = tpos.rank_in_suit;
    unsigned short ranks = static_cast<unsigned short>
                         (ris[hand][ss] | ris[partner[hand]][ss]);

    for (int s = 0; s < DDS_SUITS; s++)
        tpos.win_ranks[s] = 0;

    if ((trump != DDS_NOTRUMP) && (ss != trump) &&
            (((ris[hand][ss] == 0) && (ris[hand][trump] != 0)) ||
       ((ris[partner[hand]][ss] == 0) &&
                (ris[partner[hand]][trump] != 0))))
    {
        if ((ris[lho[hand]][ss] == 0) &&
                (ris[lho[hand]][trump] != 0))
            return false;

        /* Own side can ruff, their side can't. */
    }

    else if (ranks > (bit_map_rank[tpos.lead.rank] |
                                        ris[lho[hand]][ss]))
    {
        if ((trump != DDS_NOTRUMP) && (ss != trump) &&
                (ris[lho[hand]][trump] != 0) &&
                (ris[lho[hand]][ss] == 0))
            return false;

        /* Own side has highest card in suit, which LHO can't ruff. */

        int rr = highest_rank[ranks];
        tpos.win_ranks[ss] = bit_map_rank[rr];
    }
    else
    {
        /* No easy way to win current trick for own side. */
        return false;
    }

    int qtricks = 1;

    int cutoff;
    if (node_type(hand) == MAXNODE)
        cutoff = target - tpos.tricks_max;
    else
        cutoff = tpos.tricks_max - target + (depth >> 2) + 3;

    if (qtricks >= cutoff)
        return true;

    if (trump != DDS_NOTRUMP)
        return false;

    /* In NT, second winner (by rank) in same suit. */

    int hh;
    if (ris[hand][ss] > ris[partner[hand]][ss])
        hh = hand; /* Hand to lead next trick */
    else
        hh = partner[hand];

    if ((tpos.winner[ss].hand == hh) &&
            (tpos.second_best[ss].rank != 0) &&
            (tpos.second_best[ss].hand == hh))
    {
        qtricks++;
        tpos.win_ranks[ss] |=
            bit_map_rank[tpos.second_best[ss].rank];

        if (qtricks >= cutoff)
            return true;
    }

    for (int s = 0; s < DDS_SUITS; s++)
    {
        if ((s == ss) || (tpos.length[hh][s] == 0))
            continue;

        if ((tpos.length[lho[hh]][s] == 0) &&
                (tpos.length[rho[hh]][s] == 0) &&
                (tpos.length[partner[hh]][s] == 0))
        {
            /* Long other suit which nobody else holds. */
            qtricks += count_table[ris[hh][s]];
            if (qtricks >= cutoff)
                return true;
        }
        else if ((tpos.winner[s].rank != 0) &&
             (tpos.winner[s].hand == hh))
        {
            /* Top winners in other suits. */
            qtricks++;
            tpos.win_ranks[s] |=
                bit_map_rank[tpos.winner[s].rank];

            if (qtricks >= cutoff)
                return true;
        }
    }

    return false;
}


// ---------------------------------------------------------------------------------------
// Later tricks (DDS later_tricks.cpp).

/**
 * @brief Evaluate minimum possible tricks for a given position in double dummy analysis.
 *
 * This function estimates whether the minimum achievable tricks for the given position,
 * hand, depth, and target can satisfy the target, considering the trump suit and current state.
 *
 * @param tpos Position state
 * @param hand Current hand
 * @param depth Current search depth
 * @param target Target number of tricks
 * @param trump Trump suit
 * @param ctx Solver context
 * @return true if target can be reached, false otherwise
 */
inline bool LaterTricksMIN(
    Pos& tpos,
    const int hand,
    const int depth,
    const int target,
    const int trump)
{

    const bool depth_ok = (depth >= 0 && depth < 50);
    if ((trump == DDS_NOTRUMP) || (tpos.winner[trump].rank == 0))
    {
        int sum = 0;
        for (int ss = 0; ss < DDS_SUITS; ss++)
        {
            int hh = tpos.winner[ss].hand;
            if (hh != -1)
            {
                if (static_cast<unsigned>(hh) < static_cast<unsigned>(DDS_HANDS) &&
                        node_type(hh) == MAXNODE)
                    sum += std::max(tpos.length[hh][ss],
                     tpos.length[partner[hh]][ss]);
            }
        }

        if ((tpos.tricks_max + sum < target) && (sum > 0))
        {
            if ((tpos.tricks_max + (depth >> 2) >= target))
                return true;

            for (int ss = 0; ss < DDS_SUITS; ss++)
            {
                int win_hand = tpos.winner[ss].hand;

                if (win_hand == -1) {
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                }
                else if (static_cast<unsigned>(win_hand) >= static_cast<unsigned>(DDS_HANDS)) {
                    // Invalid hand index; avoid using partner/lho/rho with OOB index.
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                    continue;
                }
                else if (node_type(win_hand) == MINNODE)
                {
                    if ((tpos.rank_in_suit[partner[win_hand]][ss] == 0) &&
                            (tpos.rank_in_suit[lho[win_hand]][ss] == 0) &&
                            (tpos.rank_in_suit[rho[win_hand]][ss] == 0))
                        { if (depth_ok) tpos.win_ranks[ss] = 0; }
                    else
                        { if (depth_ok) tpos.win_ranks[ss] = bit_map_rank[tpos.winner[ss].rank]; }
                }
                else {
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                }
            }
            return false;
        }
    }
    else if (node_type(tpos.winner[trump].hand) == MINNODE)
    {
        if ((tpos.length[hand][trump] == 0) &&
                (tpos.length[partner[hand]][trump] == 0))
        {
            if (((tpos.tricks_max + (depth >> 2) + 1 -
                        std::max(tpos.length[lho[hand]][trump],
                                tpos.length[rho[hand]][trump])) < target))
            {
                for (int ss = 0; ss < DDS_SUITS; ss++)
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                return false;
            }
        }
        else if ((tpos.tricks_max + (depth >> 2)) < target)
        {
            for (int ss = 0; ss < DDS_SUITS; ss++)
                if (depth_ok) tpos.win_ranks[ss] = 0;
            if (depth_ok) tpos.win_ranks[trump] =
                bit_map_rank[tpos.winner[trump].rank];
            return false;
        }
        else if (tpos.tricks_max + (depth >> 2) == target)
        {
            int hh = tpos.second_best[trump].hand;
            if (hh == -1)
                return true;

            if (static_cast<unsigned>(hh) >= static_cast<unsigned>(DDS_HANDS))
                return true;

            int r2 = tpos.second_best[trump].rank;
            if ((node_type(hh) == MINNODE) && (r2 != 0))
            {
                if (tpos.length[hh][trump] > 1 ||
                        tpos.length[partner[hh]][trump] > 1)
                {
                    for (int ss = 0; ss < DDS_SUITS; ss++)
                        if (depth_ok) tpos.win_ranks[ss] = 0;
                    if (depth_ok) tpos.win_ranks[trump] = bit_map_rank[r2];
                    return false;
                }
            }
        }
    }
    else // Not NT
    {
        int hh = tpos.second_best[trump].hand;
        if (hh == -1)
            return true;
        if (static_cast<unsigned>(hh) >= static_cast<unsigned>(DDS_HANDS))
            return true;

        if ((node_type(hh) != MINNODE) ||
                (tpos.length[hh][trump] <= 1))
            return true;

        if (tpos.winner[trump].hand == rho[hh])
        {
            if (((tpos.tricks_max + (depth >> 2)) < target))
            {
                for (int ss = 0; ss < DDS_SUITS; ss++)
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                if (depth_ok) tpos.win_ranks[trump] =
                    bit_map_rank[tpos.second_best[trump].rank];
                return false;
            }
        }
        else
        {
            unsigned short aggr = tpos.aggr[trump];
            // Defensive check: rel[] is sized 8192 in ThreadData. If aggr
            // is out of bounds we avoid a crash and return a conservative result.
            if (aggr >= 8192u)
            {
                return true; // conservative fallback
            }
            int h = third_best(tpos, trump).hand;
            if (h == -1)
                return true;

            if (static_cast<unsigned>(h) >= static_cast<unsigned>(DDS_HANDS))
                return true;

            if ((node_type(h) == MINNODE) &&
                    ((tpos.tricks_max + (depth >> 2)) < target))
            {
                for (int ss = 0; ss < DDS_SUITS; ss++)
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                if (depth_ok) tpos.win_ranks[trump] = bit_map_rank[
                    static_cast<int>(static_cast<unsigned char>(third_best(tpos, trump).rank)) ];
                return false;
            }
        }
    }
    return true;
}


/**
 * @brief Evaluate maximum possible tricks for a given position in double dummy analysis.
 *
 * This function estimates whether the maximum achievable tricks for the given position,
 * hand, depth, and target can satisfy the target, considering the trump suit and current state.
 *
 * @param tpos Position state
 * @param hand Current hand
 * @param depth Current search depth
 * @param target Target number of tricks
 * @param trump Trump suit
 * @param ctx Solver context
 * @return true if target can be reached, false otherwise
 */
inline bool LaterTricksMAX(
    Pos& tpos,
    const int hand,
    const int depth,
    const int target,
    const int trump)
{

    const bool depth_ok = (depth >= 0 && depth < 50);
    if ((trump == DDS_NOTRUMP) || (tpos.winner[trump].rank == 0))
    {
        int sum = 0;
        for (int ss = 0; ss < DDS_SUITS; ss++)
        {
            int hh = tpos.winner[ss].hand;
            if (hh != -1)
            {
                if (static_cast<unsigned>(hh) < static_cast<unsigned>(DDS_HANDS) &&
                        node_type(hh) == MINNODE)
                    sum += std::max(tpos.length[hh][ss],
                     tpos.length[partner[hh]][ss]);
            }
        }

        if ((tpos.tricks_max + (depth >> 2) + 1 - sum >= target) &&
                (sum > 0))
        {
            if ((tpos.tricks_max + 1 < target))
                return false;

            for (int ss = 0; ss < DDS_SUITS; ss++)
            {
                int win_hand = tpos.winner[ss].hand;
                if (win_hand == -1) {
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                }
                else if (static_cast<unsigned>(win_hand) >= static_cast<unsigned>(DDS_HANDS)) {
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                    continue;
                }
                else if (node_type(win_hand) == MAXNODE)
                {
                    if ((tpos.rank_in_suit[partner[win_hand]][ss] == 0) &&
                            (tpos.rank_in_suit[lho[win_hand]][ss] == 0) &&
                            (tpos.rank_in_suit[rho[win_hand]][ss] == 0))
                        { if (depth_ok) tpos.win_ranks[ss] = 0; }
                    else
                        { if (depth_ok) tpos.win_ranks[ss] =
                            bit_map_rank[tpos.winner[ss].rank]; }
                }
                else {
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                }
            }
            return true;
        }
    }
    else if (node_type(tpos.winner[trump].hand) == MAXNODE)
    {
        if ((tpos.length[hand][trump] == 0) &&
                (tpos.length[partner[hand]][trump] == 0))
        {
            int maxlen = std::max(tpos.length[lho[hand]][trump],
                       tpos.length[rho[hand]][trump]);

            if ((tpos.tricks_max + maxlen) >= target)
            {
                for (int ss = 0; ss < DDS_SUITS; ss++)
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                return true;
            }
        }
        else if ((tpos.tricks_max + 1) >= target)
        {
            for (int ss = 0; ss < DDS_SUITS; ss++)
                if (depth_ok) tpos.win_ranks[ss] = 0;
            if (depth_ok) tpos.win_ranks[trump] =
                bit_map_rank[tpos.winner[trump].rank];
            return true;
        }
        else
        {
            int hh = tpos.second_best[trump].hand;
            if (hh == -1)
                return false;

            if (static_cast<unsigned>(hh) >= static_cast<unsigned>(DDS_HANDS))
                return false;

            if ((node_type(hh) == MAXNODE) &&
                    (tpos.second_best[trump].rank != 0))
            {
                if (((tpos.length[hh][trump] > 1) ||
             (tpos.length[partner[hh]][trump] > 1)) &&
                        ((tpos.tricks_max + 2) >= target))
                {
                    for (int ss = 0; ss < DDS_SUITS; ss++)
                        if (depth_ok) tpos.win_ranks[ss] = 0;
                    if (depth_ok) tpos.win_ranks[trump] =
                        bit_map_rank[tpos.second_best[trump].rank];
                    return true;
                }
            }
        }
    }

    else // trump != DDS_NOTRUMP)
    {
        int hh = tpos.second_best[trump].hand;
        if (hh == -1)
            return false;
        if (static_cast<unsigned>(hh) >= static_cast<unsigned>(DDS_HANDS))
            return false;

        if ((node_type(hh) != MAXNODE) ||
                (tpos.length[hh][trump] <= 1))
            return false;

        if (tpos.winner[trump].hand == rho[hh])
        {
            if ((tpos.tricks_max + 1) >= target)
            {
                for (int ss = 0; ss < DDS_SUITS; ss++)
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                if (depth_ok) tpos.win_ranks[trump] =
                    bit_map_rank[tpos.second_best[trump].rank] ;
                return true;
            }
        }
        else
        {
            unsigned short aggr = tpos.aggr[trump];
            // Defensive check mirroring LaterTricksMIN: ThreadData::rel has 8192 entries.
            if (aggr >= 8192u)
            {
                return false; // conservative fallback for MAX
            }
            int h = third_best(tpos, trump).hand;
            if (h == -1)
                return false;

            if (static_cast<unsigned>(h) >= static_cast<unsigned>(DDS_HANDS))
                return false;

            if ((node_type(h) == MAXNODE) &&
                    ((tpos.tricks_max + 1) >= target))
            {
                for (int ss = 0; ss < DDS_SUITS; ss++)
                    if (depth_ok) tpos.win_ranks[ss] = 0;
                if (depth_ok) tpos.win_ranks[trump] = bit_map_rank[
                    static_cast<int>(static_cast<unsigned char>(third_best(tpos, trump).rank)) ];
                return true;
            }
        }
    }
    return false;
}


// ---------------------------------------------------------------------------------------
// Move-ordering weights (DDS heuristic_sorting.cpp).

inline int rank_forces_ace(const HeuristicContext& ctx, const int cards4th);
inline void get_top_number(const HeuristicContext& ctx, const int ris, const int prank, int& top_number, int& mno);

inline void weight_alloc_trump0(HeuristicContext& context)
{
    const unsigned short suitCount = context.tpos.length[context.lead_hand][context.suit];
    const unsigned short suit_count_lh = context.tpos.length[lho[context.lead_hand]][context.suit];
    const unsigned short suit_count_rh = context.tpos.length[rho[context.lead_hand]][context.suit];
    const unsigned short aggr = context.tpos.aggr[context.suit];

    // Why?
    int countLH = (suit_count_lh == 0 ? context.curr_trick + 1 : suit_count_lh) << 2;
    int countRH = (suit_count_rh == 0 ? context.curr_trick + 1 : suit_count_rh) << 2;

    int suit_weight_d = - (((countLH + countRH) << 5) / 13);

    for (int k = context.last_num_moves; k < context.num_moves; k++)
    {
        int suit_bonus = 0;
        bool win_move = false;

        int r_rank = rel_rank[aggr][context.mply[k].rank];

        /* Discourage suit if LHO or RHO can ruff. */
        if ((context.suit != context.trump) &&
                (((context.tpos.rank_in_suit[lho[context.lead_hand]][context.suit] == 0) &&
                    (context.tpos.rank_in_suit[lho[context.lead_hand]][context.trump] != 0)) ||
         ((context.tpos.rank_in_suit[rho[context.lead_hand]][context.suit] == 0) &&
                    (context.tpos.rank_in_suit[rho[context.lead_hand]][context.trump] != 0))))
            suit_bonus = -12;

        /* Encourage suit if partner can ruff. */
        if ((context.suit != context.trump) &&
                (context.tpos.length[partner[context.lead_hand]][context.suit] == 0) &&
                (context.tpos.length[partner[context.lead_hand]][context.trump] > 0) &&
                (suit_count_rh > 0))
            suit_bonus += 17;

        /* Discourage suit if RHO has high card. */
        if ((context.tpos.winner[context.suit].hand == rho[context.lead_hand]) ||
                (context.tpos.second_best[context.suit].hand == rho[context.lead_hand]))
        {
            if (suit_count_rh != 1)
                suit_bonus += -12;
        }

        /* Try suit if LHO has winning card and partner second best.
       Exception: partner has singleton. */

        else if ((context.tpos.winner[context.suit].hand == lho[context.lead_hand]) &&
             (context.tpos.second_best[context.suit].hand == partner[context.lead_hand]))
        {
            /* This case was suggested by Joel Bradmetz. */
            if (context.tpos.length[partner[context.lead_hand]][context.suit] != 1)
                suit_bonus += 27;
        }

        /* Encourage play of suit where partner wins and
       returns the suit for a ruff. */
        if ((context.suit != context.trump) && (suitCount == 1) &&
                (context.tpos.length[context.lead_hand][context.trump] > 0) &&
                (context.tpos.length[partner[context.lead_hand]][context.suit] > 1) &&
                (context.tpos.winner[context.suit].hand == partner[context.lead_hand]))
            suit_bonus += 19;


        /* Discourage a suit selection where the search tree appears larger
       than for the altenative suits: the search is estimated to be
       small when the added number of alternative cards to play for
       the opponents is small. */

        int suit_weight_delta = suit_bonus + suit_weight_d;

        if (context.tpos.winner[context.suit].rank == context.mply[k].rank)
        {
            if ((context.suit != context.trump))
            {
                if ((context.tpos.length[partner[context.lead_hand]][context.suit] != 0) ||
                        (context.tpos.length[partner[context.lead_hand]][context.trump] == 0))
                {
                    if (((context.tpos.length[lho[context.lead_hand]][context.suit] != 0) ||
               (context.tpos.length[lho[context.lead_hand]][context.trump] == 0)) &&
                            ((context.tpos.length[rho[context.lead_hand]][context.suit] != 0) ||
               (context.tpos.length[rho[context.lead_hand]][context.trump] == 0)))
                        win_move = true;
                }
                else if (((context.tpos.length[lho[context.lead_hand]][context.suit] != 0) ||
                                    (context.tpos.rank_in_suit[partner[context.lead_hand]][context.trump] >
                   context.tpos.rank_in_suit[lho[context.lead_hand]][context.trump])) &&
                 ((context.tpos.length[rho[context.lead_hand]][context.suit] != 0) ||
                                    (context.tpos.rank_in_suit[partner[context.lead_hand]][context.trump] >
                   context.tpos.rank_in_suit[rho[context.lead_hand]][context.trump])))
                    win_move = true;
            }
            else
                win_move = true;
        }
        else if (context.tpos.rank_in_suit[partner[context.lead_hand]][context.suit] >
             (context.tpos.rank_in_suit[lho[context.lead_hand]][context.suit] |
                            context.tpos.rank_in_suit[rho[context.lead_hand]][context.suit]))
        {
            if (context.suit != context.trump)
            {
                if (((context.tpos.length[lho[context.lead_hand]][context.suit] != 0) ||
             (context.tpos.length[lho[context.lead_hand]][context.trump] == 0)) &&
                        ((context.tpos.length[rho[context.lead_hand]][context.suit] != 0) ||
             (context.tpos.length[rho[context.lead_hand]][context.trump] == 0)))
                    win_move = true;
            }
            else
                win_move = true;
        }
        else if (context.suit != context.trump)
        {
            if ((context.tpos.length[partner[context.lead_hand]][context.suit] == 0) &&
                    (context.tpos.length[partner[context.lead_hand]][context.trump] != 0))
            {
                if ((context.tpos.length[lho[context.lead_hand]][context.suit] == 0) &&
                        (context.tpos.length[lho[context.lead_hand]][context.trump] != 0) &&
                        (context.tpos.length[rho[context.lead_hand]][context.suit] == 0) &&
                        (context.tpos.length[rho[context.lead_hand]][context.trump] != 0))
                {
                    if (context.tpos.rank_in_suit[partner[context.lead_hand]][context.trump] >
                            (context.tpos.rank_in_suit[lho[context.lead_hand]][context.trump] |
               context.tpos.rank_in_suit[rho[context.lead_hand]][context.trump]))
                        win_move = true;
                }
                else if ((context.tpos.length[lho[context.lead_hand]][context.suit] == 0) &&
                 (context.tpos.length[lho[context.lead_hand]][context.trump] != 0))
                {
                    if (context.tpos.rank_in_suit[partner[context.lead_hand]][context.trump]
                            > context.tpos.rank_in_suit[lho[context.lead_hand]][context.trump])
                        win_move = true;
                }
                else if ((context.tpos.length[rho[context.lead_hand]][context.suit] == 0) &&
                 (context.tpos.length[rho[context.lead_hand]][context.trump] != 0))
                {
                    if (context.tpos.rank_in_suit[partner[context.lead_hand]][context.trump]
                            > context.tpos.rank_in_suit[rho[context.lead_hand]][context.trump])
                        win_move = true;
                }
                else
                    win_move = true;
            }
        }

        if (win_move)
        {
            /* Encourage ruffing LHO or RHO singleton, highest card. */
            if (((suit_count_lh == 1) &&
           (context.tpos.winner[context.suit].hand == lho[context.lead_hand]))
                    || ((suit_count_rh == 1) &&
                            (context.tpos.winner[context.suit].hand == rho[context.lead_hand])))
                context.mply[k].weight = suit_weight_delta + 35 + r_rank;

            /* Lead hand has the highest card. */

            else if (context.tpos.winner[context.suit].hand == context.lead_hand)
            {
                /* Also, partner has second highest card. */
                if (context.tpos.second_best[context.suit].hand == partner[context.lead_hand])
                    context.mply[k].weight = suit_weight_delta + 48 + r_rank;
                else if (context.tpos.winner[context.suit].rank == context.mply[k].rank)
                    /* If the current card to play is the highest card. */
                    context.mply[k].weight = suit_weight_delta + 31;
                else
                    context.mply[k].weight = suit_weight_delta - 3 + r_rank;
            }
            else if (context.tpos.winner[context.suit].hand == partner[context.lead_hand])
            {
                /* If partner has highest card */
                if (context.tpos.second_best[context.suit].hand == context.lead_hand)
                    context.mply[k].weight = suit_weight_delta + 42 + r_rank;
                else
                    context.mply[k].weight = suit_weight_delta + 28 + r_rank;
            }
            /* Encourage playing second highest rank if hand also has
         third highest rank. */
            else if ((context.mply[k].sequence) &&
               (context.mply[k].rank == context.tpos.second_best[context.suit].rank))
                context.mply[k].weight = suit_weight_delta + 40;
            else if (context.mply[k].sequence)
                context.mply[k].weight = suit_weight_delta + 22 + r_rank;
            else
                context.mply[k].weight = suit_weight_delta + 11 + r_rank;

            /* playing cards that previously caused search cutoff
         or was stored as the best move in a transposition table entry
         match. */

            // Only use best_move/best_move_tt if they're valid (non-empty)
            if ((context.best_move.rank > 0) &&
                    (context.best_move.suit == context.suit) &&
                    (context.best_move.rank == context.mply[k].rank))
                context.mply[k].weight += 55;
            else if ((context.best_move_tt.rank > 0) &&
               (context.best_move_tt.suit == context.suit) &&
               (context.best_move_tt.rank == context.mply[k].rank))
                context.mply[k].weight += 18;
        }
        else
        {
            /* Encourage playing the suit if the hand together with partner
         have both the 2nd highest and the 3rd highest cards such that
         the side of the hand has the highest card in the next round
         playing this suit. */

            int thirdBestHand = third_best(context.tpos, context.suit).hand;

            if ((context.tpos.second_best[context.suit].hand == partner[context.lead_hand]) &&
                    (partner[context.lead_hand] == thirdBestHand))
                suit_weight_delta += 20;
            else if (((context.tpos.second_best[context.suit].hand == context.lead_hand) &&
                                (partner[context.lead_hand] == thirdBestHand) &&
                                (context.tpos.length[partner[context.lead_hand]][context.suit] > 1)) ||
               ((context.tpos.second_best[context.suit].hand == partner[context.lead_hand]) &&
                                (context.lead_hand == thirdBestHand) &&
                                (context.tpos.length[partner[context.lead_hand]][context.suit] > 1)))
                suit_weight_delta += 13;

            /* Higher weight if LHO or RHO has the highest (winning) card as
         a singleton. */

            if (((suit_count_lh == 1) &&
           (context.tpos.winner[context.suit].hand == lho[context.lead_hand]))
                    || ((suit_count_rh == 1) &&
                            (context.tpos.winner[context.suit].hand == rho[context.lead_hand])))
                context.mply[k].weight = suit_weight_delta + r_rank + 2;
            else if (context.tpos.winner[context.suit].hand == context.lead_hand)
            {
                if (context.tpos.second_best[context.suit].hand == partner[context.lead_hand])
                    /* Opponents win by ruffing */
                    context.mply[k].weight = suit_weight_delta + 33 + r_rank;
                else if (context.tpos.winner[context.suit].rank == context.mply[k].rank)
                    /* Opponents win by ruffing */
                    context.mply[k].weight = suit_weight_delta + 38;
                else
                    context.mply[k].weight = suit_weight_delta - 14 + r_rank;
            }
            else if (context.tpos.winner[context.suit].hand == partner[context.lead_hand])
            {
                /* Opponents win by ruffing */
                context.mply[k].weight = suit_weight_delta + 34 + r_rank;
            }
            /* Encourage playing second highest rank if hand also has
         third highest rank. */
            else if ((context.mply[k].sequence) &&
               (context.mply[k].rank == context.tpos.second_best[context.suit].rank))
                context.mply[k].weight = suit_weight_delta + 35;
            else
                context.mply[k].weight = suit_weight_delta + 17 - (context.mply[k].rank);

            /* Encourage playing cards that previously caused search cutoff
         or was stored as the best move in a transposition table
         entry match. */

            if ((context.best_move.rank > 0) &&
                    (context.best_move.suit == context.suit) &&
                    (context.best_move.rank == context.mply[k].rank))
                context.mply[k].weight += 18;
        }
    }
}

// Placeholder for the rest of the functions to be moved
inline void weight_alloc_nt0(HeuristicContext& context) {
    int aggr = context.tpos.aggr[context.suit];

    /* Discourage a suit selection where the search tree appears larger
     than for the alternative suits: the search is estimated to be
     small when the added number of alternative cards to play for
     the opponents is small. */

    unsigned short suit_count_lh = context.tpos.length[lho[context.lead_hand]][context.suit];
    unsigned short suit_count_rh = context.tpos.length[rho[context.lead_hand]][context.suit];

    // Why?
    int countLH = (suit_count_lh == 0 ? context.curr_trick + 1 : suit_count_lh) << 2;
    int countRH = (suit_count_rh == 0 ? context.curr_trick + 1 : suit_count_rh) << 2;

    int suit_weight_d = - (((countLH + countRH) << 5) / 19);
    if (context.tpos.length[partner[context.lead_hand]][context.suit] == 0)
        suit_weight_d += -9;

    for (int k = context.last_num_moves; k < context.num_moves; k++)
    {
        int suit_weight_delta = suit_weight_d;
        int r_rank = rel_rank[aggr][context.mply[k].rank];

        if (context.tpos.winner[context.suit].rank == context.mply[k].rank ||
                (context.tpos.rank_in_suit[partner[context.lead_hand]][context.suit] >
         (context.tpos.rank_in_suit[lho[context.lead_hand]][context.suit] |
                    context.tpos.rank_in_suit[rho[context.lead_hand]][context.suit])))
        {
            // Can win trick, ourselves or partner.
            // FIX: No distinction?
            /* Discourage suit if RHO has second best card.
         Exception: RHO has singleton. */
            if (context.tpos.second_best[context.suit].hand == rho[context.lead_hand])
            {
                if (suit_count_rh != 1)
                    suit_weight_delta += -1;
            }
            /* Encourage playing suit if LHO has second highest rank. */
            else if (context.tpos.second_best[context.suit].hand == lho[context.lead_hand])
            {
                if (suit_count_lh != 1)
                    suit_weight_delta += 22;
                else
                    suit_weight_delta += 16;
            }

            /* Higher weight if also second best rank is present on
         current side to play, or if second best is a singleton
         at LHO or RHO. */

            if (((context.tpos.second_best[context.suit].hand != lho[context.lead_hand])
           || (suit_count_lh == 1)) &&
                    ((context.tpos.second_best[context.suit].hand != rho[context.lead_hand])
           || (suit_count_rh == 1)))
                context.mply[k].weight = suit_weight_delta + 45 + r_rank;
            else
                context.mply[k].weight = suit_weight_delta + 18 + r_rank;

            /* Encourage playing cards that previously caused search cutoff
         or was stored as the best move in a transposition table
         entry match. */

            if ((context.best_move.rank > 0) &&
                    (context.best_move.suit == context.suit) &&
                    (context.best_move.rank == context.mply[k].rank))
                context.mply[k].weight += 126;
            else if ((context.best_move_tt.rank > 0) &&
               (context.best_move_tt.suit == context.suit) &&
               (context.best_move_tt.rank == context.mply[k].rank))
                context.mply[k].weight += 32;
        }
        else
        {
            /* Discourage suit if RHO has winning or second best card.
         Exception: RHO has singleton. */

            if ((context.tpos.winner[context.suit].hand == rho[context.lead_hand]) ||
                    (context.tpos.second_best[context.suit].hand == rho[context.lead_hand]))
            {
                if (suit_count_rh != 1)
                    suit_weight_delta += -10;
            }

            /* Try suit if LHO has winning card and partner second best.
         Exception: partner has singleton. */

            else if ((context.tpos.winner[context.suit].hand == lho[context.lead_hand]) &&
               (context.tpos.second_best[context.suit].hand == partner[context.lead_hand]))
            {
                /* This case was suggested by Joel Bradmetz. */
                if (context.tpos.length[partner[context.lead_hand]][context.suit] != 1)
                    suit_weight_delta += 31;
            }

            /* Encourage playing the suit if the hand together with partner
         have both the 2nd highest and the 3rd highest cards such
         that the side of the hand has the highest card in the
         next round playing this suit. */

            int thirdBestHand = third_best(context.tpos, context.suit).hand;

            if ((context.tpos.second_best[context.suit].hand == partner[context.lead_hand]) &&
                    (partner[context.lead_hand] == thirdBestHand))
                suit_weight_delta += 35;
            else if (((context.tpos.second_best[context.suit].hand == context.lead_hand) &&
                                (partner[context.lead_hand] == thirdBestHand) &&
                                (context.tpos.length[partner[context.lead_hand]][context.suit] > 1)) ||
               ((context.tpos.second_best[context.suit].hand == partner[context.lead_hand]) &&
                                (context.lead_hand == thirdBestHand) &&
                                (context.tpos.length[partner[context.lead_hand]][context.suit] > 1)))
                suit_weight_delta += 25;

            /* Higher weight if LHO or RHO has the highest (winning) card
         as a singleton. */

            if (((suit_count_lh == 1) &&
           (context.tpos.winner[context.suit].hand == lho[context.lead_hand]))
                    || ((suit_count_rh == 1) &&
                            (context.tpos.winner[context.suit].hand == rho[context.lead_hand])))
                context.mply[k].weight = suit_weight_delta + 28 + r_rank;
            else if (context.tpos.winner[context.suit].hand == context.lead_hand)
                context.mply[k].weight = suit_weight_delta - 17 + r_rank;
            else if (! context.mply[k].sequence)
                context.mply[k].weight = suit_weight_delta + 12 + r_rank;
            else if (context.mply[k].rank == context.tpos.second_best[context.suit].rank)
                context.mply[k].weight = suit_weight_delta + 48;
            else
                context.mply[k].weight = suit_weight_delta + 29 - r_rank;

            /* Encourage playing cards that previously caused search cutoff
         or was stored as the best move in a transposition table
         entry match. */

            if ((context.best_move.rank > 0) &&
                    (context.best_move.suit == context.suit) &&
                    (context.best_move.rank == context.mply[k].rank))
                context.mply[k].weight += 47;
            else if ((context.best_move_tt.rank > 0) &&
               (context.best_move_tt.suit == context.suit) &&
               (context.best_move_tt.rank == context.mply[k].rank))
                context.mply[k].weight += 19;
        }
    }
}
inline void weight_alloc_trump_notvoid1(HeuristicContext& ctx)
{
    const Pos& tpos = ctx.tpos;
    const int trump = ctx.trump;
    const int lead_hand = ctx.lead_hand;
    const int lead_suit = ctx.lead_suit;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;
    // trackp not needed here; use context snapshots for trick state.


    const int max3rd = highest_rank[
                 tpos.rank_in_suit[partner[lead_hand]][lead_suit]];
    const int maxpd = highest_rank[
                 tpos.rank_in_suit[rho[lead_hand] ][lead_suit]];
    const int min3rd = lowest_rank [
                 tpos.rank_in_suit[partner[lead_hand]][lead_suit]];
    const int minpd = lowest_rank [
                 tpos.rank_in_suit[rho[lead_hand] ][lead_suit]];

    for (int k = 0; k < num_moves; k++)
    {
        bool win_move = false; /* If true, current move can win trick. */
    int r_rank = rel_rank[ tpos.aggr[lead_suit] ][mply[k].rank];

        if (lead_suit == trump)
        {
            if (maxpd > ctx.lead0_rank && maxpd > max3rd)
                win_move = true;
            else if (mply[k].rank > ctx.lead0_rank &&
               mply[k].rank > max3rd)
                win_move = true;
        }
        else
        {
            if (mply[k].rank > ctx.lead0_rank && mply[k].rank > max3rd)
            {
                if ((max3rd != 0) ||
                        (tpos.length[partner[lead_hand]][trump] == 0))
                    win_move = true;
                else if ((maxpd == 0)
                 && (tpos.length[rho[lead_hand]][trump] != 0)
                 && (tpos.rank_in_suit[rho[lead_hand]][trump] >
                     tpos.rank_in_suit[partner[lead_hand]][trump]))
                    win_move = true;
            }
    else if (maxpd > ctx.lead0_rank && maxpd > max3rd)
            {
                if ((max3rd != 0) ||
                        (tpos.length[partner[lead_hand]][trump] == 0))
                    win_move = true;
            }
    else if (ctx.lead0_rank > maxpd &&
           ctx.lead0_rank > max3rd &&
           ctx.lead0_rank > mply[k].rank)
            {
                if ((maxpd == 0) && (tpos.length[rho[lead_hand]][trump] != 0))
                {
                    if ((max3rd != 0) ||
                            (tpos.length[partner[lead_hand]][trump] == 0))
                        win_move = true;
                    else if (tpos.rank_in_suit[rho[lead_hand]][trump]
                   > tpos.rank_in_suit[partner[lead_hand]][trump])
                        win_move = true;
                }
            }
            else if (maxpd == 0 && tpos.length[rho[lead_hand]][trump] != 0)
                /* winnerHand is partner to first */
                win_move = true;
        }

        if (win_move)
        {
            if (min3rd > mply[k].rank)
                // Partner must be winning -- we can't.
                mply[k].weight = 40 + r_rank;
            else if ((maxpd > ctx.lead0_rank) &&
               (tpos.rank_in_suit[lead_hand][lead_suit] >
                                tpos.rank_in_suit[rho[lead_hand]][lead_suit]))
                mply[k].weight = 41 + r_rank;

            /* If rho has a card in the leading suit that
         is higher than the trick leading card but lower
         than the highest rank of the leading hand, then
         lho playing the lowest card will be the cheapest win */

            // FIX: Don't follow

    else if (mply[k].rank > ctx.lead0_rank)
            {
                if (mply[k].rank < maxpd)
                    mply[k].weight = 78 - (mply[k].rank);
                /* If played card is lower than any of the cards of
           rho, it will be the cheapest win */
                else if (mply[k].rank > max3rd)
                    mply[k].weight = 73 - (mply[k].rank);
                /* If played card is higher than any cards at partner
           of the leading hand, rho can play low, under the
           condition that he has a lower card than lho played */
                else if (mply[k].sequence) // May establish a winner
                    mply[k].weight = 62 - (mply[k].rank);
                else
                    mply[k].weight = 49 - (mply[k].rank);
            }
            else if (maxpd > 0)
                mply[k].weight = 47 - (mply[k].rank);
            else
                mply[k].weight = 40 - (mply[k].rank);
        }
        else if (mply[k].rank < min3rd || mply[k].rank < minpd)
            // Will be beaten anyway.
            mply[k].weight = -9 + r_rank;
        else if (mply[k].rank < ctx.lead0_rank)
            // Already beaten.
            mply[k].weight = -16 + r_rank;
        else if (mply[k].sequence)
            // May establish a winner
            mply[k].weight = 22 - (mply[k].rank);
        else
            mply[k].weight = 10 - (mply[k].rank);
    }
}

inline void weight_alloc_nt_notvoid1(HeuristicContext& ctx)
{
    // Faithful port of Moves::weight_alloc_nt_notvoid1(const Pos& tpos)
    const Pos& tpos = ctx.tpos;
    const int lead_hand = ctx.lead_hand;
    const int lead_suit = ctx.lead_suit;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;
    // trackp not needed; using snapshot lead0_rank.

    const int partner_lh = partner[lead_hand];
    const int rho_lh = rho[lead_hand];

    // Original logic from Moves::weight_alloc_nt_notvoid1
    const int max3rd = highest_rank[
        tpos.rank_in_suit[partner_lh][lead_suit]];
    const int maxpd = highest_rank[
        tpos.rank_in_suit[rho_lh][lead_suit] ];

    if (maxpd > ctx.lead0_rank && maxpd > max3rd)
    {
        // Partner can beat both opponents.
        for (int k = 0; k < num_moves; k++)
            mply[k].weight = -mply[k].rank;
    }
    else
    {
    int min3rd = lowest_rank [
                   tpos.rank_in_suit[partner_lh][lead_suit]];
    int minpd = lowest_rank [
                   tpos.rank_in_suit[rho_lh][lead_suit] ];

        for (int k = 0; k < num_moves; k++)
        {
            int r_rank = rel_rank[ tpos.aggr[lead_suit] ][mply[k].rank];

            if (mply[k].rank > ctx.lead0_rank && mply[k].rank > max3rd)
                // We can beat both opponents.
                mply[k].weight = 81 - mply[k].rank;

            else if ((min3rd > mply[k].rank) || (minpd > mply[k].rank))
                // Card can make no difference, so play very low.
                mply[k].weight = -3 + r_rank;

            else if (mply[k].rank < ctx.lead0_rank)
                // Can't beat the card led.
                mply[k].weight = -11 + r_rank;

            else if (mply[k].sequence)
                // Some willingness to split.
                mply[k].weight = 10 + r_rank;

            else
                mply[k].weight = 13 - mply[k].rank;
        }
    }
}
inline void weight_alloc_trump_void1(HeuristicContext& ctx)
{
    const Pos& tpos = ctx.tpos;
    const int trump = ctx.trump;
    const int suit = ctx.suit;
    const int curr_hand = ctx.curr_hand;
    const int lead_hand = ctx.lead_hand;
    const int lead_suit = ctx.lead_suit;
    const int last_num_moves = ctx.last_num_moves;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;
    // trackp not needed here; use context snapshots for trick state.

    const int partner_lh = partner[lead_hand];
    const int rho_lh = rho[lead_hand];

    unsigned short suitCount = tpos.length[curr_hand][suit];
    int suitAdd;

    if (lead_suit == trump) // We pitch
    {
        if (tpos.rank_in_suit[rho_lh][lead_suit] >
                (tpos.rank_in_suit[partner_lh][lead_suit] |
         bit_map_rank[ctx.lead0_rank]))
            // RHO can win.
            suitAdd = (suitCount << 6) / 44;
        else
        {
            // Don't pitch from Kx.
            suitAdd = (suitCount << 6) / 36;
            if ((suitCount == 2) &&
                    (tpos.second_best[suit].hand == curr_hand))
                suitAdd += -4;
        }

        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = -mply[k].rank + suitAdd;
    }
    else if (suit != trump)
    {
        // We discard on a side suit.

        if (tpos.length[partner_lh][lead_suit] != 0)
        {
            // 3rd hand will follow.
            if (tpos.rank_in_suit[rho_lh][lead_suit] >
                    (tpos.rank_in_suit[partner_lh][lead_suit] |
           bit_map_rank[ctx.lead0_rank]))
                // RHO can win.
                suitAdd = 60 + (suitCount << 6) / 44;
            else if ((tpos.length[rho_lh][lead_suit] == 0)
               && (tpos.length[rho_lh][trump] != 0))
                // Partner can ruff.
                suitAdd = 60 + (suitCount << 6) / 44;
            else
            {
                // FIX: No reason to differentiate here?
                suitAdd = -2 + (suitCount << 6) / 36;
                // Don't pitch from Kx.
                if ((suitCount == 2) &&
                        (tpos.second_best[suit].hand == curr_hand))
                    suitAdd += -4;
            }
        }
        else if ((tpos.length[rho_lh][lead_suit] == 0)
             && (tpos.rank_in_suit[rho_lh][trump] >
                 tpos.rank_in_suit[partner_lh][trump]))
            // Partner can overruff 3rd hand.
            suitAdd = 60 + (suitCount << 6) / 44;
        else if ((tpos.length[partner_lh][trump] == 0)
             && (tpos.rank_in_suit[rho_lh][lead_suit] >
                 bit_map_rank[ctx.lead0_rank]))
            // 3rd hand has no trumps, and partner has suit winner.
            suitAdd = 60 + (suitCount << 6) / 44;
        else
        {
            // FIX: No reason to differentiate here?
            suitAdd = -2 + (suitCount << 6) / 36;
            // Don't pitch from Kx.
            if ((suitCount == 2) &&
                    (tpos.second_best[suit].hand == curr_hand))
                suitAdd += -4;
        }
        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = -mply[k].rank + suitAdd;
    }
    else if (tpos.length[partner_lh][lead_suit] != 0)
    {
        // 3rd hand follows suit while we ruff.
        // Could be ruffing partner's winner!
        suitAdd = (suitCount << 6) / 44;
        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = 24 - (mply[k].rank) + suitAdd;
    }
    else if ((tpos.length[rho_lh][lead_suit] == 0)
           && (tpos.length[rho_lh][trump] != 0) &&
           (tpos.rank_in_suit[rho_lh][trump] >
                        tpos.rank_in_suit[partner_lh][trump]))
    {
        // Everybody is void, and partner can overruff.
        suitAdd = (suitCount << 6) / 44;
        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = 24 - (mply[k].rank) + suitAdd;
    }
    else
    {
        for (int k = last_num_moves; k < num_moves; k++)
        {
            if (bit_map_rank[mply[k].rank] >
                    tpos.rank_in_suit[partner_lh][trump])
            {
                // We can ruff, 3rd hand is void but can't overruff.
                suitAdd = (suitCount << 6) / 44;
                mply[k].weight = 24 - (mply[k].rank) + suitAdd;
            }
            else
            {
                // We're getting overruffed. Make trick costly for opponents.
                suitAdd = (suitCount << 6) / 36;
                // Don't ruff from Kx.
                if ((suitCount == 2) &&
                        (tpos.second_best[suit].hand == curr_hand))
                    suitAdd += -4;
                mply[k].weight = 15 - (mply[k].rank) + suitAdd;
            }
        }
    }
}
inline void weight_alloc_nt_void1(HeuristicContext& ctx)
{
    const Pos& tpos = ctx.tpos;
    const int suit = ctx.suit;
    const int curr_hand = ctx.curr_hand;
    const int lead_hand = ctx.lead_hand;
    const int lead_suit = ctx.lead_suit;
    const int last_num_moves = ctx.last_num_moves;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    const int partner_lh = partner[lead_hand];
    const int rho_lh = rho[lead_hand];

    // FIX:
    // Why the different penalties depending on partner?

    if (tpos.rank_in_suit[rho_lh][lead_suit] >
            (tpos.rank_in_suit[partner_lh][lead_suit] |
       bit_map_rank[ctx.lead0_rank]))
    {
        // Partner can win.
        unsigned short suitCount = tpos.length[curr_hand][suit];
        int suitAdd = (suitCount << 6) / 23;
        // Discourage pitch from Kx or A stiff.
        if (suitCount == 2 && tpos.second_best[suit].hand == curr_hand)
            suitAdd += -2;
        else if (suitCount == 1 && tpos.winner[suit].hand == curr_hand)
            suitAdd += -3;

        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = -mply[k].rank + suitAdd;
    }
    else
    {
        unsigned short suitCount = tpos.length[curr_hand][suit];
        int suitAdd = (suitCount << 6) / 33;

        // Discourage pitch from Kx.
        if ((suitCount == 2) &&
                (tpos.second_best[suit].hand == curr_hand))
            suitAdd += -6;

        /* Discourage suit discard of highest card. */
        else if ((suitCount == 1) &&
             (tpos.winner[suit].hand == curr_hand))
            suitAdd += -8;

        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = -mply[k].rank + suitAdd;
    }
}


// Helper functions for level 2+ weight allocation
inline int rank_forces_ace(const HeuristicContext& ctx, const int cards4th)
{
    // Figure out how high we have to play to force out the top.
    const MoveGroupType& mp = group_data[cards4th];

    int g = mp.last_group_;
    int removed = static_cast<int>(ctx.removed_ranks[ctx.lead_suit]);

    while (g >= 1 && ((mp.gap_[g] & removed) == mp.gap_[g]))
        g--;

    if (g <= 0)
        return -1;

    // RHO's second-highest rank.
    int secondRHO = (g == 0 ? 0 : mp.rank_[g-1]);

    if (secondRHO > ctx.move1_rank)
    {
        // Try to force out the top as cheaply as possible.
        int k = 0;
        while (k < ctx.num_moves && ctx.mply[k].rank > secondRHO)
            k++;

        if (k)
            return k - 1;
    }
    else if (ctx.high1 == 1)
    {
        // Try to beat 2nd hand as cheaply as possible.
        int k = 0;
        while (k < ctx.num_moves && ctx.mply[k].rank > ctx.move1_rank)
            k++;

        if (k)
            return k - 1;
    }

    return -1;
}


// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): Intentional ordering; mirrors legacy helper usage
inline void get_top_number(const HeuristicContext& ctx, const int ris, const int prank, int& top_number, int& mno)
{
    top_number = -10;

    // Find the lowest move that still overtakes partner's card.
    mno = 0;
    while (mno < ctx.num_moves - 1 && ctx.mply[1 + mno].rank > prank)
        mno++;

    const MoveGroupType& mp = group_data[ris];
    int g = mp.last_group_;

    // Remove partner's card as well.
    int removed = static_cast<int>(ctx.removed_ranks[ctx.lead_suit] |
                                 bit_map_rank[prank]);

    // Empty suit: last_group_ == -1; no sequence to measure.
    if (g < 0)
    {
        top_number = -1;
        return;
    }

    int fullseq = mp.fullseq_[g];

    while (g >= 1 && ((mp.gap_[g] & removed) == mp.gap_[g]))
        fullseq |= mp.fullseq_[--g];

    top_number = count_table[fullseq] - 1;
}
inline void weight_alloc_trump_notvoid2(HeuristicContext& ctx)
{
    const Pos& tpos = ctx.tpos;
    const int trump = ctx.trump;
    const int lead_hand = ctx.lead_hand;
    const int lead_suit = ctx.lead_suit;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    const int rho_lh = rho[lead_hand];
    const int cards4th = tpos.rank_in_suit[rho_lh][lead_suit];
    const int max4th = highest_rank[cards4th];
    const int min4th = lowest_rank[cards4th];
    const int max3rd = mply[0].rank;

    if (lead_suit == trump)
    {
    if (ctx.high1 == 0 && ctx.lead0_rank > max4th)
        {
            // Partner has already beat his LHO and will beat his RHO.
            for (int k = 0; k < num_moves; k++)
                mply[k].weight = -mply[k].rank;
            return;
        }
        else if (max3rd < min4th || max3rd < ctx.move1_rank)
        {
            // Our cards are too low to matter.
            for (int k = 0; k < num_moves; k++)
                mply[k].weight = -mply[k].rank;
            return;
        }
        else if (max3rd > max4th)
        {
            // We can win the trick.
            for (int k = 0; k < num_moves; k++)
            {
                if (mply[k].rank > max4th &&
                        mply[k].rank > ctx.move1_rank)
                    mply[k].weight = 58 - mply[k].rank;
                else
                    mply[k].weight = -mply[k].rank;
            }
        }
        else
        {
            // Figure out how high we have to play to force out the top.
            int kBonus = rank_forces_ace(ctx, cards4th);

            for (int k = 0; k < num_moves; k++)
                mply[k].weight = -mply[k].rank;

            if (kBonus != -1) // Force out ace
                mply[kBonus].weight += 20;
            return;
        }
    }

    else if (ctx.move1_suit == trump)
    {
        // 2nd hand ruffs, and we must follow suit.
        for (int k = 0; k < num_moves; k++)
            mply[k].weight = -mply[k].rank;
        return;
    }

    // So now lead_suit != trump and second hand didn't ruff.
    else if (ctx.high1 == 0)
    {
        // Partner is winning so far.
        if (max4th == 0)
        {
            // 4th hand is either ruffing or not -- play low.
            for (int k = 0; k < num_moves; k++)
                mply[k].weight = -mply[k].rank;
            return;
        }

        // So 4th hand follows.
    else if (ctx.lead0_rank > max4th)
        {
            // Partner is already winning.
            for (int k = 0; k < num_moves; k++)
                mply[k].weight = -mply[k].rank;
            return;
        }

    else if (max3rd < min4th || max3rd < ctx.move1_rank)
        {
            // Our cards are too low to matter.
            for (int k = 0; k < num_moves; k++)
                mply[k].weight = -mply[k].rank;
            return;
        }

        // So 4th hand can beat partner in the suit.
        else if (max3rd > max4th)
        {
            // We can win the trick.
            for (int k = 0; k < num_moves; k++)
            {
                if (mply[k].rank > max4th)
                    mply[k].weight = 58 - mply[k].rank;
                else
                    mply[k].weight = -mply[k].rank;
            }
        }
        else
        {
            // We can't win the trick.
            // Figure out how high we have to play to force out the top.
            int kBonus = rank_forces_ace(ctx, cards4th);

            for (int k = 0; k < num_moves; k++)
            {
                if (mply[k].rank > ctx.move1_rank &&
                        mply[k].rank > max4th) // We will win
                    mply[k].weight = 60 - mply[k].rank;

                else
                    mply[k].weight = -mply[k].rank;
            }

            if (kBonus != -1) // Force out ace
                mply[kBonus].weight += 20;
        }
    }
    else
    {
        // 2nd hand is winning so far. 4th hand is either ruffing
        // or not -- play high enough to beat 2nd hand.
        if (max4th == 0)
        {
            for (int k = 0; k < num_moves; k++)
            {
                if (mply[k].rank > ctx.move1_rank)
                    mply[k].weight = 20 - mply[k].rank;
                else
                    mply[k].weight = -mply[k].rank;
            }
            return;
        }

        // Our cards are too low to matter.
    else if (max3rd < min4th || max3rd < ctx.move1_rank)
        {
            for (int k = 0; k < num_moves; k++)
                mply[k].weight = -mply[k].rank;
            return;
        }

        // We can win the trick.
        else if (max3rd > max4th)
        {
            for (int k = 0; k < num_moves; k++)
            {
                if (mply[k].rank > ctx.move1_rank &&
                        mply[k].rank > max4th)
                    mply[k].weight = 58 - mply[k].rank;
                else
                    mply[k].weight = -mply[k].rank;
            }
            return;
        }

        // Figure out how high we have to play to force out the top.
        int kBonus = rank_forces_ace(ctx, cards4th);

        for (int k = 0; k < num_moves; k++)
        {
            if (mply[k].rank > ctx.move1_rank &&
                    mply[k].rank > max4th) // We will win
                mply[k].weight = 60 - mply[k].rank;

            else
                mply[k].weight = -mply[k].rank;
        }

        if (kBonus != -1) // Force out ace
            mply[kBonus].weight += 20;
    }
}
inline void weight_alloc_nt_notvoid2(HeuristicContext& ctx)
{
    // One of the main remaining issues here is cashing out long
    // suits. Examples:
    // AKJ opposite Q, overtake.
    // KQx opposite Jxxxx, don't block on the ace.
    // KJTx opposite 9 with Qx in dummy, do win the T.

    const Pos& tpos = ctx.tpos;
    const int lead_hand = ctx.lead_hand;
    const int lead_suit = ctx.lead_suit;
    const int curr_hand = ctx.curr_hand;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    const int rho_lh = rho[lead_hand];
    const int lho_lh = lho[lead_hand];
    const int partner_lh = partner[lead_hand];

    const int cards4th = tpos.rank_in_suit[rho_lh][lead_suit];
    const int max4th = highest_rank[cards4th];
    const int min4th = lowest_rank[cards4th];
    const int max3rd = mply[0].rank;

    if (ctx.high1 == 0 && ctx.lead0_rank > max4th)
    {
        // Partner has already beat his LHO and will beat his RHO.
        // Generally we play low and let partner win.
        for (int k = 0; k < num_moves; k++)
            mply[k].weight = -mply[k].rank;

        // This doesn't help much, not sure why. It does work.

        // if (0 && tpos.length[lead_hand][lead_suit] == 0 &&
        if (tpos.length[lead_hand][lead_suit] == 0 &&
                tpos.winner[lead_suit].hand == curr_hand)
        {
            // Partner has a singleton, and we have the ace.
            // Maybe we should overtake to run the suit.
            int oppLen = tpos.length[rho_lh][lead_suit] - 1;
            int lhoLen = tpos.length[lho_lh][lead_suit];
            if (lhoLen > oppLen)
                oppLen = lhoLen;

            int top_number, mno;
    get_top_number(ctx, tpos.rank_in_suit[partner_lh][lead_suit],
        ctx.lead0_rank, top_number, mno);

            if (oppLen <= top_number)
                mply[mno].weight += 20;
        }
        return;
    }
    else if (max3rd < min4th || max3rd < ctx.move1_rank)
    {
        // Our cards are too low to matter.
        for (int k = 0; k < num_moves; k++)
            mply[k].weight = -mply[k].rank;
        return;
    }

    int kBonus = -1;
    if (max4th > max3rd && max4th > ctx.move1_rank)
        kBonus = rank_forces_ace(ctx, cards4th);

    for (int k = 0; k < num_moves; k++)
    {
        if (mply[k].rank > ctx.move1_rank &&
                mply[k].rank > max4th) // We will win
            mply[k].weight = 60 - mply[k].rank;

        else
            mply[k].weight = -mply[k].rank;
    }

    if (kBonus != -1) // Force out ace
        mply[kBonus].weight += 20;
}
inline void weight_alloc_trump_void2(HeuristicContext& ctx)
{
    // Compared to "v2.8":
    // Moved a test for partner's win out of the k loop.

    const Pos& tpos = ctx.tpos;
    const int trump = ctx.trump;
    const int suit = ctx.suit;
    const int lead_hand = ctx.lead_hand;
    const int lead_suit = ctx.lead_suit;
    const int curr_hand = ctx.curr_hand;
    const int last_num_moves = ctx.last_num_moves;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    const int rho_lh = rho[lead_hand];

    int suitAdd;
    const unsigned short suitCount = tpos.length[curr_hand][suit];
    const int max4th = highest_rank[tpos.rank_in_suit[rho_lh][lead_suit]];

    if (lead_suit == trump || suit != trump)
    {
        // Discard small from a long suit.
        suitAdd = (suitCount << 6) / 40;
        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = -mply[k].rank + suitAdd;
        return;
    }

    else if (ctx.high1 == 0 && ctx.lead0_rank > max4th &&
           (max4th != 0 || tpos.length[rho_lh][trump] == 0))
    {
        // Partner already beat 2nd and 4th hands.
        // Don't overruff partner's sure winner.
        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = -mply[k].rank - 50;
        return;
    }

    // So now we're ruffing and partner is not already sure to win.

    for (int k = last_num_moves; k < num_moves; k++)
    {
        if (ctx.move1_suit == trump &&
                mply[k].rank < ctx.move1_rank)
        {
            // Don't underruff.
            int r_rank = rel_rank[tpos.aggr[suit]][mply[k].rank];
            suitAdd = (suitCount << 6) / 40;
            mply[k].weight = -32 + r_rank + suitAdd;
        }

        else if (ctx.high1 == 0)
        {
            // We ruff partner's winner over 2nd hand.
            if (max4th != 0)
            {
                if (tpos.second_best[lead_suit].hand == lead_hand)
                {
                    // We'd like to know whether partner has KQ or just K,
                    // but that information takes a bit of diggging. It's
                    // easier just not to ruff the king.
                    suitAdd = (suitCount << 6) / 50;
                    mply[k].weight = 36 - mply[k].rank + suitAdd;
                }
                else
                {
                    suitAdd = (suitCount << 6) / 50;
                    mply[k].weight = 48 - mply[k].rank + suitAdd;
                }
            }
            else if (bit_map_rank[mply[k].rank] >
               tpos.rank_in_suit[rho_lh][trump])
            {
                // We ruff higher than 4th hand.
                suitAdd = (suitCount << 6) / 50;
                mply[k].weight = 48 - mply[k].rank + suitAdd;
            }
            else
            {
                // Force out a higher trump in 4th hand.
                suitAdd = (suitCount << 6) / 50;
                mply[k].weight = -12 - mply[k].rank + suitAdd;
            }
        }

        // 2nd hand was winning before we ruffed.
        else if (max4th != 0)
        {
            // Just ruff low.
            suitAdd = (suitCount << 6) / 50;
            mply[k].weight = 72 - mply[k].rank + suitAdd;
        }

        else if (bit_map_rank[mply[k].rank] >
             tpos.rank_in_suit[rho_lh][trump])
        {
            // Ruff higher than 4th hand can.
            suitAdd = (suitCount << 6) / 50;
            mply[k].weight = 48 - mply[k].rank + suitAdd;
        }

        else
        {
            // Force out a higher trump in 4th hand.
            suitAdd = (suitCount << 6) / 50;
            mply[k].weight = 36 - mply[k].rank + suitAdd;
        }
    }
}
inline void weight_alloc_nt_void2(HeuristicContext& ctx)
{
    // Compared to "v2.8":
    // Took only the second branch. The first branch (partner
    // has beat his LHO and will beat his RHO) was a bit different,
    // for no reason that I could see. This is the same or a tiny
    // bit better.

    const Pos& tpos = ctx.tpos;
    const int suit = ctx.suit;
    const int curr_hand = ctx.curr_hand;
    const int last_num_moves = ctx.last_num_moves;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    const unsigned short suitCount = tpos.length[curr_hand][suit];
    int suitAdd = (suitCount << 6) / 24;

    // Try not to pitch from Kx or stiff ace.
    if (suitCount == 2 && tpos.second_best[suit].hand == curr_hand)
        suitAdd -= 4;
    if (suitCount == 1 && tpos.winner[suit].hand == curr_hand)
        suitAdd -= 4;

    for (int k = last_num_moves; k < num_moves; k++)
        mply[k].weight = -(mply[k].rank) + suitAdd;
}
inline void weight_alloc_combined_notvoid3(HeuristicContext& ctx)
{
    // We're always following suit.
    // This function is very good, but occasionally it is better
    // to beat partner's card in order to cash out a suit in NT.

    const int trump = ctx.trump;
    const int lead_suit = ctx.lead_suit;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    if (ctx.high2 == 1 ||
        (lead_suit != trump && ctx.move2_suit == trump))
    {
        // Partner is winning the trick so far, or an opponent
        // has ruffed while we must follow. Play low.

        for (int k = 0; k < num_moves; k++)
            mply[k].weight = -mply[k].rank;
    }
    else
    {
        // We're losing so far, and either trumps were led or
        // trumps don't matter in this trick.

        for (int k = 0; k < num_moves; k++)
        {
            if (mply[k].rank > ctx.move2_rank)
                // Win as cheaply as possible.
                mply[k].weight = 30 - mply[k].rank;
            else
                mply[k].weight = -mply[k].rank;
        }
    }
}
inline void weight_alloc_trump_void3(HeuristicContext& ctx)
{
    // Compared to "v2.8":
    // val removed for trump plays (doesn't really matter, though).

    // To consider:
    // r_rank vs rank

    const Pos& tpos = ctx.tpos;
    const int trump = ctx.trump;
    const int suit = ctx.suit;
    const int lead_suit = ctx.lead_suit;
    const int curr_hand = ctx.curr_hand;
    const int last_num_moves = ctx.last_num_moves;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    // Don't pitch from Kx or stiff ace.
    const int mylen = tpos.length[curr_hand][suit];
    int val = (mylen << 6) / 24;
    if ((mylen == 2) && (tpos.second_best[suit].hand == curr_hand))
        val -= 2;

    if (lead_suit == trump)
    {
        // We're not following suit, so no hope.
        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = -mply[k].rank + val;
    }
    else if (ctx.high2 == 1) // Partner is winning so far
    {
        if (suit == trump) // Don't ruff
            for (int k = last_num_moves; k < num_moves; k++)
                mply[k].weight = 2 - mply[k].rank + val;

        else // Discard from a long suit
            for (int k = last_num_moves; k < num_moves; k++)
                mply[k].weight = 25 - mply[k].rank + val;
    }
    else if (ctx.move2_suit == trump) // They've ruffed
    {
        if (suit == trump)
        {
            for (int k = last_num_moves; k < num_moves; k++)
            {
                int r_rank = rel_rank[tpos.aggr[suit]][mply[k].rank];
                if (mply[k].rank > ctx.move2_rank)
                    mply[k].weight = 33 + r_rank; // Overruff
                else
                    mply[k].weight = -13 + r_rank; // Underruff
            }
        }
        else // We discard
            for (int k = last_num_moves; k < num_moves; k++)
                mply[k].weight = 14 - (mply[k].rank) + val;
    }
    else if (suit == trump) // We ruff and win
    {
        for (int k = last_num_moves; k < num_moves; k++)
        {
            int r_rank = rel_rank[tpos.aggr[suit]][mply[k].rank];
            mply[k].weight = 33 + r_rank;
        }
    }
    else // We discard and lose
    {
        for (int k = last_num_moves; k < num_moves; k++)
            mply[k].weight = 14 - mply[k].rank + val;
    }
}
inline void weight_alloc_nt_void3(HeuristicContext& ctx)
{
    const Pos& tpos = ctx.tpos;
    const int suit = ctx.suit;
    const int curr_hand = ctx.curr_hand;
    const int last_num_moves = ctx.last_num_moves;
    const int num_moves = ctx.num_moves;
    MoveType* mply = ctx.mply;

    int mylen = tpos.length[curr_hand][suit];
    int val = (mylen << 6) / 27;
    // Try not to pitch from Kx, or to pitch a singleton winner.
    if ((mylen == 2) && (tpos.second_best[suit].hand == curr_hand))
        val -= 6;
    else if ((mylen == 1) && (tpos.winner[suit].hand == curr_hand))
        val -= 8;

    for (int k = last_num_moves; k < num_moves; k++)
        mply[k].weight = - mply[k].rank + val;
}


inline void call_heuristic(HeuristicContext& context, const WeightCase weight_case)
{
    switch (weight_case) {
        case WeightCase::Nt0:                weight_alloc_nt0(context); break;
        case WeightCase::Trump0:             weight_alloc_trump0(context); break;
        case WeightCase::NtNotVoid1:         weight_alloc_nt_notvoid1(context); break;
        case WeightCase::TrumpNotVoid1:      weight_alloc_trump_notvoid1(context); break;
        case WeightCase::NtVoid1:            weight_alloc_nt_void1(context); break;
        case WeightCase::TrumpVoid1:         weight_alloc_trump_void1(context); break;
        case WeightCase::NtNotVoid2:         weight_alloc_nt_notvoid2(context); break;
        case WeightCase::TrumpNotVoid2:      weight_alloc_trump_notvoid2(context); break;
        case WeightCase::NtVoid2:            weight_alloc_nt_void2(context); break;
        case WeightCase::TrumpVoid2:         weight_alloc_trump_void2(context); break;
        case WeightCase::CombinedNotVoid3:
        case WeightCase::CombinedNotVoid3Trump:
            weight_alloc_combined_notvoid3(context);
            break;
        case WeightCase::NtVoid3:            weight_alloc_nt_void3(context); break;
        case WeightCase::TrumpVoid3:         weight_alloc_trump_void3(context); break;
        default:
            // Should not happen; fall back to NT leading weights so moves get
            // deterministic ordering instead of stale/uninitialized weights.
            weight_alloc_nt0(context);
            break;
    }
}


}  // namespace port
}  // namespace ddfast
