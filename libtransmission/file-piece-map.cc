// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

#include <small/set.hpp>

#include "libtransmission/transmission.h"

#include "libtransmission/bitfield.h"
#include "libtransmission/block-info.h"
#include "libtransmission/file-piece-map.h"
#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/tr-assert.h"

void tr_file_piece_map::reset(tr_block_info const& block_info, uint64_t const* file_sizes, size_t n_files)
{
    file_bytes_.resize(n_files);
    file_bytes_.shrink_to_fit();

    file_pieces_.resize(n_files);
    file_pieces_.shrink_to_fit();

    auto edge_pieces = small::set<tr_piece_index_t, 1024U>{};
    edge_pieces.reserve(n_files * 2U);

    uint64_t offset = 0U;
    for (tr_file_index_t i = 0U; i < n_files; ++i)
    {
        auto const file_size = file_sizes[i];
        auto const begin_byte = offset;
        auto const begin_piece = block_info.byte_loc(begin_byte).piece;
        auto end_byte = tr_byte_index_t{};
        auto end_piece = tr_piece_index_t{};

        edge_pieces.insert(begin_piece);

        if (file_size != 0U)
        {
            end_byte = begin_byte + file_size;
            auto const final_byte = end_byte - 1U;
            auto const final_piece = block_info.byte_loc(final_byte).piece;
            end_piece = final_piece + 1U;

            edge_pieces.insert(final_piece);
        }
        else
        {
            end_byte = begin_byte;
            // TODO(ckerr): should end_piece == begin_piece, same as _bytes are?
            end_piece = begin_piece + 1U;
        }
        file_bytes_[i] = byte_span_t{ begin_byte, end_byte };
        file_pieces_[i] = piece_span_t{ begin_piece, end_piece };
        offset += file_size;
    }

    edge_pieces_.assign(std::begin(edge_pieces), std::end(edge_pieces));
}

void tr_file_piece_map::reset(tr_torrent_metainfo const& tm)
{
    auto const n = tm.file_count();
    auto file_sizes = std::vector<uint64_t>(n);
    for (tr_file_index_t i = 0U; i < n; ++i)
    {
        file_sizes[i] = tm.file_size(i);
    }
    reset({ tm.total_size(), tm.piece_size() }, std::data(file_sizes), std::size(file_sizes));
}

tr_file_piece_map::file_span_t tr_file_piece_map::file_span(tr_piece_index_t piece) const
{
    static constexpr auto Compare = CompareToSpan<tr_piece_index_t>{};
    auto const begin = std::begin(file_pieces_);
    auto const& [equal_begin, equal_end] = std::equal_range(begin, std::end(file_pieces_), piece, Compare);
    return { static_cast<tr_file_index_t>(equal_begin - begin), static_cast<tr_file_index_t>(equal_end - begin) };
}

tr_file_piece_map::file_offset_t tr_file_piece_map::file_offset(uint64_t offset) const
{
    static constexpr auto Compare = CompareToSpan<uint64_t>{};
    auto const begin = std::begin(file_bytes_);
    auto const it = std::lower_bound(begin, std::end(file_bytes_), offset, Compare);
    tr_file_index_t const file_index = std::distance(begin, it);
    auto const file_offset = offset - it->begin;
    return file_offset_t{ file_index, file_offset };
}

// ---

void tr_file_priorities::reset(tr_file_piece_map const* fpm)
{
    fpm_ = fpm;
    priorities_ = {};
}

void tr_file_priorities::set(tr_file_index_t file, tr_priority_t new_priority)
{
    if (std::empty(priorities_))
    {
        if (new_priority == TR_PRI_NORMAL)
        {
            return;
        }

        priorities_.assign(std::size(*fpm_), TR_PRI_NORMAL);
        priorities_.shrink_to_fit();
    }

    priorities_[file] = new_priority;
}

void tr_file_priorities::set(tr_file_index_t const* files, size_t n, tr_priority_t new_priority)
{
    for (size_t i = 0U; i < n; ++i)
    {
        set(files[i], new_priority);
    }
}

tr_priority_t tr_file_priorities::file_priority(tr_file_index_t file) const
{
    TR_ASSERT(file < std::size(*fpm_));

    if (std::empty(priorities_))
    {
        return TR_PRI_NORMAL;
    }

    return priorities_[file];
}

tr_priority_t tr_file_priorities::piece_priority(tr_piece_index_t piece) const
{
    // increase priority if a file begins or ends in this piece
    // because that makes life easier for code/users using at incomplete files.
    // Xrefs: f2daeb242, https://forum.transmissionbt.com/viewtopic.php?t=10473
    if (fpm_->is_edge_piece(piece))
    {
        return TR_PRI_HIGH;
    }

    // check the priorities of the files that touch this piece
    if (auto const [begin_file, end_file] = fpm_->file_span(piece); end_file <= std::size(priorities_))
    {
        auto const begin = std::begin(priorities_) + begin_file;
        auto const end = std::begin(priorities_) + end_file;
        if (auto const it = std::max_element(begin, end); it != end)
        {
            return *it;
        }
    }

    return TR_PRI_NORMAL;
}

// ---

void tr_files_wanted::reset(tr_file_piece_map const* fpm)
{
    fpm_ = fpm;
    wanted_ = tr_bitfield{ std::size(*fpm) };
    wanted_.set_has_all(); // by default we want all files
}

void tr_files_wanted::set(tr_file_index_t file, bool wanted)
{
    wanted_.set(file, wanted);
}

void tr_files_wanted::set(tr_file_index_t const* files, size_t n, bool wanted)
{
    for (size_t i = 0U; i < n; ++i)
    {
        set(files[i], wanted);
    }
}

bool tr_files_wanted::piece_wanted(tr_piece_index_t piece) const
{
    if (wanted_.has_all())
    {
        return true;
    }

    auto const [begin, end] = fpm_->file_span(piece);
    return wanted_.count(begin, end) != 0U;
}
