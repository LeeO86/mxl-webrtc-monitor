// v210ToPreview's downscale path with AVX2.
//
// The file is compiled for plain x86-64 like the rest of the image: only the functions marked
// MWM_AVX2 use AVX2, and v210ToPreview calls downscaleToPreviewAvx2 only when cpuHasAvx2().
// Lambdas do not inherit the target attribute, so the AVX2 functions use none.
//
// Same result as downscaleToPreview in v210.cpp: each output pixel is the same integer sum
// over the same source samples, divided by the same Divider. Only the order of the work
// changes:
//  - a block is 8 v210 groups (48 pixels, 128 bytes). The words of the source lines of an
//    output row are summed in registers and transposed so that each vector holds one word of
//    the 8 groups;
//  - prefix sums run over the group totals (one 8-lane scan per block) and are stored per
//    sample position: the sum of the luma before x is at block*48 + (x % 6)*8 + the lane of
//    its group;
//  - the prefix sums at the column starts are picked from there with permutes (no gather
//    instructions: with the Downfall/GDS microcode they are slow on Intel up to Ice Lake,
//    and they are slow on AMD Zen 2), and 8 columns are divided at a time;
//  - the chroma of an output row pair is summed once over the source lines of both rows (the
//    scalar code adds the two rows' column sums: the same total).

#include "media/v210_detail.hpp"

#if defined(__x86_64__)

#include <immintrin.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#define MWM_AVX2 __attribute__((target("avx2")))

namespace mwm::detail
{
namespace
{
MWM_AVX2 inline void store(std::uint32_t* p, __m256i v)
{
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(p), v);
}

MWM_AVX2 inline __m256i load(std::uint32_t const* p)
{
    return _mm256_loadu_si256(reinterpret_cast<__m256i const*>(p));
}

// Group g of a block is in lane kLane[g] of the block's vectors: a 256-bit load holds two
// groups, and the transpose works within the 128-bit halves (lanes g0 g2 g4 g6 g1 g3 g5 g7).
constexpr int kLane[8] = {0, 4, 1, 5, 2, 6, 3, 7};

// Named members, not arrays: GCC keeps arrays of vectors on the stack.
struct Quad
{
    __m256i a, b, c, d;
};

struct Six
{
    __m256i s0, s1, s2, s3, s4, s5;
};

MWM_AVX2 inline Quad zeroQuad()
{
    return {_mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256(), _mm256_setzero_si256()};
}

MWM_AVX2 inline Six zeroSix()
{
    __m256i const z = _mm256_setzero_si256();
    return {z, z, z, z, z, z};
}

// Words of a group are Cb0 Y0 Cr0 | Y1 Cb1 Y2 | Cr1 Y3 Cb2 | Y4 Cr2 Y5. Each line's words are
// split into the luma fields (mask L) and the chroma fields (mask C) and summed as they are:
// the low and the middle field have room for many lines, the high field (bits 20-29) for 4
// (12 bits up to bit 31). The sums are split into samples every 4 lines.
MWM_AVX2 inline __m256i lumaMask()
{
    return _mm256_setr_epi32(0x000ffc00, 0x3ff003ff, 0x000ffc00, 0x3ff003ff, 0x000ffc00, 0x3ff003ff, 0x000ffc00, 0x3ff003ff);
}

MWM_AVX2 inline __m256i chromaMask()
{
    return _mm256_setr_epi32(0x3ff003ff, 0x000ffc00, 0x3ff003ff, 0x000ffc00, 0x3ff003ff, 0x000ffc00, 0x3ff003ff, 0x000ffc00);
}

MWM_AVX2 inline __m256i loadWords(std::uint8_t const* p)
{
    return _mm256_loadu_si256(reinterpret_cast<__m256i const*>(p));
}

MWM_AVX2 inline void addLine(std::uint8_t const* p, __m256i maskL, __m256i maskC, Quad& l, Quad& c)
{
    __m256i const w0 = loadWords(p);
    __m256i const w1 = loadWords(p + 32);
    __m256i const w2 = loadWords(p + 64);
    __m256i const w3 = loadWords(p + 96);
    l.a = _mm256_add_epi32(l.a, _mm256_and_si256(w0, maskL));
    l.b = _mm256_add_epi32(l.b, _mm256_and_si256(w1, maskL));
    l.c = _mm256_add_epi32(l.c, _mm256_and_si256(w2, maskL));
    l.d = _mm256_add_epi32(l.d, _mm256_and_si256(w3, maskL));
    c.a = _mm256_add_epi32(c.a, _mm256_and_si256(w0, maskC));
    c.b = _mm256_add_epi32(c.b, _mm256_and_si256(w1, maskC));
    c.c = _mm256_add_epi32(c.c, _mm256_and_si256(w2, maskC));
    c.d = _mm256_add_epi32(c.d, _mm256_and_si256(w3, maskC));
}

// Word 0..3 of the block's 8 groups (lanes in kLane order), from the four loads.
MWM_AVX2 inline Quad transpose(Quad const& r)
{
    __m256i const a = _mm256_unpacklo_epi32(r.a, r.b);
    __m256i const b = _mm256_unpackhi_epi32(r.a, r.b);
    __m256i const c = _mm256_unpacklo_epi32(r.c, r.d);
    __m256i const d = _mm256_unpackhi_epi32(r.c, r.d);
    return {_mm256_unpacklo_epi64(a, c), _mm256_unpackhi_epi64(a, c), _mm256_unpacklo_epi64(b, d), _mm256_unpackhi_epi64(b, d)};
}

// Adds the luma samples (Y0..Y5) of summed words to y.
MWM_AVX2 inline void addLuma(Quad const& l, Six& y)
{
    Quad const w = transpose(l);
    __m256i const low = _mm256_set1_epi32(0xfffff);
    y.s0 = _mm256_add_epi32(y.s0, _mm256_srli_epi32(w.a, 10));
    y.s1 = _mm256_add_epi32(y.s1, _mm256_and_si256(w.b, low));
    y.s2 = _mm256_add_epi32(y.s2, _mm256_srli_epi32(w.b, 20));
    y.s3 = _mm256_add_epi32(y.s3, _mm256_srli_epi32(w.c, 10));
    y.s4 = _mm256_add_epi32(y.s4, _mm256_and_si256(w.d, low));
    y.s5 = _mm256_add_epi32(y.s5, _mm256_srli_epi32(w.d, 20));
}

// Adds the chroma samples (s0..s2 Cb0..Cb2, s3..s5 Cr0..Cr2) of summed words to c.
MWM_AVX2 inline void addChroma(Quad const& c, Six& s)
{
    Quad const w = transpose(c);
    __m256i const low = _mm256_set1_epi32(0xfffff);
    s.s0 = _mm256_add_epi32(s.s0, _mm256_and_si256(w.a, low));
    s.s3 = _mm256_add_epi32(s.s3, _mm256_srli_epi32(w.a, 20));
    s.s1 = _mm256_add_epi32(s.s1, _mm256_srli_epi32(w.b, 10));
    s.s4 = _mm256_add_epi32(s.s4, _mm256_and_si256(w.c, low));
    s.s2 = _mm256_add_epi32(s.s2, _mm256_srli_epi32(w.c, 20));
    s.s5 = _mm256_add_epi32(s.s5, _mm256_srli_epi32(w.d, 10));
}

// Sum of the groups before each group plus *carry, in kLane order; *carry becomes the sum of
// all 8 groups.
MWM_AVX2 inline __m256i exclusiveScan(__m256i t, __m256i& carry)
{
    t = _mm256_permutevar8x32_epi32(t, _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7));
    __m256i x = _mm256_add_epi32(t, _mm256_slli_si256(t, 4));
    x = _mm256_add_epi32(x, _mm256_slli_si256(x, 8));
    __m256i const top = _mm256_shuffle_epi32(x, 0xff);
    x = _mm256_add_epi32(x, _mm256_permute2x128_si256(top, top, 0x08));
    x = _mm256_add_epi32(x, carry);
    carry = _mm256_permutevar8x32_epi32(x, _mm256_set1_epi32(7));
    return _mm256_permutevar8x32_epi32(_mm256_sub_epi32(x, t), _mm256_setr_epi32(0, 2, 4, 6, 1, 3, 5, 7));
}

// Stores the prefix sums before each of the 3 sample positions of a block (8 slots apart).
MWM_AVX2 inline void storePrefix3(__m256i s0, __m256i s1, __m256i s2, __m256i& carry, std::uint32_t* out)
{
    __m256i sum = exclusiveScan(_mm256_add_epi32(_mm256_add_epi32(s0, s1), s2), carry);
    store(out, sum);
    sum = _mm256_add_epi32(sum, s0);
    store(out + 8, sum);
    store(out + 16, _mm256_add_epi32(sum, s1));
}

// The same for the 6 luma positions.
MWM_AVX2 inline void storePrefix6(Six const& y, __m256i& carry, std::uint32_t* out)
{
    __m256i const total = _mm256_add_epi32(_mm256_add_epi32(_mm256_add_epi32(y.s0, y.s1), _mm256_add_epi32(y.s2, y.s3)), _mm256_add_epi32(y.s4, y.s5));
    __m256i sum = exclusiveScan(total, carry);
    store(out, sum);
    sum = _mm256_add_epi32(sum, y.s0);
    store(out + 8, sum);
    sum = _mm256_add_epi32(sum, y.s1);
    store(out + 16, sum);
    sum = _mm256_add_epi32(sum, y.s2);
    store(out + 24, sum);
    sum = _mm256_add_epi32(sum, y.s3);
    store(out + 32, sum);
    store(out + 40, _mm256_add_epi32(sum, y.s4));
}

// One output row of a block: its source lines [from, to) into the luma prefix sums at out, and
// into the pair's chroma sums. Lines go in chunks that end where the chroma sums reach 4 lines,
// so no branch sits in the line loop.
MWM_AVX2 inline void addRow(std::uint8_t const* const* lines, std::size_t from, std::size_t to, std::size_t offset, __m256i maskL, __m256i maskC,
    Quad& c, Six& chroma, std::size_t& chromaLines, __m256i& carry, std::uint32_t* out)
{
    Six y = zeroSix();
    while (from < to)
    {
        std::size_t const n = std::min(to - from, 4 - chromaLines);
        Quad l = zeroQuad();
        for (std::size_t k = 0; k < n; ++k)
        {
            addLine(lines[from + k] + offset, maskL, maskC, l, c);
        }
        addLuma(l, y);
        from += n;
        chromaLines += n;
        if (chromaLines == 4)
        {
            addChroma(c, chroma);
            c = zeroQuad();
            chromaLines = 0;
        }
    }
    storePrefix6(y, carry, out);
}

// The prefix sums of an output row pair: py[r] for the luma of row r (source lines
// [first[r], first[r + 1]) of `lines`), pc for the chroma of both rows: Cb in slots 0..23 of a
// block, Cr in 24..47. Blocks past dataBlocks (the end column of a width that is a multiple of
// 48) have no source and add nothing.
MWM_AVX2 void prefixPair(std::uint8_t const* const* lines, std::size_t const* first, int blocks, int dataBlocks, std::uint32_t* const* py,
    std::uint32_t* pc)
{
    __m256i const maskL = lumaMask();
    __m256i const maskC = chromaMask();
    __m256i carryY0 = _mm256_setzero_si256();
    __m256i carryY1 = _mm256_setzero_si256();
    __m256i carryCb = _mm256_setzero_si256();
    __m256i carryCr = _mm256_setzero_si256();
    for (int b = 0; b < blocks; ++b)
    {
        auto const offset = static_cast<std::size_t>(b) * 128;
        auto const at = static_cast<std::size_t>(b) * 48;
        std::size_t const end = b < dataBlocks ? first[2] : 0;
        Quad c = zeroQuad();
        Six chroma = zeroSix();
        std::size_t chromaLines = 0;
        addRow(lines, std::min(first[0], end), std::min(first[1], end), offset, maskL, maskC, c, chroma, chromaLines, carryY0, py[0] + at);
        addRow(lines, std::min(first[1], end), end, offset, maskL, maskC, c, chroma, chromaLines, carryY1, py[1] + at);
        if (chromaLines > 0)
        {
            addChroma(c, chroma);
        }
        storePrefix3(chroma.s0, chroma.s1, chroma.s2, carryCb, pc + at);
        storePrefix3(chroma.s3, chroma.s4, chroma.s5, carryCr, pc + at + 24);
    }
}

// Where the prefix sums at the column starts are. Job v fills q[8v..8v+7] from a row's table,
// from the blocks at and after `base` (a column start of the next block needs a second
// vector). Each pick permutes one table vector (at `offset` from base: block*48 + j*8); its
// lane holds the group to take, with the sign bit set for the lanes it fills. Jobs whose
// column starts sit at the same places share their picks.
struct Pick
{
    std::int32_t offset = 0;
    std::int32_t lane[8] = {};
};

struct PickJob
{
    std::int32_t base = 0;
    std::int32_t begin = 0;
    std::int32_t end = 0;
};

struct PickTable
{
    std::vector<PickJob> jobs;
    std::vector<Pick> picks;
};

// first: the column starts (and the end), in samples; positions: samples per group (6 luma, 3 chroma).
PickTable buildPicks(std::vector<int> const& first, int positions)
{
    PickTable table;
    int const perBlock = positions * 8;
    std::map<std::vector<std::int32_t>, std::pair<std::int32_t, std::int32_t>> known;
    for (std::size_t v = 0; v < first.size(); v += 8)
    {
        int const base = first[v] / perBlock;
        // The table vector (offset) and the group of each lane.
        std::vector<std::int32_t> key;
        for (std::size_t i = v; i < v + 8 && i < first.size(); ++i)
        {
            int const block = first[i] / perBlock;
            int const local = first[i] - block * perBlock;
            key.push_back((block - base) * 48 + (local % positions) * 8);
            key.push_back(kLane[local / positions]);
        }
        auto it = known.find(key);
        if (it == known.end())
        {
            auto const begin = static_cast<std::int32_t>(table.picks.size());
            for (std::size_t i = 0; i < key.size(); i += 2)
            {
                if (std::find_if(table.picks.begin() + begin, table.picks.end(), [&](Pick const& pick) { return pick.offset == key[i]; }) != table.picks.end())
                {
                    continue;
                }
                Pick pick;
                pick.offset = key[i];
                for (std::size_t k = i; k < key.size(); k += 2)
                {
                    if (key[k] == key[i])
                    {
                        pick.lane[k / 2] = static_cast<std::int32_t>(static_cast<std::uint32_t>(key[k + 1]) | 0x80000000u);
                    }
                }
                table.picks.push_back(pick);
            }
            it = known.emplace(std::move(key), std::make_pair(begin, static_cast<std::int32_t>(table.picks.size()))).first;
        }
        table.jobs.push_back({base * 48, it->second.first, it->second.second});
    }
    return table;
}

MWM_AVX2 inline __m256i pickBlend(__m256i acc, __m256i v, __m256i lanes)
{
    return _mm256_castps_si256(_mm256_blendv_ps(_mm256_castsi256_ps(acc), _mm256_castsi256_ps(v), _mm256_castsi256_ps(lanes)));
}

// q0[ox] and q1[ox] = the prefix sums before column ox's first sample in table0 and table1, two
// tables with the same layout (the luma of both rows of a pair, or Cb and Cr).
MWM_AVX2 void pickColumnStarts(std::uint32_t const* table0, std::uint32_t const* table1, PickTable const& picks, std::uint32_t* q0, std::uint32_t* q1)
{
    std::size_t out = 0;
    for (auto const& job : picks.jobs)
    {
        std::uint32_t const* const t0 = table0 + job.base;
        std::uint32_t const* const t1 = table1 + job.base;
        Pick const* pick = picks.picks.data() + job.begin;
        Pick const* const end = picks.picks.data() + job.end;
        __m256i lanes = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(pick->lane));
        __m256i a0 = _mm256_permutevar8x32_epi32(load(t0 + pick->offset), lanes);
        __m256i a1 = _mm256_permutevar8x32_epi32(load(t1 + pick->offset), lanes);
        for (++pick; pick < end; ++pick)
        {
            lanes = _mm256_loadu_si256(reinterpret_cast<__m256i const*>(pick->lane));
            a0 = pickBlend(a0, _mm256_permutevar8x32_epi32(load(t0 + pick->offset), lanes), lanes);
            a1 = pickBlend(a1, _mm256_permutevar8x32_epi32(load(t1 + pick->offset), lanes), lanes);
        }
        store(q0 + out, a0);
        store(q1 + out, a1);
        out += 8;
    }
}

// The Divider constants of each output column for one line count, as arrays for vector loads.
// magic is below 2^32 for every count up to kMaxDivisor (2^l < 2·count, so magic < 2^(l+10)).
// When every count is a power of two 2^p, magic is 2^(p+9) and shift 2p+9, so the division is
// (sum + half) >> p (`bits`).
struct DividerTable
{
    std::vector<std::uint32_t> magic;
    std::vector<std::uint32_t> shift;
    std::vector<std::uint32_t> half;
    std::vector<std::uint32_t> bits;
    bool powersOfTwo = true;
};

class ColumnDividerTables
{
public:
    ColumnDividerTables() = default;

    ColumnDividerTables(std::vector<int> first, std::size_t padded)
        : first_(std::move(first))
        , padded_(padded)
    {
    }

    DividerTable const& forLines(std::uint32_t lines)
    {
        auto it = sets_.find(lines);
        if (it == sets_.end())
        {
            DividerTable table;
            table.magic.assign(padded_, 0);
            table.shift.assign(padded_, 0);
            table.half.assign(padded_, 0);
            table.bits.assign(padded_, 0);
            for (std::size_t ox = 0; ox + 1 < first_.size(); ++ox)
            {
                auto const count = static_cast<std::uint32_t>(first_[ox + 1] - first_[ox]) * lines * 4;
                Divider const d(count);
                table.magic[ox] = static_cast<std::uint32_t>(d.magic);
                table.shift[ox] = d.shift;
                table.half[ox] = d.half;
                table.bits[ox] = (d.shift - 9) / 2;
                table.powersOfTwo = table.powersOfTwo && (count & (count - 1)) == 0;
            }
            it = sets_.emplace(lines, std::move(table)).first;
        }
        return it->second;
    }

private:
    std::vector<int> first_;
    std::size_t padded_ = 0;
    std::map<std::uint32_t, DividerTable> sets_;
};

// Divider::rounded for 8 columns, then min(255): 32x32 to 64-bit products of the even and the
// odd lanes, each shifted by its own count.
MWM_AVX2 inline __m256i divideRounded(__m256i sum, DividerTable const& d, std::size_t ox)
{
    __m256i const s = _mm256_add_epi32(sum, load(d.half.data() + ox));
    __m256i q;
    if (d.powersOfTwo)
    {
        q = _mm256_srlv_epi32(s, load(d.bits.data() + ox));
    }
    else
    {
        __m256i const magic = load(d.magic.data() + ox);
        __m256i const shift = load(d.shift.data() + ox);
        __m256i const even = _mm256_srlv_epi64(_mm256_mul_epu32(s, magic), _mm256_and_si256(shift, _mm256_set1_epi64x(0xffffffff)));
        __m256i const odd = _mm256_srlv_epi64(_mm256_mul_epu32(_mm256_srli_epi64(s, 32), _mm256_srli_epi64(magic, 32)), _mm256_srli_epi64(shift, 32));
        q = _mm256_blend_epi32(even, _mm256_slli_epi64(odd, 32), 0xaa);
    }
    return _mm256_min_epu32(q, _mm256_set1_epi32(255));
}

// 8 values of at most 255 as 8 bytes.
MWM_AVX2 inline __m128i packBytes(__m256i v)
{
    __m128i const words = _mm_packus_epi32(_mm256_castsi256_si128(v), _mm256_extracti128_si256(v, 1));
    return _mm_packus_epi16(words, words);
}

// The first n (1..16) bytes of v.
MWM_AVX2 inline void storeBytes(std::uint8_t* dst, __m128i v, int n)
{
    if (n == 8)
    {
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dst), v);
        return;
    }
    if (n == 16)
    {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), v);
        return;
    }
    alignas(16) std::uint8_t bytes[16];
    _mm_store_si128(reinterpret_cast<__m128i*>(bytes), v);
    std::memcpy(dst, bytes, static_cast<std::size_t>(n));
}

// Column ox's sum from the picked prefix sums (q[ox] = sum before its first sample).
MWM_AVX2 inline __m256i columnSums(std::uint32_t const* q, int ox)
{
    return _mm256_sub_epi32(load(q + ox + 1), load(q + ox));
}

MWM_AVX2 void lumaRow(std::uint32_t const* q, DividerTable const& d, int outWidth, std::uint8_t* out)
{
    for (int ox = 0; ox < outWidth; ox += 8)
    {
        storeBytes(out + ox, packBytes(divideRounded(columnSums(q, ox), d, static_cast<std::size_t>(ox))), std::min(8, outWidth - ox));
    }
}

// One chroma row: interleaved into u for NV12, else into the u and v planes.
MWM_AVX2 void chromaRow(std::uint32_t const* qcb, std::uint32_t const* qcr, DividerTable const& d, int n, bool nv12, std::uint8_t* u, std::uint8_t* v)
{
    for (int ox = 0; ox < n; ox += 8)
    {
        auto const at = static_cast<std::size_t>(ox);
        __m128i const bu = packBytes(divideRounded(columnSums(qcb, ox), d, at));
        __m128i const bv = packBytes(divideRounded(columnSums(qcr, ox), d, at));
        int const count = std::min(8, n - ox);
        if (nv12)
        {
            storeBytes(u + at * 2, _mm_unpacklo_epi8(bu, bv), count * 2);
        }
        else
        {
            storeBytes(u + at, bu, count);
            storeBytes(v + at, bv, count);
        }
    }
}

// Entries for n columns plus the end column, in whole vectors (pick jobs write 8 at a time).
std::size_t padded(int n)
{
    return static_cast<std::size_t>((n + 7) / 8 * 8 + 8);
}

// The tables and buffers of one input and output width. The monitor converts each channel on
// its own thread at a fixed size, so each thread builds them once.
struct Columns
{
    int width = -1;
    int outWidth = -1;
    PickTable lumaPicks;
    PickTable chromaPicks;
    ColumnDividerTables lumaDividers;
    ColumnDividerTables chromaDividers;
    std::vector<std::uint32_t> prefix;
    std::vector<std::uint32_t> q0;
    std::vector<std::uint32_t> q1;
    std::vector<std::uint32_t> qcb;
    std::vector<std::uint32_t> qcr;
    std::vector<std::uint8_t const*> lines;
};

Columns& columnsFor(int width, int outWidth)
{
    thread_local Columns columns;
    if (columns.width == width && columns.outWidth == outWidth)
    {
        return columns;
    }
    int const chromaWidth = width / 2;
    int const outChromaWidth = outWidth / 2;
    std::vector<int> lumaFirst(static_cast<std::size_t>(outWidth) + 1);
    std::vector<int> chromaFirst(static_cast<std::size_t>(outChromaWidth) + 1);
    for (int x = 0; x <= outWidth; ++x)
    {
        lumaFirst[static_cast<std::size_t>(x)] = static_cast<int>(static_cast<long long>(x) * width / outWidth);
    }
    for (int x = 0; x <= outChromaWidth; ++x)
    {
        chromaFirst[static_cast<std::size_t>(x)] = static_cast<int>(static_cast<long long>(x) * chromaWidth / outChromaWidth);
    }
    columns = Columns{};
    columns.width = width;
    columns.outWidth = outWidth;
    columns.lumaPicks = buildPicks(lumaFirst, 6);
    columns.chromaPicks = buildPicks(chromaFirst, 3);
    columns.lumaDividers = ColumnDividerTables(std::move(lumaFirst), padded(outWidth));
    columns.chromaDividers = ColumnDividerTables(std::move(chromaFirst), padded(outChromaWidth));
    columns.prefix.resize(static_cast<std::size_t>(width / 48 + 1) * 48 * 3);
    columns.q0.resize(padded(outWidth));
    columns.q1.resize(padded(outWidth));
    columns.qcb.resize(padded(outChromaWidth));
    columns.qcr.resize(padded(outChromaWidth));
    return columns;
}
} // namespace

void downscaleToPreviewAvx2(std::uint8_t const* v210, int width, int rows, int step, std::size_t stride, int outWidth, int outHeight, bool nv12,
    std::uint8_t* dst)
{
    Columns& c = columnsFor(width, outWidth);
    int const outChromaWidth = outWidth / 2;
    // The end column (luma `width`, group width/6) needs a slot too: one more block than the
    // full ones. A line holds stride/128 blocks; past that the samples count as zero, and
    // samples past the width (in the line's padding) are never part of a column's sum.
    int const blocks = width / 48 + 1;
    int const dataBlocks = std::min(blocks, static_cast<int>(stride / 128));
    std::uint32_t* const py[2] = {c.prefix.data(), c.prefix.data() + static_cast<std::size_t>(blocks) * 48};
    std::uint32_t* const pc = c.prefix.data() + static_cast<std::size_t>(blocks) * 96;
    std::uint8_t* planeY = dst;
    std::uint8_t* planeU = dst + static_cast<std::size_t>(outWidth) * static_cast<std::size_t>(outHeight);
    std::uint8_t* planeV = planeU + static_cast<std::size_t>(outChromaWidth) * static_cast<std::size_t>(outHeight / 2);
    for (int pair = 0; pair < outHeight / 2; ++pair)
    {
        c.lines.clear();
        std::size_t first[3] = {0, 0, 0};
        std::uint32_t lumaLines[2] = {1, 1};
        for (int half = 0; half < 2; ++half)
        {
            int const oy = pair * 2 + half;
            int const from = static_cast<int>(static_cast<long long>(oy) * rows / outHeight);
            int const last = std::max(from + 1, static_cast<int>(static_cast<long long>(oy + 1) * rows / outHeight));
            for (int sy = from; sy < last && sy < rows; ++sy)
            {
                c.lines.push_back(v210 + static_cast<std::size_t>(sy) * static_cast<std::size_t>(step) * stride);
            }
            first[half + 1] = c.lines.size();
            lumaLines[half] = static_cast<std::uint32_t>(std::max(1, std::min(last, rows) - from));
        }
        prefixPair(c.lines.data(), first, blocks, dataBlocks, py, pc);
        pickColumnStarts(py[0], py[1], c.lumaPicks, c.q0.data(), c.q1.data());
        std::uint8_t* const rowY = planeY + static_cast<std::size_t>(pair) * 2 * static_cast<std::size_t>(outWidth);
        lumaRow(c.q0.data(), c.lumaDividers.forLines(lumaLines[0]), outWidth, rowY);
        lumaRow(c.q1.data(), c.lumaDividers.forLines(lumaLines[1]), outWidth, rowY + outWidth);
        pickColumnStarts(pc, pc + 24, c.chromaPicks, c.qcb.data(), c.qcr.data());
        auto const& divide = c.chromaDividers.forLines(std::max<std::uint32_t>(1, static_cast<std::uint32_t>(c.lines.size())));
        if (nv12)
        {
            chromaRow(c.qcb.data(), c.qcr.data(), divide, outChromaWidth, true,
                planeU + static_cast<std::size_t>(pair) * static_cast<std::size_t>(outWidth), nullptr);
        }
        else
        {
            chromaRow(c.qcb.data(), c.qcr.data(), divide, outChromaWidth, false,
                planeU + static_cast<std::size_t>(pair) * static_cast<std::size_t>(outChromaWidth),
                planeV + static_cast<std::size_t>(pair) * static_cast<std::size_t>(outChromaWidth));
        }
    }
}
} // namespace mwm::detail

#endif
